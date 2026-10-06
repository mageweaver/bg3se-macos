/**
 * BG3SE-macOS - Per-savegame snapshots of mod state (see save_snapshot.h)
 *
 * COsiSmartBuf layout, from libOsiris disassembly (write(unsigned char) at
 * 0x336c8, finalize at 0x33820, _readbuf at 0x339d0):
 *   +0x03 u8   reading (non-zero on the load side)
 *   +0x08 ptr  buffer start
 *   +0x10 ptr  cursor
 *   +0x20 u64  buffer capacity
 *   +0x28 u64  count of valid bytes from start (read side; _readbuf computes
 *              the unread remainder as [+0x28] - (cursor - start))
 *   +0x31 u8   whole story is already in memory (read side; _readbuf asserts)
 *   +0x40 obj* backing stream; vtable +0x18 = write(ptr, len) -> bytes written,
 *              +0x20 = read(ptr, len) -> bytes read
 *
 * The story is hashed as it passes through the stream: for the duration of the
 * Save/Load call the stream object's vtable pointer is swapped for a private
 * copy whose write/read slot hashes the bytes and forwards to the original.
 * When the load side has the whole story in memory (+0x31) there is no stream
 * traffic and the [start, start + count) range is hashed directly instead.
 */

#include "save_snapshot.h"
#include "campaign_key.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <dobby.h>

#include "../core/logging.h"

#define SNAP_DIR_NAME   "savestate"
#define SNAP_KEEP_MAX   400      /* newest snapshots kept; each is ~100 KB */
#define VT_COPY_SLOTS   64       /* vtable entries copied (plus 2 RTTI words) */
#define VT_SLOT_WRITE   3        /* +0x18 */
#define VT_SLOT_READ    4        /* +0x20 */

/* ------------------------------------------------------------------------ */
/* Hashing                                                                  */
/* ------------------------------------------------------------------------ */

#define FNV_OFFSET 0xcbf29ce484222325ULL
#define FNV_PRIME  0x100000001b3ULL

typedef struct {
    uint64_t hash;
    uint64_t bytes;
} StoryHash;

static void hash_bytes(StoryHash *h, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint64_t v = h->hash;
    for (size_t i = 0; i < len; i++) {
        v ^= p[i];
        v *= FNV_PRIME;
    }
    h->hash = v;
    h->bytes += len;
}

/* ------------------------------------------------------------------------ */
/* Stream interception                                                      */
/* ------------------------------------------------------------------------ */

typedef size_t (*StreamIoFn)(void *self, void *ptr, size_t len);

typedef struct {
    void  *stream;              /* object whose vptr we swapped, or NULL */
    void **orig_vptr;
    void  *vt_copy[2 + VT_COPY_SLOTS];
    StreamIoFn orig_io;
    StoryHash hash;
} StreamTap;

/* Save and load of the story never overlap, so one tap each is enough. */
static StreamTap s_save_tap;
static StreamTap s_load_tap;

static size_t tap_write(void *self, void *ptr, size_t len) {
    size_t n = s_save_tap.orig_io(self, ptr, len);
    if (ptr && n) hash_bytes(&s_save_tap.hash, ptr, n <= len ? n : len);
    return n;
}

static size_t tap_read(void *self, void *ptr, size_t len) {
    size_t n = s_load_tap.orig_io(self, ptr, len);
    if (ptr && n) hash_bytes(&s_load_tap.hash, ptr, n <= len ? n : len);
    return n;
}

static void tap_begin(StreamTap *tap, void *smart_buf, int slot, void *thunk) {
    memset(tap, 0, sizeof(*tap));
    tap->hash.hash = FNV_OFFSET;
    if (!smart_buf) return;

    void *stream = *(void **)((uint8_t *)smart_buf + 0x40);
    if (!stream) return;
    void **vptr = *(void ***)stream;
    if (!vptr) return;

    /* Keep offset-to-top and typeinfo in front of the copy, as the ABI expects. */
    memcpy(tap->vt_copy, vptr - 2, sizeof(tap->vt_copy));
    tap->orig_io = (StreamIoFn)vptr[slot];
    tap->vt_copy[2 + slot] = thunk;
    tap->orig_vptr = vptr;
    tap->stream = stream;
    *(void ***)stream = &tap->vt_copy[2];
}

