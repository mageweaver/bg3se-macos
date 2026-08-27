"""BG3 main menu automation via macOS Vision OCR + CGEvent clicks.

Detects menu state by running Vision OCR (VNRecognizeTextRequest) on a
window screenshot, then clicks buttons via Quartz CGEvent API.

REQUIRES pyobjc (see tools/requirements.txt):
    python3 -m pip install --user pyobjc-framework-Quartz
Without it every window lookup here returns nothing. This docstring used to
say "All stdlib, no pip dependencies required", which was false and helped
hide a silent failure -- see the QUARTZ_IMPORT_ERROR note below.

Architecture:
    screencapture -l <wid> -> Vision OCR (osascript JXA) -> detected buttons
        -> CGEvent click (ctypes + ApplicationServices) targeted at BG3 window

Usage:
    python3 -m bg3se_harness menu detect       # JSON: visible buttons
    python3 -m bg3se_harness menu click "Continue"
    python3 -m bg3se_harness menu wait         # Poll until menu visible
"""

from __future__ import annotations

import ctypes
import ctypes.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from .screenshot import get_window_id, get_image_dimensions


# ============================================================================
# CGEvent Click (ctypes + Quartz -- stdlib, zero deps)
# ============================================================================

_appservices = None


def _get_appservices():
    global _appservices
    if _appservices is None:
        lib = ctypes.util.find_library("ApplicationServices")
        if not lib:
            raise RuntimeError("ApplicationServices framework not found")
        _appservices = ctypes.CDLL(lib)
    return _appservices


class CGPoint(ctypes.Structure):
    _fields_ = [("x", ctypes.c_double), ("y", ctypes.c_double)]


_kCGEventLeftMouseDown = 1
_kCGEventLeftMouseUp = 2
_kCGEventMouseMoved = 5
_kCGMouseButtonLeft = 0
_kCGHIDEventTap = 0
_kCGMouseEventClickState = 1
_kCGMouseEventButtonNumber = 3
_kVK_Escape = 53
_kVK_Space = 49


def cg_move(x, y):
    """Move the physical cursor to global screen coordinates (x, y)."""
    qs = _get_appservices()
    point = CGPoint(float(x), float(y))

    qs.CGWarpMouseCursorPosition.argtypes = [CGPoint]
    qs.CGWarpMouseCursorPosition.restype = ctypes.c_int32
    return qs.CGWarpMouseCursorPosition(point) == 0


def cg_click(x, y, hover_delay=0.25, hold_delay=0.08, warp=True):
    """Send a mouse click at global screen coordinates (x, y) via CGEvent."""
    qs = _get_appservices()
    point = CGPoint(float(x), float(y))

    qs.CGEventCreateMouseEvent.restype = ctypes.c_void_p
    qs.CGEventCreateMouseEvent.argtypes = [
        ctypes.c_void_p, ctypes.c_uint32, CGPoint, ctypes.c_uint32,
    ]
    qs.CGEventPost.argtypes = [ctypes.c_uint32, ctypes.c_void_p]
    qs.CGEventSetIntegerValueField.argtypes = [
        ctypes.c_void_p, ctypes.c_uint32, ctypes.c_int64,
    ]
    qs.CFRelease.argtypes = [ctypes.c_void_p]

    moved = cg_move(x, y) if warp else False

    ev_move = qs.CGEventCreateMouseEvent(
        None, _kCGEventMouseMoved, point, _kCGMouseButtonLeft,
    )
    ev_down = qs.CGEventCreateMouseEvent(
        None, _kCGEventLeftMouseDown, point, _kCGMouseButtonLeft,
    )
    ev_up = qs.CGEventCreateMouseEvent(
        None, _kCGEventLeftMouseUp, point, _kCGMouseButtonLeft,
    )
    if not ev_down or not ev_up:
        if ev_move:
            qs.CFRelease(ev_move)
        if ev_down:
            qs.CFRelease(ev_down)
        if ev_up:
            qs.CFRelease(ev_up)
        return False

    for event in (ev_down, ev_up):
        qs.CGEventSetIntegerValueField(
            event, _kCGMouseEventButtonNumber, _kCGMouseButtonLeft,
        )
        qs.CGEventSetIntegerValueField(event, _kCGMouseEventClickState, 1)

    if ev_move:
        qs.CGEventPost(_kCGHIDEventTap, ev_move)
        time.sleep(hover_delay)
    qs.CGEventPost(_kCGHIDEventTap, ev_down)
    time.sleep(hold_delay)
    qs.CGEventPost(_kCGHIDEventTap, ev_up)

    if ev_move:
        qs.CFRelease(ev_move)
    qs.CFRelease(ev_down)
    qs.CFRelease(ev_up)
    return {"success": True, "moved": moved, "warp": warp}


