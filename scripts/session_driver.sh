#!/bin/bash
# session_driver.sh — drive BG3 from launch to a running session with
# stall DETECTION, bounded phases, and self-diagnosis artifacts.
#
# Never waits unbounded: every game state has a stall budget. On stall it
# captures a screenshot + game-state + SE log tail into a JSONL diagnosis
# file, attempts a bounded recovery (menu Continue click computed from live
# window geometry), and gives a definitive verdict on exit:
#   exit 0 VERDICT=SESSION_RUNNING   session reached, PID tracked
#   exit 1 VERDICT=GAME_DIED         process exited (crash report listed)
#   exit 2 VERDICT=STUCK             stall budget + recoveries exhausted
#   exit 3 VERDICT=REPLACED          PID changed mid-drive (watchdog relaunch)
#   exit 4 VERDICT=PREFLIGHT_FAILED  Steam, window attestation, or memory check failed
#   exit 5 VERDICT=AMBIGUOUS_ATTACH  attach mode found zero/multiple eligible processes
#
# Usage: session_driver.sh [--launch] [--attach] [--soak N] [--diagnosis FILE]
#   --launch     quit any running game and launch fresh (headless, -continueGame)
#   --attach     attach to an existing BG3 process (exactly one must exist)
#   --soak N     after session loads, keep watching the same PID for N seconds
#   --diagnosis  JSONL output path (default: docs/bugs/session-driver-diagnosis.jsonl)

set -u
cd "$(dirname "$0")/.." || exit 9
REPO="$PWD"

LAUNCH=0; ATTACH=0; SOAK=0
DIAG="$REPO/docs/bugs/session-driver-diagnosis.jsonl"
while [ $# -gt 0 ]; do
    case "$1" in
        --launch) LAUNCH=1 ;;
        --attach) ATTACH=1 ;;
        --soak)
            if [ $# -lt 2 ]; then
                echo "ERROR: --soak requires a numeric argument" >&2
                exit 4
            fi
            SOAK="$2"
            if ! echo "$SOAK" | grep -qE '^[0-9]+$'; then
                echo "ERROR: --soak value must be a non-negative integer, got '$SOAK'" >&2
                exit 4
            fi
            shift
            ;;
        --diagnosis)
            if [ $# -lt 2 ]; then
                echo "ERROR: --diagnosis requires a path argument" >&2
                exit 4
            fi
            DIAG="$2"; shift
            ;;
        -*)
            echo "ERROR: unknown option '$1'" >&2
            exit 4
            ;;
        *)
            echo "ERROR: unexpected argument '$1'" >&2
            exit 4
            ;;
    esac
    shift
done

# Per-state stall budgets (seconds without a state transition)
budget_for() {
    case "$1" in
        Menu) echo 15 ;;                      # menu = auto-continue failed; recover fast
        Init|LoadModule|LoadMenu|StopLoading) echo 90 ;;
        LoadSession|LoadLevel|PrepareRunning|Save) echo 240 ;;  # loads are slow
        *) echo 120 ;;
    esac
}
MAX_RECOVERIES=3
OVERALL_DEADLINE=600

SELOG="$HOME/Library/Application Support/BG3SE/logs/latest.log"
START_EPOCH=$(date +%s)
# BSD find's -newermt does NOT accept @epoch syntax (it silently matches
# nothing), so keep a human-readable stamp for every find invocation.
START_STAMP=$(date "+%Y-%m-%d %H:%M:%S")

log()  { echo "[driver $(date +%H:%M:%S)] $*"; }

diagnose() {  # diagnose <event> <detail>
    local shot=""
    shot="$REPO/.screenshots/driver-$(date +%H%M%S).jpg"
    PYTHONPATH=tools python3 -m bg3se_harness screenshot >/dev/null 2>&1 \
        && cp "$REPO/.screenshots/latest.jpg" "$shot" 2>/dev/null || shot=""
    python3 - "$1" "$2" "$shot" >> "$DIAG" <<'PY'
import json, sys, subprocess, glob, os, time
event, detail, shot = sys.argv[1:4]
nl = sorted(glob.glob('network.2026-*.log'), key=os.path.getmtime)
tail = ''
if nl:
    lines = open(nl[-1], errors='replace').readlines()[-3:]
    tail = ' | '.join(l.strip()[-90:] for l in lines)
selog = os.path.expanduser('~/Library/Application Support/BG3SE/logs/latest.log')
setail = ''
if os.path.exists(selog):
    setail = ' | '.join(l.strip()[-110:] for l in open(selog, errors='replace').readlines()[-3:])
print(json.dumps({'t': time.strftime('%Y-%m-%d %H:%M:%S'), 'event': event,
                  'detail': detail, 'screenshot': shot,
                  'network_tail': tail, 'se_tail': setail}))
PY
    log "DIAGNOSED $1: $2 ${shot:+(screenshot: $shot)}"
}

