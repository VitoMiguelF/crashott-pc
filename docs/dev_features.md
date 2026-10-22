# Recursos DEV que sobraram no Crash of the Titans (Xbox 360, retail)

Levantamento de 2026-10-22: o que os desenvolvedores (Radical Entertainment) deixaram no executável e nos dados
do jogo que serve para automação de testes, mods e depuração, e o que o runtime do CrashRecomp expõe a partir
disso. Build do jogo, segundo o próprio jogo: `g_bakedBuildData` = "Built by: hmillar ... 2007-07-26",
`g_bakedP4Data` = 37008 (globais Lua).

Como usar está no README, "Recursos DEV (Lua, invencibilidade, teleporte...)". Código: `runtime/dev/`
(`dev_tools.cpp`, `dev_cheats.cpp`, `dev_library.lua.inc`). Provas: perfis `dev_*` do `config/run.toml`, capturas em
`build\dev\`.

## Ranking

Do mais útil para testes automatizados e mods ao menos útil.

| # | Recurso | Onde | Alcançável no retail? | Exposto pelo runtime | Provado em jogo |
|---|---|---|---|---|---|
| 1 | **Lua dentro do jogo** (compilador Lua inteiro no exe) | `luaL_loadbuffer` `sub_823C9530`, `lua_pcall` `sub_822995E8`, estado em `*(0x824FBFE8)+1056` | Não (nenhuma entrada) | `CRASH_DEV_LUA`, `CRASH_DEV_LUA_FILE`, `CRASH_DEV_LUA_AT`, `dev_lua*` | sim (`dev_api`) |
| 2 | **API C++ → Lua**: 1128 métodos de ~110 classes + ~720 enums | registro `sub_822D54A8` (métodos), `sub_822D5340` (propriedades) | Só pelos scripts | via Lua (`dev.*`) | sim |
| 3 | **Invencibilidade** | `CDamageableBehaviour::AdjustHitpoints` `sub_82110448` | Não (o enum `EInputMapEvent_GOD_MODE` = 42 existe, mas nenhum código o lê) | `CRASH_DEV_INVINCIBLE=1`, `dev_invincible` | sim (`dev_invencivel`) |
| 4 | **Teleporte** | `CPhysicsBehaviour.Teleport(Vector)` `sub_82142338`; `CObjective.DO_TeleportPlayer(locator, jogador)` | Só pelos scripts | `CRASH_DEV_TELEPORT=x,y,z`, `dev.teleport`, `dev.pos` | sim (`dev_teleporte`) |
| 5 | **Estatísticas** (mojo, mortes, combos, medalhas, idolos) | `CStatsManager` (`Managers.StatsManager`) | Só pelos scripts | `CRASH_DEV_MOJO=n`, `dev.give_mojo` | sim (`dev_mojo`, HUD mostra 500) |
| 6 | **Objetivos de designer** (`DO_*` / `WAIT_*`: spawn, skip point, unlocks, mensagens) | `CObjective`, 138 métodos; `NewObjective` | Só pelos scripts | `dev.objective(nome, fn)` | parcial (unlocks sim; `DO_SkipAhead` sem efeito na vila) |
| 7 | **Desbloquear** (as 20 skins do retail **+ a não usada Crash_Hulk = 22**; fases; golpes e upgrades) | `DO_UnlockSkin/Level/Move`; skins em `0x824A8218` (20 ints, -1 = livre) | Só pelos scripts | `CRASH_DEV_UNLOCK_ALL=1`, `dev.unlock_all({ESkin_Crash_Hulk})`, `dev.unlock_skin` | sim no estado do jogo (`dev_desbloquear`: lista `21 22 10 5 ... 23`); falta ver no menu de skins |
| 8 | **GameLink** (depurador remoto da Radical: `DumpValues`, `GetObjectChildren`, `GetFieldList`, `ToggleField`, `ReloadProps`, `ReloadRoom`, `ReloadNPCs`, `ScreenShot`, `PrintStack`, `CommandCompletion`, `GetNPCs`) | `script/gamelinkutil.lua` (o `init.lua` pula quando `RAD_RELEASE`) | Não | `CRASH_DEV_GAMELINK=1`, `dev.gamelink()` | carrega e lista a árvore de objetos; as outras funções esperam argumentos do PC (formato a descobrir) |
| 9 | **Cheats de aparência** (Big Head / Shadow Crash) | `SettingsDatabase` `m_CheatBigHeadCrash` (+94) / `m_CheatShadowCrash` (+93); verificador `sub_821DB680` no update do `CFrontendManager` | **Sim**: pausa, segura RT, X X Y A (Big Head) / Y X Y A (Shadow) | `CRASH_DEV_SHADOW=1`; Big Head pelo código | sim (`dev_sombra`, `dev_cheat_cabecao`) |
| 10 | **SettingsDatabase** (o "Watcher"/tweak da Radical: câmera, debug, memória, desenho, som) | instância estática `0x824A8B80`, classe Lua `getobj(":SettingsDatabase")` | Não | `dev.setting(nome, valor)` | Shadow sim; o resto ver tabela abaixo |
| 11 | **Fase escondida SoundTest** | índice 20 da lista de fases; `levels\SoundTest` | Não (nenhum menu leva lá) | `boot = "level:SoundTest"` + mod de teste | carrega e roda, cena preta (só HUD) |
| 12 | **TestLevels.lua** (script de automação dos devs: `TapControllerButton`, `LoadLevel`, `WaitForFrontEnd`, `ReturnToStart`) | carregado no boot por `CScriptManager::vf01` | Carrega, mas chama `testManager`, `SendControllerChange`, `TestGetMicroseconds`, `IsFrontEndLoaded`, que não existem no retail | não | não (morreria na primeira chamada) |
| 13 | **Pacotes de debug** (`levels\GlobalPackagesDebug.p3d`, `LevelPackagesDebug.p3d`, `NoHashDebug.p3d`) | escolhidos por `m_MemoryUseDebugPackages` (+84) em `sub_8224D488` | Não | via `dev.setting` antes do carregamento (não testado) | não |
| 14 | `print` do jogo / stdout | Lua `print` `sub_823CCF40` → `fputs` `sub_823D3410` | os scripts trocam `print` por função vazia no retail | `devprint` (o print original) e stdout do CRT no log `[lua] ...` | sim |

## 1. Lua dentro do jogo

O retail traz o compilador Lua completo (`loadstring`, mensagens de erro do parser), então código **fonte** roda no
estado Lua do jogo. O runtime roda cada bloco depois do update de scripts de cada quadro
(`CScriptManager::vf10`, `sub_82230DE0`), na thread do jogo e fora de qualquer chamada Lua.

Antes do primeiro bloco o runtime define:
- `devprint(...)`: o `print` original da biblioteca base (o jogo troca o global `print` por uma função vazia).
  Sai no log como `[lua] ...`;
- `_TRACEBACK`: o `luaG_errormsg` da Radical (`sub_822A2C28`) chama esse global quando não há `errfunc`; ele vem da
  biblioteca `debug`, que não existe no retail. Sem ele **todo erro de execução** virava "error in error handling"
  ou crash. O nosso registra `[lua] erro: <mensagem>` e devolve a mensagem, então `pcall` funciona;
- a biblioteca `dev.*` (abaixo).

O `currentline()` do jogo (`sub_822A1DB0`) foi trocado por uma versão que confere os ponteiros (só formata
mensagens de erro); o original lia um `CallInfo` velho e derrubava o jogo em erros vindos de métodos C++.

**Dialeto**: é "Lua 5.1 (work)" com cara de 5.0: **não tem `#`** (use `table.getn` ou `table.insert`) **nem `...` em
expressões** (use `arg`). Só existem as bibliotecas base, `string`, `table`, `math` e `coroutine` (sem `debug`, `io`,
`os`). Os `.lua`/`.blua` do jogo são bytecode no formato do Lua 5.0 (o cabeçalho diz 5.1): `tools/luadec.py`
desmonta e reconstrói todos (ver README, "Ler os scripts do jogo"). `luaV_execute` é `sub_8229E7A0` (switch de
36 opcodes em `0x8229E874`): os do 5.0 até `RETURN` (27), depois `FORLOOP` 28, `FORPREP` 29, `TFORLOOP` 30,
`TFORPREP` 31, `SETLIST` 32, `SETLISTO` 33, `CLOSE` 34, `CLOSURE` 35. `string.dump` (`sub_823CA8A8` ->
`luaU_dump` `sub_822A1CF0`) existe mas derruba o jogo em `DumpString` (`sub_822A1548`) até com
`string.dump(loadstring("return 1"))`.