def system_events_click(x, y):
    """Send a mouse click through System Events at global screen coordinates."""
    script = (
        'tell application "System Events"\n'
        f'  click at {{{int(x)}, {int(y)}}}\n'
        'end tell'
    )
    try:
        r = subprocess.run(
            ["osascript", "-e", script],
            capture_output=True, text=True, timeout=5,
        )
        if r.returncode == 0:
            return {"success": True, "method": "SystemEvents_click_at"}
        return {
            "success": False,
            "method": "SystemEvents_click_at",
            "error": r.stderr.strip() or r.stdout.strip(),
            "returncode": r.returncode,
        }
    except (subprocess.TimeoutExpired, OSError) as exc:
        return {"success": False, "method": "SystemEvents_click_at", "error": str(exc)}


def _normalise_click_result(result, method):
    if isinstance(result, dict):
        normalised = {"method": method, **result}
        normalised["success"] = bool(normalised.get("success"))
        return normalised
    return {"method": method, "success": bool(result)}


def activate_bg3():
    """Bring an already-running BG3 to the foreground for input probes.

    Activates by PID via NSRunningApplication. Never uses
    `tell application ... to activate` — LaunchServices treats that as a
    launch request when the app is not running, which spawned fresh BG3
    instances (Steam bounce pairs) mid-automation whenever the tracked
    process had died.
    """
    pid = _get_bg3_pid()
    if not pid:
        return {"success": False, "method": "nsrunningapplication",
                "error": "BG3 not running"}
    try:
        from AppKit import (
            NSRunningApplication,
            NSApplicationActivateIgnoringOtherApps,
        )
        app = NSRunningApplication.runningApplicationWithProcessIdentifier_(pid)
        if app is None:
            return {"success": False, "method": "nsrunningapplication",
                    "pid": pid, "error": f"no running application for pid {pid}"}
        ok = bool(app.activateWithOptions_(NSApplicationActivateIgnoringOtherApps))
        return {"success": ok, "method": "nsrunningapplication", "pid": pid}
    except Exception as exc:
        return {"success": False, "method": "nsrunningapplication",
                "pid": pid, "error": str(exc)}


def cg_key(keycode):
    """Send a key press at the system level via CGEvent."""
    qs = _get_appservices()

    qs.CGEventCreateKeyboardEvent.restype = ctypes.c_void_p
    qs.CGEventCreateKeyboardEvent.argtypes = [
        ctypes.c_void_p, ctypes.c_uint16, ctypes.c_bool,
    ]
    qs.CGEventPost.argtypes = [ctypes.c_uint32, ctypes.c_void_p]
    qs.CFRelease.argtypes = [ctypes.c_void_p]

    ev_down = qs.CGEventCreateKeyboardEvent(None, keycode, True)
    ev_up = qs.CGEventCreateKeyboardEvent(None, keycode, False)
    if not ev_down or not ev_up:
        if ev_down:
            qs.CFRelease(ev_down)
        if ev_up:
            qs.CFRelease(ev_up)
        return False

    qs.CGEventPost(_kCGHIDEventTap, ev_down)
    time.sleep(0.05)
    qs.CGEventPost(_kCGHIDEventTap, ev_up)

    qs.CFRelease(ev_down)
    qs.CFRelease(ev_up)
    return True


def cg_key_to_pid(pid: int, keycode: int) -> dict:
    """Send a key press to BG3 via System Events key code (Accessibility).

    CGEventPostToPid is deprecated and leaks keys to the terminal on
    macOS Sequoia. System Events ``key code`` routes through the
    Accessibility framework and never touches the HID event stream.
    The process does NOT need to be frontmost.
    """
    script = (
        'tell application "System Events"\n'
        f'  if exists process "Baldur\'s Gate 3" then\n'
        f'    tell process "Baldur\'s Gate 3"\n'
        f'      key code {keycode}\n'
        f'    end tell\n'
        f'  end if\n'
        'end tell'
    )
    try:
        r = subprocess.run(
            ["osascript", "-e", script],
            capture_output=True, text=True, timeout=5,
        )
        if r.returncode == 0:
            return {"success": True, "method": "SystemEvents_key_code"}
        return {"success": False, "method": "SystemEvents_key_code",
                "error": r.stderr.strip()}
    except (subprocess.TimeoutExpired, OSError) as exc:
        return {"success": False, "method": "SystemEvents_key_code",
                "error": str(exc)}


