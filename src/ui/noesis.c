/**
 * noesis.c - Noesis UI bridge. See noesis.h.
 */

#include "noesis.h"

#include "../core/logging.h"
#include "../core/safe_memory.h"
#include "../resource/resource_manager.h"
#include "../hooks/arm64_hook.h"
#include "../core/version_detect.h"

#include <dobby.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <string.h>
#include <dispatch/dispatch.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdlib.h>

// Exported Noesis entry points. All are `T` symbols in the main binary, so
// dlsym finds them without any address table to keep in step with game updates.
#define SYM_FIND_NAME     "_ZNK6Noesis16FrameworkElement8FindNameEPKc"
#define SYM_CHILD_COUNT   "_ZN6Noesis16VisualTreeHelper16GetChildrenCountEPKNS_6VisualE"
#define SYM_GET_CHILD     "_ZN6Noesis16VisualTreeHelper8GetChildEPKNS_6VisualEj"
#define SYM_GET_ROOT      "_ZN6Noesis16VisualTreeHelper7GetRootEPKNS_6VisualE"
#define SYM_LOG_COUNT     "_ZN6Noesis17LogicalTreeHelper16GetChildrenCountEPKNS_16FrameworkElementE"
#define SYM_LOG_CHILD     "_ZN6Noesis17LogicalTreeHelper8GetChildEPKNS_16FrameworkElementEj"
#define SYM_SYMBOL_STR    "_ZN6Noesis13SymbolManager9GetStringEj"
#define SYM_GET_NAME      "_ZNK6Noesis16FrameworkElement7GetNameEv"
/* RTTI, so a Visual* that is NOT a FrameworkElement can be rejected before it
 * reaches an accessor that assumes one. */
#define SYM_FE_CLASSTYPE  "_ZN6Noesis16FrameworkElement18StaticGetClassTypeEPNS_7TypeTagIS0_EE"
#define SYM_IS_ASSIGNABLE "_ZNK6Noesis9TypeClass16IsAssignableFromEPKNS_4TypeE"
#define SYM_BO_CLASSTYPE  "_ZNK6Noesis10BaseObject12GetClassTypeEv"
#define SYM_VT_BASEOBJECT "_ZTVN6Noesis10BaseObjectE"
/* The engine's own checked dynamic cast: NULL-safe, walks TypeClass ancestry
 * via IsDescendantOf, and applies the ancestor's this-pointer offset -- the
 * multiple-inheritance adjustment a hand-rolled assignability check misses. */
#define SYM_CAST          "_ZN6Noesis4CastEPKNS_9TypeClassEPNS_10BaseObjectE"

typedef void *(*FindNameFn)(const void *element, const char *name);
typedef int (*ChildCountFn)(const void *visual);
typedef void *(*GetChildFn)(const void *visual, unsigned int index);

/*
 * LogicalTreeHelper::GetChild returns Ptr<BaseComponent> -- NOT a raw pointer.
 *
 * Proven from disassembly (2026-08-28): the helper is a three-instruction tail
 * trampoline (`ldr x9,[x0]; ldr x2,[x9,#0x438]; br x2`) into the virtual
 * GetLogicalChild, and Viewbox's override does `mov x19, x8 ... str x0,[x19];
 * ldadd w9,w8,[x8]` -- it stores the result through the x8 indirect-result
 * register and BUMPS A REFCOUNT at result+0x8. A plain-pointer typedef never
 * supplies x8, so the engine stored through whatever garbage x8 held:
 *   - x8 = 0x17            -> the "BaseCollection::GetComponent at 0x17" crash
 *   - x8 = a __TEXT address -> SIGBUS KERN_PROTECTION_FAILURE mid-probe
 *   - x8 = writable garbage -> one silently corrupted word per call, on every
 *     "successful" logical-tree walk to date
 * Third occurrence of this exact trap (CreateView's Ptr<View>, the functor
 * result_out, now this). Rule: any Noesis/engine return of Ptr<T> or a struct
 * needs a memory-class C return type so the compiler provides x8.
 *
 * A >16-byte struct return is AAPCS64 memory-class: the compiler passes x8
 * pointing at `r`, exactly what the callee expects.
 */
typedef struct { void *p; void *pad[3]; } NsPtrRet;
typedef NsPtrRet (*LogChildPtrFn)(const void *element, unsigned int index);

