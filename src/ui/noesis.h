/**
 * BG3SE-macOS - Noesis UI bridge
 *
 * The macOS build links Noesis: the binary carries 82,000-odd Noesis symbols,
 * including the visual-tree helpers as exported functions. Ext.UI was
 * nonetheless a stub whose header asserted "macOS BG3 does not use Noesis",
 * which was simply wrong, and MCM's ESC-menu integration failed against it with
 * "ContentRoot not found".
 *
 * THREADING. An earlier version of this header claimed Noesis could only be
 * touched from a game thread, on the evidence that console tree walks kept
 * ending with the game gone. That was measured and is wrong. From the console:
 * GetRoot, child counts, and an 18-node recursive walk all survive; what dies
 * is a scan making thousands of calls in one tick while holding the Lua gate,
 * and it dies by SIGTERM with no crash report -- an unresponsive app being
 * killed, not a torn tree. The real rule is a budget, not an affinity: keep
 * per-tick work small. Whether a genuine affinity constraint also exists is
 * still unproven in either direction, so treat concurrent mutation as unsafe.
 *
 * REACHING THE ROOT is a read, not a hook. The ResourceManager holds the
 * gui::GameUI, which holds the ui::Canvas; resolve_canvas_root() walks that
 * chain and validates the vtable. Hooking Noesis::GUI::CreateView was tried
 * first and cost four crashes -- Ptr<View> returns via the indirect-result
 * register, which a void* declaration silently corrupts.
 */

#ifndef BG3SE_NOESIS_H
#define BG3SE_NOESIS_H

#include <stdbool.h>
#include <stdint.h>

/** Register a view root discovered elsewhere. */
void noesis_register_root(void *root);

/** Resolve the Noesis exports. Safe to call twice. */
bool noesis_init(void);

/** True once at least one view root has been captured. */
bool noesis_ready(void);

/**
 * The most recently created view root, or NULL.
 *
 * The game builds several views; the last one created is the one MCM's ESC-menu
 * lookup is interested in, and callers that want a different one can walk from
 * here.
 */
void *noesis_get_root(void);

/** Number of captured view roots, and the root at `index`. */
int noesis_root_count(void);
void *noesis_root_at(int index);

/** Noesis::FrameworkElement::FindName -- a named element beneath `element`. */
void *noesis_find_name(void *element, const char *name);

/** Noesis::VisualTreeHelper::GetChildrenCount / GetChild. */
int noesis_child_count(void *visual);
void *noesis_get_child(void *visual, unsigned int index);

/**
 * Noesis::SymbolManager::GetString -- resolves an interned Symbol id to text.
 * Not needed for Name (see below); kept for callers holding a raw Symbol.
 */
const char *noesis_symbol_string(unsigned int symbol);

/**
 * The element's Name, or NULL when unnamed.
 *
 * Calls the exported Noesis::FrameworkElement::GetName, which does the
 * NameProperty lookup internally and returns a plain const char* in x0. There
 * is no struct offset to discover here -- an earlier attempt to reverse one by
 * scanning live elements was wasted effort that predated finding the export.
 */
const char *noesis_element_name(void *element);

/** Noesis::LogicalTreeHelper -- the logical tree, which is what mods walk. */
int noesis_logical_child_count(void *element);
void *noesis_logical_child(void *element, unsigned int index);

/** Noesis::VisualTreeHelper::GetRoot -- the root above any visual. */
void *noesis_root_of(void *visual);

#endif // BG3SE_NOESIS_H