def dismiss_splash_background(pid: int) -> dict:
    """Dismiss BG3 splash screen without stealing focus.

    Sends Space key directly to the BG3 process via CGEventPostToPid.
    Never falls back to the aggressive method — headless/background mode
    must not broadcast keys to the system HID tap.
    """
    return cg_key_to_pid(pid, _kVK_Space)


def dismiss_splash_aggressive():
    """Dismiss the BG3 splash screen via System Events Escape+Space.

    Sends Escape (skips Bink intro videos) then Space (dismisses
    "Press Any Key") through the Accessibility framework. Does not
    use CGEvent HID tap, so keys never leak to the terminal.
    """
    script = (
        'tell application "System Events"\n'
        '  if exists process "Baldur\'s Gate 3" then\n'
        '    tell process "Baldur\'s Gate 3"\n'
        f'      key code {_kVK_Escape}\n'
        '      delay 0.2\n'
        f'      key code {_kVK_Space}\n'
        '    end tell\n'
        '    return "ok"\n'
        '  else\n'
        '    return "no_process"\n'
        '  end if\n'
        'end tell'
    )
    try:
        r = subprocess.run(
            ["osascript", "-e", script],
            capture_output=True, text=True, timeout=5,
        )
        if r.returncode == 0 and "ok" in r.stdout:
            return {"success": True, "method": "SystemEvents_escape_space"}
        return {"success": False, "error": r.stderr.strip() or r.stdout.strip()}
    except (subprocess.TimeoutExpired, OSError) as exc:
        return {"success": False, "error": str(exc)}


# ============================================================================
# Window geometry
# ============================================================================

def _get_bg3_pid():
    """Return the first BG3 PID, or None if the game is not running."""
    for cmd in (
        ["pgrep", "-x", "Baldur's Gate 3"],
        ["pgrep", "-f", "Baldur.s.Gate.3"],
    ):
        try:
            result = subprocess.run(
                cmd,
                capture_output=True, text=True, timeout=5,
            )
        except (subprocess.TimeoutExpired, OSError):
            continue
        if result.returncode == 0 and result.stdout.strip():
            try:
                return int(result.stdout.strip().split()[0])
            except ValueError:
                return None
    return None


def _rect_from_quartz(bounds):
    """Normalize a Quartz bounds dictionary to x/y/width/height."""
    if not bounds:
        return None
    try:
        return {
            "x": int(bounds.get("X", 0)),
            "y": int(bounds.get("Y", 0)),
            "width": int(bounds.get("Width", 0)),
            "height": int(bounds.get("Height", 0)),
        }
    except (TypeError, ValueError, AttributeError):
        return None


# Resolved once at import so a MISSING DEPENDENCY is distinguishable from a
# genuinely absent window. Both used to funnel into "BG3 window not found",
# which cost real debugging time on 2026-08-27: pyobjc was simply not
# installed, OCR and screenshots were silently dead, and the menu watchdog
# fell back to blind splash-dismiss fractions forever.
try:
    import Quartz as _Quartz
    QUARTZ_IMPORT_ERROR = None
except ImportError as _e:                                    # pragma: no cover
    _Quartz = None
    QUARTZ_IMPORT_ERROR = (
        "pyobjc Quartz is not installed -- window geometry, OCR and clicks are "
        "unavailable. Install with: python3 -m pip install --user "
        "pyobjc-framework-Quartz  (original error: %s)" % _e
    )


def quartz_unavailable():
    """Human-readable reason Quartz is missing, or None when it is present."""
    return QUARTZ_IMPORT_ERROR


def _get_quartz_window_info(window_id=None, pid=None):
    """Return Quartz CGWindow metadata for the BG3 game window."""
    if QUARTZ_IMPORT_ERROR:
        return None
    try:
        import Quartz

        if pid is None:
            pid = _get_bg3_pid()
        if pid is None:
            return None

        windows = Quartz.CGWindowListCopyWindowInfo(
            Quartz.kCGWindowListOptionAll, Quartz.kCGNullWindowID,
        )
        candidates = []
        for window in windows:
            owner_pid = int(window.get("kCGWindowOwnerPID", 0) or 0)
            if owner_pid != pid:
                continue
            number = int(window.get("kCGWindowNumber", 0) or 0)
            name = str(window.get("kCGWindowName", "") or "")
            if window_id is not None and str(number) != str(window_id):
                continue
            candidates.append({
                "id": number,
                "owner_pid": owner_pid,
                "owner_name": str(window.get("kCGWindowOwnerName", "") or ""),
                "name": name,
                "bounds": _rect_from_quartz(window.get("kCGWindowBounds")),
                "layer": int(window.get("kCGWindowLayer", 0) or 0),
                "alpha": float(window.get("kCGWindowAlpha", 0) or 0),
                "is_onscreen": bool(window.get("kCGWindowIsOnscreen", False)),
                "memory_usage": int(window.get("kCGWindowMemoryUsage", 0) or 0),
            })

        if not candidates:
            return None
        for item in candidates:
            if item.get("name"):
                return item
        return candidates[0]
    except Exception:
        return None


