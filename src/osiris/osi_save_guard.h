/**
 * BG3SE-macOS - Keep a poisoned Osiris database from killing the save
 *
 * Osiris serialises every RETE database on save. A GUIDSTRING column is written
 * by COsiSmartBuf::writeGuidString, which asserts the value is a well-formed
 * GUID and reports the failure at severity 1 -- a throw, on the ServerWorker
 * thread, with only C frames between it and the engine's save flow. Nothing
 * catches it, so the process aborts:
 *
 *     COsiSmartBuf::writeGuidString  <- "Attempting to write an invalid GUIDSTRING"
 *     COsiTypedValueBase::Write
 *     CTuple::Write
 *     CReteDBase::Write
 *     COsiris::_WriteReteDBases
 *     COsiris::Save
 *     esv::StoryImplementation::SavegameVisit
 *     esv::SaveSystem::DoSaveFlow
 *
 * That is a time bomb, not a bug report. The row is inserted at some arbitrary
 * earlier moment -- nothing whatsoever connects the abort to whatever put a
 * non-GUID into a GUID column -- and the symptom the player sees is "the game
 * crashes when I save", with every save from then on doing the same. Observed
 * 2026-09-22 on a 729-mod load order.
 *
 * Neither this port nor upstream validates the value on the way in: both hand
 * any Lua string to the engine's string interner for a GUIDSTRING column
 * (upstream TypedValue::SetValue -> OsiStringMake(s, OsiIsGuidStringAlias)).
 * The engine's own assert is the first and only check, and it is fatal.
 *
 * It is fatal in both directions. Letting the save through only moves the abort
 * to COsiSmartBuf::readGuidString on the next load ("Reading invalid
 * GUIDString", under COsiris::Load -> CReteDBase::CReteDBase -> CTuple::Read),
 * so a value that reaches disk makes the save unloadable. Both must be handled
 * or the crash simply changes address.
 *
 * What this does
 * --------------
 * Replaces the assert *call* with a NOP in each function -- one instruction
 * each, nothing reimplemented. Disassembly of the arm64 build
 * (writeGuidString 0x36238, readGuidString 0x358f0) shows why that is faithful
 * rather than a patch over the problem:
 *
 *   - the validation result reaches the reporter only as its condition
 *     (`w0 = w8 & 1`, with severity 1 -- it throws when the condition is false);
 *   - the block holding the call is entered by the VALID path too, past the
 *     `mov w8, #0` that only the `len < 36` fall-through uses, carrying the
 *     loop's `cset w8, gt` instead. So a well-formed GUID already calls the same
 *     reporter, with the condition satisfied, and nothing happens. Removing the
 *     call is a no-op for valid values;
 *   - everything after it still runs, including the result each function had
 *     already committed to: writeGuidString tail-calls
 *     COsiSmartBuf::write(char const*) with the original pointer, and
 *     readGuidString returns the 1 it moved into w19 *before* reporting, with
 *     the string already in the caller's buffer.
 *
 * The offending value is logged instead, which is the thing that was missing:
 * it names what is in the row, and a bad value is nearly always recognisable
 * (an empty string, or a name that was passed where a UUID belonged). The
 * logging is a separate hook, so if it fails the crash is still fixed.
 *
 * The patch site is verified to hold the expected `blr x9` before anything is
 * written, so a game update that moves it declines with a warning instead of
 * corrupting the instruction stream.
 *
 * This does not repair the row. A database that already holds a bad value keeps
 * holding it, and will log once per save and load until whatever inserts it is
 * fixed.
 */

#ifndef BG3SE_OSIRIS_SAVE_GUARD_H
#define BG3SE_OSIRIS_SAVE_GUARD_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Install the guard. Idempotent and sticky on failure: a second call after a
 * successful install is a no-op, and after a failed one it does not retry.
 * Safe to call before libOsiris is loaded -- it reports false and can be
 * called again later.
 *
 * Returns true when the guard is in place.
 */
bool osi_save_guard_install(void);

/**
 * True when `s` is what writeGuidString accepts: at least 36 characters, whose
 * last 36 are a GUID (hex with '-' at 8, 13, 18 and 23). The engine checks the
 * tail, not the whole string, so BG3's usual `Name_<uuid>` form is valid.
 *
 * Exposed so the insert path can warn while it still knows which mod and which
 * argument is responsible -- the one place the value can still be attributed.
 */
bool osi_guid_string_valid(const char *s);

/** Number of invalid values seen by the guard this session (0 when healthy). */
unsigned osi_save_guard_reject_count(void);

#ifdef __cplusplus
}
#endif

#endif // BG3SE_OSIRIS_SAVE_GUARD_H
