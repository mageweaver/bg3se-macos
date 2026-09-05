/**
 * lua_ui.h - Lua Bindings for Ext.UI (Noesis)
 *
 * Ext.UI.GetRoot() hands out live Noesis elements (see src/ui/noesis.c).
 * Ext.UI.RegisterType / Instantiate and element.DataContext are emulated on
 * the Lua side: the DataContext is remembered per element, and a click on
 * that element -- observed by noesis.c's BaseButton::OnClick hook -- runs the
 * ctx's command handlers. That is how MCM's ESC-menu button opens MCM here.
 */

#ifndef LUA_UI_H
#define LUA_UI_H

#include "../../lib/lua/src/lua.h"

/**
 * Register Ext.UI namespace functions.
 * @param L Lua state
 * @param ext_table_idx Index of the Ext table on the stack
 */
void lua_ext_register_ui(lua_State *L, int ext_table_idx);

/**
 * Deliver queued Noesis button clicks to the DataContext handlers Lua
 * installed. Call once per tick with the Lua gate held.
 */
void lua_ui_process_clicks(lua_State *L);

#endif // LUA_UI_H
