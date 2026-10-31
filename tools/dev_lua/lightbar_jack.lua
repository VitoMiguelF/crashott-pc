-- Profile barra_de_luz (config/run.toml, kernel/pad_lights.h): Crash is knocked out (the light bar blinks
-- red) and comes back; then a Spike is spawned (NIS_20, its L1_E1 spot; the level pre-spawns its
-- template) and put in front of Crash; the profile's X stuns it (CRASH_DEV_ONE_HIT_TITANS) and B jacks it
-- (the titan's colour), START pauses (breathing).
@gameplay_ready+1
devprint("lightbar ko")
dev.damage(1000)
@gameplay_ready+12
dev.objective("lightbar_spike", function(o) o:DO_SpawnEnemy("LightbarGroup", "LightbarSpike", "Characters:Spike", "NIS_20", "", 20, false) end)
@gameplay_ready+14
local streamer = objectManager:lookupObject(":Streamer")
local a = streamer:GetActorByName("LightbarSpike") or streamer:GetActorByName("ref_LightbarSpike")
if a ~= nil then
  local p = dev.pos()
  -- Crash looks along -z at the level start and after a respawn there (the camera behind him).
  a.m_BehaviourController.m_PhysicsBehaviour:Teleport(tovector(p.x, p.y + 0.5, p.z - 2.5))
  local v = a:GetTransformRow(3)
  devprint("lightbar spike at " .. v.x .. " " .. v.y .. " " .. v.z)
else
  devprint("lightbar no spike")
end