game_pid_by_path() {
    # Return PIDs of processes whose executable path ends with the BG3 binary.
    pgrep -f "MacOS/Baldur's Gate 3" 2>/dev/null
}

game_state() {  # last GameStateMachine target state from THIS run's network log.
    # Only trust logs modified after the driver started — the newest log on
    # disk can belong to a previous session (stale-log false positives).
    # A harness-launched game writes network.*.log to the repo root; a
    # Steam-relaunched one writes to the game install dir (Steam's cwd).
    local nl
    nl=$(find . "$HOME/Library/Application Support/Steam/steamapps/common/Baldurs Gate 3" \
              -maxdepth 1 -name "network.2026-*.log" -newermt "$START_STAMP" -print0 2>/dev/null \
         | xargs -0 ls -t 2>/dev/null | head -1)
    [ -n "$nl" ] || { echo "NoLogYet"; return; }
    local st
    st=$(grep -o "to: [A-Za-z]*" "$nl" 2>/dev/null | tail -1 | cut -d' ' -f2)
    echo "${st:-NoTransitions}"
}

get_process_start_time() {
    ps -p "$1" -o lstart= 2>/dev/null
}

verify_identity() {
    # verify_identity <pid> <expected_start_time>
    # Returns 0 if process is alive AND start time matches.
    local pid="$1" expected_start="$2"
    kill -0 "$pid" 2>/dev/null || return 1
    local current_start
    current_start=$(get_process_start_time "$pid")
    [ -n "$current_start" ] && [ "$current_start" = "$expected_start" ]
}

recover_menu() {  # recover_menu <attempt#>
    PYTHONPATH=tools python3 - "$1" <<'PY'
import sys, time, subprocess
from bg3se_harness import menu
attempt = int(sys.argv[1])
subprocess.run(['osascript', '-e',
    'tell application "System Events" to set visible of process "Baldur\'s Gate 3" to true'],
    capture_output=True)
menu.activate_bg3(); time.sleep(0.6)
info = menu._get_quartz_window_info()
if not info:
    print("recover: no window info"); raise SystemExit(1)
b = info['bounds']
def click(fx, fy, pause=0.45):
    x, y = b['x'] + int(b['width'] * fx), b['y'] + int(b['height'] * fy)
    menu.cg_click(x, y); time.sleep(pause)
    return x, y
if attempt <= 1:
    x, y = click(0.292, 0.380)
    print(f"recover: clicked Continue at ({x},{y}) in {b['width']}x{b['height']}@({b['x']},{b['y']})")
else:
    for row in range(8):
        click(0.786, 0.305 + row * 0.075)
    x, y = click(0.441, 0.888)
    print(f"recover: Mod Verification sweep — 8 checkboxes + Start Game at ({x},{y})")
PY
}

# ---- Phase 0: launch or attach --------------------------------------------
LAUNCH_ID=""
PID=""
PID_START_TIME=""

if [ "$ATTACH" = 1 ]; then
    CANDIDATES=$(game_pid_by_path)
    COUNT=$(echo "$CANDIDATES" | grep -c '[0-9]' || true)
    if [ "$COUNT" -eq 0 ]; then
        diagnose AMBIGUOUS_ATTACH "no BG3 process found for attach"
        echo "VERDICT=AMBIGUOUS_ATTACH"; exit 5
    elif [ "$COUNT" -gt 1 ]; then
        diagnose AMBIGUOUS_ATTACH "multiple BG3 processes found: $CANDIDATES"
        echo "VERDICT=AMBIGUOUS_ATTACH"; exit 5
    fi
    PID=$(echo "$CANDIDATES" | head -1)
    PID_START_TIME=$(get_process_start_time "$PID")
    log "attached to PID $PID (start: $PID_START_TIME)"
