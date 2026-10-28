-- Profile mp_host (config/run.toml, docs/internals/multiplayer.md): the host's player 1 walks in the
-- village, then is teleported to L1_E1_R2 so the client's mirror has a big, visible move.
@gameplay_ready+9
dev.teleport(220, 60, 46, 0)
@gameplay_ready+14
dev.pos(0)
