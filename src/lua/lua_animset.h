#ifndef BG3SE_LUA_ANIMSET_H
#define BG3SE_LUA_ANIMSET_H

#include <lua.h>
#include <stdbool.h>

/**
 * Push { AnimationSubSets = <proxy> } for an ls::AnimationSetResource.
 * Returns false (pushing nothing) if the resource has no bank.
 */
bool lua_animset_push_bank(lua_State *L, void *resource);

#endif // BG3SE_LUA_ANIMSET_H
