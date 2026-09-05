/**
 * BG3SE-macOS - Root template field layouts
 *
 * Field tables for the GameObjectTemplate hierarchy so Ext.Template proxies
 * can read and write template properties in place, the way upstream's
 * Ext.Template.GetRootTemplate(id).CharacterVisualResourceID = x does.
 *
 * Offsets are NOT taken from upstream's RootTemplates.h field order (its
 * STDString and container sizes differ from this build); each one was read out
 * of the template's own Visit() in the 7398727 arm64 binary, which pairs every
 * field address with its EoCFS::str<Name> FixedString. See template_layouts.c.
 */

#ifndef BG3SE_TEMPLATE_LAYOUTS_H
#define BG3SE_TEMPLATE_LAYOUTS_H

#include "../staticdata/staticdata_fields.h"
#include "template_manager.h"

/** Fields shared by every template (ls::GameObjectTemplate). */
extern const ResourceLayout g_template_layout_base;

/** eoc::CharacterTemplate: base fields plus the character-only ones. */
extern const ResourceLayout g_template_layout_character;

/** The most specific layout known for a template's type. Never NULL. */
const ResourceLayout *template_layout_for(GameObjectTemplate *tmpl);

#endif /* BG3SE_TEMPLATE_LAYOUTS_H */