**Acesso aos objetos**:

| O quê | Como |
|---|---|
| Gerentes | `Managers.StatsManager`, `.SaveGameManager`, `.CameraManager`, `.FrontendManager`, `.WorldManager`, `.Sound`, `.GameDatabase` |
| Objetos por nome | `getobj(":SettingsDatabase")`, `getobj(":UberManager")`, `getobj(":objectManager")`, `getobj(":Streamer")` |
| Jogador | `Managers.GameDatabase:GetCurrentPlayer(0)` (ator) → `.m_BehaviourController` (o `Crash`) → `.m_DamageableBehaviour`, `.m_PhysicsBehaviour`, ... |
| Vetores | `tovector(x, y, z)`; campos `.x .y .z` |

**Riscos**: os métodos nativos não conferem tipos nem número de argumentos. `sm:GetValueInt(EStatInt_MojoCount)`
(falta o primeiro argumento) ou `ator:m_BehaviourController()` (é campo, não método) **derrubam o jogo** com
access violation, sem passar pelo `pcall`. Confira a assinatura na lista de métodos antes.

## 2. A API que o C++ expõe ao Lua

Tirada de duas formas: em jogo (`tools/dev_lua/dump_globals.lua`, perfil `dev_api`: 1298 globais) e no código
(cada método registrado por `sub_822D54A8(classe, nome, binding)`, cujo template RTTI
`script::MethodN<Classe, Retorno, Args...>` dá a assinatura: 1128 métodos/propriedades). As mais úteis:

