# BG3SE Documentation Gap Analysis: Comprehensive Feature Parity Roadmap

## Executive Summary

This document provides an exhaustive audit of the 4 documentation files in `/bg3se/Docs/` (API.md, Debugger.md, ReleaseNotes.md, VirtualTextures.md) against the current bg3se-macos v0.11.0 implementation. It identifies all gaps, prioritizes features for implementation, and provides architectural guidance for reaching parity with Windows BG3SE.

**Key Finding:** bg3se-macos currently implements approximately **30%** of the Windows BG3SE feature set. The core Osiris integration is solid (95%), but critical systems like networking, user variables, engine events, and full component access are entirely missing.

---

## Current Implementation Status (v0.11.0)

### Already Implemented (from ROADMAP.md)

| Feature | Status | Notes |
|---------|--------|-------|
| **Ext.Stats** (basic) | 🔄 In Progress | Get, GetAll, DumpTypes - no property read/write |
| **Ext.Entity** (basic) | ✅ Complete | GUID lookup, basic components (Transform, Level, Physics, Visual) |
| **Osi.* metatable** | ✅ Complete | Lazy lookup, type dispatch, Query/Call/Event/Proc/Database |
| **Ext.Json** | ✅ Complete | Parse/Stringify with options |
| **Ext.IO** | ✅ Basic | LoadFile, SaveFile |
| **Ext.Print, Require** | ✅ Complete | Basic utilities |
| **Ext.Osiris.RegisterListener** | ✅ Complete | Event callbacks before/after |
| **TypeId Discovery** | ✅ Complete | 11 component indices discovered at SessionLoaded |
| **Safe Memory APIs** | ✅ Complete | mach_vm_read for crash-safe access |
| **Function Name Caching** | ✅ Complete | OsiFunctionDef->Signature->Name |

---

## Gap Analysis: Features Missing from Roadmap

### Category 1: Architecture & State Management (CRITICAL)

#### 1.1 Client/Server Lua States

**From Docs (API.md lines 94-105):**
> "The game is split into client and server components... the extender keeps multiple Lua states: one for the server and one for each client."

**Current State:** bg3se-macos has a single Lua state (server-side only)

**Gap:**
- No `BootstrapClient.lua` loading
- No client-side APIs (UI modification, level scaling formulas, status chances)
- No context switching between client/server
- Annotations **C** (Client only), **S** (Server only), **R** (Restricted) not honored

**Implementation Needed:**
```c
// src/lua/lua_state.c - NEW MODULE
typedef struct {
    lua_State *server_state;
    lua_State *client_state;  // Per-client in multiplayer, single in SP
} LuaStateManager;

void lua_state_init_client(void);
void lua_state_init_server(void);
lua_State* lua_state_get_current_context(void);
```

**Files to Reference:**
- `BG3Extender/Lua/LuaBinding.h` - State management
- `BG3Extender/Osiris/OsirisExtender.cpp` - Server state initialization

**Priority:** CRITICAL - Blocks all client-side mods

---

#### 1.2 PersistentVars (Savegame Persistence)

**From Docs (API.md lines 106-128):**
> "For keeping data through multiple play sessions it is possible to store them in the savegame by storing them in `Mods[ModTable].PersistentVars`."

**Current State:** Not implemented - all mod state lost on reload

**Gap:**
- `PersistentVars` table not created per-mod
- No savegame serialization hook
- No restoration before `SessionLoaded` event
- Mods cannot persist quest state, custom items, user settings

**Implementation Needed:**
```lua
-- Target API
PersistentVars = {}  -- Created per mod

function doStuff()
    PersistentVars['QuestProgress'] = 5
end

function OnSessionLoaded()
    -- PersistentVars restored from savegame
    _P(PersistentVars['QuestProgress'])  -- Prints 5
end
```

```c
// src/mod/persistent_vars.c - NEW MODULE
void persistent_vars_serialize(const char *mod_uuid, char **json_out);
void persistent_vars_deserialize(const char *mod_uuid, const char *json);
void persistent_vars_hook_savegame(void);
```

**Files to Reference:**
- Windows BG3SE savegame integration (needs RE work on macOS)
- `BG3Extender/Lua/Libs/ClientServer.inl` - PersistentVars implementation

**Priority:** CRITICAL - Mods lose all state on reload

---

#### 1.3 Object Scopes/Lifetimes (Userdata Expiration)

**From Docs (API.md lines 197-241):**
> "Most `userdata` types are now bound to their enclosing *extender scope*... 'smuggling' objects outside of listeners is no longer allowed."

**Current State:** Objects live forever (memory leak risk, potential crashes)

**Gap:**
- No scope tracking for userdata
- No expiration when listener exits
- Error: "Attempted to read object of type 'X' whose lifetime has expired" never thrown
- Subproperties don't inherit parent lifetime