/* Balance the AddRef GetLogicalChild performs. We cannot run the destructor
 * chain from C, so if our reference turns out to be the LAST one the node is
 * pinned alive instead (a leak, never a double-free or corruption). */
static void ns_ptr_release_ref(void *obj) {
    if (!obj) return;
    _Atomic(int32_t) *rc = (_Atomic(int32_t) *)((uint8_t *)obj + 0x8);
    int32_t prev = atomic_fetch_sub_explicit(rc, 1, memory_order_acq_rel);
    if (prev <= 1) {
        atomic_fetch_add_explicit(rc, 1, memory_order_relaxed);   /* pin */
        LOG_IMGUI_WARN("Noesis: released the last reference to %p — pinning "
                       "it alive rather than hand-rolling destruction", obj);
    }
}
typedef void *(*GetRootFn)(const void *visual);
typedef const char *(*SymbolStringFn)(unsigned int symbol);
typedef const char *(*GetNameFn)(const void *element);
typedef const void *(*StaticClassTypeFn)(void *tag);
typedef bool (*IsAssignableFromFn)(const void *typeClass, const void *other);
typedef const void *(*GetClassTypeFn)(const void *obj);
typedef void *(*NsCastFn)(const void *typeClass, void *obj);

static FindNameFn s_find_name = NULL;
static ChildCountFn s_child_count = NULL;
static GetChildFn s_get_child = NULL;
static GetRootFn s_get_root = NULL;
static ChildCountFn s_log_count = NULL;
static LogChildPtrFn s_log_child = NULL;
static SymbolStringFn s_symbol_str = NULL;
static GetNameFn s_get_name = NULL;
static StaticClassTypeFn s_fe_type_fn = NULL; /* resolved at init, CALLED lazily */
static const void *s_fe_type = NULL;          /* FrameworkElement's TypeClass, on demand */
static IsAssignableFromFn s_is_assignable = NULL;
static int s_classtype_slot = -1;             /* index of GetClassType from an object's vptr */
static NsCastFn s_cast = NULL;                /* Noesis::Cast, dlsym'd */

/* Defined further down, next to the image-bounds helpers they depend on. */
static void resolve_classtype_slot(void *self);
static bool visual_is_plausible(const void *visual);



/*
 * Captured view roots.
 *
 * The game creates a handful of views, not hundreds, and it never tells us when
 * one dies -- so this keeps a small fixed set and drops the oldest rather than
 * growing without bound. Entries are validated before use: a stale root is a
 * pointer into freed memory, and reading it is the one way this can crash the
 * game rather than merely fail.
 */
#define NOESIS_MAX_ROOTS 16

static void *s_roots[NOESIS_MAX_ROOTS];
static int s_root_count = 0;
static bool s_initialized = false;

void noesis_register_root(void *root) {
    if (!root) return;

    for (int i = 0; i < s_root_count; i++) {
        if (s_roots[i] == root) {
            // Seen already: move it to the end so "most recent" stays true.
            for (int j = i; j < s_root_count - 1; j++) s_roots[j] = s_roots[j + 1];
            s_roots[s_root_count - 1] = root;
            return;
        }
    }

    if (s_root_count == NOESIS_MAX_ROOTS) {
        memmove(&s_roots[0], &s_roots[1], sizeof(s_roots[0]) * (NOESIS_MAX_ROOTS - 1));
        s_root_count--;
    }
    s_roots[s_root_count++] = root;
    LOG_IMGUI_INFO("Noesis view root captured: %p (%d tracked)", root, s_root_count);
}

/*
 * CreateView is deliberately not hooked.
 *
 * Noesis::GUI::CreateView returns Ptr<View>, a refcounted smart pointer, which
 * comes back through the AArch64 indirect-result register rather than x0. A
 * replacement declared to return void* reads a register holding nothing
 * meaningful and hands the caller garbage: gui::GameUI::InitGameControlFromUI
 * dereferenced it and the game died on its first UI update.
 *
 * FrameworkElement::OnPostInit is used instead. It returns void and takes only
 * `this`, so there is no return convention to get wrong, and every element the
 * game builds passes through it -- from any one of them
 * VisualTreeHelper::GetRoot reaches the root.
 */


