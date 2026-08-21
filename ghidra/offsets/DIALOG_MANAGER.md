# Ext.Utils.GetDialogManager — assessment

Investigated 2026-08-20.

## What Windows does

    dlg::DialogManager* GetDialogManager(lua_State* L)
    {
        auto ds = ...GetEntitySystemHelpers()->GetSystem<esv::DialogSystem>();
        return ds ? ds->GameInterface.DialogManager : nullptr;
    }

Server context only; returns nullptr on the client.

## Step 1 is solved

The system resolves the same way `esv::replication::ReplicationSystem` does,
which is already implemented in `replication_system.c`:

    ls::TypeId<esv::DialogSystem, ecs::SystemsContext>::m_TypeIndex  @ 0x10891a888

(`ecl::DialogSystem` also exists, and `ls::TypeContext<ecs::SystemsContext>::
RegisterType<esv::DialogSystem>` confirms it is registered in the systems
context.) Read the index there, index the world's system array at stride 0xf8,
instance at entry+0x00 — the exact path `replication_system_get` already uses.

## Why it is still not worth doing

Two problems remain, and the second is the blocker.

1. `GameInterface.DialogManager`'s offset within `esv::DialogSystem` is unknown.
   `esv::DialogSystem` exports no methods, so there is no function to read the
   offset from; it would need a caller scan or field probing.

2. `DialogManager` is not an opaque handle. Windows exposes it as a Lua object
   with roughly twenty fields, most of them `LegacyRefMap`s of complex nested
   types — `DialogInstance*`, `FlagDescription*`, `INodeConstructor*`,
   `ScriptFlag`, `Variable`, `SpeakerGroup`, `ActorRefCount`. Returning the
   pointer alone gives mod code nothing; parity means porting that whole object
   graph.

That is the same scale as `GetGlobalSwitches`, and carries the same risk: those
layouts come from Windows headers, and `GlobalSwitches` has already been proven
not to transfer (SkipSplashScreen at 0x6AC here vs 0x13E6 there). A field-by-field
port would be guesswork that fails silently.

## Recorded so a future attempt starts ahead

The system TypeId above removes the first unknown. What would make the rest
cheap is the same thing `GlobalSwitches` needs: a registration or reflection
table mapping field names to offsets, of the kind that produced the 666-entry
RemoveComponent table and the ReplicatedTypeContext globals.