def _get_window_bounds():
    """Get BG3 window bounds {x, y, width, height} via osascript."""
    script = (
        'tell application "System Events"\n'
        '  set bgProc to first process whose name is "Baldur\'s Gate 3"\n'
        '  set {x, y} to position of window 1 of bgProc\n'
        '  set {w, h} to size of window 1 of bgProc\n'
        '  return (x as text) & "," & (y as text) & "," '
        '& (w as text) & "," & (h as text)\n'
        'end tell'
    )
    try:
        result = subprocess.run(
            ["osascript", "-e", script],
            capture_output=True, text=True, timeout=10,
        )
        if result.returncode != 0:
            return None
        parts = result.stdout.strip().split(",")
        if len(parts) != 4:
            return None
        return {
            "x": int(parts[0].strip()),
            "y": int(parts[1].strip()),
            "width": int(parts[2].strip()),
            "height": int(parts[3].strip()),
        }
    except (subprocess.TimeoutExpired, ValueError, OSError):
        return None


def _get_main_screen_scale():
    """Return NSScreen.mainScreen backing scale factor when available."""
    script = (
        'ObjC.import("AppKit");\n'
        'var s = $.NSScreen.mainScreen;\n'
        'if (!s) { "0"; } else { String(s.backingScaleFactor); }\n'
    )
    try:
        result = subprocess.run(
            ["osascript", "-l", "JavaScript", "-e", script],
            capture_output=True, text=True, timeout=5,
        )
        if result.returncode != 0:
            return None
        return float(result.stdout.strip())
    except (subprocess.TimeoutExpired, ValueError, OSError):
        return None


def _point_inside(point, bounds):
    if not point or not bounds:
        return None
    x = point.get("x")
    y = point.get("y")
    return (
        bounds["x"] <= x <= bounds["x"] + bounds["width"]
        and bounds["y"] <= y <= bounds["y"] + bounds["height"]
    )


def _coordinate_debug_for_bbox(bbox, img_w, img_h, system_bounds, quartz_bounds):
    """Return coordinate candidates for a Vision bbox center."""
    center_x_norm = bbox.get("x", 0) + bbox.get("width", 0) / 2
    center_y_norm = bbox.get("y", 0) + bbox.get("height", 0) / 2
    px_x = center_x_norm * img_w
    px_y = (1.0 - center_y_norm) * img_h

    candidates = {}
    selected_basis = None
    selected = None

    if system_bounds:
        scale_x = img_w / system_bounds["width"] if system_bounds["width"] else 1
        scale_y = img_h / system_bounds["height"] if system_bounds["height"] else 1
        point = {
            "x": system_bounds["x"] + int(px_x / scale_x),
            "y": system_bounds["y"] + int(px_y / scale_y),
        }
        candidates["system_events_points"] = {
            **point,
            "scale_x": scale_x,
            "scale_y": scale_y,
            "inside": _point_inside(point, system_bounds),
        }
        selected_basis = "system_events_points"
        selected = point

    if quartz_bounds:
        q_scale_x = img_w / quartz_bounds["width"] if quartz_bounds["width"] else 1
        q_scale_y = img_h / quartz_bounds["height"] if quartz_bounds["height"] else 1
        q_point = {
            "x": quartz_bounds["x"] + int(px_x / q_scale_x),
            "y": quartz_bounds["y"] + int(px_y / q_scale_y),
        }
        q_pixel = {
            "x": quartz_bounds["x"] + int(px_x),
            "y": quartz_bounds["y"] + int(px_y),
        }
        candidates["quartz_bounds_scaled"] = {
            **q_point,
            "scale_x": q_scale_x,
            "scale_y": q_scale_y,
            "inside": _point_inside(q_point, quartz_bounds),
        }
        candidates["quartz_pixel_space"] = {
            **q_pixel,
            "inside": _point_inside(q_pixel, quartz_bounds),
        }
        if selected is None:
            selected_basis = "quartz_bounds_scaled"
            selected = q_point

    return {
        "bbox": bbox,
        "normalized_center": {
            "x": round(center_x_norm, 5),
            "y": round(center_y_norm, 5),
        },
        "pixel_center": {"x": int(px_x), "y": int(px_y)},
        "selected_basis": selected_basis,
        "selected": selected,
        "candidates": candidates,
    }