bool noesis_init(void) {
    if (s_initialized) return s_find_name != NULL;
    s_initialized = true;

    void *self = dlopen(NULL, RTLD_NOW);
    if (!self) {
        LOG_IMGUI_WARN("Noesis: cannot open the main image");
        return false;
    }

    s_find_name = (FindNameFn)dlsym(self, SYM_FIND_NAME);
    s_child_count = (ChildCountFn)dlsym(self, SYM_CHILD_COUNT);
    s_get_child = (GetChildFn)dlsym(self, SYM_GET_CHILD);
    s_get_root = (GetRootFn)dlsym(self, SYM_GET_ROOT);
    s_log_count = (ChildCountFn)dlsym(self, SYM_LOG_COUNT);
    s_log_child = (LogChildPtrFn)dlsym(self, SYM_LOG_CHILD);
    s_symbol_str = (SymbolStringFn)dlsym(self, SYM_SYMBOL_STR);
    s_get_name = (GetNameFn)dlsym(self, SYM_GET_NAME);

    /*
     * RTTI guard: resolve the SYMBOLS here, but do NOT call
     * StaticGetClassType yet.
     *
     * Calling it during our init crashed the game at boot on 2026-08-27:
     * every launch died ~15s in, inside NsRegisterReflectionCoreTypeConverter
     * -> Noesis::IdOf -> strlen(NULL), while the launch immediately before the
     * change ran 13 minutes. StaticGetClassType forces Noesis type
     * registration, and our constructor runs before Noesis has finished its
     * own reflection setup. It is resolved lazily instead, on first real use,
     * by which point the UI exists.
     */
    s_fe_type_fn = (StaticClassTypeFn)dlsym(self, SYM_FE_CLASSTYPE);
    s_is_assignable = (IsAssignableFromFn)dlsym(self, SYM_IS_ASSIGNABLE);
    s_cast = (NsCastFn)dlsym(self, SYM_CAST);
    resolve_classtype_slot(self);
    if (!s_fe_type_fn || !s_is_assignable || s_classtype_slot < 0) {
        LOG_IMGUI_WARN("Noesis: FrameworkElement RTTI unavailable "
                       "(typefn=%p assignable=%p slot=%d) — Name and "
                       "logical-tree access will refuse elements",
                       (void *)s_fe_type_fn, (void *)s_is_assignable,
                       s_classtype_slot);
    }
    if (!s_find_name || !s_child_count || !s_get_child) {
        LOG_IMGUI_WARN("Noesis: exports missing (FindName=%p count=%p child=%p) "
                       "— Ext.UI stays inert",
                       (void *)s_find_name, (void *)s_child_count,
                       (void *)s_get_child);
        s_find_name = NULL;
        return false;
    }

    /*
     * No hook. Two attempts at capturing a view root by interception both took
     * the game down:
     *
     *   GUI::CreateView returns Ptr<View>, a smart pointer returned through the
     *   indirect-result register, so a void*-returning replacement handed the
     *   caller garbage and InitGameControlFromUI dereferenced it.
     *
     *   FrameworkElement::OnPostInit has the right signature but an ADRP inside
     *   its first four instructions. Relocating that into a trampoline changes
     *   the address it computes, and the result was heap corruption inside
     *   DependencyObject::Init -- a free of a pointer that was never allocated.
     *
     * Both are hooks into the middle of UI construction, where being slightly
     * wrong corrupts rather than fails. The tree helpers below are safe to call
     * on an element obtained some other way, so they stay; discovering a root
     * needs a route that reads rather than intercepts, and that is not written
     * yet.
     */
    LOG_IMGUI_INFO("Noesis tree helpers resolved; no view root source (see noesis.c)");
    return true;
}

bool noesis_ready(void) {
    /* Resolve on demand: the canvas does not exist until the game builds it,
     * so asking early must not latch a permanent "no". */
    return s_find_name != NULL && noesis_get_root() != NULL;
}

/* ------------------------------------------------------------------------- */
/* Button clicks                                                             */
/* ------------------------------------------------------------------------- */

/*
 * How MCM's ESC-menu button reaches Lua.
 *
 * Upstream wires the button by replacing its DataContext with a Lua-registered
 * type whose CustomEvent command runs a Lua handler. That needs Noesis type
 * registration from Lua, which we do not have. What we do have is the exported
 * Noesis::BaseButton::OnClick(): every button the game builds -- LSButton
 * included -- funnels through it on the main thread, and its prologue is four
 * plain frame-setup instructions (sub/stp/stp/add; the first ADRP is at +0x14),
 * so Dobby's 16-byte patch relocates nothing PC-relative. The replacement
 * records the button and its XAML Name, then calls the original, so the sound
 * and the game's own command still run. The game's command for MCM's button is
 * a CustomEvent named "OpenModMenuConfig" that its state machine does not know,
 * so it is dropped there -- harmless. Lua sees the click on the next tick, when
 * lua_ui drains the queue under the gate.
 *
 * GetName is called here, on the main thread inside Noesis' own input
 * dispatch, which is where noesis.h says it is safe.
 */
