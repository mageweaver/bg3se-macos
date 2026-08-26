/**
 * BG3SE-macOS - GuidResource field layout lookup
 */

#include "staticdata_fields.h"

#include <string.h>

const ResourceLayout *resource_layout_find(const char *type_name) {
    if (!type_name) return NULL;
    for (int i = 0; i < g_resource_layout_count; i++) {
        if (strcmp(g_resource_layouts[i].type_name, type_name) == 0) {
            return &g_resource_layouts[i];
        }
    }
    return NULL;
}

const ResourceField *resource_layout_field(const ResourceLayout *layout, const char *name) {
    if (!layout || !name) return NULL;
    for (int i = 0; i < layout->field_count; i++) {
        if (strcmp(layout->fields[i].name, name) == 0) {
            return &layout->fields[i];
        }
    }
    return NULL;
}

bool resource_field_is_array(ResourceFieldKind kind) {
    switch (kind) {
        case RF_ARRAY_GUID:
        case RF_ARRAY_FIXEDSTRING:
        case RF_ARRAY_I32:
        case RF_ARRAY_U8:
            return true;
        default:
            return false;
    }
}
