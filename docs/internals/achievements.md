# Achievements (Xbox 360)

Code: `runtime/achievements/` (`achievements.cpp` data, store, input, sounds; `achievements_draw.cpp` hint, screen,
popup, `--achievements-preview`). Full write-up (PT): `runtime/NOTES.md` "Conquistas do Xbox 360"; GUIA
"Conquistas do Xbox 360". Profiles: `conquistas`, `conquistas_menu`, `conquistas_hub`.

## Where the game awards them

- No `XUserWriteAchievements` / `XamUserCreateAchievementEnumerator` / `XUserReadAchievements` import: the game
  never reads achievements back.
- The title's `XUserWriteAchievements` (`sub_822649A0(count, XUSER_ACHIEVEMENT*, XOVERLAPPED*)`) is
  `XMsgStartIORequest(app 0xFB, message 0x000B0008, overlapped, { u32 count, u32 ptr }, 8)`. Each entry is
  `{ u32 user index, u32 achievement id }`. `kernel/xmsg_imports.cpp` hands it to `achievements::WriteMessage`; the
  overlapped completes as before.
- Caller: `CObjectiveManager::vf10` (`sub_8220A7A0`) pops a queue at `this+252` (28-byte entries: 24 bytes of
  `XUSER_ACHIEVEMENT`s + count) filled by `sub_8220A708(objectives, XUSER_ACHIEVEMENT*, count)` (9 call sites).
  `objectives = sub_821FA440(sub_821FB4F0(), 10)`. `sub_8220A708` skips when bit 0x80 of byte 352 of
  `sub_821FB4F0()` is set.
- The console's `achievement <id>` calls `sub_8220A708` with a runtime buffer (`GuestAlloc`), so it goes through
  the game's own path and back through the XMsg.

## The SPA

- Resource `565507E4` in the XEX resource header (0x000002FF): 614 921 bytes at 0x82530000 in the loaded image,
  read in place (`g_memory.Translate`), never copied to disk.
- XDBF: header `{ 'XDBF', version, entry table length, entry count, free table length, free count }`, entries
  `{ u16 namespace, u64 id, u32 offset, u32 length }` (18 bytes), free table (8 bytes each), data.
  Namespace 1: 'XACH' (achievements), 'XSTC' (default language = 1). Namespace 2: PNG images by id (52).
  Namespace 3: 'XSTR' string tables by language: 1 English, 3 German, 4 French, 5 Spanish, 6 Italian.
- 'XACH' / 'XSTR': `{ magic, version, size, u16 count }`. Achievement record (36 bytes): `u16 id, label,
  description, unachieved; u32 image; u16 gamerscore, ?; u32 flags; 16 bytes`. Flags 0x8 = shown while locked;
  7 achievements lack it (secret). 47 achievements, 1000 G.
- Text language: the game's (`settings::StartupLanguage`); Portuguese and Dutch use English.

## Store and UI

- `<userdata>/achievements.toml`, `[unlocked]` `id = "YYYY-MM-DD HH:MM:SS"` (local time) + the name as a comment.
  Universal (not in the saves). A repeated write is ignored.
- Popup: render thread, its own bitmap channel (`BITMAP_ACHIEVEMENT_POPUP`), bottom centre, 5.5 s with fades.
  Sound: `FE_SFX` "select" through the frontend sound player, queued when the popup appears and played on the game
  thread (`CScriptManager::vf10` hook in `dev_tools.cpp` and the frontend condition hook in `frontend_hooks.cpp`).
- Screen: opened with BACK on nodes 70 (main menu), 575 (HUB), 1081 / 1109 / 1134 (pause), not while another
  runtime screen has the pad, and not when the online HUB panel hides BACK (`mp::menu::HiddenButtons`). BACK is
  removed from the game's pad on those nodes. Keyboard: Tab is the menus' Y (`pc_input`), so the Map action's keys
  are read raw (`KeyHeld`) on the main menu and the pause; on the HUB Tab is CONTINUE STORY and does not open it.
- Hint `(BACK) ACHIEVEMENTS` (`BITMAP_ACHIEVEMENT_HINT`) at the bottom centre of the main menu and the pause, between
  the game's own prompts; a click / tap on it opens the screen (Android: the touch is the mouse in menus).