**Implementation Needed:**
```c
// src/entity/userdata_scope.c - NEW MODULE
typedef struct {
    uint32_t scope_id;
    uint32_t created_in_scope;
    bool expired;
} UserdataScopeInfo;

void scope_begin(void);
void scope_end(void);
bool scope_check_expired(void *userdata);
```

**Files to Reference:**
- `BG3Extender/Lua/Shared/Proxies/` - Userdata proxy implementation
- `BG3Extender/Lua/Shared/LuaLifetime.h`

**Priority:** HIGH - Memory leaks, potential crashes

---

### Category 2: Networking & Synchronization (CRITICAL)

#### 2.1 NetChannel API (New Request/Response Networking)

**From Docs (API.md lines 860-981):**
> "NetChannel API provides a structured abstraction for request/response and message broadcasting... supersedes the legacy NetMessage approach."

**Current State:** Not implemented

**Gap:**
- No `Ext.Net.CreateChannel()` / `Net.CreateChannel()`
- No message sending (SendToServer, SendToClient, Broadcast)
- No request/reply pattern (RequestToServer, RequestToClient)
- No handlers (SetHandler, SetRequestHandler)
- No `Ext.Net.IsHost()`

**Target API (from docs):**
```lua
-- Create channel
local channel = Net.CreateChannel(ModuleUUID, "MyChannel")

-- Server handler
channel:SetHandler(function(data, user)
    Osi.TemplateAddTo(data.Template, data.Target, data.Amount)
end)

channel:SetRequestHandler(function(data, user)
    return { Result = CheckSomething(data) }
end)

-- Client sending
channel:SendToServer({ Items = {{"template-1", 1}} })
channel:RequestToServer({ Target = uuid }, function(response)
    _P("Got response: ", response.Result)
end)
```

**Implementation Needed:**
```c
// src/net/net_channel.c - NEW MODULE
typedef struct {
    char *module_uuid;
    char *channel_name;
    lua_ref handler;
    lua_ref request_handler;
} NetChannel;

NetChannel* net_channel_create(const char *module_uuid, const char *name);
void net_channel_send_to_server(NetChannel *ch, const char *json_data);
void net_channel_send_to_client(NetChannel *ch, int user_id, const char *json_data);
void net_channel_broadcast(NetChannel *ch, const char *json_data);
void net_channel_request_to_server(NetChannel *ch, const char *json_data, lua_ref callback);
```

**Files to Reference:**
- `BG3Extender/Lua/Libs/Net.inl` - NetChannel implementation
- `BG3Extender/Networking/` - Network layer

**Priority:** CRITICAL - Multiplayer mods completely broken

---

#### 2.2 User Variables (Entity.Vars with Auto-Sync)

**From Docs (API.md lines 1198-1315):**
> "v10 adds support for attaching custom properties to entities. These properties support automatic network synchronization and savegame persistence."

**Current State:** Not implemented

**Gap:**
- No `Ext.Vars.RegisterUserVariable()` function
- No `entity.Vars` property on entities
- No automatic sync between server/client
- No savegame persistence for user variables
- No caching behavior

**Target API (from docs):**
```lua
-- Registration (in BootstrapServer/Client.lua)
Ext.Vars.RegisterUserVariable("NRD_MyVar", {
    Server = true,
    Client = true,
    SyncToClient = true,
    Persistent = true,
    SyncOnTick = true
})

-- Usage
entity.Vars.NRD_MyVar = { health = 100, mana = 50 }
local data = entity.Vars.NRD_MyVar
```

**Implementation Needed:**
```c
// src/vars/user_variables.c - NEW MODULE
typedef struct {
    char *name;
    bool server;
    bool client;
    bool sync_to_client;
    bool sync_to_server;
    bool persistent;
    bool sync_on_tick;
    bool sync_on_write;
    bool dont_cache;
} UserVariablePrototype;

void user_vars_register(const char *name, UserVariablePrototype *proto);
void user_vars_set(void *entity, const char *name, const char *json_value);
const char* user_vars_get(void *entity, const char *name);
void user_vars_sync(void);
```

**Priority:** CRITICAL - Can't attach mod data to entities

---

#### 2.3 Mod Variables (Per-Mod Storage)

**From Docs (API.md lines 1326-1346):**
> "Mod variables are the equivalent of user variables for mods; i.e. they store and synchronize a set of variables for each mod."

**Current State:** Not implemented

**Gap:**
- No `Ext.Vars.RegisterModVariable()`
- No `Ext.Vars.GetModVariables()`
- No `Ext.Vars.SyncModVariables()`

**Priority:** HIGH - Can't store per-mod global state with sync

---

### Category 3: Engine Events (CRITICAL)

#### 3.1 Ext.Events API

**From Docs (API.md lines 449-477):**
> "Subscribing to engine events can be done through the `Ext.Events` table."

**Current State:** Not implemented (only `Ext.Osiris.RegisterListener` works)

