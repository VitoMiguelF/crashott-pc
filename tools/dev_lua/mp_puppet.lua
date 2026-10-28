-- Profile mp_marionete (config/run.toml, docs/internals/multiplayer.md): Crash-looking actors that
-- are not players (class Cheerleader: dialogue + fight tree + render, as Cortex / Tiny / N.Gin in
-- the cutscenes) next to player 1 in the village.
@gameplay_ready+1
function mp_puppet(name, subtype, x, z)
  local p = dev.player(0):GetTransformRow(3)
  local pos = tovector(p.x + x, p.y, p.z + z)
  local rot = tovector(0, 0, 0)
  local scale = tovector(1, 1, 1)
  devprint("mp puppet " .. name .. " (" .. subtype .. ") ...")
  local actor, bc = newActor(Cheerleader, ".:", name, pos, rot, scale, function(bc)
    SetBehaviourDataCheerleader(bc, "obj:Crash", "", "Cheerleader", subtype, "Ground", ERenderObjectType_CHARACTER, "crash", EShadowType_NONE, "", false)
  end, nil, false, true)
  pos:release() rot:release() scale:release()
  devprint("mp puppet " .. name .. " made: " .. tostring(exists(actor)))
  return actor
end
@gameplay_ready+2
mp_puppet("mp_puppet_1", "Crash", 3, 0)
@gameplay_ready+4
mp_puppet("mp_puppet_2", "Cortex", -3, 0)
@gameplay_ready+6
local a = objectManager:lookupObject(":Streamer"):GetActorByName("mp_puppet_1")
if exists(a) then local v = a:GetTransformRow(3) devprint("mp puppet 1 at " .. v.x .. " " .. v.y .. " " .. v.z) end
