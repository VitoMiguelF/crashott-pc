-- Profiles mp_stream_* (config/run.toml, docs/internals/multiplayer.md). CRASH_MP_ANCHOR keeps a
-- stand-in for a remote player at the village start (room L1_E1_R1). Player 1 goes R2 (adjacent to
-- R1) -> R4 (adjacent to R2) -> R5 (adjacent to R4), each on the floor next to one of the level's
-- reset locators. The game's resident set (current room R1 + R2) holds only the first.
@gameplay_ready+3
dev.teleport(220, 60, 46, 0)
@gameplay_ready+6
dev.pos(0)
dev.teleport(-3.9, 66, 377.6, 0)
@gameplay_ready+9
dev.pos(0)
@gameplay_ready+10
dev.teleport(-194.6, 78, 239.3, 0)
@gameplay_ready+13
dev.pos(0)
@gameplay_ready+16
dev.pos(0)