static void tap_end(StreamTap *tap) {
    if (tap->stream) {
        *(void ***)tap->stream = tap->orig_vptr;
        tap->stream = NULL;
    }
}

static void log_smart_buf(const char *when, void *b) {
    if (!b) return;
    uint8_t *p = (uint8_t *)b;
    LOG_LUA_INFO("[SaveSnapshot] %s buf: reading=%u inmem=%u start=%p cur=%p cap=%llu "
                  "count=%llu stream=%p", when, p[0x3], p[0x31], *(void **)(p + 0x8),
                  *(void **)(p + 0x10), (unsigned long long)*(uint64_t *)(p + 0x20),
                  (unsigned long long)*(uint64_t *)(p + 0x28), *(void **)(p + 0x40));
}

/* ------------------------------------------------------------------------ */
/* Paths and file copying                                                   */
/* ------------------------------------------------------------------------ */

static const char *support_dir(void) {
    static char dir[PATH_MAX];
    if (dir[0]) return dir;
    const char *home = getenv("HOME");
    if (!home) return NULL;
    snprintf(dir, sizeof(dir), "%s/Library/Application Support/BG3SE", home);
    return dir;
}

static bool copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) return false;
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp", dst);
    FILE *out = fopen(tmp, "wb");
    if (!out) { fclose(in); return false; }

    char buf[16384];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
    }
    if (ferror(in)) ok = false;
    fclose(in);
    if (fclose(out) != 0) ok = false;
    if (ok && rename(tmp, dst) != 0) ok = false;
    if (!ok) unlink(tmp);
    return ok;
}

static bool is_json(const char *name) {
    size_t n = strlen(name);
    return n > 5 && strcmp(name + n - 5, ".json") == 0;
}

/* Remove every *.json in `dir` (not recursive). */
static void clear_json_files(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    char path[PATH_MAX];
    while ((e = readdir(d)) != NULL) {
        if (!is_json(e->d_name)) continue;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        unlink(path);
    }
    closedir(d);
}

/* Copy every *.json in `src` into `dst` (created if missing). Returns count, -1 on error. */
static int copy_json_files(const char *src, const char *dst) {
    DIR *d = opendir(src);
    if (!d) return 0;
    mkdir(dst, 0755);
    struct dirent *e;
    char from[PATH_MAX], to[PATH_MAX];
    int count = 0;
    while ((e = readdir(d)) != NULL) {
        if (!is_json(e->d_name)) continue;
        snprintf(from, sizeof(from), "%s/%s", src, e->d_name);
        snprintf(to, sizeof(to), "%s/%s", dst, e->d_name);
        if (!copy_file(from, to)) { closedir(d); return -1; }
        count++;
    }
    closedir(d);
    return count;
}

/* The three per-campaign stores (user_variables.c, lua_persistentvars.c). */
typedef struct {
    char modvars[PATH_MAX];
    char uservars[PATH_MAX];
    char persist_dir[PATH_MAX];
} CampaignStore;

static bool campaign_store_paths(const char *key, CampaignStore *out) {
    const char *base = support_dir();
    if (!base || !key || !key[0]) return false;
    snprintf(out->modvars, sizeof(out->modvars), "%s/modvars/%s.json", base, key);
    snprintf(out->uservars, sizeof(out->uservars), "%s/uservars/%s.json", base, key);
    snprintf(out->persist_dir, sizeof(out->persist_dir), "%s/persistentvars/%s", base, key);
    return true;
}

static void snapshot_dir_for(uint64_t hash, char *out, size_t out_size) {
    snprintf(out, out_size, "%s/%s/%016llx", support_dir(), SNAP_DIR_NAME,
             (unsigned long long)hash);
}