# ============================================================================
# Vision OCR (macOS 12+, stdlib -- runs via osascript JXA)
# ============================================================================

_VISION_OCR_JXA = r'''
ObjC.import("Vision");
ObjC.import("AppKit");

function run(argv) {
    var imagePath = argv[0];
    var url = $.NSURL.fileURLWithPath(imagePath);
    var image = $.NSImage.alloc.initWithContentsOfURL(url);
    if (!image || !image.isValid) {
        return JSON.stringify({"error": "Could not load image"});
    }

    var cgRef = image.CGImageForProposedRect(null, null, null);
    if (!cgRef) {
        return JSON.stringify({"error": "Could not get CGImage"});
    }

    var request = $.VNRecognizeTextRequest.alloc.init;
    request.recognitionLevel = 1;
    request.usesLanguageCorrection = true;

    var handler = $.VNImageRequestHandler.alloc.initWithCGImageOptions(cgRef, null);
    handler.performRequestsError($.NSArray.arrayWithObject(request), null);

    var results = request.results;
    var items = [];
    var count = results.count;

    for (var i = 0; i < count; i++) {
        var obs = results.objectAtIndex(i);
        var text = obs.topCandidates(1).objectAtIndex(0).string.js;
        var box = obs.boundingBox;

        items.push({
            "text": text,
            "confidence": obs.confidence,
            "bbox": {
                "x": box.origin.x,
                "y": box.origin.y,
                "width": box.size.width,
                "height": box.size.height
            }
        });
    }

    return JSON.stringify({"results": items});
}
'''


def _ocr_screenshot(image_path):
    """Run Vision OCR on an image file.

    Returns list of {text, confidence, bbox}. bbox uses Vision's normalized
    coordinates: origin bottom-left, values 0-1.
    """
    try:
        result = subprocess.run(
            ["osascript", "-l", "JavaScript", "-e", _VISION_OCR_JXA, str(image_path)],
            capture_output=True, text=True, timeout=30,
        )
        if result.returncode != 0:
            return []
        data = json.loads(result.stdout.strip())
        if "error" in data:
            print(f"[menu] OCR error: {data['error']}", file=sys.stderr)
            return []
        return data.get("results", [])
    except (subprocess.TimeoutExpired, json.JSONDecodeError, OSError) as e:
        print(f"[menu] OCR failed: {e}", file=sys.stderr)
        return []


def _capture_window_screenshot(window_id=None):
    """Capture BG3 window to a temp PNG. Returns path or None."""
    wid = window_id or get_window_id()
    if not wid:
        return None

    tmp_fd, tmp_path = tempfile.mkstemp(suffix=".png", prefix="bg3se_menu_")
    os.close(tmp_fd)

    result = subprocess.run(
        ["screencapture", "-l", wid, "-x", "-o", tmp_path],
        capture_output=True,
    )
    if result.returncode != 0 or not Path(tmp_path).exists():
        try:
            os.unlink(tmp_path)
        except OSError:
            pass
        return None
    return tmp_path


_DEBUG_IMAGE_JXA = r'''
ObjC.import("AppKit");

function run(argv) {
    var inputPath = argv[0];
    var outputPath = argv[1];
    var jsonPath = argv[2];

    var raw = $.NSString.stringWithContentsOfFileEncodingError(jsonPath, $.NSUTF8StringEncoding, null);
    var items = JSON.parse(ObjC.unwrap(raw));
    var image = $.NSImage.alloc.initWithContentsOfFile(inputPath);
    if (!image || !image.isValid) {
        return JSON.stringify({"success": false, "error": "could not load image"});
    }

    image.lockFocus;
    $.NSColor.redColor.setStroke;
    $.NSColor.redColor.setFill;

    var size = image.size;
    for (var i = 0; i < items.length; i++) {
        var box = items[i].bbox || {};
        var x = (box.x || 0) * size.width;
        var y = (box.y || 0) * size.height;
        var w = (box.width || 0) * size.width;
        var h = (box.height || 0) * size.height;
        var rect = $.NSMakeRect(x, y, w, h);
        var path = $.NSBezierPath.bezierPathWithRect(rect);
        path.setLineWidth(3.0);
        path.stroke;

        var cx = x + w / 2.0;
        var cy = y + h / 2.0;
        $.NSBezierPath.strokeLineFromPointToPoint($.NSMakePoint(cx - 12, cy), $.NSMakePoint(cx + 12, cy));
        $.NSBezierPath.strokeLineFromPointToPoint($.NSMakePoint(cx, cy - 12), $.NSMakePoint(cx, cy + 12));
    }

    var rep = $.NSBitmapImageRep.alloc.initWithFocusedViewRect($.NSMakeRect(0, 0, size.width, size.height));
    image.unlockFocus;

    var data = rep.representationUsingTypeProperties($.NSBitmapImageFileTypePNG, $());
    var ok = data.writeToFileAtomically(outputPath, true);
    return JSON.stringify({"success": !!ok});
}
'''