| Classe | Métodos (assinatura) |
|---|---|
| `CGameDatabase` | `GetCurrentPlayer(int)`, `GetCurrentCamera(int)`, `SetDifficultyLevel(EDifficultyLevel)` |
| `CUberManager` | `SetLevelIndex(int)`, `GetLevelName(int, bool)`, `StartPreload(int)`, `StartPreloadLevelByName(str)`, `GameTimeGetMilliseconds()`, `HaltGame(bool, bool)`, `ForceReload()` |
| `Frontend` | `StartLevel(int)`, `StartLevelByName(str)`, `StartNextLevel()`, `PauseGame(bool)`, `SetEpisodeComplete(bool)`, `CheckSkinToView(ESkin)`, `SelectSkin()` |
| `CStatsManager` | `GetValueInt(EStatClass, EStatInt)`, `IncrementValueInt(EStatInt, n)`, `SetValueBool(EStatBool, bool)`, `SetEpisodeMedal(str, EEpisodeMedal)` |
| `CDamageableBehaviour` | `TakeDamage(float, ator)`, `SetCurrentHitpoints(float)`, `IsDead()`, `AllowDamageType(bool, str)` |
| `CPhysicsBehaviour` | `Teleport(Vector)` e mais 45 |
| `CActor` | `GetTransformRow(int)` (linha 3 = posição), `SetActorName(str)`, `PrepareForDelete(bool)` |
| `CSaveGameManager` | `SaveGame(slot)`, `LoadGame(slot)`, `AutoSaveGame()`, `IsSlotEmpty(slot)` |
| `CCameraManager` | `ScriptableCameraEnable(bool)`, `ScriptableCameraPosition(x,y,z)`, `ScriptableCameraLookat(x,y,z)`, `SetCameraFOV(...)` |
| `CObjective` | 138: `DO_TeleportPlayer`, `DO_SkipAhead`, `DO_SetSkipPoint`, `DO_SpawnEnemy`, `DO_SpawnEnemyFromGameLink`, `DO_SpawnPropFromGameLink`, `DO_KillPlayerFromGameLink`, `DO_Unlock*`, `DO_StatSetInt`, `DO_DisplayMessageDebug`, `DO_LockAllControllers`, `DO_Sequencer_*` (move o jogador por roteiro: `MoveToPosition`, `Input`, `InputString`), `WAIT_*` |

A lista completa (privada, derivada do jogo) sai do `dev_api` no `run.log`.

## 3. Invencibilidade