elif [ "$LAUNCH" = 1 ]; then
    log "launching fresh (headless, -continueGame)"
    PYTHONPATH=tools python3 -m bg3se_harness quit --force >/dev/null 2>&1
    sleep 3

    LAUNCH_STDOUT=$(mktemp)
    LAUNCH_STDERR=$(mktemp)
    PYTHONPATH=tools python3 -m bg3se_harness launch --headless --continue --timeout 120 \
        > "$LAUNCH_STDOUT" 2> "$LAUNCH_STDERR"
    LAUNCH_EXIT=$?

    if [ "$LAUNCH_EXIT" -ne 0 ]; then
        cat "$LAUNCH_STDERR" >&2
        diagnose PREFLIGHT_FAILED "launch exited with code $LAUNCH_EXIT"
        rm -f "$LAUNCH_STDOUT" "$LAUNCH_STDERR"
        echo "VERDICT=PREFLIGHT_FAILED"; exit 4
    fi

    # Copy launch JSON for diagnosis
    cp "$LAUNCH_STDOUT" /tmp/session_driver_launch.json 2>/dev/null

    # Extract PID and launch_id from launch JSON (path passed via argv, not
    # interpolated into the Python literal)
    PID=$(python3 -c "import json,sys; d=json.load(open(sys.argv[1])); print(d.get('pid',''))" "$LAUNCH_STDOUT" 2>/dev/null)
    LAUNCH_ID=$(python3 -c "import json,sys; d=json.load(open(sys.argv[1])); print(d.get('launch_id',''))" "$LAUNCH_STDOUT" 2>/dev/null)
    rm -f "$LAUNCH_STDOUT" "$LAUNCH_STDERR"

    if [ -z "$PID" ] || [ "$PID" = "None" ]; then
        diagnose LAUNCH_FAILED "no PID in launch output"
        echo "VERDICT=PREFLIGHT_FAILED"; exit 4
    fi

    PID_START_TIME=$(get_process_start_time "$PID")
    log "launched PID $PID (launch_id=$LAUNCH_ID, start=$PID_START_TIME)"

    # Launch-settling phase: require the same PID to stay alive for the
    # full settle window (15s, checked every 3s). If the direct PID
    # disappears, scan for a Steam replacement and restart the window.
    SETTLE_WINDOW=15
    SETTLE_CHECK=3
    SETTLE_DEADLINE=$(($(date +%s) + SETTLE_WINDOW))
    SETTLED=0
    while :; do
        NOW=$(date +%s)
        [ "$NOW" -ge "$SETTLE_DEADLINE" ] && { SETTLED=1; break; }
        if kill -0 "$PID" 2>/dev/null; then
            sleep "$SETTLE_CHECK"
            continue
        fi
        log "direct PID $PID gone during settle phase, scanning for replacement..."
        sleep 2
        NEW_CANDIDATES=$(game_pid_by_path)
        NEW_COUNT=$(echo "$NEW_CANDIDATES" | grep -c '[0-9]' || true)
        if [ "$NEW_COUNT" -eq 1 ]; then
            NEW_PID=$(echo "$NEW_CANDIDATES" | head -1)
            if [ "$NEW_PID" != "$PID" ]; then
                PID="$NEW_PID"
                PID_START_TIME=$(get_process_start_time "$PID")
                log "adopted Steam-relaunched PID $PID (start=$PID_START_TIME), restarting settle window"
                SETTLE_DEADLINE=$(($(date +%s) + SETTLE_WINDOW))
            fi
        elif [ "$NEW_COUNT" -gt 1 ]; then
            diagnose AMBIGUOUS_ATTACH "multiple BG3 processes during settle: $NEW_CANDIDATES"
            echo "VERDICT=AMBIGUOUS_ATTACH"; exit 5
        elif [ "$NEW_COUNT" -eq 0 ]; then
            diagnose GAME_DIED "no BG3 process found during settle phase"
            echo "VERDICT=GAME_DIED"; exit 1
        fi
    done
    if [ "$SETTLED" -eq 0 ]; then
        diagnose GAME_DIED "settle phase exhausted without stable PID"
        echo "VERDICT=GAME_DIED"; exit 1
    fi
    log "settle phase passed: PID $PID alive for ${SETTLE_WINDOW}s"
else
    # Neither --launch nor --attach: try to find existing game
    CANDIDATES=$(game_pid_by_path)
    COUNT=$(echo "$CANDIDATES" | grep -c '[0-9]' || true)
    if [ "$COUNT" -eq 0 ]; then
        diagnose LAUNCH_FAILED "no game process found and --launch not specified"
        echo "VERDICT=GAME_DIED"; exit 1
    elif [ "$COUNT" -gt 1 ]; then
        diagnose AMBIGUOUS_ATTACH "multiple BG3 processes: $CANDIDATES"
        echo "VERDICT=AMBIGUOUS_ATTACH"; exit 5
    fi
    PID=$(echo "$CANDIDATES" | head -1)
    PID_START_TIME=$(get_process_start_time "$PID")
    log "found existing PID $PID (start=$PID_START_TIME)"