def _write_debug_image(source_path, output_path, ocr_results):
    """Write a PNG copy annotated with OCR boxes and centers."""
    dest = Path(output_path)
    dest.parent.mkdir(parents=True, exist_ok=True)
    fd, json_path = tempfile.mkstemp(suffix=".json", prefix="bg3se_menu_boxes_")
    os.close(fd)
    try:
        Path(json_path).write_text(json.dumps(ocr_results))
        result = subprocess.run(
            [
                "osascript", "-l", "JavaScript", "-e", _DEBUG_IMAGE_JXA,
                str(source_path), str(dest), json_path,
            ],
            capture_output=True, text=True, timeout=15,
        )
        if result.returncode == 0:
            try:
                data = json.loads(result.stdout.strip())
                if data.get("success") and dest.exists():
                    return {"success": True, "path": str(dest), "method": "jxa_appkit"}
            except json.JSONDecodeError:
                pass
        shutil.copy2(source_path, dest)
        return {
            "success": True,
            "path": str(dest),
            "method": "copy_fallback",
            "warning": result.stderr.strip() or result.stdout.strip(),
        }
    except (subprocess.TimeoutExpired, OSError) as exc:
        try:
            shutil.copy2(source_path, dest)
            return {
                "success": True,
                "path": str(dest),
                "method": "copy_fallback",
                "warning": str(exc),
            }
        except OSError as copy_exc:
            return {"success": False, "error": str(copy_exc)}
    finally:
        try:
            os.unlink(json_path)
        except OSError:
            pass


# ============================================================================
# Known BG3 menu button labels
# ============================================================================

KNOWN_BUTTONS = [
    "Continue",
    "New Game",
    "Load Game",
    "Multiplayer",
    "Options",
    "Credits",
    "Quit Game",
    "Start Game",
    "Cancel",
    "Mod Verification",
    "Honour Mode",
    "Click to Continue",
    "Press Any Key",
]


def _normalize(text):
    return text.lower().strip().replace("  ", " ")


def _fuzzy_match(ocr_text, target):
    """Check if OCR text is a fuzzy match for a target button label."""
    norm_ocr = _normalize(ocr_text)
    norm_target = _normalize(target)
    if norm_ocr == norm_target:
        return True
    # Only match if the target label appears within the OCR text
    # (not the reverse — short OCR fragments like "on" must not match "Options")
    if norm_target in norm_ocr:
        return True
    cleaned = norm_ocr.replace("0", "o").replace("l", "i")
    cleaned_target = norm_target.replace("0", "o").replace("l", "i")
    return cleaned == cleaned_target


# ============================================================================
# Public API
# ============================================================================

def collect_geometry(capture=False):
    """Collect BG3 window geometry from Quartz, System Events, and screenshot."""
    pid = _get_bg3_pid()
    window_id = get_window_id()
    quartz = _get_quartz_window_info(window_id=window_id, pid=pid)
    system_bounds = _get_window_bounds()
    screenshot = None

    if capture and window_id:
        screenshot_path = _capture_window_screenshot(window_id=window_id)
        if screenshot_path:
            try:
                width, height = get_image_dimensions(screenshot_path)
                screenshot = {"width": width, "height": height}
            finally:
                try:
                    os.unlink(screenshot_path)
                except OSError:
                    pass

    result = {
        "pid": pid,
        "window_id": int(window_id) if window_id else None,
        "quartz": quartz,
        "system_events": {"bounds": system_bounds} if system_bounds else None,
        "main_screen_scale": _get_main_screen_scale(),
    }
    if screenshot:
        result["screenshot"] = screenshot
        for label, bounds in (
            ("system_events", system_bounds),
            ("quartz", quartz.get("bounds") if quartz else None),
        ):
            if bounds and bounds.get("width") and bounds.get("height"):
                result[f"{label}_screenshot_scale"] = {
                    "x": screenshot["width"] / bounds["width"],
                    "y": screenshot["height"] / bounds["height"],
                }
    if not pid:
        result["error"] = "BG3 process not found"
    elif not window_id:
        result["error"] = "BG3 window not found"
    return result


