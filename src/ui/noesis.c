/**
 * noesis.c - Noesis UI bridge. See noesis.h.
 */

#include "noesis.h"

#include "../core/logging.h"
#include "../core/safe_memory.h"
#include "../resource/resource_manager.h"

#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <string.h>

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

typedef void *(*FindNameFn)(const void *element, const char *name);
typedef int (*ChildCountFn)(const void *visual);
typedef void *(*GetChildFn)(const void *visual, unsigned int index);
typedef void *(*GetRootFn)(const void *visual);
typedef const char *(*SymbolStringFn)(unsigned int symbol);
typedef const char *(*GetNameFn)(const void *element);
typedef const void *(*StaticClassTypeFn)(void *tag);
typedef bool (*IsAssignableFromFn)(const void *typeClass, const void *other);
typedef const void *(*GetClassTypeFn)(const void *obj);

static FindNameFn s_find_name = NULL;
static ChildCountFn s_child_count = NULL;
static GetChildFn s_get_child = NULL;
static GetRootFn s_get_root = NULL;
static ChildCountFn s_log_count = NULL;
static GetChildFn s_log_child = NULL;
static SymbolStringFn s_symbol_str = NULL;
static GetNameFn s_get_name = NULL;
static const void *s_fe_type = NULL;          /* FrameworkElement's TypeClass */
static IsAssignableFromFn s_is_assignable = NULL;
static int s_classtype_slot = -1;             /* index of GetClassType from an object's vptr */

/* Defined further down, next to the image-bounds helpers they depend on. */
static void resolve_classtype_slot(void *self);
static bool is_framework_element(const void *obj);



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
    s_log_child = (GetChildFn)dlsym(self, SYM_LOG_CHILD);
    s_symbol_str = (SymbolStringFn)dlsym(self, SYM_SYMBOL_STR);
    s_get_name = (GetNameFn)dlsym(self, SYM_GET_NAME);

    /* RTTI guard. Optional: if any piece is missing the guard stays off and
     * FrameworkElement-only accessors refuse rather than guess. */
    StaticClassTypeFn fe_type_fn = (StaticClassTypeFn)dlsym(self, SYM_FE_CLASSTYPE);
    s_is_assignable = (IsAssignableFromFn)dlsym(self, SYM_IS_ASSIGNABLE);
    if (fe_type_fn) s_fe_type = fe_type_fn(NULL);
    resolve_classtype_slot(self);
    if (!s_fe_type || !s_is_assignable || s_classtype_slot < 0) {
        LOG_IMGUI_WARN("Noesis: FrameworkElement RTTI unavailable "
                       "(type=%p assignable=%p slot=%d) — Name and logical-tree "
                       "access will refuse non-verified elements",
                       (void *)s_fe_type, (void *)s_is_assignable, s_classtype_slot);
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
    const struct mach_header_64 *hdr = (const struct mach_header_64 *)_dyld_get_image_header(0);
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

/*
 * True only when `obj` really is a FrameworkElement.
 *
 * This exists because of a crash: LogicalTreeHelper::GetChild takes a
 * FrameworkElement*, but VisualChild() hands back a Visual*, which need not be
 * one. visual_is_plausible only proves the vtable is image-resident, so a
 * non-FE Visual sailed through, GetChild read a bogus children-collection
 * field, and Noesis::BaseCollection::GetComponent faulted at 0x17.
 *
 * Fails CLOSED: if the RTTI plumbing did not resolve, this returns false and
 * callers refuse the operation rather than gambling.
 */
static bool is_framework_element(const void *obj) {
    if (!s_fe_type || !s_is_assignable || s_classtype_slot < 0) return false;
    if (!visual_is_plausible(obj)) return false;

    void *vptr = NULL;
    if (!safe_memory_read_pointer((mach_vm_address_t)obj, &vptr) || !vptr) return false;

    void *fn = NULL;
    if (!safe_memory_read_pointer(
            (mach_vm_address_t)((void **)vptr + s_classtype_slot), &fn) || !fn) {
        return false;
    }
    /* Never call through a slot that is not code in this image. */
    if (s_image_hi && ((uintptr_t)fn < s_image_lo || (uintptr_t)fn >= s_image_hi)) {
        return false;
    }

    const void *type = ((GetClassTypeFn)fn)(obj);
    if (!type) return false;
    return s_is_assignable(s_fe_type, type);
}

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
    if (!s_find_name || !name || !is_framework_element(element)) return NULL;
    return s_find_name(element, name);
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

const char *noesis_element_name(void *element) {
    /* GetName is a non-virtual FrameworkElement method; same hazard as the
     * logical tree if handed a plain Visual*. */
    if (!s_get_name || !is_framework_element(element)) return NULL;

    const char *name = s_get_name(element);
    // An unnamed element yields the empty string, not NULL; report both as nil
    // so callers need only one check.
    return (name && name[0]) ? name : NULL;
}

int noesis_logical_child_count(void *element) {
    /* LogicalTreeHelper takes a FrameworkElement*. Passing a plain Visual*
     * here is what crashed the game on 2026-08-27 (BaseCollection::GetComponent
     * faulted at 0x17 reading a bogus children collection). */
    if (!s_log_count || !is_framework_element(element)) return 0;
    return s_log_count(element);
}

void *noesis_logical_child(void *element, unsigned int index) {
    if (!s_log_child || !is_framework_element(element)) return NULL;
    if ((int)index >= noesis_logical_child_count(element)) return NULL;
    return s_log_child(element, index);
}

void *noesis_root_of(void *visual) {
    if (!s_get_root || !visual_is_plausible(visual)) return NULL;
    return s_get_root(visual);
}