/* Keep the newest SNAP_KEEP_MAX snapshot directories. */
static void prune_snapshots(void) {
    char root[PATH_MAX];
    snprintf(root, sizeof(root), "%s/%s", support_dir(), SNAP_DIR_NAME);
    DIR *d = opendir(root);
    if (!d) return;

    typedef struct { char name[32]; time_t mtime; } Entry;
    Entry *list = NULL;
    size_t count = 0, cap = 0;
    struct dirent *e;
    char path[PATH_MAX];
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' || strlen(e->d_name) != 16) continue;
        snprintf(path, sizeof(path), "%s/%s", root, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        if (count == cap) {
            cap = cap ? cap * 2 : 64;
            Entry *grown = realloc(list, cap * sizeof(Entry));
            if (!grown) break;
            list = grown;
        }
        snprintf(list[count].name, sizeof(list[count].name), "%s", e->d_name);
        list[count].mtime = st.st_mtime;
        count++;
    }
    closedir(d);

    while (count > SNAP_KEEP_MAX) {
        size_t oldest = 0;
        for (size_t i = 1; i < count; i++) {
            if (list[i].mtime < list[oldest].mtime) oldest = i;
        }
        char dir[PATH_MAX];
        snprintf(dir, sizeof(dir), "%s/%s", root, list[oldest].name);
        char sub[PATH_MAX];
        snprintf(sub, sizeof(sub), "%s/persistentvars", dir);
        clear_json_files(sub);
        rmdir(sub);
        clear_json_files(dir);
        snprintf(sub, sizeof(sub), "%s/campaign.txt", dir);
        unlink(sub);
        rmdir(dir);
        list[oldest] = list[--count];
    }
    free(list);
}

/* ------------------------------------------------------------------------ */
/* Save side                                                                */
/* ------------------------------------------------------------------------ */

static void write_snapshot(uint64_t hash, uint64_t bytes) {
    const char *key = campaign_key_get();
    CampaignStore store;
    if (!campaign_store_paths(key, &store)) {
        LOG_LUA_INFO("[SaveSnapshot] save %016llx: no campaign loaded, not snapshotting",
                     (unsigned long long)hash);
        return;
    }

    char root[PATH_MAX], dir[PATH_MAX], path[PATH_MAX];
    snprintf(root, sizeof(root), "%s/%s", support_dir(), SNAP_DIR_NAME);
    mkdir(root, 0755);
    snapshot_dir_for(hash, dir, sizeof(dir));
    mkdir(dir, 0755);

    /* Start clean: the same story hash saved twice replaces the old copy. */
    snprintf(path, sizeof(path), "%s/persistentvars", dir);
    clear_json_files(path);
    clear_json_files(dir);

    bool ok = true;
    if (access(store.modvars, F_OK) == 0) {
        snprintf(path, sizeof(path), "%s/modvars.json", dir);
        ok &= copy_file(store.modvars, path);
    }
    if (access(store.uservars, F_OK) == 0) {
        snprintf(path, sizeof(path), "%s/uservars.json", dir);
        ok &= copy_file(store.uservars, path);
    }
    snprintf(path, sizeof(path), "%s/persistentvars", dir);
    int pv = copy_json_files(store.persist_dir, path);
    if (pv < 0) ok = false;

    snprintf(path, sizeof(path), "%s/campaign.txt", dir);
    FILE *f = fopen(path, "w");
    if (f) { fprintf(f, "%s\n", key); fclose(f); } else ok = false;

    /* Bump mtime so pruning treats a re-saved snapshot as new. */
    utimes(dir, NULL);

    if (ok) {
        LOG_LUA_INFO("[SaveSnapshot] save %016llx (%llu story bytes): mod state snapshotted "
                     "(%d PersistentVars files)", (unsigned long long)hash,
                     (unsigned long long)bytes, pv);
    } else {
        LOG_LUA_ERROR("[SaveSnapshot] save %016llx: snapshot incomplete (%s) -- loading "
                      "this save will keep the current mod state", (unsigned long long)hash,
                      strerror(errno));
        snprintf(path, sizeof(path), "%s/campaign.txt", dir);
        unlink(path);  /* no campaign.txt = not applied on load */
    }
    prune_snapshots();
}

typedef int (*OsiSaveFn)(void *self, void *smart_buf, bool a, bool b);
static OsiSaveFn s_orig_save = NULL;

static int hooked_osiris_save(void *self, void *smart_buf, bool a, bool b) {
    log_smart_buf("save begin", smart_buf);
    tap_begin(&s_save_tap, smart_buf, VT_SLOT_WRITE, (void *)tap_write);
    bool tapped = s_save_tap.stream != NULL;

    int result = s_orig_save(self, smart_buf, a, b);

    tap_end(&s_save_tap);
    log_smart_buf("save end", smart_buf);

    if (!tapped) {
        LOG_LUA_WARN("[SaveSnapshot] COsiris::Save had no backing stream; save not "
                     "snapshotted (mod state will not follow this save)");
    } else if (!result) {
        LOG_LUA_WARN("[SaveSnapshot] COsiris::Save failed; not snapshotting");
    } else if (s_save_tap.hash.bytes == 0) {
        LOG_LUA_WARN("[SaveSnapshot] COsiris::Save streamed 0 bytes through the tap; "
                     "not snapshotting");
    } else {
        write_snapshot(s_save_tap.hash.hash, s_save_tap.hash.bytes);
    }
    return result;
}

