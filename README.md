# CrashRecomp: Crash of the Titans, Definitive Edition (PC)

A native Windows port of **Crash of the Titans** (Xbox 360), made by statically recompiling the game's
PowerPC code to x86-64 with [XenonRecomp](https://github.com/hedge-dev/XenonRecomp), plus a runtime that
replaces the Xbox 360 (graphics, audio, input, files, saves) and a set of PC improvements.

**[Português do Brasil abaixo](#português-do-brasil)**

> ### v1.04: almost 3x faster, and on the way to 100% native rendering
>
> The renderer no longer imitates the Xbox 360's GPU step by step where it doesn't need to: the scene is
> drawn **in one pass** instead of the console's three screen tiles, the game's Direct3D command building
> runs as **native code**, and the shaders are **compiled at install** instead of while playing.
>
> | Heaviest scene (L1_E1 village, 1080p, unlimited FPS) | v1.03 | **v1.04** |
> |---|---|---|
> | Average frame rate | ~180 fps | **~500 fps** (2.8x) |
> | Worst 1% of frames (1% low) | ~105 fps | **~265 fps** |
> | 99th percentile frame time | 6.6 ms | **2.7 ms** |
> | CPU used by the vblank thread | ~70% of a core | ~5% |
>
> Measured on a Ryzen 7 5700X + RTX 5080. The gain is on the CPU side (the GPU was never the limit), so
> slower CPUs should gain in a similar proportion (not measured yet: reports welcome). Next: replacing
> the remaining Xbox 360 GPU emulation with direct Direct3D-to-Vulkan calls, step by step
> ([the plan](docs/internals/native-gpu-plan.md#road-to-100-native)).

![Title screen](docs/screenshots/title.jpg)

> **No game files are included.** You need your own legally obtained copy of Crash of the Titans for
> Xbox 360. The installer builds everything from **your** copy on **your** PC. This repository has only
> original code, tools and documentation. Crash of the Titans is © Activision / Radical Entertainment;
> this is an unofficial fan project, not affiliated with or endorsed by them.

## Features

- **Native PC game**: story, menus and saves run natively, at the original 30 fps or unlocked
  (60 / 120 / unlimited), in a window, borderless or fullscreen, at internal resolutions up to 4K.
- **DLSS / DLAA** (NVIDIA RTX) and **FSR 3.1** (any GPU) upscaling, FXAA / SMAA, anisotropic filtering,
  adjustable motion blur, depth of field and shadow softness.
- **Fast native rendering path**: the scene is drawn in one pass instead of the Xbox 360's three screen
  tiles, the game's Direct3D command building runs as native code and its shaders are compiled at install
  and warmed up in the background. In the heaviest scene (the first level's village, unlimited FPS) this
  went from ~170 to ~500 fps on a Ryzen 7 5700X + RTX 5080.
- **PC controls**: keyboard and mouse with rebindable keys, mouse camera, mouse in every menu, Xbox and
  **PlayStation (DualSense)** controllers, button icons that follow the last device used, and **local
  co-op** with two controllers (choose who is player 1 and player 2 in SETTINGS > CONTROLS).
- **Native settings screen** in the game's own style (GAME, AUDIO, VIDEO, GRAPHICS, CONTROLS and
  DEFINITIVE EDITION tabs), plus a **launcher** before playing: a simple page with the main options
  (language, resolution, FPS limit, HD AI Remaster, lighting ORIGINAL / REMASTER) and an ADVANCED view
  with every tab of the settings screen (the same rows and texts: both read `runtime/settings/options.*`).
- **Brazilian Portuguese translation** of every text, and **subtitles** for all dialogue and cutscenes
  (English transcript + PT-BR translation, optional download).
- **Definitive Edition extras**: PS2 mojo colours, an experimental **free third-person camera**, a
  **lighting remaster** (SSAO, one-bounce indirect light, contact and soft shadows), EXIT GAME, an
  on-screen keyboard for names and save deletion, and **skippable cutscenes and intro videos** (hold
  B / Esc; a small prompt shows in the corner).
- **HD AI Remaster** (optional download, ~4.5 GB): AI-upscaled textures and smoothed models for the
  whole game.
- **Modding**: mods folder with load order, HD texture packs, and tools that export the game's sounds,
  videos, textures and 3D models to open formats (WAV, MP4, PNG, glTF) and build them back into mods.
- **Dev tools for automation**: test profiles with direct boot into any level, event-anchored input
  scripts, screenshots, input recording (F5), Lua inside the game's own scripting, benchmarks, and an
  optional **in-game developer console** (unlock all, mojo, god mode, teleport, level select, live skin
  switching, including the unused green **Crash Hulk** skin the game never shipped).

| Settings | Launcher |
|---|---|
| ![Settings](docs/screenshots/settings.jpg) | ![Launcher](docs/screenshots/launcher.jpg) |

| HD AI Remaster (off / on) | Lighting remaster (before / after) |
|---|---|
| ![HD AI Remaster](docs/screenshots/hd_ai_remaster.jpg) | ![Lighting](docs/screenshots/lighting.jpg) |

## Requirements

- Windows 10/11 x64 and a GPU with **Vulkan 1.3** and an up-to-date driver (for example NVIDIA GTX 900
  or newer, AMD RX 400 or newer, Intel Iris Xe / Arc). The renderer is Vulkan only (Direct3D 12 was
  dropped in October 2026). Linux / Steam Deck through Proton is the goal and still being tested. The
  installer downloads Microsoft's `dxcompiler.dll` (HLSL to SPIR-V) next to the exe.
- Your Crash of the Titans (Xbox 360) game files: the folder with `default.xex`, `default.rcf`,
  `english.rcf`, `sound1.rcf`, `sound2.rcf` and `shaders\` (extracted from your disc). Only the known
  retail `default.xex` is supported (the installer checks its SHA-256).
- About 1.5 GB of free space (plus 3 GB to copy the game into the folder, 4.5 GB for the HD AI Remaster,
  18 GB for the asset export, mostly WAV sounds).

**Nothing else to install.** Since v1.03 the release zip carries its own build tools in `toolchain\`
([llvm-mingw](https://github.com/mstorsjo/llvm-mingw) clang, [CMake](https://cmake.org),
[Ninja](https://ninja-build.org) and the official embeddable [Python](https://www.python.org) with numpy and
Pillow): no Visual Studio, LLVM, Python or Git, and nothing is installed on the system. The installer
only downloads FFmpeg (LGPL), NVIDIA DLSS and AMD FSR from their official sources, checked by SHA-256
(see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)), and the optional extras you choose.

Advanced: to build with an installed Visual Studio 2022+ / Build Tools and LLVM `clang-cl` instead, set
**BUILD TOOLS = VISUAL STUDIO** in the installer (or `--toolchain msvc`); it offers to install the missing
ones with `winget`.

## Install

1. Download **`CrashRecomp-v1.04.zip`** (the latest, ~150 MB) from the [releases](https://github.com/VitoMiguelF/crashott-pc/releases)
   and unzip it. (A `git clone` of this repository has no `toolchain\`: see [CONTRIBUTING.md](CONTRIBUTING.md).)
2. Run **`CrashSetup.exe`**. Choose your game folder, pick the options (subtitles, HD AI Remaster, asset
   export for modding, copying the game into the folder) and click **INSTALL**. It recompiles the game
   code and builds everything with the tools in `toolchain\` (about 2 minutes on an 8-core PC, longer on
   slower ones).

   ![Installer](docs/screenshots/installer.jpg)

   Without the GUI, the same in a terminal, with the bundled Python:

   ```bat
   toolchain\python\python.exe tools\setup.py --game D:\Games\Crash --subtitles
   toolchain\python\python.exe tools\setup.py --game D:\Games\Crash --remaster --extract-assets --copy-game
   toolchain\python\python.exe tools\setup.py --game D:\Games\Crash --assets-only   :: retry only the asset export
   ```

   Release zips placed next to `CrashSetup.exe` are used instead of downloading them again. If only
   the asset export fails, the game is installed anyway: **TRY AGAIN** reruns just that step.
3. Play with **`dist\CrashLauncher.exe`** (options, then PLAY) or `dist\CrashRecomp.exe`.

### Known issues

- **v1.01: ASSETS FOR MODDING fails with `ModuleNotFoundError: No module named 'numpy'`.** The v1.01
  installer doesn't install the Python packages the asset export needs (fixed in v1.02). The game
  itself is installed fine. Install them and run the installer again (finished steps are skipped):

  ```bat
  python -m pip install numpy pillow
  ```

  Use the same Python the installer uses: if `python` isn't found or is a different one, try
  `py -3 -m pip install numpy pillow`.

## Playing

`dist\` is the game folder: `CrashLauncher.exe`, `CrashRecomp.exe`, `game\` (your dump, if copied),
`mods\`, `userdata\` (saves, `settings.toml`, shader cache), `languages\`, `data\` (subtitles, HD AI
Remaster). Nothing depends on where the project is. Shortcuts in game: F1 overlay, F12 screenshot,
Alt+Enter fullscreen, F8 lighting remaster on/off, and with **DEFINITIVE EDITION > DEVELOPER CONSOLE** on
(off by default, or `--dev-console`), the key left of 1 or F10 opens the developer console (`help`).

Default settings are the original experience (1280x720, 30 fps, no upscaler, extras off). Turn things on
in **SETTINGS** (main menu or pause > OPTIONS) or in the launcher; for the HD AI Remaster, install it and
then switch **DEFINITIVE EDITION > HD AI REMASTER** on. The launcher opens on its simple view: **LIGHTING
REMASTER** turns on the whole lighting remaster (SSAO high, indirect light, contact and soft shadows),
ORIGINAL turns it off, and CUSTOM means a mix set one by one; **ADVANCED** (bottom-right corner, or Y / F2)
shows every option, and the launcher remembers the last view. Mods' HD texture packs need no option: they
load from `mods\` by themselves (`[graphics] hd_textures = false` in `settings.toml` turns them off).

## Mods and exporting the game's assets

Put each mod in `dist\mods\<name>\` (optional `load_order.txt`). Mods can replace any `.rcf` entry, add
HD textures without touching the game files, or replace sounds.

The game's assets are **not** distributed, but you can export them from your own copy (installer option
**ASSETS FOR MODDING**, or with the bundled Python, which already has numpy and Pillow):

```bat
set PY=toolchain\python\python.exe
%PY% tools\assets.py all          :: everything to private\assets (about 9 minutes, 18 GB)
%PY% tools\textures.py export     :: or step by step: textures -> PNG
%PY% tools\models.py export       :: models, skeletons, animations -> glTF (.glb)
%PY% tools\audio.py export        :: sounds and voices -> WAV
%PY% tools\audio.py videos        :: Bink videos -> MP4
```

(Another Python 3.11+ works too after `python -m pip install -r tools\requirements.txt`.)

Edit the exported files and build them into a mod with `textures.py build`, `models.py build` or
`audio.py build --mod <name>`. The full guide (formats, indexes, examples) is in
[docs/GUIA_DO_PROJETO.md](docs/GUIA_DO_PROJETO.md), section "Extraindo e modificando assets" (PT-BR).
Mods made from the game's files are derived content: share them with care.

## Documentation

- [docs/GUIA_DO_PROJETO.md](docs/GUIA_DO_PROJETO.md): the full project guide (PT-BR): layout, building by
  hand, test profiles (`tools\run_game.py`), every feature and setting, performance, modding, reading
  the game's code.
- [runtime/NOTES.md](runtime/NOTES.md): the runtime's development notes (PT-BR), step by step.
- [docs/dev_features.md](docs/dev_features.md): developer console, Lua, invincibility, teleport, unlocks, skins.
- [docs/internals/](docs/internals/README.md): short English cheat sheets of addresses, hooks and traps per area.
- [CONTRIBUTING.md](CONTRIBUTING.md): setting up, branches and pull requests, tests, reporting bugs.

## Credits and licenses

- Code of this project: MIT ([LICENSE](LICENSE)), by VitoMiguelF, written with Claude Code.
- Build toolchain in the release zip: llvm-mingw (Apache 2.0 with LLVM Exceptions; mingw-w64), CMake
  (BSD-3), Ninja (Apache 2.0), Python (PSF) with NumPy (BSD-3) and Pillow (MIT-CMU).
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) by hedge-dev (MIT), with local fixes in
  [patches/XenonRecomp.patch](patches/XenonRecomp.patch).
- FFmpeg (LGPL), NVIDIA DLSS (RTX SDK license), AMD FidelityFX FSR, SMAA (MIT), Dear ImGui (MIT), SIMDe,
  libmspack, tiny-AES-c: see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
- The subtitles and the HD AI Remaster are optional fan-made release assets, kept out of git so they can
  be removed on request.

---

# Português do Brasil

Um port nativo para Windows de **Crash of the Titans** (Xbox 360), feito recompilando estaticamente o
código PowerPC do jogo para x86-64 com o [XenonRecomp](https://github.com/hedge-dev/XenonRecomp), mais
um runtime que faz o papel do Xbox 360 (gráficos, áudio, controles, arquivos, saves) e várias melhorias
de PC.

> ### v1.04: quase 3x mais rápido, e a caminho da renderização 100% nativa
>
> O renderizador deixou de imitar a GPU do Xbox 360 passo a passo onde não precisa: a cena é desenhada
> **numa passada só** em vez das três faixas de tela do console, a montagem de comandos Direct3D do jogo
> roda como **código nativo** e os shaders são **compilados na instalação**, não durante o jogo.
>
> | Cena mais pesada (vila do L1_E1, 1080p, FPS livre) | v1.03 | **v1.04** |
> |---|---|---|
> | FPS médio | ~180 fps | **~500 fps** (2,8x) |
> | Pior 1% dos quadros (1% low) | ~105 fps | **~265 fps** |
> | Tempo de quadro no percentil 99 | 6,6 ms | **2,7 ms** |
> | CPU usada pela thread de vblank | ~70% de um núcleo | ~5% |
>
> Medido num Ryzen 7 5700X + RTX 5080. O ganho é do lado da CPU (a placa de vídeo nunca foi o limite),
> então CPUs mais lentas devem ganhar numa proporção parecida (ainda não medido: relatos são bem-vindos).
> Próximo passo: trocar o resto da emulação da GPU do Xbox 360
> por chamadas diretas do Direct3D para o Vulkan, etapa por etapa
> ([o plano](docs/internals/native-gpu-plan.md#road-to-100-native)).

> **Nenhum arquivo do jogo está incluído.** Você precisa da sua própria cópia original de Crash of the
> Titans para Xbox 360. O instalador monta tudo a partir da **sua** cópia, no **seu** PC. Este
> repositório tem só código, ferramentas e documentação próprios. Crash of the Titans é © Activision /
> Radical Entertainment; este é um projeto de fã não oficial, sem vínculo com elas.

## Recursos

- **Jogo nativo de PC**: história, menus e saves rodam nativamente, a 30 fps como o original ou
  sem limite (60 / 120 / livre), em janela, sem bordas ou tela cheia, com resolução interna até 4K.
- **DLSS / DLAA** (NVIDIA RTX) e **FSR 3.1** (qualquer placa), FXAA / SMAA, filtro anisotrópico, motion
  blur, profundidade de campo e suavidade das sombras ajustáveis.
- **Renderização nativa rápida**: a cena é desenhada numa passada só em vez das três faixas de tela do
  Xbox 360, a montagem de comandos Direct3D do jogo roda como código nativo e os shaders são compilados na
  instalação e aquecidos em segundo plano. Na cena mais pesada (a vila da primeira fase, FPS livre) isso
  foi de ~170 para ~500 fps num Ryzen 7 5700X + RTX 5080.
- **Controles de PC**: teclado e mouse com teclas configuráveis, câmera pelo mouse, mouse em todos os
  menus, controles Xbox e **PlayStation (DualSense)**, ícones dos botões que seguem o último dispositivo e
  **co-op local** com dois controles (escolha quem é o jogador 1 e o 2 em SETTINGS > CONTROLES).
- **Tela de configurações nativa** no estilo do jogo (abas JOGO, ÁUDIO, VÍDEO, GRÁFICOS, CONTROLES e
  DEFINITIVE EDITION) e um **launcher** antes de jogar: uma página simples com as opções principais
  (idioma, resolução, limite de FPS, HD AI Remaster, iluminação ORIGINAL / REMASTER) e a vista AVANÇADO
  com todas as abas da tela de configurações (as mesmas linhas e textos: as duas leem
  `runtime/settings/options.*`).
- **Tradução completa para português do Brasil** e **legendas** em todas as falas e cenas (transcrição
  em inglês + tradução PT-BR, download opcional).
- **Extras da Definitive Edition**: cores do mojo do PS2, **câmera livre em terceira pessoa**
  (experimental), **remaster da iluminação** (SSAO, luz indireta, sombras de contato e suaves), SAIR DO
  JOGO, teclado na tela para o nome e apagar saves, e **cenas e vídeos de abertura que dá para pular**
  (segure B / Esc; um aviso pequeno aparece no canto).
- **HD AI Remaster** (download opcional, ~4,5 GB): texturas ampliadas por IA e modelos suavizados no jogo
  todo.
- **Mods**: pasta de mods com ordem de carregamento, pacotes de texturas HD e ferramentas que exportam
  sons, vídeos, texturas e modelos 3D do jogo para formatos abertos (WAV, MP4, PNG, glTF) e os montam de
  volta em mods.
- **Ferramentas de desenvolvimento e automação**: perfis de teste com boot direto em qualquer fase,
  roteiros de botões ancorados em eventos, capturas, gravação de controle (F5), Lua dentro dos scripts do
  jogo, benchmarks e um **console de desenvolvedor** opcional dentro do jogo (unlock all, mojo, invencível,
  teleporte, escolher fase, trocar de skin na hora, inclusive a skin verde **Crash Hulk**, que o jogo nunca
  usou).

## Requisitos

- Windows 10/11 x64 e uma placa de vídeo com **Vulkan 1.3** e driver atualizado (por exemplo NVIDIA
  GTX 900 ou mais nova, AMD RX 400 ou mais nova, Intel Iris Xe / Arc). O renderizador é só Vulkan
  (o Direct3D 12 saiu em outubro de 2026). Linux / Steam Deck pelo Proton é o objetivo e ainda está em teste. O
  instalador baixa o `dxcompiler.dll` da Microsoft (HLSL para SPIR-V) ao lado do exe.
- Os arquivos do seu Crash of the Titans (Xbox 360): a pasta com `default.xex`, `default.rcf`,
  `english.rcf`, `sound1.rcf`, `sound2.rcf` e `shaders\` (extraída do seu disco). Só o `default.xex`
  conhecido da versão comercial é suportado (o instalador confere o SHA-256).
- Cerca de 1,5 GB livres (mais 3 GB para copiar o jogo para a pasta, 4,5 GB para o HD AI Remaster e 18 GB
  para exportar os assets).

**Nada mais para instalar.** Desde a v1.03 o zip da release traz as próprias ferramentas de compilação em
`toolchain\` ([llvm-mingw](https://github.com/mstorsjo/llvm-mingw) com clang, [CMake](https://cmake.org),
[Ninja](https://ninja-build.org) e o [Python](https://www.python.org) embutido oficial com numpy e Pillow):
sem Visual Studio, LLVM, Python ou Git, e nada é instalado no sistema. O instalador só baixa FFmpeg (LGPL),
NVIDIA DLSS e AMD FSR das fontes oficiais, conferidos pelo SHA-256 (veja
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)), e os extras opcionais que você escolher.

Avançado: para compilar com um Visual Studio 2022+ / Build Tools e o `clang-cl` do LLVM instalados, escolha
**FERRAMENTAS = VISUAL STUDIO** no instalador (ou `--toolchain msvc`); ele oferece instalar o que faltar
pelo `winget`.

## Instalação

1. Baixe **`CrashRecomp-v1.04.zip`** (a mais recente, ~150 MB) nas [releases](https://github.com/VitoMiguelF/crashott-pc/releases) e
   descompacte. (Um `git clone` deste repositório não tem a `toolchain\`: veja o [CONTRIBUTING.md](CONTRIBUTING.md).)
2. Abra o **`CrashSetup.exe`**. Escolha a pasta do jogo, escolha as opções (legendas, HD AI Remaster, assets
   para modding, copiar o jogo para a pasta) e clique em **INSTALAR**. Ele recompila o código do jogo e
   compila tudo com as ferramentas de `toolchain\` (uns 2 minutos num PC de 8 núcleos, mais nos mais lentos).

   Sem a interface, o mesmo num terminal, com o Python que vem junto:

   ```bat
   toolchain\python\python.exe tools\setup.py --game D:\Jogos\Crash --subtitles
   toolchain\python\python.exe tools\setup.py --game D:\Jogos\Crash --remaster --extract-assets --copy-game
   toolchain\python\python.exe tools\setup.py --game D:\Jogos\Crash --assets-only   :: tenta de novo só a exportação de assets
   ```

   Zips da release colocados ao lado do `CrashSetup.exe` são usados em vez de baixar de novo. Se só a
   exportação de assets falhar, o jogo fica instalado mesmo assim: **TENTAR DE NOVO** refaz só essa etapa.
3. Jogue pelo **`dist\CrashLauncher.exe`** (opções e depois JOGAR) ou pelo `dist\CrashRecomp.exe`.

### Problemas conhecidos

- **v1.01: ASSETS PARA MODDING falha com `ModuleNotFoundError: No module named 'numpy'`.** O instalador
  da v1.01 não instala os pacotes Python que a exportação de assets usa (corrigido na v1.02). O jogo em
  si é instalado normalmente. Instale-os e rode o instalador de novo (as etapas prontas são puladas):

  ```bat
  python -m pip install numpy pillow
  ```

  Use o mesmo Python do instalador: se `python` não for encontrado ou for outro, tente
  `py -3 -m pip install numpy pillow`.

## Jogando

`dist\` é a pasta do jogo: `CrashLauncher.exe`, `CrashRecomp.exe`, `game\` (o seu jogo, se copiado),
`mods\`, `userdata\` (saves, `settings.toml`, cache de shaders), `languages\` e `data\` (legendas, HD AI
Remaster). Nada depende de onde o projeto está. Atalhos no jogo: F1 overlay, F12 captura, Alt+Enter tela
cheia, F8 liga/desliga o remaster da iluminação e, com **DEFINITIVE EDITION > CONSOLE DE DESENVOLVEDOR** ligado
(desligado por padrão, ou `--dev-console`), a tecla antes do 1 ou F10 abre o console de desenvolvedor (`help`).

As opções padrão são a experiência original (1280x720, 30 fps, sem upscaler, extras desligados). Ligue o
que quiser em **SETTINGS** (menu principal ou pausa > OPÇÕES) ou no launcher. Para o HD AI Remaster,
instale-o e ligue **DEFINITIVE EDITION > HD AI REMASTER**. Para jogar em português, IDIOMA = PORTUGUÊS (BR).
O launcher abre na vista simples: **ILUMINAÇÃO REMASTER** liga todo o remaster da iluminação (SSAO alta, luz
indireta, sombras de contato e suaves), ORIGINAL desliga, e PERSONALIZADO quer dizer uma mistura ajustada uma a
uma; **AVANÇADO** (canto inferior direito, ou Y / F2) mostra todas as opções, e o launcher lembra a última vista.
Os pacotes de texturas HD dos mods não precisam de opção: carregam sozinhos de `mods\` (`[graphics]
hd_textures = false` no `settings.toml` os desliga).

**Dois jogadores**: ligue dois controles (Xbox ou DualSense, em qualquer combinação). Na fase, aperte **B**
no segundo controle quando aparecer JOIN GAME: o jogador 2 entra na mochila do Crash, e **Y** o faz descer.
O teclado e o mouse ficam sempre com o jogador 1; com um controle só, ele é o jogador 1. Para trocar quem é
quem, use **SETTINGS > CONTROLES > JOGADOR 1 / JOGADOR 2**.

## Mods e exportação dos assets do jogo

Cada mod fica em `dist\mods\<nome>\` (`load_order.txt` opcional). Um mod pode trocar qualquer entrada dos
`.rcf`, acrescentar texturas HD sem mexer nos arquivos do jogo ou trocar sons.

Os assets do jogo **não** são distribuídos, mas você pode exportá-los da sua cópia (opção **ASSETS PARA
MODDING** do instalador, ou com o Python que vem junto, que já tem numpy e Pillow):

```bat
set PY=toolchain\python\python.exe
%PY% tools\assets.py all          :: tudo para private\assets (cerca de 9 minutos, 18 GB)
%PY% tools\textures.py export     :: ou por etapa: texturas -> PNG
%PY% tools\models.py export       :: modelos, esqueletos, animações -> glTF (.glb)
%PY% tools\audio.py export        :: sons e vozes -> WAV
%PY% tools\audio.py videos        :: vídeos Bink -> MP4
```

(Outro Python 3.11+ também serve, depois de `python -m pip install -r tools\requirements.txt`.)

Edite os arquivos exportados e monte o mod com `textures.py build`, `models.py build` ou
`audio.py build --mod <nome>`. O guia completo (formatos, índices, exemplos) está em
[docs/GUIA_DO_PROJETO.md](docs/GUIA_DO_PROJETO.md), seção "Extraindo e modificando assets". Mods feitos a
partir dos arquivos do jogo são conteúdo derivado: compartilhe com cuidado.

## Documentação

- [docs/GUIA_DO_PROJETO.md](docs/GUIA_DO_PROJETO.md): o guia completo do projeto: estrutura, compilação à
  mão, perfis de teste (`tools\run_game.py`), cada recurso e opção, desempenho, mods, leitura do código
  do jogo.
- [runtime/NOTES.md](runtime/NOTES.md): as notas de desenvolvimento do runtime, passo a passo.
- [docs/dev_features.md](docs/dev_features.md): console de desenvolvedor, Lua, invencibilidade, teleporte, desbloqueios, skins.
- [docs/internals/](docs/internals/README.md): resumos curtos em inglês de endereços, hooks e armadilhas por área.
- [CONTRIBUTING.md](CONTRIBUTING.md): como configurar, branches e pull requests, testes, reportar bugs.

## Créditos e licenças

- Código deste projeto: MIT ([LICENSE](LICENSE)), por VitoMiguelF, escrito com o Claude Code.
- Ferramentas de compilação do zip da release: llvm-mingw (Apache 2.0 com exceções do LLVM; mingw-w64),
  CMake (BSD-3), Ninja (Apache 2.0), Python (PSF) com NumPy (BSD-3) e Pillow (MIT-CMU).
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) da hedge-dev (MIT), com correções locais em
  [patches/XenonRecomp.patch](patches/XenonRecomp.patch).
- FFmpeg (LGPL), NVIDIA DLSS (licença RTX SDK), AMD FidelityFX FSR, SMAA (MIT), Dear ImGui (MIT), SIMDe,
  libmspack, tiny-AES-c: veja [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
- As legendas e o HD AI Remaster são assets opcionais da release, feitos por fãs e mantidos fora do git
  para poderem ser retirados se for pedido.
