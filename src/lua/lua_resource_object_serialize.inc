/**
 * Ext.Types.Serialize / Unserialize support for resource-object proxies.
 *
 * Included by lua_resource_object.c so it can see ResourceObjectUD and
 * RESOURCE_OBJECT_MT.
 *
 * Root templates (esv::Character::Template and friends) are pushed as these
 * proxies, not as component proxies, so Ext.Types.Serialize refused them:
 *
 *   Ext.Types.Serialize: unsupported value type 'userdata'
 *   (expected component or component-array userdata)
 *
 * That is not an edge case. Mods clone templates through
 * Serialize -> Unserialize inside an xpcall and discard the error, so the clone
 * silently does nothing: AppearanceEditEnhanced's Main.lua:217 copies a
 * character's template during a resculpt this way and it has never once run on
 * this port.
 *
 * Both directions go through the proxy's own __index / __newindex rather than
 * touching memory directly, so every conversion, bounds check and read-only
 * rule the resource layer already enforces applies unchanged.
 *
 * Scope: scalar fields only -- strings (GUID, FixedString, STDString,
 * TranslatedString handles), numbers and booleans. Arrays and nested structs
 * come back from __index as their own userdata and are skipped rather than
 * flattened, because writing them back needs ownership operations this layer
 * cannot perform. The component path skips ownership-bearing fields for the
 * same reason.
 *
 * A field the target refuses raises out of __newindex, as it does for
 * components. Callers already expect that: upstream's TryToReserializeObject
 * wraps the pair in xpcall.
 */

/* True for values we can round-trip: everything __index hands back that is not
 * a live proxy. */
static bool resource_serializable_value(lua_State *L, int index) {
    switch (lua_type(L, index)) {
        case LUA_TSTRING:
        case LUA_TNUMBER:
        case LUA_TBOOLEAN:
            return true;
        default:
            return false;
    }
}

/* True when this proxy IS the global template bank's own object, i.e. shared by
 * every character built from it, rather than a per-instance copy.
 *
 * Identified by layout first -- only the template layouts describe a
 * GameObjectTemplate, so a static-data resource is never misread as one -- then
 * by looking the template's own GUID up in the bank and comparing pointers.
 * A per-instance copy carries the same GUID but a different address, so it
 * passes. */
static bool resource_object_is_shared_root_template(const ResourceLayout *layout,
                                                    void *obj) {
    if (!obj || !layout) return false;
    if (layout != &g_template_layout_base && layout != &g_template_layout_character) {
        return false;
    }

    char guid[40];
    if (!template_get_guid_string((GameObjectTemplate *)obj, guid, sizeof(guid))) {
        return false;
    }

    GameObjectTemplate *banked =
        template_get_by_guid(TEMPLATE_MANAGER_GLOBAL_BANK, guid);
    return (void *)banked == obj;
}

bool lua_resource_object_serialize(lua_State *L, int index) {
    int abs = lua_absindex(L, index);
    ResourceObjectUD *ud =
        (ResourceObjectUD *)luaL_testudata(L, abs, RESOURCE_OBJECT_MT);
    if (!ud || !ud->layout) return false;

    lua_createtable(L, 0, ud->layout->field_count);
    int table = lua_gettop(L);

    for (int i = 0; i < ud->layout->field_count; i++) {
        const char *name = ud->layout->fields[i].name;
        if (!name) continue;

        /* Through __index: a field this build does not expose yields nil,
         * exactly as a direct read would. */
        lua_getfield(L, abs, name);
        if (resource_serializable_value(L, -1)) {
            lua_setfield(L, table, name);   /* pops the value */
        } else {
            lua_pop(L, 1);
        }
    }

    return true;
}

bool lua_resource_object_unserialize(lua_State *L, int objIndex, int tableIndex) {
    int absObj = lua_absindex(L, objIndex);
    int absTable = lua_absindex(L, tableIndex);

    ResourceObjectUD *ud =
        (ResourceObjectUD *)luaL_testudata(L, absObj, RESOURCE_OBJECT_MT);
    if (!ud || !ud->layout) return false;

    luaL_checktype(L, absTable, LUA_TTABLE);

    /* Refuse a SHARED root template.
     *
     * entity.ServerCharacter.Template is the template bank's object, not a
     * per-character copy, so writing it changes every character using that
     * template for the rest of the process. Demonstrated live on 2026-09-13:
     * unserializing one origin's template onto another's left BOTH characters
     * (and Ext.Template.GetRootTemplate for both GUIDs) reporting the same
     * template, and only a full relaunch restored the bank.
     *
     * Upstream permits this write, so refusing it is a deliberate divergence.
     * It is worth it: the damage is process-wide, silent, and survives a save
     * reload. Serializing is unaffected -- reads cannot corrupt anything -- and
     * per-instance templates (what a character-creation copy carries) are not
     * in the bank, so cloning those still works, which is what mods actually
     * need. */
    if (resource_object_is_shared_root_template(ud->layout, ud->obj)) {
        return luaL_error(L,
            "Ext.Types.Unserialize: refusing to write a shared root template; "
            "it is the template bank's own object and the change would affect "
            "every character using it for the rest of the session");
    }

    for (int i = 0; i < ud->layout->field_count; i++) {
        const char *name = ud->layout->fields[i].name;
        if (!name) continue;
        /* Upstream's Unserialize skips read-only properties instead of
         * failing the whole object (AEE clones whole templates this way). */
        if (ud->layout->fields[i].readonly) continue;

        lua_getfield(L, absTable, name);
        if (!resource_serializable_value(L, -1)) {
            lua_pop(L, 1);
            continue;
        }

        /* Through __newindex, so read-only and unsupported fields are refused
         * there rather than written behind the layer's back. */
        lua_setfield(L, absObj, name);      /* pops the value */
    }

    return true;
}