#define SYM_BUTTON_ONCLICK "_ZN6Noesis10BaseButton7OnClickEv"
#define NOESIS_CLICK_QUEUE_MAX 32

typedef void (*ButtonOnClickFn)(void *button);

static ButtonOnClickFn s_orig_button_onclick = NULL;
static pthread_mutex_t s_click_mutex = PTHREAD_MUTEX_INITIALIZER;
static NoesisClick s_click_queue[NOESIS_CLICK_QUEUE_MAX];
static int s_click_head = 0, s_click_count = 0;
static bool s_click_hook_installed = false;

static void hooked_button_onclick(void *button) {
    NoesisClick click;
    click.element = button;
    click.name[0] = '\0';
    if (button && s_get_name) {
        const char *nm = s_get_name(button);
        if (nm && nm[0]) {
            strncpy(click.name, nm, sizeof click.name - 1);
            click.name[sizeof click.name - 1] = '\0';
        }
    }

    pthread_mutex_lock(&s_click_mutex);
    if (s_click_count < NOESIS_CLICK_QUEUE_MAX) {
        s_click_queue[(s_click_head + s_click_count) % NOESIS_CLICK_QUEUE_MAX] = click;
        s_click_count++;
    }
    pthread_mutex_unlock(&s_click_mutex);

    if (s_orig_button_onclick) s_orig_button_onclick(button);
}

bool noesis_install_click_hook(void) {
    if (s_click_hook_installed) return true;

    void *self = dlopen(NULL, RTLD_NOW);
    void *target = self ? dlsym(self, SYM_BUTTON_ONCLICK) : NULL;
    if (!target) {
        LOG_IMGUI_WARN("Noesis: %s not exported — UI button clicks will not reach Lua",
                       SYM_BUTTON_ONCLICK);
        return false;
    }
    if (!s_get_name) {
        void *nm = dlsym(self, SYM_GET_NAME);
        s_get_name = (GetNameFn)nm;
    }
    if (arm64_has_prologue_adrp(target)) {
        /* Not expected on this build (see the comment above); refuse rather
         * than relocate a PC-relative instruction, which is exactly what
         * corrupted the OnPostInit attempt. */
        LOG_IMGUI_WARN("Noesis: BaseButton::OnClick prologue has an ADRP on this build — "
                       "click hook skipped");
        return false;
    }

    void *orig = NULL;
    int r = DobbyHook(target, (void *)hooked_button_onclick, &orig);
    if (r != 0 || !orig) {
        LOG_IMGUI_WARN("Noesis: DobbyHook(BaseButton::OnClick) failed (%d)", r);
        return false;
    }
    s_orig_button_onclick = (ButtonOnClickFn)orig;
    s_click_hook_installed = true;
    LOG_IMGUI_INFO("Noesis: BaseButton::OnClick hooked at %p (UI button clicks -> Lua)", target);
    return true;
}

int noesis_drain_clicks(NoesisClick *out, int max) {
    int n = 0;
    pthread_mutex_lock(&s_click_mutex);
    while (n < max && s_click_count > 0) {
        out[n++] = s_click_queue[s_click_head];
        s_click_head = (s_click_head + 1) % NOESIS_CLICK_QUEUE_MAX;
        s_click_count--;
    }
    pthread_mutex_unlock(&s_click_mutex);
    return n;
}

int noesis_root_count(void) { return s_root_count; }

void *noesis_root_at(int index) {
    if (index < 0 || index >= s_root_count) return NULL;
    return s_roots[index];
}


static uintptr_t s_image_lo = 0, s_image_hi = 0;

static void image_bounds(void);

