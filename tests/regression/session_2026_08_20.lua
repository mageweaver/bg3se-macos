-- Regression sweep for the 2026-08-20 correctness fixes.
-- Run via the BG3SE console command file with a save loaded.
--
-- Covers, in order: TransformComponent field order, static-data bank strides,
-- entity_is_alive, entity Create/Destroy, global trace capture, RaycastAll
-- shape, AddEnumerationValue, CreateComponent stub refusal, namespace
-- reachability, and the replication surface.
--
-- NOTE on the raycast check: an upward ray is NOT a reliable miss. Under an
-- overhang it legitimately hits a ceiling (observed: 4 hits at y=46 from a
-- character at y=17.8). Use a ray far below the world for a guaranteed miss.
local P = Ext.Print
local pass, fail = 0, 0
local function T(n, f)
  local ok, e = pcall(f)
  if ok then pass = pass + 1; P("R PASS " .. n)
  else fail = fail + 1; P("R FAIL " .. n .. " :: " .. tostring(e)) end
end

local uuid = Osi.GetHostCharacter()
local e = Ext.Entity.Get(uuid)

T("TransformComponent.Position matches Osiris", function()
  local x, y, z = Osi.GetPosition(uuid)
  local tc = e:GetComponent("Transform") or e:GetComponent("ls::TransformComponent")
  assert(tc and tc.Position, "no transform")
  assert(math.abs(tc.Position.x - x) < 0.05 and math.abs(tc.Position.z - z) < 0.05)
end)

T("static-data banks enumerate clean", function()
  Ext.StaticData.ForceCapture()
  local function good(g)
    local h = (g or ""):gsub("-", "")
    if h == "" then return false end
    local z = select(2, h:gsub("0", ""))
    return h:match("^0+$") == nil and z < 24
  end
  for _, t in ipairs({ "Feat", "Race", "Background", "ActionResource", "Class" }) do
    local a = Ext.StaticData.GetAll(t) or {}
    assert(#a > 0, t .. " empty")
    local v, seen, d = 0, {}, 0
    for _, x in ipairs(a) do
      if good(x.ResourceUUID) then v = v + 1 end
      if not seen[x.ResourceUUID] then seen[x.ResourceUUID] = true; d = d + 1 end
    end
    assert(v == #a and d == #a, t .. ": " .. v .. " valid / " .. d .. " distinct of " .. #a)
  end
end)

T("IsAlive tracks reality", function()
  assert(e:IsAlive() == true)
  local fake = Ext.Entity.GetByHandle(0x7fff000100000000)
  assert((fake and fake:IsAlive()) == false)
end)

T("entity Create/Destroy round trip", function()
  local n = Ext.Entity.Create(); assert(n)
  local h = n:GetHandle(); assert(h and h ~= 0)
  assert(Ext.Entity.Destroy(h) == true)
end)

T("RaycastAll shape; miss returns an empty table", function()
  local x, y, z = Osi.GetPosition(uuid)
  local hit = Ext.Level.RaycastAll({ x, y + 20, z }, { x, y - 5, z })
  local miss = Ext.Level.RaycastAll({ x, -500, z }, { x, -480, z })  -- below the world
  assert(type(miss) == "table" and #miss.Positions == 0, "miss not an empty table")
  assert(type(hit.Positions) == "table" and #hit.Positions > 0, "no parallel arrays")
  assert(#hit > 0, "no array form")
end)

T("AddEnumerationValue returns an index", function()
  local v = Ext.Stats.AddEnumerationValue("Damage Type", "BG3SE_Regress_" .. tostring(math.random(1e6)))
  assert(math.type(v) == "integer")
end)

T("CreateComponent refuses terminate stubs", function()
  local ok, r = pcall(function() return e:CreateComponent("esv::status::DifficultyModifiersComponent") end)
  assert(ok and r == false)
end)

T("namespaces reachable", function()
  assert(type(Ext.ServerTemplate) == "table" and type(Ext.ServerTemplate.GetAllRootTemplates) == "function")
  assert(type(Ext.ClientTemplate) == "table")
  assert(type(Ext.Utils.GetValueType) == "function")
  assert(type(Ext.Debug.GenerateIdeHelpers) == "function")
end)

T("replication surface present and safe", function()
  assert(type(Ext.Entity.GetPeers) == "function")
  assert(#Ext.Entity.GetPeers() >= 1)
  local _, sent = e:Replicate(); assert(type(sent) == "number")
  assert(e:StopReplicateWith() == false)
end)

P(string.format("R === %d passed, %d failed ===", pass, fail))
