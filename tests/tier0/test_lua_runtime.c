/*
 * Tier 0: LuaRuntime ownership tests (Wave 7 Phase 1, E2.0/E2.1).
 *
 * E2.1 gate: two synthetic states simultaneously report different contexts.
 * Also covers generation bump on unregister, coroutine main-thread
 * resolution, double-register refusal, and the unregistered-state fallback.
 */

#include "test_harness.h"
#include "lua_runtime.h"

#include <lauxlib.h>
#include <lualib.h>

/* Must run before any client registration latches dual-VM mode. */
TEST(state_for_falls_back_before_dual_mode) {
    /* Ordering guard: the latch is process-global and never resets. If a test
     * added before this one ever registers a client, fail loudly here. */
    ASSERT_EQ(lua_runtime_client()->generation, 0);
    ASSERT_FALSE(lua_runtime_client()->alive);

    lua_State *sv = luaL_newstate();
    ASSERT_NOT_NULL(sv);
    lua_runtime_register(LUA_CONTEXT_SERVER, sv);

    ASSERT_EQ(lua_runtime_state_for(LUA_CONTEXT_SERVER), sv);
    ASSERT_EQ(lua_runtime_state_for(LUA_CONTEXT_CLIENT), sv);

    lua_runtime_unregister(LUA_CONTEXT_SERVER);
    ASSERT_NULL(lua_runtime_state_for(LUA_CONTEXT_SERVER));
    lua_close(sv);
}

TEST(state_for_exact_only_after_dual_mode) {
    lua_State *sv = luaL_newstate();
    lua_State *cl = luaL_newstate();
    ASSERT_NOT_NULL(sv);
    ASSERT_NOT_NULL(cl);
    lua_runtime_register(LUA_CONTEXT_SERVER, sv);
    lua_runtime_register(LUA_CONTEXT_CLIENT, cl);

    ASSERT_EQ(lua_runtime_state_for(LUA_CONTEXT_CLIENT), cl);

    /* Client dies first: its context must resolve to NULL, never to the
     * server VM, whose registry would mis-resolve client refs. */
    lua_runtime_unregister(LUA_CONTEXT_CLIENT);
    ASSERT_NULL(lua_runtime_state_for(LUA_CONTEXT_CLIENT));
    ASSERT_EQ(lua_runtime_state_for(LUA_CONTEXT_SERVER), sv);

    lua_runtime_unregister(LUA_CONTEXT_SERVER);
    lua_close(sv);
    lua_close(cl);
}

TEST(dual_states_report_distinct_contexts) {
    lua_State *sv = luaL_newstate();
    lua_State *cl = luaL_newstate();
    ASSERT_NOT_NULL(sv);
    ASSERT_NOT_NULL(cl);

    lua_runtime_register(LUA_CONTEXT_SERVER, sv);
    lua_runtime_register(LUA_CONTEXT_CLIENT, cl);

    ASSERT_TRUE(lua_runtime_server()->alive);
    ASSERT_TRUE(lua_runtime_client()->alive);
    ASSERT_EQ(lua_runtime_context_of(sv), LUA_CONTEXT_SERVER);
    ASSERT_EQ(lua_runtime_context_of(cl), LUA_CONTEXT_CLIENT);
    ASSERT_NE(lua_runtime_for_state(sv), lua_runtime_for_state(cl));

    lua_runtime_unregister(LUA_CONTEXT_SERVER);
    lua_runtime_unregister(LUA_CONTEXT_CLIENT);
    lua_close(sv);
    lua_close(cl);
}

TEST(coroutine_resolves_to_main_state_runtime) {
    lua_State *sv = luaL_newstate();
    ASSERT_NOT_NULL(sv);
    lua_runtime_register(LUA_CONTEXT_SERVER, sv);

    lua_State *co = lua_newthread(sv);
    ASSERT_EQ(lua_runtime_for_state(co), lua_runtime_server());
    ASSERT_EQ(lua_runtime_context_of(co), LUA_CONTEXT_SERVER);
    lua_pop(sv, 1);

    lua_runtime_unregister(LUA_CONTEXT_SERVER);
    lua_close(sv);
}

