/**
 * BG3SE-macOS - Noesis UI bridge
 *
 * The macOS build links Noesis: the binary carries 82,000-odd Noesis symbols,
 * including the visual-tree helpers as exported functions. Ext.UI was
 * nonetheless a stub whose header asserted "macOS BG3 does not use Noesis",
 * which was simply wrong, and MCM's ESC-menu integration failed against it with
 * "ContentRoot not found".
 *
 * The one thing not exported is a way to reach the root. Noesis::GUI::CreateView
 * is handed the root FrameworkElement of every view the game builds, so hooking
 * it captures them as they appear -- no struct layout to reverse, and it keeps
 * working across game updates as long as the symbol survives.
 */

#ifndef BG3SE_NOESIS_H
#define BG3SE_NOESIS_H

#include <stdbool.h>
#include <stdint.h>

/** Resolve the Noesis exports and hook view creation. Safe to call twice. */
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

/** Noesis::VisualTreeHelper::GetRoot -- the root above any visual. */
void *noesis_root_of(void *visual);

#endif // BG3SE_NOESIS_H