/*
 * Locating the view root.
 *
 * Norbyte reads it rather than intercepting anything:
 *
 *     (*ls__gGlobalResourceManager)->UIManager->field_88.Canvas
 *
 * The shape holds here, the offsets do not. On this build the object hanging
 * off the resource manager is gui::GameUI, not a ui::UIManager, and the canvas
 * sits directly in it:
 *
 *     ls::ResourceManager::m_ptr -> +0x128 (gui::GameUI) -> +0xe0 (ui::Canvas)
 *
 * Both offsets were found by walking live memory and identifying each object by
 * its vtable, not taken from upstream's headers -- those describe a different
 * build and have been wrong about this one repeatedly.
 *
 * The result is checked against the ui::Canvas vtable before being handed out,
 * so a layout change after a game update yields nothing rather than a wrong
 * pointer that something later calls into.
 */
#define RM_GAMEUI_OFFSET      0x128
#define GAMEUI_CANVAS_OFFSET  0xe0
#define UI_CANVAS_VTABLE_RVA  0x87bc830   /* vtable for ui::Canvas, past the ABI header */

static void *resolve_canvas_root(void) {
    void *rm = resource_manager_get();
    if (!rm) return NULL;

    void *game_ui = NULL;
    if (!safe_memory_read_pointer((mach_vm_address_t)((uint8_t *)rm + RM_GAMEUI_OFFSET),
                                  &game_ui) || !game_ui) {
        return NULL;
    }

    void *canvas = NULL;
    if (!safe_memory_read_pointer((mach_vm_address_t)((uint8_t *)game_ui + GAMEUI_CANVAS_OFFSET),
                                  &canvas) || !canvas) {
        return NULL;
    }

    void *vtable = NULL;
    if (!safe_memory_read_pointer((mach_vm_address_t)canvas, &vtable) || !vtable) return NULL;

    image_bounds();
    if (s_image_lo && (uintptr_t)vtable - s_image_lo != UI_CANVAS_VTABLE_RVA) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            LOG_IMGUI_WARN("Noesis: object at ResourceManager+0x%x/+0x%x is not a "
                           "ui::Canvas (vtable rva 0x%lx, expected 0x%x) — layout "
                           "changed; Ext.UI has no root",
                           RM_GAMEUI_OFFSET, GAMEUI_CANVAS_OFFSET,
                           (unsigned long)((uintptr_t)vtable - s_image_lo),
                           UI_CANVAS_VTABLE_RVA);
        }
        return NULL;
    }
    return canvas;
}

void *noesis_get_root(void) {
    if (s_root_count > 0) return s_roots[s_root_count - 1];

    void *canvas = resolve_canvas_root();
    if (canvas) {
        noesis_register_root(canvas);
        return canvas;
    }
    return NULL;
}

/*
 * Is this plausibly a live Noesis object?
 *
 * "Has a non-null first word" was not enough. Calling GetChildrenCount on such
 * a pointer dereferences its vtable and calls through it, so a wrong guess is
 * not a failed read -- it is a call to an arbitrary address. A candidate now has
 * to carry a vtable pointer that lands inside the main image, which is where
 * every real Noesis vtable lives, and the slot being called has to be readable.
 */
static void image_bounds(void) {
    if (s_image_hi) return;
    /* NOT _dyld_get_image_header(0): since 2026-08-29 Steam's overlay loads
     * ahead of the executable, which sits at index 1, and every vtable RVA
     * computed against index 0 came out negative. version_detect holds the
     * header main.c found by name. */
    const struct mach_header_64 *hdr =
        (const struct mach_header_64 *)version_detect_get_binary_base();
    if (!hdr) return;
    unsigned long size = 0;
    uint8_t *text = getsegmentdata((void *)hdr, "__TEXT", &size);
    if (!text || !size) return;
    s_image_lo = (uintptr_t)hdr;
    s_image_hi = (uintptr_t)text + size + (64u << 20);   /* __TEXT plus the data segments after it */
}

/*
 * Resolve GetClassType's vtable slot once, by scanning BaseObject's vtable for
 * the address dlsym gives for BaseObject::GetClassType.
 *
 * The slot is NOT hardcoded: __DATA_CONST uses chained fixups, so the raw file
 * bytes are encoded fixup entries rather than final addresses and a static dump
 * reads several slots as zero. At runtime the fixups are applied and the scan
 * is exact. Per the Itanium ABI an object's vptr points 0x10 past the `vtable
 * for X` symbol (offset-to-top + typeinfo), so the scan starts there and the
 * recovered index is usable directly against an object's vptr.
 */