Todo dano passa por `AdjustHitpoints` (`sub_82110448(this, delta, damager)`: golpes dos inimigos pelo
`CDamageableBehaviour::vf09`, dano contínuo `vf15`, `TakeDamage`, `SetCurrentHitpoints`). HP em `*(this+64)`,
máximo em `*(this+68)`. Com `CRASH_DEV_INVINCIBLE=1` o delta negativo vira 0 quando o ator do behaviour (`+28`) é
um dos jogadores (`CGameDatabase` estático em `0x824A76A4`, `+16`/`+20`). Log: `[dev] invincible: blocked N damage`.
Não cobre mortes que não passam por HP (cair num poço / `CDeathTriggerVolumeBehaviour`): ver follow-ups.

## 4. Cheats originais e o SettingsDatabase

`CSettingsDatabase` (instância `0x824A8B80`, sub-objetos por categoria com acessores `sub_82233D70` etc.) é o
banco de "tweaks" da Radical, exposto ao Lua por nome. O C++ do retail ainda lê:

| Campo | Lido por | Efeito |
|---|---|---|
| `m_CheatShadowCrash` (+93) | `sub_820A3DC8`, `sub_82223AC8` | **Shadow Crash** (provado só ligando o campo) |
| `m_CheatBigHeadCrash` (+94) | `sub_820B8F80`, `sub_820B90F8`, verificador | **Big Head**; só ligar o campo não basta, o verificador aplica a escala nos jogadores no toggle (código: pausa + RT + X X Y A, provado) |
| `m_SlowMotion` (+80, 1.0) | câmera `sub_820E0498`, `CUberManager::vf10` | aceita o valor; o relógio `GameTimeGetMilliseconds` não muda, efeito não comprovado |
| `m_MemoryUseDebugPackages` (+84) | `sub_8224D488`, `sub_8224E068` | carrega `GlobalPackagesDebug.p3d` no lugar de `GlobalPackages.p3d` (não testado) |
| `m_DrawPlayerPosition` (+100), `m_DrawRoomNumber` (+92), `m_MemoryDrawHUD` (+71) | `sub_82143BB8`, `sub_82250510`, `CMemoryManager::vf10` | o desenho de texto de debug parece ter sido compilado fora (não testado) |
| `m_UseDevToolMemory` (+85), `m_MemoryUseBudgeting` (+75), `m_MemoryDoASyncLoading` (+69) | gerência de memória | arriscado |
| `m_CameraFOV`, `m_CameraBlendTransition`, `m_CameraAspect` | câmera | tunáveis |
| `m_PAL`, `m_Nordic`, `m_Sound` | vídeo, idioma, som | — |

Sem leitor no C++ (só existem no Lua): `m_DebugDecisionTreeLoad`, `m_SelectButtonDebugCamera`,
`m_ManualCameraController`, `m_DrawFrameRate`, `m_PrintActorMemUsage`, `m_PrintDeterminismGameState`,
`m_FileMonitor`, `m_LockToVSync`, `m_DrawHUD`, `m_Render*`.

O verificador de cheats (`sub_821DB680`) roda no update do `CFrontendManager` com o jogo pausado (bit 0x10 de
`frontend+16653` = "pausado" do quadro anterior) e usa o mapa de input `EInputMapType_CHEAT` (3): ACCEPT (RT) +
LEFT → Shadow, ACCEPT + RIGHT → Big Head. Nenhum outro evento é lido; `GOD_MODE` (42) e `RESURRECT` (32) estão
no enum sem código.

## 5. Skins

O enum `ESkin` tem 25 valores. O jogo desbloqueia 20 (lista em `0x824A8218`, exatamente 20 posições): o Crash
(21), 17 skins de inimigos por contagem de "jacks"/derrotas (estatísticas `EStatInt_*Jacked` / `*Deaths`, funções
`Unlock<X>Skin` do `objectives.lua`), Halloween (24, 50% do jogo) e Valentine (23, todos os idolos de ouro).
Cortex (1), Yuktopus (18), Uka Uka (19), Nina SpiderBot (20) e **Crash_Hulk (22)** nunca são desbloqueadas; 18-20
nem têm nome na tabela da ação (crash). A Crash_Hulk tem nome e boneco vodu no menu (`c_voodooFE_ball_crashhulk`)
e entra na lista com `dev.unlock_skin(ESkin_Crash_Hulk)`.

## 6. Outros achados