bool save_snapshot_install(void) {
    static bool installed = false;
    if (installed) return true;

    void *h = dlopen("@rpath/libOsiris.dylib", RTLD_NOLOAD);
    if (!h) h = dlopen("@executable_path/../Frameworks/libOsiris.dylib", RTLD_NOLOAD);
    if (!h) return false;

    void *fn = dlsym(h, "_ZN7COsiris4SaveER12COsiSmartBufbb");
    if (!fn) {
        LOG_LUA_WARN("[SaveSnapshot] COsiris::Save not exported; mod state will not "
                     "follow savegames");
        installed = true;  /* nothing to retry */
        return false;
    }
    if (DobbyHook(fn, (void *)hooked_osiris_save, (void **)&s_orig_save) != 0) {
        LOG_LUA_WARN("[SaveSnapshot] could not hook COsiris::Save; mod state will not "
                     "follow savegames");
        installed = true;
        return false;
    }
    installed = true;
    LOG_LUA_INFO("[SaveSnapshot] COsiris::Save hooked at %p", fn);
    return true;
}

/* ------------------------------------------------------------------------ */
/* Load side                                                                */
/* ------------------------------------------------------------------------ */

static bool read_snapshot_campaign(uint64_t hash, char *out, size_t out_size);

static bool s_pending_load = false;
static uint64_t s_pending_hash = 0;
static uint64_t s_pending_bytes = 0;
/* Last load whose story had a snapshot, for a rebuild that follows it. */
static uint64_t s_snap_hash = 0;
static uint64_t s_snap_bytes = 0;
static time_t s_snap_time = 0;

void save_snapshot_story_load_begin(void *smart_buf) {
    s_pending_load = false;
    log_smart_buf("load begin", smart_buf);

    uint8_t *b = (uint8_t *)smart_buf;
    bool in_memory = b && b[0x31];
    tap_begin(&s_load_tap, in_memory ? NULL : smart_buf, VT_SLOT_READ, (void *)tap_read);

    /* Bytes already buffered never pass through the stream again: the whole
     * story when it is in memory, or whatever the constructor prefetched.
     * _readbuf moves the unread tail to the front and appends new reads, so
     * hashing the buffered bytes once here and every stream read after it
     * covers the story exactly once, in order. */
    if (b) {
        uint8_t *start = *(uint8_t **)(b + 0x8);
        uint64_t count = *(uint64_t *)(b + 0x28);
        if (start && count) hash_bytes(&s_load_tap.hash, start, (size_t)count);
    }
}

void save_snapshot_story_load_end(void *smart_buf, int result) {
    tap_end(&s_load_tap);
    log_smart_buf("load end", smart_buf);
    if (!result || s_load_tap.hash.bytes == 0) {
        LOG_LUA_INFO("[SaveSnapshot] story load (%s, %llu bytes hashed): no snapshot lookup",
                     result ? "ok" : "failed", (unsigned long long)s_load_tap.hash.bytes);
        return;
    }
    uint64_t hash = s_load_tap.hash.hash;
    char key[128];
    bool has_snap = read_snapshot_campaign(hash, key, sizeof(key));

    /* A changed mod list makes the game rebuild the story right after loading
     * the save's: a second COsiris::Load with a hash no snapshot was ever
     * written for. Replacing the pending hash with it lost the save's snapshot,
     * so the campaign key had to wait for DB_Avatars and arrived after
     * SessionLoaded; every mod restoring per-playthrough state on load
     * (transmog, AEE) saw empty data. Keep the save's snapshot instead. The
     * rebuild follows within seconds (2.6 s observed); 20 s keeps a New Game
     * started after loading a save from inheriting that save's state. */
    if (!has_snap && s_snap_hash != 0 && time(NULL) - s_snap_time < 20) {
        s_pending_hash = s_snap_hash;
        s_pending_bytes = s_snap_bytes;
        s_pending_load = true;
        LOG_LUA_INFO("[SaveSnapshot] story %016llx has no snapshot but follows %016llx "
                     "(story rebuilt after a mod-list change); keeping the save's snapshot",
                     (unsigned long long)hash, (unsigned long long)s_snap_hash);
        return;
    }

    s_pending_hash = hash;
    s_pending_bytes = s_load_tap.hash.bytes;
    s_pending_load = true;
    if (has_snap) {
        s_snap_hash = hash;
        s_snap_bytes = s_pending_bytes;
        s_snap_time = time(NULL);
    }
    LOG_LUA_INFO("[SaveSnapshot] story loaded: %016llx (%llu bytes)",
                 (unsigned long long)s_pending_hash, (unsigned long long)s_pending_bytes);
}