/*
 * Run a Noesis call on the MAIN thread, where the engine expects it.
 *
 * Why this exists (2026-08-28): two silent, report-less process deaths shared
 * one shape -- Noesis type registration forced from the wrong thread. Calling
 * StaticGetClassType during our init killed every boot ~15s in
 * (NsRegisterReflectionCoreTypeConverter -> IdOf -> strlen(NULL)), and calling
 * GetClassType on ui::Canvas from the console thread killed a loaded session
 * mid-probe with nothing in either crash reporter. GetClassType lazily
 * REGISTERS the type if it has never been materialized, and registration is
 * only safe where Noesis runs -- the main thread. Windows BG3SE never faces
 * this because its Lua executes on the game thread by construction.
 *
 * dispatch_async + semaphore rather than dispatch_sync, with a timeout that
 * FAILS CLOSED: if the main thread is blocked on the Lua gate that our caller
 * holds (console eval), a dispatch_sync would deadlock permanently. With the
 * timeout the console call returns failure, the gate is released, and the main
 * thread proceeds -- degraded but recoverable.
 */
#define NOESIS_UI_CALL_TIMEOUT_MS 400

/*
 * Heap box shared by the waiting thread and the dispatched block. Plain-C
 * blocks do NOT retain captured dispatch objects, and __block stack storage
 * dies with its frame, so everything either side may touch after a timeout
 * lives on the heap, refcounted: each side decrements once, the side reaching
 * zero frees the box, the semaphore, and -- unless ownership was transferred
 * to the caller -- the payload.
 */
typedef struct {
    void (*fn)(void *payload);
    void *payload;
    bool free_payload;           /* cleared when the caller takes ownership */
    dispatch_semaphore_t sem;
    _Atomic int refs;
} UiCallBox;

static void ui_call_box_release(UiCallBox *box) {
    if (atomic_fetch_sub_explicit(&box->refs, 1, memory_order_acq_rel) == 1) {
        if (box->free_payload) free(box->payload);
        dispatch_release(box->sem);
        free(box);
    }
}

/*
 * Run fn(payload) on the MAIN thread and return the payload for the caller to
 * read and free -- or NULL on timeout, after which the payload is untouchable
 * (the block may still run and write to it later; the box frees it).
 *
 * Why this exists (2026-08-28): two silent, report-less process deaths shared
 * one shape -- Noesis type registration forced from the wrong thread.
 * StaticGetClassType called during init killed every boot ~15s in
 * (NsRegisterReflectionCoreTypeConverter -> IdOf -> strlen(NULL)), and
 * GetClassType on ui::Canvas from the console thread killed a loaded session
 * mid-probe with nothing in either crash reporter. GetClassType lazily
 * REGISTERS a type that has never been materialized, and registration is only
 * safe where Noesis runs. Windows BG3SE never faces this: its Lua executes on
 * the game thread by construction.
 *
 * The timeout FAILS CLOSED on the known deadlock shape: a console eval holds
 * the Lua gate; if the main thread is blocked acquiring that gate, our block
 * never runs; the wait times out, the caller returns failure and releases the
 * gate, and the main thread proceeds. Degraded but recoverable.
 */
static void *noesis_ui_call(void (*fn)(void *), void *heap_payload) {
    if (pthread_main_np()) {
        fn(heap_payload);
        return heap_payload;                    /* caller reads + frees */
    }

    UiCallBox *box = (UiCallBox *)malloc(sizeof *box);
    if (!box) { free(heap_payload); return NULL; }
    box->fn = fn;
    box->payload = heap_payload;
    box->free_payload = true;
    box->sem = dispatch_semaphore_create(0);
    atomic_init(&box->refs, 2);

    dispatch_async(dispatch_get_main_queue(), ^{
        box->fn(box->payload);                  /* payload untouched after this */
        dispatch_semaphore_signal(box->sem);    /* box alive: we hold a ref */
        ui_call_box_release(box);
    });

    long timed_out = dispatch_semaphore_wait(
        box->sem, dispatch_time(DISPATCH_TIME_NOW,
                                (int64_t)NOESIS_UI_CALL_TIMEOUT_MS * 1000000));
    if (timed_out) {
        LOG_IMGUI_WARN("Noesis: main-thread call timed out (%dms) — failing "
                       "closed (main thread likely blocked on the Lua gate "
                       "this caller holds)", NOESIS_UI_CALL_TIMEOUT_MS);
        ui_call_box_release(box);
        return NULL;
    }
    /* Success: the block is past its last payload touch (it signalled after
     * fn). Take ownership before dropping our ref; release-ordering on the
     * refcount makes the cleared flag visible to whichever side frees. */
    box->free_payload = false;
    ui_call_box_release(box);
    return heap_payload;
}

