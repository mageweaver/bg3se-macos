/**
 * BG3SE-macOS - Keep a poisoned Osiris database from killing the save or the
 * load. See osi_save_guard.h for why, and why neutralising one instruction is
 * faithful to the engine.
 */

#include "osi_save_guard.h"

#include <ctype.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <dobby.h>

#include "../core/logging.h"
#include "../hooks/arm64_hook.h"

/* `blr x9` -- the assert call on both the write and the read path.
 * BLR Xn = 0xD63F0000 | (Rn << 5); Rn = 9. */
#define INSN_BLR_X9 0xD63F0120u
#define INSN_NOP    0xD503201Fu

/* Offsets of that call inside each function, from disassembly of the arm64
 * slice (game build 4.1.1.7398727). Verified against the expected encoding
 * before anything is written, so a game update that moves them is a declined
 * patch and a warning, never a corrupted instruction stream. */
#define WRITE_ASSERT_OFFSET 0x40    /* COsiSmartBuf::writeGuidString + 64  */
#define READ_ASSERT_OFFSET  0x210   /* COsiSmartBuf::readGuidString  + 528 */

typedef void *(*WriteGuidStringFn)(void *buf, const char *s);
typedef int (*ReadGuidStringFn)(void *buf, char *out);
typedef void *(*ReteDBaseCtorFn)(void *self, void *buf, const char *name);

static WriteGuidStringFn s_orig_write = NULL;
static ReadGuidStringFn s_orig_read = NULL;
static ReteDBaseCtorFn s_orig_dbase_ctor = NULL;
static unsigned s_rejects = 0;

/* Name of the database currently being deserialised. A bad value on its own
 * says what is wrong but not where, and "which database" is usually enough to
 * identify what inserted it. CReteDBase's constructor deserialises its own
 * tuples, so a value read while this is set belongs to this database.
 * Read/written only on the thread doing the load. */
static const char *s_loading_dbase = NULL;

/* Distinct bad values already reported. A poisoned row is written on every save
 * and read on every load, so an unbounded log would be the loudest thing in the
 * file and say nothing new after the first line. */
#define MAX_REPORTED 8
static char s_reported[MAX_REPORTED][64];
static unsigned s_reported_count = 0;

bool osi_guid_string_valid(const char *s) {
    if (!s) return false;
    size_t len = strlen(s);
    if (len < 36) return false;
    const char *g = s + len - 36;   /* engine validates the tail, not the whole string */
    for (int i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (g[i] != '-') return false;
        } else if (!isxdigit((unsigned char)g[i])) {
            return false;
        }
    }
    return true;
}

unsigned osi_save_guard_reject_count(void) { return s_rejects; }

/* Render a value for the log. A bad value is often empty or non-printable, and
 * "" on its own line tells nobody anything, so length and escaping are part of
 * the message. */
static void describe(const char *s, char *out, size_t outSize) {
    if (!s) { snprintf(out, outSize, "(null)"); return; }
    size_t len = strlen(s);
    size_t w = (size_t)snprintf(out, outSize, "len=%zu \"", len);
    for (size_t i = 0; i < len && w + 6 < outSize; i++) {
        unsigned char c = (unsigned char)s[i];
        if (isprint(c) && c != '"' && c != '\\') {
            out[w++] = (char)c;
        } else {
            w += (size_t)snprintf(out + w, outSize - w, "\\x%02x", c);
        }
    }
    if (w + 2 <= outSize) { out[w++] = '"'; out[w] = '\0'; }
    else if (outSize) out[outSize - 1] = '\0';
}

static bool already_reported(const char *s) {
    for (unsigned i = 0; i < s_reported_count; i++) {
        if (strncmp(s_reported[i], s, sizeof(s_reported[0]) - 1) == 0) return true;
    }
    if (s_reported_count < MAX_REPORTED) {
        snprintf(s_reported[s_reported_count], sizeof(s_reported[0]), "%s", s);
        s_reported_count++;
    }
    return false;
}

static void report(const char *s, const char *direction, const char *consequence) {
    s_rejects++;
    if (s && already_reported(s)) return;
    char desc[256];
    describe(s, desc, sizeof(desc));
    LOG_OSIRIS_ERROR("Osiris %s: database %s holds a GUIDSTRING that is not a GUID "
                     "-- %s. The engine would %s; continuing instead. Something "
                     "inserted a non-GUID into a GUID column; the row stays bad until "
                     "that is fixed.", direction,
                     s_loading_dbase ? s_loading_dbase : "(unknown)",
                     desc, consequence);
}