**Gap:**
- No `Ext.Events` table
- No Subscribe/Unsubscribe pattern
- No Priority option (lower = called first, default 100)
- No Once option (auto-unsubscribe after first call)
- No handler ID for unsubscription

**Target API (from docs):**
```lua
-- Subscribe with options
local handlerId = Ext.Events.GameStateChanged:Subscribe(function(e)
    _P("State change from " .. e.FromState .. " to " .. e.ToState)
end, {
    Priority = 50,
    Once = true
})

-- Unsubscribe
Ext.Events.GameStateChanged:Unsubscribe(handlerId)
```

**Available Events (from API.md lines 1651-1689):**
| Event | When | Notes |
|-------|------|-------|
| `ModuleLoadStarted` | Before mod data loads | Use for `AddPathOverride` |
| `StatsLoaded` | After stats entries loaded | Apply stat modifications |
| `SessionLoading` | Session setup started | Early initialization |
| `SessionLoaded` | Session ready | PersistentVars available |
| `ResetCompleted` | After `reset` command | Lua state reloaded |
| `GameStateChanged` | Pause, unpause, etc. | State transitions |
| `Tick` | Every game loop (~30hz) | Use `Ext.OnNextTick()` helper |

**Implementation Needed:**
```c
// src/events/engine_events.c - NEW MODULE
typedef struct {
    char *event_name;
    lua_ref handler;
    int priority;
    bool once;
    uint32_t handler_id;
} EventSubscription;

void events_subscribe(const char *event, lua_ref handler, int priority, bool once);
void events_unsubscribe(const char *event, uint32_t handler_id);
void events_dispatch(const char *event, void *event_data);
```

**Files to Reference:**
- `BG3Extender/Lua/Shared/LuaEventHelpers.h`
- `BG3Extender/Lua/Libs/Events.inl`

**Priority:** CRITICAL - Can't react to engine lifecycle

---

### Category 4: Entity Component System (HIGH)

#### 4.1 Entity Iteration & Discovery

**From Docs (API.md lines 999-1065):**
> "`Entity:GetAllComponentNames()` returns all engine component types... `Entity:GetAllComponents()` returns all components that are attached."

**Current State:** Not implemented

**Gap:**
- No `entity:GetAllComponentNames()` - can't discover what components exist
- No `entity:GetAllComponents()` - can't iterate components
- No `entity:CreateComponent(name)` - can't add new components
- No replication control (`SetReplicationFlags`, `GetReplicationFlags`)

**Implementation Needed:**
```c
// src/entity/entity_iteration.c - NEW MODULE
int entity_get_all_component_names(void *entity, char ***names_out);
int entity_get_all_components(void *entity, void ***components_out);
void* entity_create_component(void *entity, const char *name);
```

**Priority:** HIGH - Can't introspect entities

---

#### 4.2 Component Property Access

**From ROADMAP.md issue tracking:**
> "Component property access incomplete (needs IndexedProperties + pools)"

**Current State:** Can detect components exist, but can't read/write properties

**Gap:**
- Can get component pointer but can't access fields
- No mapping of property indices to pool offsets
- Missing: health values, inventory items, stat values, armor data

**Implementation Needed:**
```c
// src/entity/component_properties.c - NEW MODULE
typedef struct {
    uint32_t property_index;
    uint32_t pool_offset;
    uint32_t data_type;  // int, float, string, etc.
} PropertyMapping;

void* component_get_property(void *component, const char *property_name);
void component_set_property(void *component, const char *property_name, void *value);
```

**Files to Reference:**
- `BG3Extender/GameDefinitions/Components/Components.h`
- `BG3Extender/GameDefinitions/PropertyMaps.h`

**Priority:** CRITICAL - Can't read character health, inventory, etc.

---

### Category 5: Stats System (HIGH)

#### 5.1 Stats Property Read/Write

**From Docs (API.md lines 599-706):**
> "Stat attributes can be retrieved by reading the appropriate property... Stat attributes can be updated using simple table assignment."

**Current State:** Only Name, Type, Level, Using accessible

**Gap:**
- No IndexedProperties access (Damage, DamageType, SpellFlags, etc.)
- No table assignment for property writes
- No `stat:Sync()` method for replication
- No complex property handling (Requirements, SpellSuccess tables)
- No reassignment pattern for table properties

**Target API (from docs):**
```lua
local spell = Ext.Stats.Get("Shout_FlameBlade")
local useCosts = spell.UseCosts  -- Read
spell.UseCosts = "BonusActionPoint:1"  -- Write
spell:Sync()  -- Replicate to clients

-- Table property modification (must reassign)
local requirements = spell.Requirements
table.insert(requirements, {Name = "Immobile", Param = -1, Not = false})
spell.Requirements = requirements  -- Reassign to trigger update
```

**Implementation Needed:**
- Complete IndexedProperties discovery via Ghidra
- Pool offset mapping for each property type
- `__index` metamethod for property reads
- `__newindex` metamethod for property writes
- `Sync()` method to trigger replication

