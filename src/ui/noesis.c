/**
 * noesis.c - Noesis UI bridge. See noesis.h.
 */

#include "noesis.h"

#include "../core/logging.h"
#include "../core/safe_memory.h"

#include <dlfcn.h>
#include <string.h>

// Exported Noesis entry points. All are `T` symbols in the main binary, so
// dlsym finds them without any address table to keep in step with game updates.
#define SYM_FIND_NAME     "_ZNK6Noesis16FrameworkElement8FindNameEPKc"
#define SYM_CHILD_COUNT   "_ZN6Noesis16VisualTreeHelper16GetChildrenCountEPKNS_6VisualE"
#define SYM_GET_CHILD     "_ZN6Noesis16VisualTreeHelper8GetChildEPKNS_6VisualEj"
#define SYM_GET_ROOT      "_ZN6Noesis16VisualTreeHelper7GetRootEPKNS_6VisualE"

typedef void *(*FindNameFn)(const void *element, const char *name);
typedef int (*ChildCountFn)(const void *visual);
typedef void *(*GetChildFn)(const void *visual, unsigned int index);
typedef void *(*GetRootFn)(const void *visual);

static FindNameFn s_find_name = NULL;
static ChildCountFn s_child_count = NULL;
static GetChildFn s_get_child = NULL;
static GetRootFn s_get_root = NULL;


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
    return s_find_name != NULL && s_root_count > 0;
}

int noesis_root_count(void) { return s_root_count; }

void *noesis_root_at(int index) {
    if (index < 0 || index >= s_root_count) return NULL;
    return s_roots[index];
}

void *noesis_get_root(void) {
    return s_root_count > 0 ? s_roots[s_root_count - 1] : NULL;
}

/* A visual is a live object with a vtable; a freed one usually is not readable. */
static bool visual_is_plausible(const void *visual) {
    if (!visual) return false;
    void *vtable = NULL;
    if (!safe_memory_read_pointer((mach_vm_address_t)visual, &vtable)) return false;
    return vtable != NULL;
}

void *noesis_find_name(void *element, const char *name) {
    if (!s_find_name || !name || !visual_is_plausible(element)) return NULL;
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

void *noesis_root_of(void *visual) {
    if (!s_get_root || !visual_is_plausible(visual)) return NULL;
    return s_get_root(visual);
}
