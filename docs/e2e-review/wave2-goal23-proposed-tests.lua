-- Proposed tier-1/tier-2 coverage for Wave 2, Goal 2.3.
-- These are intentionally not wired into src/lua/lua_ext.c.

BG3SE_AddTest(1, "Stats.Goal23.HonestSurface", function()
  AssertType(Ext.Stats.GetStatsLoadedMods, "function", "GetStatsLoadedMods")
  AssertType(Ext.Stats.GetStatsLoadedBefore, "function", "GetStatsLoadedBefore")
  AssertType(Ext.Stats.TreasureTable.Get, "function", "TreasureTable.Get")
  AssertType(Ext.Stats.TreasureTable.GetLegacy, "function",
    "TreasureTable.GetLegacy")
  AssertType(Ext.Stats.TreasureCategory.GetLegacy, "function",
    "TreasureCategory.GetLegacy")

  -- These mutation variants are deliberately gated until their allocator and
  -- container ABIs are proven. They must not claim success.
  assert(Ext.Stats.AddAttribute(
    "Weapon", "BG3SE_Goal23_MustNotExist", "FixedString") == false,
    "allocator-gated AddAttribute must return false")
  assert(Ext.Stats.AddEnumerationValue(
    "DamageType", "BG3SE_Goal23_MustNotExist") == nil,
    "allocator-gated AddEnumerationValue must return nil")

  assert(Ext.Stats.TreasureTable.Get(
    "BG3SE_Goal23_MissingTreasureTable") == nil,
    "unknown treasure table must return nil")
  assert(Ext.Stats.TreasureCategory.GetLegacy(
    "BG3SE_Goal23_MissingTreasureCategory") == nil,
    "unknown treasure category must return nil")
end)

BG3SE_AddTest(1, "Stats.Goal23.ModuleLoadOrder", function()
  local loaded = Ext.Stats.GetStatsLoadedMods()
  AssertType(loaded, "table", "GetStatsLoadedMods result")
  assert(#loaded > 0, "expected at least the base module in load order")

  local base = Ext.Mod.GetBaseMod()
  AssertNotNil(base, "base module")
  local baseUuid = base.UUID or (base.Info and base.Info.ModuleUUID)
  AssertType(baseUuid, "string", "base module UUID")

  local throughBase = Ext.Stats.GetStatsLoadedBefore(baseUuid)
  AssertType(throughBase, "table", "GetStatsLoadedBefore result")
  assert(#throughBase > 0, "base-module boundary should be inclusive")
  assert(throughBase[#throughBase] == baseUuid,
    "load-order prefix must end at the requested module")

  for i, moduleId in ipairs(throughBase) do
    assert(moduleId == loaded[i],
      "GetStatsLoadedBefore must be a prefix of GetStatsLoadedMods")
  end
end)

BG3SE_AddTest(2, "Stats.Goal23.TreasureReads", function()
  -- Both fixtures are defined in
  -- Public/Shared/Stats/Generated/TreasureTable.txt.
  local tableInfo = Ext.Stats.TreasureTable.Get("Gold_Meager")
  AssertNotNil(tableInfo, "Gold_Meager treasure table")
  assert(tableInfo.Name == "Gold_Meager", "treasure table name mismatch")
  AssertType(tableInfo.Address, "number", "treasure table address")
  AssertType(tableInfo.MinLevel, "number", "treasure table MinLevel")
  AssertType(tableInfo.MaxLevel, "number", "treasure table MaxLevel")
  AssertType(tableInfo.SubTables, "table", "treasure subtables")
  assert(#tableInfo.SubTables > 0, "Gold_Meager should have a subtable")
  AssertType(tableInfo.SubTables[1].TotalCount, "number",
    "treasure subtable TotalCount")

  local legacy = Ext.Stats.TreasureTable.GetLegacy("Gold_Meager")
  AssertNotNil(legacy, "legacy Gold_Meager read")
  assert(legacy.Address == tableInfo.Address,
    "Get and GetLegacy should resolve the same manager entry")

  local category = Ext.Stats.TreasureCategory.GetLegacy("I_OBJ_GoldCoin")
  AssertNotNil(category, "I_OBJ_GoldCoin treasure category")
  assert(category.Category == "I_OBJ_GoldCoin",
    "treasure category name mismatch")
  AssertType(category.Address, "number", "treasure category address")
  AssertType(category.Items, "table", "treasure category items")
end)

BG3SE_AddTest(2, "Stats.Goal23.PrototypeSyncHonesty", function()
  local status = Ext.Stats.Get("BURNING")
  AssertNotNil(status, "BURNING StatusData fixture")
  assert(Ext.Stats.Sync("BURNING") == true,
    "status sync should report success when StatusPrototype::Init is mapped")
  AssertNotNil(Ext.Stats.GetCachedStatus("BURNING"),
    "status sync should refresh an accessible cached prototype")

  local passive = Ext.Stats.Create(
    "BG3SE_Goal23_GatedPassive", "PassiveData")
  AssertNotNil(passive, "temporary PassiveData stat")
  assert(Ext.Stats.Sync("BG3SE_Goal23_GatedPassive") == false,
    "PassivePrototype::Init is absent; sync must fail honestly")
end)