fi

if [ -z "$PID" ] || ! kill -0 "$PID" 2>/dev/null; then
    diagnose LAUNCH_FAILED "PID $PID not alive after launch/attach"
    echo "VERDICT=GAME_DIED"; exit 1
fi
log "driving PID $PID"

# ---- Phase 1: bounded progress loop to PrepareRunning ---------------------
LAST_STATE=""; LAST_CHANGE=$(date +%s); RECOVERIES=0
while :; do
    NOW=$(date +%s)
    [ $((NOW - START_EPOCH)) -ge $OVERALL_DEADLINE ] && {
        diagnose STUCK "overall deadline ${OVERALL_DEADLINE}s exceeded in state '$LAST_STATE'"
        PYTHONPATH=tools python3 -m bg3se_harness quit --force >/dev/null 2>&1
        echo "VERDICT=STUCK"; exit 2
    }

    # Verify full identity (PID + start time)
    if ! verify_identity "$PID" "$PID_START_TIME"; then
        if ! kill -0 "$PID" 2>/dev/null; then
            IPS=$(find ~/Library/Logs/DiagnosticReports -name "Baldur*.ips" -newermt "$START_STAMP" 2>/dev/null | head -1)
            diagnose GAME_DIED "process gone in state '$LAST_STATE'${IPS:+; crash report: $IPS}"
            echo "VERDICT=GAME_DIED"; exit 1
        fi
        # PID alive but start time changed = replaced
        diagnose REPLACED "PID $PID identity changed (start time mismatch)"
        echo "VERDICT=REPLACED"; exit 3
    fi

    STATE=$(game_state)
    if [ "$STATE" != "$LAST_STATE" ]; then
        log "state: ${LAST_STATE:-<none>} -> $STATE"
        LAST_STATE="$STATE"; LAST_CHANGE=$NOW
    fi
    [ "$STATE" = "Running" ] || [ "$STATE" = "PrepareRunning" ] && break

    BUDGET=$(budget_for "$STATE")
    if [ $((NOW - LAST_CHANGE)) -ge "$BUDGET" ]; then
        if [ "$RECOVERIES" -ge "$MAX_RECOVERIES" ]; then
            diagnose STUCK "state '$STATE' stalled ${BUDGET}s; $RECOVERIES recoveries exhausted"
            PYTHONPATH=tools python3 -m bg3se_harness quit --force >/dev/null 2>&1
            echo "VERDICT=STUCK"; exit 2
        fi
        RECOVERIES=$((RECOVERIES + 1))
        diagnose STALL "state '$STATE' unchanged for ${BUDGET}s (recovery $RECOVERIES/$MAX_RECOVERIES)"
        osascript -e 'tell application "System Events" to tell process "Baldur'"'"'s Gate 3" to set frontmost to true' \
                  -e 'tell application "System Events" to keystroke " "' >/dev/null 2>&1
        recover_menu "$RECOVERIES" || diagnose RECOVERY_FAILED "menu click helper failed"
        LAST_CHANGE=$(date +%s)
    fi
    sleep 5
done

log "session reached '$LAST_STATE' — hiding window"
osascript -e 'tell application "System Events" to set visible of process "Baldur'"'"'s Gate 3" to false' >/dev/null 2>&1
diagnose SESSION_RUNNING "PID $PID reached $LAST_STATE at +$(( $(date +%s) - START_EPOCH ))s"

# ---- Phase 2: optional PID-tracked soak -----------------------------------
if [ "$SOAK" -gt 0 ]; then
    log "soaking ${SOAK}s (PID $PID must survive with matching identity)"
    END=$(( $(date +%s) + SOAK ))
    while [ "$(date +%s)" -lt "$END" ]; do
        sleep 10
        if ! verify_identity "$PID" "$PID_START_TIME"; then
            if ! kill -0 "$PID" 2>/dev/null; then
                IPS=$(find ~/Library/Logs/DiagnosticReports -name "Baldur*.ips" -newermt "$START_STAMP" 2>/dev/null | head -1)
                diagnose GAME_DIED "PID $PID died during soak${IPS:+; crash report: $IPS}"
                echo "VERDICT=GAME_DIED"; exit 1
            fi
            diagnose REPLACED "PID $PID identity changed during soak"
            echo "VERDICT=REPLACED"; exit 3
        fi
    done
    diagnose SOAK_PASSED "PID $PID survived ${SOAK}s soak"
fi
echo "VERDICT=SESSION_RUNNING"
exit 0