/*
 * All engine-touching operations funnel through one main-thread handler.
 * Every input and output lives in this heap payload; strings returned by the
 * engine (SymbolManager interns, Name buffers) outlive the call.
 */
typedef enum {
    NSOP_CLASSIFY,      /* obj -> type_name, is_fe (may lazily register types) */
    NSOP_ELEMENT_NAME,  /* obj -> str_out (FrameworkElement::GetName)          */
    NSOP_FIND_NAME,     /* obj + name_in -> ptr_out                            */
    NSOP_LOG_COUNT,     /* obj -> int_out                                      */
    NSOP_LOG_CHILD,     /* obj + idx -> ptr_out                                */
} NsOpKind;

typedef struct {
    NsOpKind kind;
    void *obj;
    const char *name_in;
    unsigned idx;
    const char *str_out;
    void *ptr_out;
    int int_out;
    bool is_fe;
} NsOp;

/* MAIN THREAD ONLY. The one place allowed to call GetClassType / Cast /
 * FrameworkElement methods, because type registration is safe here. */
static void nsop_run(void *payload) {
    NsOp *op = (NsOp *)payload;
    void *obj = op->obj;

    if (!visual_is_plausible(obj)) return;

    /* Lazy FrameworkElement TypeClass -- first use is on the main thread by
     * construction now, which is the entire point. */
    if (!s_fe_type && s_fe_type_fn) s_fe_type = s_fe_type_fn(NULL);

    /* Engine-checked downcast; NULL when obj is not a FrameworkElement.
     * Includes the ancestor this-pointer adjustment Noesis::Cast applies. */
    void *fe = (s_cast && s_fe_type) ? s_cast(s_fe_type, obj) : NULL;
    op->is_fe = (fe != NULL);

    switch (op->kind) {
    case NSOP_CLASSIFY: {
        if (s_classtype_slot < 0 || !s_symbol_str) return;
        void *vptr = NULL;
        if (!safe_memory_read_pointer((mach_vm_address_t)obj, &vptr) || !vptr) return;
        void *fn = NULL;
        if (!safe_memory_read_pointer(
                (mach_vm_address_t)((void **)vptr + s_classtype_slot), &fn) || !fn) return;
        if (s_image_hi && ((uintptr_t)fn < s_image_lo || (uintptr_t)fn >= s_image_hi)) return;
        const void *type = ((GetClassTypeFn)fn)(obj);
        if (!type) return;
        uint32_t symbol = 0;
        if (safe_memory_read_u32((mach_vm_address_t)((const uint8_t *)type + 0x8), &symbol)
            && symbol != 0) {
            op->str_out = s_symbol_str(symbol);
        }
        break;
    }
    case NSOP_ELEMENT_NAME:
        if (fe && s_get_name) {
            const char *nm = s_get_name(fe);
            op->str_out = (nm && nm[0]) ? nm : NULL;
        }
        break;
    case NSOP_FIND_NAME:
        if (fe && s_find_name && op->name_in) op->ptr_out = s_find_name(fe, op->name_in);
        break;
    case NSOP_LOG_COUNT:
        if (fe && s_log_count) op->int_out = s_log_count(fe);
        break;
    case NSOP_LOG_CHILD:
        if (fe && s_log_count && s_log_child) {
            int n = s_log_count(fe);
            if ((int)op->idx < n) {
                NsPtrRet r = s_log_child(fe, op->idx);   /* x8 now supplied */
                op->ptr_out = r.p;
                ns_ptr_release_ref(r.p);   /* balance the callee's AddRef */
            }
        }
        break;
    }
}

/* Marshal one op; returns a payload the CALLER frees, or NULL on timeout. */
static NsOp *nsop(NsOpKind kind, void *obj, const char *name_in, unsigned idx) {
    NsOp *op = (NsOp *)calloc(1, sizeof *op);
    if (!op) return NULL;
    op->kind = kind; op->obj = obj; op->name_in = name_in; op->idx = idx;
    return (NsOp *)noesis_ui_call(nsop_run, op);
}