/* Instrumentation only -- records the name, changes nothing. */
static void *hooked_dbase_ctor(void *self, void *buf, const char *name) {
    const char *prev = s_loading_dbase;   /* restore rather than clear: nested reads */
    s_loading_dbase = name;
    void *r = s_orig_dbase_ctor(self, buf, name);
    s_loading_dbase = prev;
    return r;
}

/* The string is in hand on the way out, so it can be named before the engine
 * sees it. */
static void *hooked_write_guid_string(void *buf, const char *s) {
    if (!osi_guid_string_valid(s)) report(s, "save", "abort the save here");
    return s_orig_write(buf, s);
}

/* On the way in the value only exists once the engine has read it into the
 * caller's buffer, so it can only be named afterwards. The assert that used to
 * fire between those two points is patched out, so the original returns
 * normally and there is no exception to unwind through the trampoline. */
static int hooked_read_guid_string(void *buf, char *out) {
    int r = s_orig_read(buf, out);
    if (!osi_guid_string_valid(out)) report(out, "load", "abort the load here");
    return r;
}

/* ---------------------------------------------------------------------------
 * Story-patch sanitiser
 *
 * Surviving the save and the load is not enough. When the story has changed
 * since the save, the engine rebuilds it by dumping every database to a text
 * patch file and re-parsing it (COsiris::Merge -> _RunPatchFile -> yyparse).
 * A bare value comes back out as a statement like
 *
 *     DB_CantMove(S_Player_Jin);
 *
 * and the parser cannot make a GUIDSTRING of it, so the tuple it builds has no
 * valid value and CReteDBase::find aborts in _TupleRefs ("Value is not valid!
 * Investigate!"). That assert is NOT patched out -- it means the engine is
 * about to operate on a value it has itself declared unusable, which is a
 * different and worse thing than refusing to serialise one.
 *
 * Instead the statement is removed before the parser sees it. That is possible
 * here and nowhere else, because at this one point the data is plain text at a
 * clean boundary, and because the load has already run: the read guard above
 * has recorded exactly which values the engine rejected, so the removal is
 * driven by the engine's own verdict rather than by guessing which unquoted
 * token ought to have been a GUID. Enum constants also appear as bare
 * identifiers in this file, so guessing would drop valid rows.
 *
 * The rows dropped are ones the engine cannot represent at all -- the
 * alternative to dropping them is the abort. They are not silently gone: each
 * is logged. The databases in memory still hold the bad row, so it must still
 * be deleted (Osi.<DB>:Delete) and the game saved for the save itself to come
 * out clean.
 * ------------------------------------------------------------------------- */

/* macOS wchar_t is 4 bytes. The paths here are ASCII. Returns false if the
 * string does not fit or is not ASCII, in which case the file is left alone. */
static bool wide_to_utf8(const wchar_t *w, char *out, size_t outSize) {
    if (!w || outSize == 0) return false;
    size_t i = 0;
    for (; w[i] && i + 1 < outSize; i++) {
        if ((unsigned long)w[i] > 0x7f) return false;
        out[i] = (char)w[i];
    }
    if (w[i]) return false;             /* truncated */
    out[i] = '\0';
    return true;
}

/* True when `line` uses `val` as a whole argument token, e.g. "(S_Player_Jin)"
 * or ",S_Player_Jin)" -- not as a prefix of a longer identifier. */
static bool line_uses_value(const char *line, size_t lineLen, const char *val) {
    size_t vlen = strlen(val);
    if (vlen == 0 || vlen > lineLen) return false;
    for (const char *p = line; (p = memchr(p, val[0], (size_t)(line + lineLen - p))); p++) {
        if ((size_t)(line + lineLen - p) < vlen) break;
        if (memcmp(p, val, vlen) != 0) continue;
        char before = (p == line) ? '\0' : p[-1];
        char after = p[vlen];
        bool beforeOk = !(isalnum((unsigned char)before) || before == '_' || before == '-');
        bool afterOk = !(isalnum((unsigned char)after) || after == '_' || after == '-');
        if (beforeOk && afterOk) return true;
    }
    return false;
}