**Priority:** CRITICAL - Can't modify game stats

---

#### 5.2 Stats Creation

**From Docs (API.md lines 622-639):**
> "`Ext.Stats.Create(name, type, template)` creates a new stats entry."

**Current State:** Not implemented

**Gap:**
- No `Ext.Stats.Create()` function
- Can't add new weapons, spells, statuses at runtime
- No template cloning

**Priority:** HIGH - Can't create new game content

---

#### 5.3 Stats ExtraData

**From Docs (API.md lines 751-760):**
> "`Ext.ExtraData` is an object containing all entries from `Data.txt`."

**Current State:** Not implemented

**Gap:**
- No `Ext.Stats.ExtraData` table
- Can't access game balance values from Data.txt

**Priority:** LOW - Minor convenience feature

---

### Category 6: Enumerations & Bitfields (MEDIUM)

#### 6.1 Proper Enum Objects

**From Docs (API.md lines 292-351):**
> "Enum values returned from functions are `userdata` values instead of `string`."

**Current State:** Not implemented (enums returned as raw values or strings)

**Gap:**
- No `Label`, `Value`, `EnumName` properties on enum values
- No `Ext.Enums` table for enum access
- No comparison operators (`==` with labels, values, other enums)
- No proper `__tostring` implementation
- No JSON serialization to string labels

**Target API (from docs):**
```lua
local bt = entity.CurrentTemplate.BloodSurfaceType
_P(bt.Label)     -- "Blood"
_P(bt.Value)     -- 16
_P(bt.EnumName)  -- "SurfaceType"

bt == "Blood"    -- true (comparison with label)
bt == 16         -- true (comparison with value)
bt == Ext.Enums.SurfaceType.Blood  -- true
```

**Priority:** MEDIUM - Affects API ergonomics

---

#### 6.2 Bitfield Objects

**From Docs (API.md lines 353-447):**
> "Bitfields returned from functions are `userdata` values instead of `table`."

**Current State:** Not implemented

**Gap:**
- No `__Labels`, `__Value`, `__EnumName` properties
- No bitwise operators (`~`, `|`, `&`)
- No flag querying (`af.DrunkImmunity`)
- No iteration via `pairs`/`ipairs`

**Target API (from docs):**
```lua
local af = entity.Stats.AttributeFlags
af.DrunkImmunity   -- true
af | "FreezeImmunity"  -- Add flag
af & {"DrunkImmunity", "BleedingImmunity"}  -- Filter
```

**Priority:** MEDIUM - Affects API ergonomics

---

### Category 7: Utility Functions (MEDIUM)

#### 7.1 Timer API

**From ROADMAP.md Phase 2.3:**
> "Scheduling API for delayed and periodic callbacks. Essential for mods that need timed actions."

**Current State:** Not implemented

**Target API (from ROADMAP):**
```lua
Ext.Timer.WaitFor(1000, function()
    Ext.Print("1 second later!")
end)

local timerId = Ext.Timer.RegisterTimer(500, function()
    Ext.Print("Every 500ms")
end)

Ext.Timer.Cancel(timerId)
```

**Implementation Needed:**
```c
// src/timer/timer_manager.c - NEW MODULE
typedef struct {
    uint32_t id;
    uint64_t deadline_ms;
    uint64_t interval_ms;  // 0 for one-shot
    lua_ref callback;
    bool cancelled;
} Timer;

uint32_t timer_wait_for(uint64_t delay_ms, lua_ref callback);
uint32_t timer_register(uint64_t interval_ms, lua_ref callback);
void timer_cancel(uint32_t id);
void timer_tick(uint64_t current_time_ms);
```

**Priority:** HIGH - Essential for many mods

---

#### 7.2 Ext.Math Library

**From Docs (API.md lines 1488-1649):**
> "The extender math library `Ext.Math` contains... vector/matrix operations."

**Current State:** Not implemented

**Gap:**
- No vector operations (Add, Sub, Mul, Div, Normalize, Cross, Dot, etc.)
- No matrix operations (Inverse, Transpose, Rotate, Translate, Scale)
- No geometry functions (Distance, Angle, Reflect)
- No transform utilities (BuildRotation, ExtractEulerAngles, Decompose)
- No scalar functions (Clamp, Lerp, Fract, Sign, etc.)

