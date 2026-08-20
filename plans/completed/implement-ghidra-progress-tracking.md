# Plan: Implement Ghidra Progress Tracking (Issue #17)

## Overview

Add real-time progress visibility to Ghidra headless analysis scripts and ensure optimized analysis is always used.

## Implementation Steps

### Step 1: Create Progress Utility Module

**File:** `ghidra/scripts/progress_utils.py`

```python
"""Shared progress tracking utilities for Ghidra scripts."""
import time

PROGRESS_FILE = "/tmp/ghidra_progress.log"

def init_progress():
    """Clear progress log at script start."""
    with open(PROGRESS_FILE, "w") as f:
        f.write("[%s] === Ghidra Script Started ===\n" % time.strftime("%H:%M:%S"))

def progress(msg, pct=None):
    """Log progress to file and console for real-time monitoring.

    Args:
        msg: Status message to display
        pct: Optional percentage (0-100)
    """
    line = "[%s] %s" % (time.strftime("%H:%M:%S"), msg)
    if pct is not None:
        line += " (%d%%)" % pct
        try:
            monitor.setMaximum(100)
            monitor.setProgress(pct)
        except:
            pass  # monitor may not be available in all contexts
    try:
        monitor.setMessage(msg)
    except:
        pass
    print(line)
    with open(PROGRESS_FILE, "a") as f:
        f.write(line + "\n")

def finish_progress():
    """Mark script completion."""
    progress("=== Script Complete ===", 100)
```

### Step 2: Create Wrapper Script

**File:** `ghidra/scripts/run_analysis.sh`

```bash
#!/bin/bash
# Wrapper script that ensures optimize_analysis.py is always used
# Usage: ./run_analysis.sh <postscript.py> [additional args...]

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
POSTSCRIPT="$1"
shift  # Remove first arg, pass rest through

if [ -z "$POSTSCRIPT" ]; then
    echo "Usage: $0 <postscript.py> [additional args...]"
    echo "Example: $0 find_modifierlist_offsets.py"
    exit 1
fi

# Clear progress log
> /tmp/ghidra_progress.log

echo "Starting Ghidra analysis with optimized settings..."
echo "Monitor progress: tail -f /tmp/ghidra_progress.log"
echo ""

JAVA_HOME="/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home" \
  ~/ghidra/support/analyzeHeadless ~/ghidra_projects BG3Analysis \
  -process BG3_arm64_thin \
  -scriptPath "$SCRIPT_DIR" \
  -preScript optimize_analysis.py \
  -postScript "$POSTSCRIPT" \
  "$@" \
  2>&1 | tee /tmp/ghidra_output.log

echo ""
echo "Analysis complete. Output saved to /tmp/ghidra_output.log"
```

### Step 3: Update optimize_analysis.py

Add progress tracking to the prescript:

```python
# At the top of optimize_analysis.py
import time
PROGRESS_FILE = "/tmp/ghidra_progress.log"

def progress(msg, pct=None):
    line = "[%s] %s" % (time.strftime("%H:%M:%S"), msg)
    if pct is not None:
        line += " (%d%%)" % pct
    print(line)
    with open(PROGRESS_FILE, "a") as f:
        f.write(line + "\n")

# Clear log at start
with open(PROGRESS_FILE, "w") as f:
    f.write("")

progress("Optimizing Ghidra Analysis Settings", 0)

# ... existing code ...

progress("Disabling slow analyzers", 25)
for analyzer in slow_analyzers:
    # ...

progress("Enabling needed analyzers", 50)
for analyzer in needed_analyzers:
    # ...

progress("Optimization complete, starting analysis", 75)
```

### Step 4: Update Each Script with Progress

For each script, add progress calls at key points:

**Example pattern for find_modifierlist_offsets.py:**

```python
from progress_utils import init_progress, progress, finish_progress

def main():
    init_progress()

    progress("Searching ModifierList symbols", 10)
    ml_symbols = search_for_symbol("ModifierList")
    progress("Found %d ModifierList symbols" % len(ml_symbols), 30)

    progress("Searching RPGStats symbols", 40)
    rpg_symbols = search_for_symbol("RPGStats")
    progress("Found %d RPGStats symbols" % len(rpg_symbols), 60)

    progress("Searching stat type name strings", 70)
    # ... search code ...

    progress("Analysis complete", 90)
    finish_progress()
```

### Step 5: Update Documentation

```markdown
### Ghidra Analysis (Optimized)

Always use the wrapper script for Ghidra analysis:

\`\`\`bash
./ghidra/scripts/run_analysis.sh find_modifierlist_offsets.py
\`\`\`

Monitor progress in real-time:
\`\`\`bash
tail -f /tmp/ghidra_progress.log
\`\`\`
```

**Skill update:** Already done in bg3se-macos-ghidra skill.

## Scripts to Update (Checklist)

1. [ ] Create `ghidra/scripts/progress_utils.py`
2. [ ] Create `ghidra/scripts/run_analysis.sh`
3. [ ] Update `optimize_analysis.py` with progress
4. [ ] Update `find_rpgstats.py` with progress
5. [ ] Update `find_uuid_mapping.py` with progress
6. [ ] Update `find_entity_offsets.py` with progress
7. [ ] Update `quick_component_search.py` with progress
8. [ ] Update `find_modifierlist_offsets.py` with progress
9. [ ] Update `find_globalstringtable.py` with progress

## Testing

1. Run wrapper script with a simple postscript
2. Verify progress log updates in real-time via `tail -f`
3. Verify analysis completes successfully
4. Verify output is captured to `/tmp/ghidra_output.log`

## Estimated Effort

- Progress utility module: 15 min
- Wrapper script: 15 min
- Update each script (9 scripts): 45 min
- Documentation: 15 min
- Testing: 30 min

**Total: ~2 hours**

## Success Criteria

1. `tail -f /tmp/ghidra_progress.log` shows real-time updates during analysis
2. Wrapper script automatically applies optimized analysis settings
3. All existing scripts continue to work correctly
4. Analysis time remains ~30-45 minutes (not regressed)