def detect_menu(debug_image=None):
    """Detect which menu buttons are visible via OCR.

    Returns dict with buttons (matched known labels with screen coords),
    raw_ocr (all recognized text), and window bounds.
    """
    if QUARTZ_IMPORT_ERROR:
        # Do not report a dependency problem as an absent window.
        return {"error": QUARTZ_IMPORT_ERROR, "buttons": [], "raw_ocr": [],
                "geometry": {}}

    window_id = get_window_id()
    screenshot_path = _capture_window_screenshot(window_id=window_id)
    if not screenshot_path:
        geometry = collect_geometry(capture=False)
        return {"error": "BG3 window not found", "buttons": [], "geometry": geometry}

    try:
        pid = _get_bg3_pid()
        quartz = _get_quartz_window_info(window_id=window_id, pid=pid)
        bounds = _get_window_bounds()
        ocr_results = _ocr_screenshot(screenshot_path)
        img_w, img_h = get_image_dimensions(screenshot_path)
        debug_result = None
        if debug_image:
            debug_result = _write_debug_image(screenshot_path, debug_image, ocr_results)
    finally:
        try:
            os.unlink(screenshot_path)
        except OSError:
            pass

    buttons = []
    raw_ocr = []
    quartz_bounds = quartz.get("bounds") if quartz else None
    for item in ocr_results:
        text = item["text"]
        conf = item.get("confidence", 0)
        bbox = item.get("bbox", {})

        raw_ocr.append({"text": text, "confidence": round(conf, 3)})

        matched_label = None
        for known in KNOWN_BUTTONS:
            if _fuzzy_match(text, known):
                matched_label = known
                break

        if matched_label and img_w and img_h:
            coordinate_debug = _coordinate_debug_for_bbox(
                bbox, img_w, img_h, bounds, quartz_bounds,
            )
            selected = coordinate_debug.get("selected") or {"x": 0, "y": 0}

            buttons.append({
                "text": matched_label,
                "screen_x": selected["x"],
                "screen_y": selected["y"],
                "confidence": round(conf, 3),
                "coordinate_debug": coordinate_debug,
            })

    geometry = {
        "pid": pid,
        "window_id": int(window_id) if window_id else None,
        "quartz": quartz,
        "system_events": {"bounds": bounds} if bounds else None,
        "screenshot": {"width": img_w, "height": img_h},
        "main_screen_scale": _get_main_screen_scale(),
    }
    if bounds:
        geometry["system_events_screenshot_scale"] = {
            "x": img_w / bounds["width"] if bounds["width"] else None,
            "y": img_h / bounds["height"] if bounds["height"] else None,
        }
    if quartz_bounds:
        geometry["quartz_screenshot_scale"] = {
            "x": img_w / quartz_bounds["width"] if quartz_bounds["width"] else None,
            "y": img_h / quartz_bounds["height"] if quartz_bounds["height"] else None,
        }

    result = {"buttons": buttons, "raw_ocr": raw_ocr, "geometry": geometry}
    if bounds:
        result["window"] = bounds
    if debug_result:
        result["debug_image"] = debug_result
    return result


def _bounds_for_click_basis(geometry):
    system = geometry.get("system_events") if geometry else None
    if system and system.get("bounds"):
        return system["bounds"], "system_events_bounds"
    quartz = geometry.get("quartz") if geometry else None
    if quartz and quartz.get("bounds"):
        return quartz["bounds"], "quartz_bounds"
    return None, None


def click_fraction(x_fraction, y_fraction, method="both", activate=True):
    """Click a point expressed as a fraction of the BG3 window bounds.

    Fractions use the same top-left-origin global coordinate basis reported by
    System Events/Quartz window bounds. This is useful when OCR fails but a
    known menu layout is visible.
    """
    geometry = collect_geometry(capture=True)
    bounds, basis = _bounds_for_click_basis(geometry)
    if not bounds:
        return {
            "success": False,
            "error": "BG3 window bounds not found",
            "geometry": geometry,
        }

    x = bounds["x"] + int(bounds["width"] * float(x_fraction))
    y = bounds["y"] + int(bounds["height"] * float(y_fraction))
    point = {"x": x, "y": y}
    activate_result = activate_bg3() if activate else None
    if activate:
        time.sleep(0.3)

    results = []
    success = False
    if method in ("cgevent", "both"):
        clicked = _normalise_click_result(cg_click(x, y), "CGEventPost")
        results.append(clicked)
        success = success or clicked["success"]
        if method == "both":
            time.sleep(0.15)
    if method in ("system-events", "both"):
        se_result = system_events_click(x, y)
        results.append(se_result)
        success = success or bool(se_result.get("success"))

    return {
        "success": success,
        "point": point,
        "fraction": {"x": float(x_fraction), "y": float(y_fraction)},
        "basis": basis,
        "bounds": bounds,
        "method": method,
        "activate": activate_result,
        "results": results,
        "geometry": geometry,
    }