**Complete API Surface (from docs):**
```lua
-- Vector operations
Ext.Math.Add(a, b)
Ext.Math.Sub(a, b)
Ext.Math.Mul(a, b)
Ext.Math.Div(a, b)
Ext.Math.Reflect(I, N)
Ext.Math.Angle(a, b)
Ext.Math.Cross(x, y)
Ext.Math.Distance(p0, p1)
Ext.Math.Dot(x, y)
Ext.Math.Length(x)
Ext.Math.Normalize(x)
Ext.Math.Perpendicular(x, normal)
Ext.Math.Project(x, normal)

-- Matrix operations
Ext.Math.Determinant(x)
Ext.Math.Inverse(x)
Ext.Math.Transpose(x)
Ext.Math.OuterProduct(c, r)
Ext.Math.Rotate(m, angle, axis)
Ext.Math.Translate(m, translation)
Ext.Math.Scale(m, scale)

-- Matrix construction
Ext.Math.BuildRotation4(v, angle)
Ext.Math.BuildRotation3(v, angle)
Ext.Math.BuildTranslation(v)
Ext.Math.BuildScale(v)
Ext.Math.BuildFromEulerAngles4(angles)
Ext.Math.BuildFromEulerAngles3(angles)
Ext.Math.BuildFromAxisAngle3(axis, angle)
Ext.Math.BuildFromAxisAngle4(axis, angle)

-- Decomposition
Ext.Math.ExtractEulerAngles(m)
Ext.Math.ExtractAxisAngle(m, axis)
Ext.Math.Decompose(m, scale, yawPitchRoll, translation)

-- Scalar functions
Ext.Math.Fract(x)
Ext.Math.Trunc(x)
Ext.Math.Sign(x)
Ext.Math.Clamp(val, min, max)
Ext.Math.Lerp(x, y, a)
Ext.Math.Acos(x)
Ext.Math.Asin(x)
Ext.Math.Atan(y_over_x)
Ext.Math.Atan2(x, y)
```

**Implementation Approach:**
- Use GLM library (header-only C++ math)
- Or implement manually in C with ARM NEON intrinsics
- Expose via Lua C API

**Priority:** MEDIUM - Affects transform/physics mods

---

#### 7.3 Input Injection (Ext.Input)

**From ReleaseNotes.md v22:**
> "Added support for programmatic triggering of input events via `Ext.Input.InjectKeyPress`, `InjectKeyDown`, `InjectKeyUp`."

**Current State:** Not implemented

**Gap:**
- No `Ext.Input` namespace
- No `InjectKeyPress`, `InjectKeyDown`, `InjectKeyUp`
- No `GetInputManager()` (v23+)

**Priority:** LOW - Nice-to-have for automation

---

#### 7.4 Physics Queries (Ext.Level)

**From ReleaseNotes.md v22:**
> "Added physics query functions to `Ext.Level`: `RaycastClosest`, `RaycastAny`, `RaycastAll`, `SweepSphere*`, `SweepCapsule*`, `SweepBox*`, `SweepCylinder*`, `TestBox`, `TestSphere`."

**Current State:** Not implemented

**Gap:**
- No `Ext.Level` namespace
- No raycast functions
- No sweep functions
- No overlap tests

**Priority:** LOW - Affects advanced gameplay mods

---

### Category 8: Console & Debugging (HIGH)

#### 8.1 Debug Console

**From Docs (API.md lines 131-193):**
> "The extender allows commands to be entered to the console window."

**Current State:** Not implemented

**Gap:**
- No console window/REPL
- No `client`/`server` context switching
- No `reset` command for Lua VM reload
- No command execution in Lua environment
- No multiline mode (`--[[ ... ]]--`)
- No output to file support