static void sanitize_patch_file(const char *path) {
    if (s_reported_count == 0) return;          /* nothing rejected -> nothing to do */

    FILE *f = fopen(path, "rb");
    if (!f) return;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return; }
    long size = ftell(f);
    if (size <= 0 || size > (200L << 20)) { fclose(f); return; }
    rewind(f);
    char *buf = (char *)malloc((size_t)size);
    if (!buf) { fclose(f); return; }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) { free(buf); return; }

    char *out = (char *)malloc((size_t)size);
    if (!out) { free(buf); return; }

    size_t w = 0, dropped = 0;
    const char *p = buf;
    const char *end = buf + size;
    while (p < end) {
        const char *nl = (const char *)memchr(p, '\n', (size_t)(end - p));
        size_t lineLen = nl ? (size_t)(nl - p) + 1 : (size_t)(end - p);
        bool drop = false;
        for (unsigned i = 0; i < s_reported_count && !drop; i++) {
            if (s_reported[i][0] && line_uses_value(p, lineLen, s_reported[i])) drop = true;
        }
        if (drop) {
            dropped++;
            if (dropped <= 8) {
                int n = (int)lineLen;
                while (n > 0 && (p[n - 1] == '\n' || p[n - 1] == '\r')) n--;
                LOG_OSIRIS_WARN("Osiris story patch: dropped a row the engine cannot "
                                "parse as a GUIDSTRING -- %.*s", n, p);
            }
        } else {
            memcpy(out + w, p, lineLen);
            w += lineLen;
        }
        p += lineLen;
    }

    if (dropped == 0) { free(out); free(buf); return; }

    f = fopen(path, "wb");
    if (f) {
        bool ok = (fwrite(out, 1, w, f) == w);
        fclose(f);
        LOG_OSIRIS_ERROR("Osiris story patch: removed %zu unparseable row(s) from %s "
                         "so the story merge can complete%s. The databases in memory "
                         "still hold them -- delete the row and save for the save "
                         "itself to come out clean.", dropped, path,
                         ok ? "" : " (WRITE FAILED)");
    }
    free(out);
    free(buf);
}

typedef int (*RunPatchFileFn)(void *self, const wchar_t *path, const wchar_t *mode);
static RunPatchFileFn s_orig_run_patch = NULL;

static int hooked_run_patch_file(void *self, const wchar_t *path, const wchar_t *mode) {
    char p[1024];
    if (wide_to_utf8(path, p, sizeof(p))) sanitize_patch_file(p);
    return s_orig_run_patch(self, path, mode);
}

/**
 * Replace the assert call with a NOP.
 *
 * On both functions the block holding this call is reached by the valid path
 * too -- it is entered past the `mov w8, #0` that the invalid fall-through
 * uses, carrying the loop's `cset w8, gt` instead, and `w0 = w8 & 1` is the
 * assert's condition. So the valid path calls the same reporter with the
 * condition satisfied, which does nothing. Removing the call is therefore a
 * no-op for well-formed GUIDs and drops only the throw for malformed ones;
 * everything else in both functions still runs, including the return value each
 * had already chosen (`writeGuidString` tail-calls
 * `COsiSmartBuf::write(char const*)`; `readGuidString` returns the 1 it set
 * before reporting).
 */
static bool patch_assert_call(void *fn, unsigned offset, const char *what) {
    if (!fn) return false;
    uint32_t *site = (uint32_t *)((char *)fn + offset);
    if (*site == INSN_NOP) return true;             /* already patched */
    if (*site != INSN_BLR_X9) {
        LOG_OSIRIS_WARN("Osiris GUIDSTRING guard: %s+0x%x is 0x%08x, expected blr x9 "
                        "(0x%08x) -- declining to patch. The game build has moved; an "
                        "invalid GUIDSTRING in a database will still be fatal.",
                        what, offset, *site, INSN_BLR_X9);
        return false;
    }
    if (!arm64_write_instruction(site, INSN_NOP)) {
        LOG_OSIRIS_WARN("Osiris GUIDSTRING guard: could not write to %s+0x%x",
                        what, offset);
        return false;
    }
    return true;
}

