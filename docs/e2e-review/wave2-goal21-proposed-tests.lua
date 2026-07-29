-- Proposed tier-2 coverage for Wave 2, Goal 2.1.
-- These are intentionally not wired into src/lua/lua_ext.c.

local function Goal21_FirstComponent(componentName)
  local entities = Ext.Entity.GetAllEntitiesWithComponent(componentName)
  for _, entity in ipairs(entities) do
    local component = entity:GetComponent(componentName)
    if component ~= nil then
      return component
    end
  end
  return nil
end

BG3SE_AddTest(2, "Entity.ComponentWrite.HealthHpRoundTrip", function()
  local entity = Ext.Entity.Get(Osi.GetHostCharacter())
  AssertNotNil(entity, "host entity")

  local health = entity:GetComponent("eoc::HealthComponent")
  AssertNotNil(health, "host HealthComponent")

  local original = health.Hp
  AssertType(original, "number", "original HealthComponent.Hp")
  local candidate = original > 1 and original - 1 or original + 1

  local writeOk, writeErr = pcall(function()
    health.Hp = candidate
  end)
  local observed = health.Hp

  -- Restore even when the round-trip assertion would fail.
  local restoreOk, restoreErr = pcall(function()
    health.Hp = original
  end)

  assert(restoreOk, "HealthComponent.Hp restore failed: " .. tostring(restoreErr))
  assert(writeOk, "HealthComponent.Hp write failed: " .. tostring(writeErr))
  assert(observed == candidate, "HealthComponent.Hp did not round-trip")
  assert(health.Hp == original, "HealthComponent.Hp was not restored")
end)

BG3SE_AddTest(2, "Entity.ComponentWrite.FixedStringRefused", function()
  local componentName = "esv::OriginalTemplateComponent"
  local originalTemplate = Goal21_FirstComponent(componentName)
  AssertNotNil(originalTemplate, componentName .. " fixture")

  local before = originalTemplate.TemplateId
  local ok, err = pcall(function()
    originalTemplate.TemplateId = "Goal21_MustNotIntern"
  end)

  assert(not ok, "FixedString write should raise a Lua error")
  AssertType(err, "string", "FixedString refusal error")
  assert(originalTemplate.TemplateId == before,
    "refused FixedString write changed game memory")
end)

BG3SE_AddTest(2, "Entity.ComponentWrite.OneFrameRefused", function()
  -- Run this proposed fixture immediately after a save completes, while the
  -- one-frame event component is present.
  local componentName = "esv::SaveCompletedOneFrameComponent"
  local oneFrame = Goal21_FirstComponent(componentName)
  AssertNotNil(oneFrame, componentName .. " fixture")

  local ok, err = pcall(function()
    oneFrame.Value = not oneFrame.Value
  end)

  assert(not ok, "OneFrame component write should raise a Lua error")
  AssertType(err, "string", "OneFrame refusal error")
end)