static void resolve_classtype_slot(void *self) {
    if (s_classtype_slot >= 0) return;

    void *vt_sym = dlsym(self, SYM_VT_BASEOBJECT);
    void *fn = dlsym(self, SYM_BO_CLASSTYPE);
    if (!vt_sym || !fn) return;

    void **vptr = (void **)((uint8_t *)vt_sym + 0x10);
    for (int i = 0; i < 24; i++) {
        void *slot = NULL;
        if (!safe_memory_read_pointer((mach_vm_address_t)&vptr[i], &slot)) break;
        if (slot == fn) {
            s_classtype_slot = i;
            LOG_IMGUI_INFO("Noesis: GetClassType at vtable slot %d", i);
            return;
        }
    }
    LOG_IMGUI_WARN("Noesis: GetClassType slot not found — the FrameworkElement "
                   "guard stays off and logical-tree access is refused");
}

static bool visual_is_plausible(const void *visual);


static bool visual_is_plausible(const void *visual) {
    if (!visual) return false;
    image_bounds();

    void *vtable = NULL;
    if (!safe_memory_read_pointer((mach_vm_address_t)visual, &vtable)) return false;
    if (!vtable) return false;

    uintptr_t vt = (uintptr_t)vtable;
    if (s_image_hi && (vt < s_image_lo || vt >= s_image_hi)) return false;

    /* The vtable itself must be readable, and its first slot a code address. */
    void *slot0 = NULL;
    if (!safe_memory_read_pointer((mach_vm_address_t)vt, &slot0)) return false;
    uintptr_t s0 = (uintptr_t)slot0;
    return s_image_hi == 0 || (s0 >= s_image_lo && s0 < s_image_hi);
}

void *noesis_find_name(void *element, const char *name) {
    NsOp *op = nsop(NSOP_FIND_NAME, element, name, 0);
    if (!op) return NULL;
    void *r = op->ptr_out;
    free(op);
    return r;
}

int noesis_child_count(void *visual) {
    if (!s_child_count || !visual_is_plausible(visual)) return 0;
    return s_child_count(visual);
}

void *noesis_get_child(void *visual, unsigned int index) {
    if (!s_get_child || !visual_is_plausible(visual)) return NULL;
    if ((int)index >= noesis_child_count(visual)) return NULL;
    return s_get_child(visual, index);
}

const char *noesis_symbol_string(unsigned int symbol) {
    if (!s_symbol_str || symbol == 0) return NULL;
    return s_symbol_str(symbol);
}

/*
 * The Noesis type name of any object, e.g. "Grid", "TextBlock", "ContentPresenter".
 *
 * Upstream (BG3Extender Lua/Libs/ClientUI/NsHelpers.inl) reaches types via
 * o->GetClassType() and checks with TypeHelpers::IsDescendantOf before casting;
 * this is the same reach, done through the vtable slot resolved at init.
 *
 * Noesis::Type::GetName is `ldr w0,[x0,#8]; b SymbolManager::GetString` -- the
 * Symbol id lives at Type+0x8 -- so the id is read directly and resolved with
 * the SymbolManager entry point already held. Works for ANY BaseObject, not
 * just FrameworkElements, which is what makes it useful for telling them apart.
 */
bool noesis_is_framework_element(void *obj) {
    NsOp *op = nsop(NSOP_CLASSIFY, obj, NULL, 0);
    if (!op) return false;              /* timeout: fail closed */
    bool r = op->is_fe;
    free(op);
    return r;
}

const char *noesis_type_name(void *obj) {
    NsOp *op = nsop(NSOP_CLASSIFY, obj, NULL, 0);
    if (!op) return NULL;
    const char *r = op->str_out;        /* interned; outlives the call */
    free(op);
    return r;
}

const char *noesis_element_name(void *element) {
    NsOp *op = nsop(NSOP_ELEMENT_NAME, element, NULL, 0);
    if (!op) return NULL;
    const char *r = op->str_out;
    free(op);
    return r;
}

int noesis_logical_child_count(void *element) {
    NsOp *op = nsop(NSOP_LOG_COUNT, element, NULL, 0);
    if (!op) return 0;
    int r = op->int_out;
    free(op);
    return r;
}

void *noesis_logical_child(void *element, unsigned int index) {
    NsOp *op = nsop(NSOP_LOG_CHILD, element, NULL, index);
    if (!op) return NULL;
    void *r = op->ptr_out;
    free(op);
    return r;
}

void *noesis_root_of(void *visual) {
    if (!s_get_root || !visual_is_plausible(visual)) return NULL;
    return s_get_root(visual);
}
