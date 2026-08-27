/**
 * BG3SE-macOS - Noesis UI bridge
 *
 * The macOS build links Noesis: the binary carries 82,000-odd Noesis symbols,
 * including the visual-tree helpers as exported functions. Ext.UI was
 * nonetheless a stub whose header asserted "macOS BG3 does not use Noesis",
 * which was simply wrong, and MCM's ESC-menu integration failed against it with
 * "ContentRoot not found".
 *
 * THREADING. Noesis is driven by the game's own threads. Our Lua service tick
 * runs on a GCD queue, not the game's main thread, so calls made from there --
 * the console, and Ext.Timer callbacks -- race the UI. That is not theoretical:
 * every attempt to walk the tree from the console has ended with the game gone,
 * usually with no crash report, which is what a torn tree looks like from the
 * outside. Reads of plain memory are fine; anything that calls into Noesis is
 * not.
 *
 * Mod code invoked from fake_Event runs on a game thread and is the safe
 * caller. Whether Ext.Timer callbacks are safe depends on which tick delivered
 * them, and that is worth settling before anything relies on it.
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
 * Noesis::SymbolManager::GetString -- names are interned Symbol ids, not char*,
 * so reading a Name means resolving the id stored in the element.
 */
const char *noesis_symbol_string(unsigned int symbol);

/** The element's Name, or NULL. */
const char *noesis_element_name(void *element);

/** Where the Name symbol sits in a FrameworkElement; -1 until discovered. */
void noesis_set_name_offset(int offset);
int noesis_get_name_offset(void);

/** Noesis::LogicalTreeHelper -- the logical tree, which is what mods walk. */
int noesis_logical_child_count(void *element);
void *noesis_logical_child(void *element, unsigned int index);

/** Noesis::VisualTreeHelper::GetRoot -- the root above any visual. */
void *noesis_root_of(void *visual);

#endif // BG3SE_NOESIS_H
