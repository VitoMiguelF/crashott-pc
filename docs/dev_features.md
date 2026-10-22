# Recursos DEV que sobraram no Crash of the Titans (Xbox 360, retail)

Levantamento de 2026-10-22: o que os desenvolvedores (Radical Entertainment) deixaram no executável e nos dados
do jogo que serve para automação de testes, mods e depuração, e o que o runtime do CrashRecomp expõe a partir
disso. Build do jogo, segundo o próprio jogo: `g_bakedBuildData` = "Built by: hmillar ... 2007-07-26",
`g_bakedP4Data` = 37008 (globais Lua).

Como usar está no guia (docs/GUIA_DO_PROJETO.md), "Recursos DEV (Lua, invencibilidade, teleporte...)" e "Console
de desenvolvedor". Código: `runtime/dev/` (`dev_tools.cpp`, `dev_cheats.cpp`, `dev_library.lua.inc`, `dev_console.cpp`,
`dev_skins.cpp`). Provas: perfis `dev_*` e `console_*` do `config/run.toml`, capturas em `build\dev\` e `build\console\`.

**Console de desenvolvedor** (2026-10-22): tudo isso com o jogo aberto, por comandos (`unlock all`, `mojo 500`,
`god on`, `tp`, `pos`, `shadow`, `bighead`, `skin <nome> [now]`, `level <nome>`, `reload`, `lua <código>`, `help`).
Desligado por padrão: SETTINGS > DEFINITIVE EDITION > CONSOLE DE DESENVOLVEDOR, `--dev-console` ou
`CRASH_DEV_CONSOLE=1`; abre com a tecla antes do 1 ou F10.

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
| 7 | **Desbloquear** (as 20 skins do retail **+ a não usada Crash_Hulk = 22**; fases; golpes e upgrades) | `DO_UnlockSkin/Level/Move`; skins em `0x824A8218` (20 ints, -1 = livre) | Só pelos scripts | `CRASH_DEV_UNLOCK_ALL=1`, `dev.unlock_all({ESkin_Crash_Hulk})`, `dev.unlock_skin` | sim (`dev_desbloquear`); console `unlock all` / `unlock skin hulk`; a Crash_Hulk no menu de skins e vestida em jogo (`console_skin_menu`, seção 5) |
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

**Como o jogo aplica a skin** (2026-10-22, `runtime/dev/dev_skins.cpp`):

| O quê | Onde |
|---|---|
| ESkin → nome | `sub_82215548` (tabela: 0 Chiratta ... 17 Scorporilla, 21 Crash, 22 Crash_Hulk, 23 Valentine, 24 Skeleton = Halloween; 18-20 sem nome) |
| ESkin → template | `sub_82215730`: hash `"Characters:" + ("Crash_" exceto 21) + nome` (o hash de nomes do jogo, `sub_822846C0`, h = h*31 + c): o template `Characters:Crash_Ratcicle` etc. de `levels\GlobalPackages.p3d` |
| skin atual | `0x824A8270`, o hash do template (lê `sub_822159E8`, grava `sub_822159C0`; `Characters:Crash` no boot, `sub_82421420`); não vai para o save |
| `Frontend:SelectSkin` (VESTIR) | `sub_820F6180` → `sub_821D9270(frontend, skin)`: troca a skin atual, carrega o pacote do template novo (`CStreamManager` `sub_8224D3C8`) e solta o antigo (`sub_8224CEC8`) |
| tela de skins | `CSkinScreenAction` (`sub_820BBB68(tela, skin)`: um switch próprio ESkin → template de pré-visualização e índice dos textos; skin mostrada em `0x82497508` {skin, índice, hash}); esquerda/direita = `sub_822159F8` (a próxima/anterior da lista de desbloqueadas) |
| carregar/soltar fase | `CStreamManager::vf05` (`sub_8224E068`) carrega o template do jogador 2 e o da skin atual; `vf04` (`sub_8224FAD8`) solta |
| nascer | `CActionSpawnPlayer` (construtor `sub_822114E8`) guarda o hash da skin atual (+8) e cria aquele template |
| golpes | a árvore de luta do Crash (`HLSkinWeapon`, ramos `ContentX`): `Crash:CheckSkinType(ESkin)` (`sub_821534A0` = `sub_82215730(skin) == atual`) → `self:SetWeaponType("Crash_X")` |
| cenas | `CCinematicActorAction::vf04` (`sub_821B6388`): o ator do Crash na cena é `hash atual + "_nis_NN"` |

**Trocar com o jogo aberto** (`skin <nome>` no console): no frontend (HUB, menus) o runtime chama o mesmo
`sub_821D9270` do VESTIR. Dentro de uma fase, trocar o template do jogador vivo não é seguro (o pacote antigo está
em uso); a skin fica pendente e entra logo antes do `vf05` da próxima carga (o `vf04` já soltou a antiga e o
`vf05` carrega a nova); `skin <nome> now` faz isso e reinicia a fase (`UberManager:ForceReload`, que passa por
`vf04`/`vf05`). Provado com Ratcicle e Crash_Hulk no L1_E1 (`console_dev`, `console_hulk`).

**A Crash_Hulk (22) ficou pela metade no jogo** e o runtime completa:

1. **Nome do template errado**: `sub_82215730` monta `Characters:Crash_Crash_Hulk` (prefixo `Crash_` + nome
   `Crash_Hulk`), que não existe; o template é `Characters:Crash_Hulk` (pacote `ab4cc04a`, com o modelo
   `344a15f9` = `Crash_HulkShape`). O gancho devolve o hash certo para 22 (e o do Crash normal para 1, 18, 19 e
   20, que antes davam crash ou nada).
2. **Sem colisão**: o objeto `crash` dos pacotes da Hulk não tem `crashEllipsoidShape` nem
   `Interactor_VolumeShape` (os das outras 20 skins têm): o Crash nascia e **atravessava o chão**. O
   `CPhysicsBehaviour::vf12` (`sub_82147568`) e o `CIntersectionBehaviour::vf12` (`sub_8211EA88`) procuram as
   partes na entrada de pacotes do template (r5, a entrada do `CStreamManager` em `*0x824A90CC`, hash em +20); para
   a Hulk o runtime passa a entrada do `Characters:Crash` normal (sempre carregada: é o template do jogador 2).
3. **Tela de skins**: o switch de `sub_820BBB68` manda 1, 18-20 e 22 para "nenhum", que soltava a
   pré-visualização anterior e depois a lia já solta (crash ao chegar na Hulk com esquerda/direita). Para 22 o
   gancho faz o que o switch faz com as outras, com o template da Hulk e os textos 22 do próprio jogo ("CRASH
   HULK" / "CRASH!": as listas de textos têm as 25).
4. **Desbloqueio sem vaga**: a lista de 20 já é ocupada pelas 20 do retail. Com `[dev] hulk_skin = true`
   (`unlock skin hulk`/`unlock all` no console) `IsSkinUnlocked` (`sub_82215900`) responde sim para 22 e
   esquerda/direita da tela de skins (`sub_822159F8`) passam pela Hulk depois da última da lista. Ela não tem boneco
   próprio no HUB: aparece ao abrir qualquer skin desbloqueada e andar com esquerda/direita.
5. **Cenas**: não há `Characters:Crash_Hulk_nis_NN`; enquanto ela é a skin atual, o ator das cenas usa o do Crash
   normal (o gancho troca o hash atual só durante `sub_821B6388`).

Prova: `console_skin_menu` (HUB → RATNICIAN SKIN → direita: CRASH HULK → VESTIR → CONTINUE STORY → a fase com a
Hulk), capturas em `build\console\`.

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
  skins guarda 20: com a Crash_Hulk a última do retail (Ratcicle) não cabe (por isso o console guarda a Hulk em
  `[dev] hulk_skin`, fora da lista).
- Console: `unlock all` grava no save quando o jogo salvar (é o progresso do jogo); `level` numa fase reinicia
  pela fase nova (`UberManager:SetLevelIndex` + `ForceReload`; o `StartLevelByName` em jogo deixa o jogo
  esperando o menu e trava), fora dela vai pelo HUB ou pelo menu principal; outras telas esperam até chegar num
  dos dois.

## Follow-ups (promissores, não terminados)

1. ~~**Decompilador dos `.blua`**~~: feito em `tools/luadec.py` (2026-10-22); os 723 scripts reconstruídos compilam
   no compilador do jogo (perfil `dev_luadec`).
2. **Implementar o `testManager`** que o `TestLevels.lua` espera (`SendControllerChange`, `TestGetMicroseconds`,
   `IsFrontEndLoaded`) em cima do input do runtime: os scripts de teste dos devs voltariam a rodar.
3. **GameLink completo**: descobrir o formato dos argumentos (ids `@@OBJ:n@@`) de `GetFieldList`/`DumpValues`/
   `ToggleField`. (Mandar Lua com o jogo aberto já dá: console de desenvolvedor, `lua <código>`; falta um canal
   de fora do jogo, pipe/porta local, como o GameLink do PC.)
4. **Desbloquear o resto** (play zones, mojo rooms, concept art 0-19, enemy info, tutoriais): objetivos pequenos, um
   por bloco, devem funcionar (não verificado); e ler o estado do save para provar.
5. ~~**Skin Crash_Hulk em jogo**~~: feito (2026-10-22, seção 5): no menu de skins, vestida, com colisão.
6. ~~**Big Head só pela flag**~~: feito pelo console (`bighead on|off`, `dev::SetBigHead`, a mesma escala do
   verificador); falta só a variável `CRASH_DEV_BIGHEAD` para os perfis (hoje: `dev_console = [..., "bighead on"]`).
7. **Invencibilidade contra poços**: hook no `CDeathTriggerVolumeBehaviour` / respawn.
8. **Teleporte para checkpoints**: listar os locators de cada fase (`CWorldManager`, skip points) e usar
   `DO_TeleportPlayer` / `DO_SkipAhead`; o skip ahead não fez nada na vila do L1_E1.
9. **Slow motion / frame step**: entender o que `m_SlowMotion` escala no `CUberManager::vf10`.
10. **Desenho de debug** (`m_DrawPlayerPosition`, `m_DrawRoomNumber`, `%.2fms (%2.0ffps)`): ver se o caminho até o texto
   ainda existe e só falta a função de print.
11. **SoundTest/CombatArena01**: ver se há geometria (câmera livre) ou só o teste de som.
