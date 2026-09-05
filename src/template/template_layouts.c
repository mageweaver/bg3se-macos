/**
 * BG3SE-macOS - Root template field layouts
 *
 * How each offset was verified (7398727 arm64, scratch disassembly of the
 * thin slice): the engine serialises templates through
 * CharacterTemplate::Visit / EoCGameObjectTemplate::Visit /
 * GameObjectTemplate::Visit, and each property is visited as
 *
 *     add  x0, x<this>, #<offset>          ; &this->Field
 *     adrp/add x2, EoCFS::str<Name>        ; the property's FixedString name
 *     bl   ls::TemplateVariable<T>::VisitNoDefault(...)
 *
 * so the offset and the name sit side by side. TemplateVariable<T> is
 * upstream's OverrideableProperty<T>: the value first, then an IsOverridden
 * byte, which is why FixedString properties are 8 apart and Race (a Guid)
 * carries its flag at +0x538.
 *
 * Only fields that were read out of Visit() are listed. Anything else a mod
 * asks for comes back nil rather than a guess.
 */

#include "template_layouts.h"

#define FIELDS(arr) arr, (int)(sizeof(arr) / sizeof(arr[0]))

/* ls::GameObjectTemplate — GameObjectTemplate::Visit (0x105f79790). */
static const ResourceField fields_GameObjectTemplate[] = {
    { "Id",               0x10, RF_FIXEDSTRING, NULL, 0 },  // the root template GUID text
    { "TemplateName",     0x14, RF_FIXEDSTRING, NULL, 0 },
    { "ParentTemplateId", 0x18, RF_FIXEDSTRING, NULL, 0 },  // Visit tests it against -1 for "has parent"
    { "Name",             0x20, RF_STDSTRING,   NULL, 0 },
};

/* eoc::CharacterTemplate — EoCGameObjectTemplate::Visit (0x1012a8ea0) and
 * CharacterTemplate::Visit (0x101216868). */
static const ResourceField fields_CharacterTemplate[] = {
    { "Id",                        0x10,  RF_FIXEDSTRING,      NULL, 0 },
    { "TemplateName",              0x14,  RF_FIXEDSTRING,      NULL, 0 },
    { "ParentTemplateId",          0x18,  RF_FIXEDSTRING,      NULL, 0 },
    { "Name",                      0x20,  RF_STDSTRING,        NULL, 0 },
    { "DisplayName",               0xb8,  RF_TRANSLATEDSTRING, NULL, 0 },  // EoCGameObjectTemplate
    { "Icon",                      0x198, RF_FIXEDSTRING,      NULL, 0 },
    { "Stats",                     0x1a0, RF_FIXEDSTRING,      NULL, 0 },
    { "SpellSet",                  0x1a8, RF_FIXEDSTRING,      NULL, 0 },
    { "Equipment",                 0x1b0, RF_FIXEDSTRING,      NULL, 0 },
    { "DefaultDialog",             0x210, RF_FIXEDSTRING,      NULL, 0 },
    { "GeneratePortrait",          0x220, RF_STDSTRING,        NULL, 0 },
    { "SoundInitEvent",            0x260, RF_FIXEDSTRING,      NULL, 0 },
    { "ExplodedResourceID",        0x320, RF_FIXEDSTRING,      NULL, 0 },
    { "ExplosionFX",               0x328, RF_FIXEDSTRING,      NULL, 0 },
    { "AnubisConfigName",          0x340, RF_FIXEDSTRING,      NULL, 0 },
    { "CharacterVisualResourceID", 0x4b8, RF_FIXEDSTRING,      NULL, 0 },
    { "ActivationGroupId",         0x4cc, RF_FIXEDSTRING,      NULL, 0 },
    { "Race",                      0x528, RF_GUID,             NULL, 0 },  // IsOverridden byte at 0x538
    { "Title",                     0x540, RF_TRANSLATEDSTRING, NULL, 0 },
};

const ResourceLayout g_template_layout_base = {
    "GameObjectTemplate", FIELDS(fields_GameObjectTemplate), 0x40, true
};

const ResourceLayout g_template_layout_character = {
    "CharacterTemplate", FIELDS(fields_CharacterTemplate), 0x700, true
};

const ResourceLayout *template_layout_for(GameObjectTemplate *tmpl) {
    if (tmpl && template_get_type(tmpl) == TEMPLATE_TYPE_CHARACTER) {
        return &g_template_layout_character;
    }
    return &g_template_layout_base;
}