**Features Needed:**
- Terminal-based console (macOS doesn't have overlay)
- Context switching between client/server Lua states
- Command history
- Direct Lua execution
- Variable persistence within session

**Priority:** HIGH - Significantly improves developer experience

---

#### 8.2 Custom Console Commands

**From Docs (API.md lines 142-150):**
> "Commands prefixed by a `!` will trigger callbacks registered via `RegisterConsoleCommand`."

**Current State:** Not implemented

**Gap:**
- No `Ext.RegisterConsoleCommand(name, callback)`
- No `!command arg1 arg2` invocation

**Target API (from docs):**
```lua
Ext.RegisterConsoleCommand("test", function(cmd, a1, a2, ...)
    _P("Cmd: " .. cmd .. ", args: ", a1, ", ", a2)
end)
-- Invoke: !test 123 456
```

**Priority:** MEDIUM - Developer convenience

---

### Category 9: UI Systems (MEDIUM)

#### 9.1 Noesis UI (Custom ViewModels)

**From Docs (API.md lines 1099-1195):**
> "SE supports the creation and modification of Noesis viewmodels."

**Current State:** Not implemented

**Gap:**
- No `Ext.UI.RegisterType()` for ViewModel registration
- No `Ext.UI.Instantiate()` for ViewModel creation
- No `Ext.UI.GetRoot()` for UI tree access
- No DataContext manipulation
- No Command handlers

**Complete API (from docs):**
```lua
-- Register ViewModel type
Ext.UI.RegisterType("PREFIX_MyType", {
    MyString = {Type = "String", WriteCallback = func, Notify = true},
    MyCommand = {Type = "Command"},
    MyCollection = {Type = "Collection"}
}, wrappedTypeName)

-- Supported property types:
-- Bool, Int8-64, UInt8-64, Single, Double, String
-- Collection, Command, Object, Color, Vector2/3, Point, Rect

-- Instantiate
local vm = Ext.UI.Instantiate("PREFIX_MyType")
vm.MyString = "value"
vm.MyCommand:SetHandler(function() ... end)

-- Wrap existing DataContext
local vm = Ext.UI.Instantiate("se::PREFIX_MyType", existingVM)
widget.DataContext = vm

-- UI access
Ext.UI.GetRoot()
Ext.UI.GetCursorControl()  -- v22+
Ext.UI.GetDragDrop()       -- v22+
```

**Priority:** MEDIUM - Affects UI mods

---

#### 9.2 IMGUI Debug Overlay

**From ReleaseNotes.md v23-27:**
> IMGUI system with windows, tables, fonts, events

**Current State:** Not implemented

**Gap:**
- No IMGUI initialization
- No window management
- No table rendering
- No font loading
- No UI scaling

**Priority:** LOW - Developer debugging tool

---

### Category 10: Database Operations (MEDIUM)

#### 10.1 Osiris Database Get/Delete

**From Docs (API.md lines 541-571):**
> "`Osi.DB_*:Get(nil, nil, nil)` fetches all rows... `:Delete()` removes rows."

**Current State:** Partially implemented (need to verify)

**Gap Verification Needed:**
- Does `Osi.DB_Players:Get(nil)` work? (claimed in README)
- Does `:Delete()` work?
- Does filtered Get work? `Osi.DB_X:Get(value, nil, nil)`

**Target API (from docs):**
```lua
-- Get all rows
local rows = Osi.DB_MyDatabase:Get(nil, nil, nil)

-- Get filtered rows
local rows = Osi.DB_MyDatabase:Get("filter_value", nil, nil)

-- Insert
Osi.DB_MyDatabase(value1, value2, value3)

-- Delete
Osi.DB_MyDatabase:Delete(nil, nil, nil)  -- All
Osi.DB_MyDatabase:Delete("filter", nil, nil)  -- Filtered
```

**Priority:** MEDIUM - Already partially working

---

### Category 11: Advanced Features (LOW)

#### 11.1 Virtual Textures System

**From Docs (VirtualTextures.md):**
> "To load custom .GTS files, Script Extender v12 or later is required."

**Current State:** Not implemented

**Gap:**
- No `VirtualTextures.json` parsing
- No GTex mapping
- No tileset loading

**Priority:** LOW - Advanced graphical modding

---

#### 11.2 Debugger Support

**From Docs (Debugger.md):**
> "The debugger requires VS Code... supports single-stepping, breakpoints, watches."

**Current State:** Not implemented

**Gap:**
- No `EnableLuaDebugger` setting
- No debugger server (127.0.0.1:9998)
- No VS Code integration

**Priority:** LOW - Developer convenience

---

#### 11.3 Mod Info API

**From Docs (API.md lines 1458-1486):**
> "`Ext.Mod.IsModLoaded(guid)`, `GetLoadOrder()`, `GetModInfo(guid)`"

**Current State:** Partially implemented (mod detection works)

**Gap Verification Needed:**
- Does `Ext.Mod.IsModLoaded()` work?
- Does `Ext.Mod.GetLoadOrder()` work?
- Does `Ext.Mod.GetModInfo()` work?

**Priority:** LOW - Minor utility

---

## Feature Parity Matrix

### API Namespaces

| Namespace | Windows BG3SE | bg3se-macos | Parity |
|-----------|---------------|-------------|--------|
| `Ext.Utils` | ✅ Full | ⚠️ Partial (Print only) | 20% |
| `Ext.IO` | ✅ Full | ✅ LoadFile, SaveFile | 80% |
| `Ext.Json` | ✅ Full | ✅ Parse, Stringify | 90% |
| `Ext.Stats` | ✅ Full | ⚠️ Read-only basics | 20% |
| `Ext.Entity` | ✅ Full | ⚠️ Basic access | 40% |
| `Ext.Osiris` | ✅ Full | ✅ RegisterListener | 90% |
| `Ext.Events` | ✅ Full | ❌ Not impl | 0% |
| `Ext.Vars` | ✅ Full | ❌ Not impl | 0% |
| `Ext.Net` | ✅ Full | ❌ Not impl | 0% |
| `Ext.UI` | ✅ Full | ❌ Not impl | 0% |
| `Ext.Math` | ✅ Full | ❌ Not impl | 0% |
| `Ext.Timer` | ✅ Full | ❌ Not impl | 0% |
| `Ext.Input` | ✅ Full | ❌ Not impl | 0% |
| `Ext.Level` | ✅ Full | ❌ Not impl | 0% |
| `Ext.Mod` | ✅ Full | ⚠️ Partial | 50% |
| `Ext.Types` | ✅ Full | ❌ Not impl | 0% |
| `Osi.*` | ✅ Full | ✅ Dynamic metatable | 95% |

### Overall Parity: **~30%**

---

## Implementation Priorities

### Phase A: Critical Blockers (Breaks Most Mods)

| Priority | Feature | Effort | Impact |
|----------|---------|--------|--------|
| A1 | Ext.Events API | Medium | CRITICAL |
| A2 | PersistentVars | Medium | CRITICAL |
| A3 | Stats Property Read/Write | High | CRITICAL |
| A4 | Component Property Access | High | CRITICAL |
| A5 | NetChannel API | High | CRITICAL |
| A6 | User Variables | High | CRITICAL |

### Phase B: High Impact (Breaks Many Mods)

| Priority | Feature | Effort | Impact |
|----------|---------|--------|--------|
| B1 | Client Lua State | High | HIGH |
| B2 | Timer API | Low | HIGH |
| B3 | Console/REPL | Medium | HIGH |
| B4 | GetAllComponents | Low | HIGH |
| B5 | Stats Create/Sync | Medium | HIGH |
| B6 | Userdata Lifetime Scoping | Medium | HIGH |

### Phase C: Medium Impact (Developer Experience)

| Priority | Feature | Effort | Impact |
|----------|---------|--------|--------|
| C1 | Ext.Math Library | Medium | MEDIUM |
| C2 | Enum/Bitfield Objects | Medium | MEDIUM |
| C3 | Console Commands | Low | MEDIUM |
| C4 | Mod Variables | Medium | MEDIUM |
| C5 | More Component Types | High | MEDIUM |

### Phase D: Low Impact (Nice-to-Have)

| Priority | Feature | Effort | Impact |
|----------|---------|--------|--------|
| D1 | Noesis UI | High | MEDIUM |
| D2 | IMGUI | High | LOW |
| D3 | Input Injection | Medium | LOW |
| D4 | Physics Queries | Medium | LOW |
| D5 | Virtual Textures | Medium | LOW |
| D6 | Debugger Support | High | LOW |

---

## Critical Questions Requiring Clarification

1. **Client/Server Architecture on macOS:** How to handle when macOS BG3 is client-only (no dedicated server)? Single Lua state simulating both?

2. **PersistentVars Savegame Format:** What's the savegame format hook point on macOS? Needs RE work.

3. **NetChannel Wire Protocol:** JSON over game network layer? Need to hook send/receive functions.

4. **IndexedProperties Schema:** How to map property names to pool offsets? Ghidra analysis required per component type.

5. **Userdata Scope Tracking:** How to track listener execution contexts? Need hook at event dispatch entry/exit.

---

## References

### BG3SE Documentation Files (Audited)
- `/Users/tomdimino/Desktop/Programming/bg3se/Docs/API.md` (1689 lines, v22)
- `/Users/tomdimino/Desktop/Programming/bg3se/Docs/Debugger.md` (130 lines)
- `/Users/tomdimino/Desktop/Programming/bg3se/Docs/ReleaseNotes.md` (180 lines, v22-v28)
- `/Users/tomdimino/Desktop/Programming/bg3se/Docs/VirtualTextures.md` (123 lines)

### bg3se-macos Current Implementation
- `/Users/tomdimino/Desktop/Programming/bg3se-macos/ROADMAP.md`
- `/Users/tomdimino/Desktop/Programming/bg3se-macos/README.md`

### Windows BG3SE Reference Implementation
- `BG3Extender/Lua/Libs/` - Lua API implementations
- `BG3Extender/GameDefinitions/` - Entity/Component structures
- `BG3Extender/Osiris/` - Osiris integration
- `BG3Extender/Networking/` - Network layer

---

## Appendix A: Complete API Checklist

### Ext.Utils
- [ ] `Print(...)`
- [ ] `MonotonicTime()`
- [ ] `GetMemoryUsage()` (v27+)
- [ ] `Reset()` (v28+ client/server separation)

### Ext.IO
- [x] `LoadFile(path)`
- [x] `SaveFile(path, content)`
- [ ] `AddPathOverride(original, new)`

### Ext.Json
- [x] `Parse(json)`
- [x] `Stringify(value, options)` (Beautify, StringifyInternalTypes, IterateUserdata, AvoidRecursion, MaxDepth)

### Ext.Stats
- [x] `Get(name)` (basic properties only)
- [x] `GetStats(type)` / `GetAll(type)`
- [ ] `GetStatsLoadedBefore(modGuid, type)`
- [ ] `Create(name, type, template)`
- [ ] Property read via `__index`
- [ ] Property write via `__newindex`
- [ ] `stat:Sync()` method
- [ ] Level scaling parameter
- [ ] `ExtraData` table

### Ext.Entity
- [x] `Get(guid/handle/netId)`
- [x] `IsReady()`
- [ ] `Create()` (v22+)
- [ ] `Destroy(entity)` (v22+)
- [x] `entity:GetComponent(name)`
- [x] `entity.ComponentName` shorthand
- [ ] `entity:GetAllComponents()`
- [ ] `entity:GetAllComponentNames()`
- [ ] `entity:CreateComponent(name)`
- [ ] `entity:RemoveComponent(name)` (v22+)
- [x] `entity:IsAlive()`
- [ ] `entity:GetEntityType()`
- [ ] `entity:GetSalt()`
- [ ] `entity:GetIndex()`
- [ ] `entity:GetNetId()` (v23+)
- [ ] `entity:Replicate(component)`
- [ ] `entity:SetReplicationFlags()`
- [ ] `entity:GetReplicationFlags()`

### Ext.Events
- [ ] `EventName:Subscribe(handler, options)`
- [ ] `EventName:Unsubscribe(handlerId)`
- [ ] `Priority` option
- [ ] `Once` option
- [ ] `ModuleLoadStarted` event
- [ ] `StatsLoaded` event
- [ ] `SessionLoading` event
- [ ] `SessionLoaded` event
- [ ] `ResetCompleted` event
- [ ] `GameStateChanged` event
- [ ] `Tick` event
- [ ] `Ext.OnNextTick(func)` helper

### Ext.Vars
- [ ] `RegisterUserVariable(name, options)`
- [ ] `entity.Vars.VarName` access
- [ ] `SyncUserVariables()`
- [ ] `RegisterModVariable(uuid, name, options)`
- [ ] `GetModVariables(uuid)`
- [ ] `SyncModVariables([uuid])`

### Ext.Net
- [ ] `CreateChannel(moduleUUID, name)` / `Net.CreateChannel()`
- [ ] `channel:SetHandler(func)`
- [ ] `channel:SetRequestHandler(func)`
- [ ] `channel:SendToServer(data)`
- [ ] `channel:SendToClient(data, user)`
- [ ] `channel:Broadcast(data)`
- [ ] `channel:RequestToServer(data, callback)`
- [ ] `channel:RequestToClient(data, user, callback)`
- [ ] `IsHost()`

### Ext.UI
- [ ] `RegisterType(name, properties, wrappedType)`
- [ ] `Instantiate(typeName, [wrappedVM])`
- [ ] `GetRoot()`
- [ ] `GetCursorControl()` (v22+)
- [ ] `GetDragDrop()` (v22+)
- [ ] ViewModel property access
- [ ] Command handlers

### Ext.Math (all missing)
- [ ] Vector ops: Add, Sub, Mul, Div, Normalize, Cross, Dot, etc.
- [ ] Matrix ops: Inverse, Transpose, Rotate, Translate, Scale, etc.
- [ ] Construction: BuildRotation, BuildTranslation, etc.
- [ ] Decomposition: ExtractEulerAngles, Decompose, etc.
- [ ] Scalar: Clamp, Lerp, Fract, Sign, Acos, Asin, Atan, etc.

### Ext.Timer (all missing)
- [ ] `WaitFor(delay, callback)`
- [ ] `RegisterTimer(interval, callback)`
- [ ] `Cancel(timerId)`

### Ext.Input (all missing)
- [ ] `InjectKeyPress(key)`
- [ ] `InjectKeyDown(key)`
- [ ] `InjectKeyUp(key)`
- [ ] `GetInputManager()` (v23+)

### Ext.Level (all missing)
- [ ] `RaycastClosest`, `RaycastAny`, `RaycastAll`
- [ ] `SweepSphere*`, `SweepCapsule*`, `SweepBox*`, `SweepCylinder*`
- [ ] `TestBox`, `TestSphere`
- [ ] `GetActivePathfindingRequests()` (v22+)

### Ext.Mod
- [ ] `IsModLoaded(guid)`
- [ ] `GetLoadOrder()`
- [ ] `GetModInfo(guid)`

### Console
- [ ] Console window/REPL
- [ ] `client`/`server` context switching
- [ ] `reset` command
- [ ] Multiline mode
- [ ] `RegisterConsoleCommand(name, callback)`
- [ ] `!command` invocation
- [ ] `DumpExport(object)`

### Osi.* (mostly complete)
- [x] Dynamic metatable with lazy lookup
- [x] Query output parameters
- [x] Function type detection
- [x] Calls, Queries, Events, PROCs
- [x] `DB_*:Get()` (partial)
- [ ] `DB_*:Delete()` (verify)
- [x] `Ext.Osiris.RegisterListener()`

### Other
- [ ] PersistentVars
- [ ] Enum objects with Label/Value/EnumName
- [ ] Bitfield objects with operators
- [ ] Userdata lifetime scoping
- [ ] Virtual textures
- [ ] Debugger support
- [ ] IMGUI overlay

---

**Document Version:** 1.0
**Created:** 2025-12-03
**Author:** the CLI (gap analysis from BG3SE documentation audit)