def click_menu_button(button_name):
    """Click a specific menu button by name.

    Activates BG3 window, runs OCR to find the button, clicks via CGEvent.
    """
    # Activate BG3 window
    try:
        activate_bg3()
        time.sleep(0.3)
    except (subprocess.TimeoutExpired, OSError):
        pass

    detection = detect_menu()
    if "error" in detection and not detection.get("buttons"):
        return {"success": False, "error": detection["error"]}

    target = None
    for btn in detection["buttons"]:
        if _fuzzy_match(btn["text"], button_name):
            target = btn
            break

    if not target:
        available = [b["text"] for b in detection["buttons"]]
        return {
            "success": False,
            "error": f"Button '{button_name}' not found",
            "available_buttons": available,
            "raw_ocr": [r["text"] for r in detection.get("raw_ocr", [])],
        }

    clicked = _normalise_click_result(
        cg_click(target["screen_x"], target["screen_y"]),
        "CGEventPost",
    )
    return {
        "success": clicked["success"],
        "click": clicked,
        "button": target["text"],
        "screen_x": target["screen_x"],
        "screen_y": target["screen_y"],
        "coordinate_basis": target.get("coordinate_debug", {}).get("selected_basis"),
        "coordinate_debug": target.get("coordinate_debug"),
        "geometry": detection.get("geometry"),
    }


def wait_for_menu(timeout=60, poll_interval=3):
    """Poll until the main menu is visible (any known button detected)."""
    start = time.monotonic()
    attempts = 0

    while (time.monotonic() - start) < timeout:
        attempts += 1
        detection = detect_menu()
        if detection.get("buttons"):
            detection["wait_elapsed_s"] = round(time.monotonic() - start, 1)
            detection["attempts"] = attempts
            return detection
        time.sleep(poll_interval)

    return {
        "error": "Timed out waiting for menu",
        "timeout": timeout,
        "attempts": attempts,
        "buttons": [],
    }


def dismiss_splash():
    """Dismiss the 'Click to Continue' splash screen via Space key."""
    try:
        subprocess.run(
            ["osascript", "-e",
             'tell application "System Events"\n'
             '  set frontmost of process "Baldur\'s Gate 3" to true\n'
             '  delay 0.5\n'
             '  key code 49\n'
             'end tell'],
            capture_output=True, timeout=10,
        )
        return {"success": True, "action": "sent_space"}
    except subprocess.TimeoutExpired:
        return {"success": False, "error": "osascript timed out"}
    except (FileNotFoundError, OSError) as e:
        return {"success": False, "error": str(e)}


# ============================================================================
# CLI handler
# ============================================================================

def cmd_menu(args):
    """CLI handler for menu subcommands."""
    subcmd = args.menu_command

    if subcmd == "detect":
        result = detect_menu(debug_image=getattr(args, "debug_image", None))
        print(json.dumps(result, indent=2))
        return 0 if result.get("buttons") else 1

    elif subcmd == "click":
        result = click_menu_button(args.button)
        print(json.dumps(result, indent=2))
        return 0 if result.get("success") else 1

    elif subcmd == "click-fraction":
        result = click_fraction(
            args.x_fraction,
            args.y_fraction,
            method=getattr(args, "method", "both"),
            activate=not getattr(args, "no_activate", False),
        )
        print(json.dumps(result, indent=2))
        return 0 if result.get("success") else 1

    elif subcmd == "wait":
        timeout = getattr(args, "timeout", 60) or 60
        result = wait_for_menu(timeout=timeout)
        print(json.dumps(result, indent=2))
        return 0 if result.get("buttons") else 1

    elif subcmd == "dismiss":
        result = dismiss_splash()
        print(json.dumps(result, indent=2))
        return 0 if result.get("success") else 1

    elif subcmd == "geometry":
        result = collect_geometry(capture=getattr(args, "capture", False))
        print(json.dumps(result, indent=2))
        return 0 if not result.get("error") else 1

    return 1