static bool read_snapshot_campaign(uint64_t hash, char *out, size_t out_size) {
    char dir[PATH_MAX], path[PATH_MAX];
    snapshot_dir_for(hash, dir, sizeof(dir));
    snprintf(path, sizeof(path), "%s/campaign.txt", dir);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    if (!fgets(out, (int)out_size, f)) out[0] = '\0';
    fclose(f);
    out[strcspn(out, "\r\n")] = '\0';
    return out[0] != '\0';
}

bool save_snapshot_pending_campaign(char *out, size_t out_size) {
    if (!s_pending_load || !out || out_size == 0) return false;
    return read_snapshot_campaign(s_pending_hash, out, out_size);
}

bool save_snapshot_apply_pending(const char *campaign_key) {
    if (!s_pending_load) return false;
    s_pending_load = false;

    char dir[PATH_MAX], path[PATH_MAX];
    snapshot_dir_for(s_pending_hash, dir, sizeof(dir));

    snprintf(path, sizeof(path), "%s/campaign.txt", dir);
    FILE *f = fopen(path, "r");
    if (!f) {
        LOG_LUA_INFO("[SaveSnapshot] no snapshot for story %016llx (save predates snapshots, "
                     "or a new game); keeping current mod state",
                     (unsigned long long)s_pending_hash);
        return false;
    }
    char saved_key[CAMPAIGN_KEY_MAX] = {0};
    if (!fgets(saved_key, sizeof(saved_key), f)) saved_key[0] = '\0';
    fclose(f);
    saved_key[strcspn(saved_key, "\r\n")] = '\0';

    if (!campaign_key || strcmp(saved_key, campaign_key) != 0) {
        LOG_LUA_WARN("[SaveSnapshot] snapshot %016llx belongs to campaign %s, not %s; ignored",
                     (unsigned long long)s_pending_hash, saved_key,
                     campaign_key ? campaign_key : "(none)");
        return false;
    }

    CampaignStore store;
    if (!campaign_store_paths(campaign_key, &store)) return false;

    char base[PATH_MAX];
    snprintf(base, sizeof(base), "%s/modvars", support_dir());
    mkdir(base, 0755);
    snprintf(base, sizeof(base), "%s/uservars", support_dir());
    mkdir(base, 0755);
    snprintf(base, sizeof(base), "%s/persistentvars", support_dir());
    mkdir(base, 0755);

    bool ok = true;
    snprintf(path, sizeof(path), "%s/modvars.json", dir);
    if (access(path, F_OK) == 0) ok &= copy_file(path, store.modvars);
    else unlink(store.modvars);

    snprintf(path, sizeof(path), "%s/uservars.json", dir);
    if (access(path, F_OK) == 0) ok &= copy_file(path, store.uservars);
    else unlink(store.uservars);

    mkdir(store.persist_dir, 0755);
    clear_json_files(store.persist_dir);
    snprintf(path, sizeof(path), "%s/persistentvars", dir);
    int pv = copy_json_files(path, store.persist_dir);
    if (pv < 0) ok = false;

    if (ok) {
        LOG_LUA_INFO("[SaveSnapshot] restored mod state saved with story %016llx "
                     "(%d PersistentVars files)", (unsigned long long)s_pending_hash, pv);
    } else {
        LOG_LUA_ERROR("[SaveSnapshot] restoring snapshot %016llx failed part-way (%s); "
                      "mod state may be mixed", (unsigned long long)s_pending_hash,
                      strerror(errno));
    }
    return true;
}