bool osi_save_guard_install(void) {
    static bool installed = false;
    static bool failed = false;
    if (installed) return true;
    if (failed) return false;

    void *h = dlopen("@rpath/libOsiris.dylib", RTLD_NOLOAD);
    if (!h) h = dlopen("@executable_path/../Frameworks/libOsiris.dylib", RTLD_NOW);
    if (!h) {
        /* Not an error yet: libOsiris may simply not be loaded. Caller may retry. */
        return false;
    }

    /* dlsym wants the name without the leading underscore the symbol table shows. */
    void *writeFn = dlsym(h, "_ZN12COsiSmartBuf15writeGuidStringEPKc");
    void *readFn = dlsym(h, "_ZN12COsiSmartBuf14readGuidStringEPc");
    if (!writeFn || !readFn) {
        LOG_OSIRIS_WARN("Osiris GUIDSTRING guard: missing libOsiris exports "
                        "(write=%p read=%p); an invalid GUIDSTRING in a database will "
                        "abort the save and the load", writeFn, readFn);
        failed = true;
        return false;
    }

    /* Defuse first. If the logging hooks below fail, the crash is still fixed --
     * and an exception can no longer be thrown out of the hooked function, which
     * is what makes the read hook safe (unwinding through a Dobby trampoline has
     * no unwind info and would terminate on its own). */
    bool wp = patch_assert_call(writeFn, WRITE_ASSERT_OFFSET, "writeGuidString");
    bool rp = patch_assert_call(readFn, READ_ASSERT_OFFSET, "readGuidString");
    if (!wp && !rp) {
        failed = true;
        return false;
    }

    /* Logging only. Both offsets patched above sit well past the prologue Dobby
     * relocates, so the patches stay in the live body. */
    if (DobbyHook(writeFn, (void *)hooked_write_guid_string, (void **)&s_orig_write) != 0) {
        LOG_OSIRIS_WARN("Osiris GUIDSTRING guard: hook on writeGuidString failed; "
                        "saves are safe but a bad value will not be named");
        s_orig_write = NULL;
    }
    if (rp && DobbyHook(readFn, (void *)hooked_read_guid_string, (void **)&s_orig_read) != 0) {
        LOG_OSIRIS_WARN("Osiris GUIDSTRING guard: hook on readGuidString failed; "
                        "loads are safe but a bad value will not be named");
        s_orig_read = NULL;
    }

    /* Names the database a bad value was read from. Instrumentation, not a fix:
     * if it does not install, the value is still reported, just without a name. */
    void *ctorFn = dlsym(h, "_ZN10CReteDBaseC2EP12COsiSmartBufPKc");
    if (!ctorFn || DobbyHook(ctorFn, (void *)hooked_dbase_ctor,
                             (void **)&s_orig_dbase_ctor) != 0) {
        s_orig_dbase_ctor = NULL;
        LOG_OSIRIS_WARN("Osiris GUIDSTRING guard: could not hook CReteDBase's "
                        "constructor (%p); a bad value will be reported without "
                        "naming its database", ctorFn);
    }

    /* Removes rows the parser cannot turn into a GUIDSTRING from the story patch
     * file, which is the only place the merge can be made to survive them. */
    void *patchFn = dlsym(h, "_ZN7COsiris13_RunPatchFileEPKwS1_");
    if (!patchFn || DobbyHook(patchFn, (void *)hooked_run_patch_file,
                              (void **)&s_orig_run_patch) != 0) {
        s_orig_run_patch = NULL;
        LOG_OSIRIS_WARN("Osiris GUIDSTRING guard: could not hook _RunPatchFile (%p); "
                        "a story merge over a bad row will still abort", patchFn);
    }

    installed = true;
    /* WARN rather than INFO despite being a success line: the failure modes this
     * guard covers are diagnosed from logs captured at WARN (the INFO stream is
     * dominated by mod output and gets turned off), and "did the guard install?"
     * is the first question asked of any such log -- a missing line there is
     * otherwise indistinguishable from a guard that never ran. The per-part
     * flags are included for the same reason: each can fail independently. */
    LOG_OSIRIS_WARN("Osiris GUIDSTRING guard installed (write patch=%d hook=%d, "
                    "read patch=%d hook=%d, dbname=%d, storypatch=%d)",
                    wp, s_orig_write != NULL, rp, s_orig_read != NULL,
                    s_orig_dbase_ctor != NULL, s_orig_run_patch != NULL);
    return true;
}
