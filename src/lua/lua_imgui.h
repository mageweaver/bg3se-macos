/**
 * @file lua_imgui.h
 * @brief Lua bindings for Ext.IMGUI namespace
 *
 * Provides ImGui overlay functionality to Lua mods.
 */

#ifndef LUA_IMGUI_H
#define LUA_IMGUI_H

#include "lua.h"
#include "../imgui/imgui_objects.h"

/**
 * Register Ext.IMGUI namespace with all functions.
 *
 * @param L Lua state
 * @param ext_idx Stack index of the Ext table
 */
void lua_imgui_register(lua_State *L, int ext_idx);

/**
 * Fire an IMGUI event callback.
 *
 * @param handle Object handle
 * @param event Event type
 * @param ... Event-specific arguments (depends on event type)
 */
void lua_imgui_fire_event(ImguiHandle handle, ImguiEventType event, ...);

// Drain queued IMGUI event callbacks on the main thread (they are enqueued from
// the render thread by lua_imgui_fire_event). Call once per game tick.
void lua_imgui_process_events(lua_State *L);

/**
 * Clean up Lua references for an IMGUI object before destruction.
 * Must be called before imgui_object_destroy() to prevent memory leaks.
 *
 * @param handle Object handle to clean up
 */
void lua_imgui_cleanup_refs(ImguiHandle handle);

#endif /* LUA_IMGUI_H */