- **Fases fora da campanha** no disco: `levels\SoundTest` (está na lista do jogo como índice 20; faltam
  `spawn_normal.blua` e `cameraoverrides.blua`, e sem eles o carregamento derruba o jogo; com um mod que traz os
  dois como Lua vazio ela carrega: `private\assets\test_mods\dev\soundtest`), `levels\AI_pathfind` e
  `levels\CombatArena01` (só cascas: skybox e câmeras, fora da lista). O dicionário de nomes cita ainda
  `TestLevel`, `TestLevel2`, `Arttest01` e `cinematics\crash_test.p3d`, que não estão no RCF.
- **Props de teste** (`prp_TestRamp`, `prp_MoveTest`, `RyanTestPropTemplate`...) em `NonSpawnableTemplateActor`.
- **Globais de build**: `RAD_RELEASE = true`, `RAD_DEBUG = RAD_TUNE = false`, `_BREAK`, `breakpoint`, `g_bLiveTuning`.
- **Frontend**: nenhuma página de debug; só os restos de Wii/PS2 já conhecidos e o `ExitShowXenonDeviceSelect`.
- **Textos**: nada de cheat/debug nos bibles.
- **printf/DbgPrint**: só 3 chamadas de `DbgPrint` (CRT/D3D); o log de debug do jogo foi compilado fora. O que
  sobra é o stdout do CRT (Lua `print`), agora no log.

## Riscos

- Métodos nativos com argumentos errados derrubam o jogo (sem validação). Erros **Lua** são seguros agora.
- `m_UseDevToolMemory` / `m_MemoryUseBudgeting` / pacotes de debug mexem na alocação: não use em jogo normal.
- Os blocos rodam no estado do jogo: variáveis globais criadas ficam até sair da fase.
- `dev.unlock_all` mexe no progresso: só em saves de teste (os perfis usam `tests\userdata_base`).
- Objetivos (`NewObjective`) têm limites que derrubam o jogo ou somem em silêncio: um objetivo de ~200 passos
  nunca roda; 9 objetivos criados no mesmo quadro crasham (`sub_82200E60`); valores de enum sem nome na tabela
  da ação crasham (skins 18 Yuktopus, 19 Uka Uka, 20 Nina; concept art 20-24; enemy info 1 Cortex). A lista de
  skins guarda 20: com a Crash_Hulk a última do retail (Ratcicle) não cabe.

## Follow-ups (promissores, não terminados)

1. ~~**Decompilador dos `.blua`**~~: feito em `tools/luadec.py` (2026-10-22); os 723 scripts reconstruídos compilam
   no compilador do jogo (perfil `dev_luadec`).
2. **Implementar o `testManager`** que o `TestLevels.lua` espera (`SendControllerChange`, `TestGetMicroseconds`,
   `IsFrontEndLoaded`) em cima do input do runtime: os scripts de teste dos devs voltariam a rodar.
3. **GameLink completo**: descobrir o formato dos argumentos (ids `@@OBJ:n@@`) de `GetFieldList`/`DumpValues`/
   `ToggleField` e ligar um console (pipe/porta local) para mandar Lua com o jogo aberto, como o GameLink do PC.
4. **Desbloquear o resto** (play zones, mojo rooms, concept art 0-19, enemy info, tutoriais): objetivos pequenos, um
   por bloco, devem funcionar (não verificado); e ler o estado do save para provar.
5. **Skin Crash_Hulk em jogo**: confirmar no menu de skins depois do `dev.unlock_all` e achar como a skin
   escolhida é aplicada (`Frontend:SelectSkin`, `Crash.CheckSkinType`).
6. **Big Head só pela flag**: chamar a mesma aplicação de escala do verificador (`sub_821DB2A0` e o laço dos jogadores)
   para um `CRASH_DEV_BIGHEAD`.
7. **Invencibilidade contra poços**: hook no `CDeathTriggerVolumeBehaviour` / respawn.
8. **Teleporte para checkpoints**: listar os locators de cada fase (`CWorldManager`, skip points) e usar
   `DO_TeleportPlayer` / `DO_SkipAhead`; o skip ahead não fez nada na vila do L1_E1.
9. **Slow motion / frame step**: entender o que `m_SlowMotion` escala no `CUberManager::vf10`.
10. **Desenho de debug** (`m_DrawPlayerPosition`, `m_DrawRoomNumber`, `%.2fms (%2.0ffps)`): ver se o caminho até o texto
   ainda existe e só falta a função de print.
11. **SoundTest/CombatArena01**: ver se há geometria (câmera livre) ou só o teste de som.