TEST(generation_bumps_on_unregister) {
    lua_State *sv = luaL_newstate();
    ASSERT_NOT_NULL(sv);
    uint32_t gen_before = lua_runtime_server()->generation;

    lua_runtime_register(LUA_CONTEXT_SERVER, sv);
    ASSERT_EQ(lua_runtime_server()->generation, gen_before);

    lua_runtime_unregister(LUA_CONTEXT_SERVER);
    ASSERT_EQ(lua_runtime_server()->generation, gen_before + 1);
    ASSERT_FALSE(lua_runtime_server()->alive);
    ASSERT_NULL(lua_runtime_server()->L);
    lua_close(sv);
}

TEST(double_register_refused) {
    lua_State *a = luaL_newstate();
    lua_State *b = luaL_newstate();
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);

    lua_runtime_register(LUA_CONTEXT_SERVER, a);
    lua_runtime_register(LUA_CONTEXT_SERVER, b);
    ASSERT_EQ(lua_runtime_server()->L, a);

    lua_runtime_unregister(LUA_CONTEXT_SERVER);
    lua_close(a);
    lua_close(b);
}

/* E2.2 gate: repeated dual init/shutdown leaves balanced generations and no
 * stale published pointers. */
TEST(repeated_dual_cycles_stay_balanced) {
    for (int i = 0; i < 3; i++) {
        lua_State *sv = luaL_newstate();
        lua_State *cl = luaL_newstate();
        ASSERT_NOT_NULL(sv);
        ASSERT_NOT_NULL(cl);
        lua_runtime_register(LUA_CONTEXT_SERVER, sv);
        lua_runtime_register(LUA_CONTEXT_CLIENT, cl);
        ASSERT_EQ(lua_runtime_state_for(LUA_CONTEXT_SERVER), sv);
        ASSERT_EQ(lua_runtime_state_for(LUA_CONTEXT_CLIENT), cl);

        uint32_t sgen = lua_runtime_server()->generation;
        uint32_t cgen = lua_runtime_client()->generation;
        lua_runtime_unregister(LUA_CONTEXT_CLIENT);
        lua_runtime_unregister(LUA_CONTEXT_SERVER);
        ASSERT_EQ(lua_runtime_server()->generation, sgen + 1);
        ASSERT_EQ(lua_runtime_client()->generation, cgen + 1);
        ASSERT_NULL(lua_runtime_state_for(LUA_CONTEXT_SERVER));
        ASSERT_NULL(lua_runtime_state_for(LUA_CONTEXT_CLIENT));
        ASSERT_NULL(lua_runtime_server()->L);
        ASSERT_NULL(lua_runtime_client()->L);
        lua_close(cl);
        lua_close(sv);
    }
}

TEST(register_null_state_is_noop) {
    bool was_alive = lua_runtime_server()->alive;
    uint32_t gen = lua_runtime_server()->generation;
    lua_runtime_register(LUA_CONTEXT_SERVER, NULL);
    ASSERT_EQ(lua_runtime_server()->alive, was_alive);
    ASSERT_EQ(lua_runtime_server()->generation, gen);
    ASSERT_NULL(lua_runtime_server()->L);
}

TEST(unregister_when_not_registered_is_noop) {
    uint32_t gen = lua_runtime_server()->generation;
    lua_runtime_unregister(LUA_CONTEXT_SERVER); /* already dead */
    ASSERT_EQ(lua_runtime_server()->generation, gen);
    ASSERT_FALSE(lua_runtime_server()->alive);
}

TEST(unregistered_state_falls_back) {
    lua_State *stray = luaL_newstate();
    ASSERT_NOT_NULL(stray);
    ASSERT_NULL(lua_runtime_for_state(stray));
    ASSERT_EQ(lua_runtime_context_of(stray), lua_context_get());
    lua_close(stray);
}

void register_lua_runtime_tests(void) {
    RUN_TEST(state_for_falls_back_before_dual_mode);
    RUN_TEST(state_for_exact_only_after_dual_mode);
    RUN_TEST(dual_states_report_distinct_contexts);
    RUN_TEST(coroutine_resolves_to_main_state_runtime);
    RUN_TEST(generation_bumps_on_unregister);
    RUN_TEST(double_register_refused);
    RUN_TEST(repeated_dual_cycles_stay_balanced);
    RUN_TEST(register_null_state_is_noop);
    RUN_TEST(unregister_when_not_registered_is_noop);
    RUN_TEST(unregistered_state_falls_back);
}
