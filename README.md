# CrashRecomp: Crash of the Titans, Definitive Edition (PC)

A native Windows port of **Crash of the Titans** (Xbox 360), made by statically recompiling the game's
PowerPC code to x86-64 with [XenonRecomp](https://github.com/hedge-dev/XenonRecomp), plus a runtime that
replaces the Xbox 360 (graphics, audio, input, files, saves) and a set of PC improvements.

**[Português do Brasil abaixo](#português-do-brasil)**

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
- **PC controls**: keyboard and mouse with rebindable keys, mouse camera, mouse in every menu, Xbox and
  **PlayStation (DualSense)** controllers, button icons that follow the last device used.
- **Native settings screen** in the game's own style (GAME, AUDIO, VIDEO, GRAPHICS, CONTROLS and
  DEFINITIVE EDITION tabs), plus a **launcher** with the same options before playing.
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
  scripts, screenshots, input recording (F5), a Lua console into the game's own scripting, benchmarks.

| Settings | Launcher |
|---|---|
| ![Settings](docs/screenshots/settings.jpg) | ![Launcher](docs/screenshots/launcher.jpg) |

| HD AI Remaster (off / on) | Lighting remaster (before / after) |
|---|---|
| ![HD AI Remaster](docs/screenshots/hd_ai_remaster.jpg) | ![Lighting](docs/screenshots/lighting.jpg) |

## Requirements

- Windows 10/11 x64, a DirectX 12 GPU.
- Your Crash of the Titans (Xbox 360) game files: the folder with `default.xex`, `default.rcf`,
  `english.rcf`, `sound1.rcf`, `sound2.rcf` and `shaders\` (extracted from your disc). Only the known
  retail `default.xex` is supported (the installer checks its SHA-256).
- To build (the installer offers to install the missing ones with `winget`):
  - [Visual Studio 2022+ or the Build Tools](https://visualstudio.microsoft.com/downloads/) with
    **Desktop development with C++**;
  - [LLVM](https://github.com/llvm/llvm-project/releases) (`clang-cl`), [CMake](https://cmake.org),
    [Ninja](https://ninja-build.org), [Git](https://git-scm.com), [Python 3.11+](https://www.python.org).
- About 3 GB of free space (plus 4.5 GB for the HD AI Remaster, 4 GB for the asset export).

FFmpeg (LGPL), NVIDIA DLSS and AMD FSR are downloaded by the installer from their official sources and
checked by SHA-256 (see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)).

## Install

1. Download **`CrashRecomp-v1.0.zip`** from the [releases](https://github.com/VitoMiguelF/crashott-pc/releases)
   and unzip it (or `git clone` this repository).
2. Run **`CrashSetup.exe`**. Choose your game folder, check the prerequisites, pick the options
   (subtitles, HD AI Remaster, asset export for modding, copying the game into the folder) and click
   **INSTALL**. It recompiles the game code and builds everything (5 to 20 minutes).

   ![Installer](docs/screenshots/installer.jpg)

   Without the GUI, the same in a terminal:

   ```bat
   python tools\setup.py --game D:\Games\Crash --subtitles
   python tools\setup.py --game D:\Games\Crash --remaster --extract-assets --copy-game
   ```

   Release zips placed next to `CrashSetup.exe` are used instead of downloading them again.
3. Play with **`dist\CrashLauncher.exe`** (options, then PLAY) or `dist\CrashRecomp.exe`.

## Playing

`dist\` is the game folder: `CrashLauncher.exe`, `CrashRecomp.exe`, `game\` (your dump, if copied),
`mods\`, `userdata\` (saves, `settings.toml`, shader cache), `languages\`, `data\` (subtitles, HD AI
Remaster). Nothing depends on where the project is. Shortcuts in game: F1 overlay, F12 screenshot,
Alt+Enter fullscreen, F8 lighting remaster on/off.

Default settings are the original experience (1280x720, 30 fps, no upscaler, extras off). Turn things on
in **SETTINGS** (main menu or pause > OPTIONS) or in the launcher; for the HD AI Remaster, install it and
then switch **DEFINITIVE EDITION > HD AI REMASTER** on.

## Mods and exporting the game's assets

Put each mod in `dist\mods\<name>\` (optional `load_order.txt`). Mods can replace any `.rcf` entry, add
HD textures without touching the game files, or replace sounds.

The game's assets are **not** distributed, but you can export them from your own copy (installer option
**ASSETS FOR MODDING**, or):

```bat
python tools\assets.py all          :: everything to private\assets (about 8 minutes, 4 GB)
python tools\textures.py export     :: or step by step: textures -> PNG
python tools\models.py export       :: models, skeletons, animations -> glTF (.glb)
python tools\audio.py export        :: sounds and voices -> WAV
python tools\audio.py videos        :: Bink videos -> MP4
```

Edit the exported files and build them into a mod with `textures.py build`, `models.py build` or
`audio.py build --mod <name>`. The full guide (formats, indexes, examples) is in
[docs/GUIA_DO_PROJETO.md](docs/GUIA_DO_PROJETO.md), section "Extraindo e modificando assets" (PT-BR).
Mods made from the game's files are derived content: share them with care.

## Documentation

- [docs/GUIA_DO_PROJETO.md](docs/GUIA_DO_PROJETO.md): the full project guide (PT-BR): layout, building by
  hand, test profiles (`tools\run_game.py`), every feature and setting, performance, modding, reading
  the game's code.
- [runtime/NOTES.md](runtime/NOTES.md): the runtime's development notes (PT-BR), step by step.
- [docs/dev_features.md](docs/dev_features.md): Lua console, invincibility, teleport, unlocks.

## Credits and licenses

- Code of this project: MIT ([LICENSE](LICENSE)), by VitoMiguelF, written with Claude Code.
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

> **Nenhum arquivo do jogo está incluído.** Você precisa da sua própria cópia original de Crash of the
> Titans para Xbox 360. O instalador monta tudo a partir da **sua** cópia, no **seu** PC. Este
> repositório tem só código, ferramentas e documentação próprios. Crash of the Titans é © Activision /
> Radical Entertainment; este é um projeto de fã não oficial, sem vínculo com elas.

## Recursos

- **Jogo nativo de PC**: história, menus e saves rodam nativamente, a 30 fps como o original ou
  sem limite (60 / 120 / livre), em janela, sem bordas ou tela cheia, com resolução interna até 4K.
- **DLSS / DLAA** (NVIDIA RTX) e **FSR 3.1** (qualquer placa), FXAA / SMAA, filtro anisotrópico, motion
  blur, profundidade de campo e suavidade das sombras ajustáveis.
- **Controles de PC**: teclado e mouse com teclas configuráveis, câmera pelo mouse, mouse em todos os
  menus, controles Xbox e **PlayStation (DualSense)**, ícones dos botões que seguem o último dispositivo.
- **Tela de configurações nativa** no estilo do jogo (abas JOGO, ÁUDIO, VÍDEO, GRÁFICOS, CONTROLES e
  DEFINITIVE EDITION) e um **launcher** com as mesmas opções antes de jogar.
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
  roteiros de botões ancorados em eventos, capturas, gravação de controle (F5), console Lua dentro dos
  scripts do jogo, benchmarks.

## Requisitos

- Windows 10/11 x64 e uma placa de vídeo com DirectX 12.
- Os arquivos do seu Crash of the Titans (Xbox 360): a pasta com `default.xex`, `default.rcf`,
  `english.rcf`, `sound1.rcf`, `sound2.rcf` e `shaders\` (extraída do seu disco). Só o `default.xex`
  conhecido da versão comercial é suportado (o instalador confere o SHA-256).
- Para compilar (o instalador oferece instalar o que faltar pelo `winget`): Visual Studio 2022+ ou as
  Build Tools com **Desenvolvimento para desktop com C++**, LLVM (`clang-cl`), CMake, Ninja, Git e
  Python 3.11+.
- Cerca de 3 GB livres (mais 4,5 GB para o HD AI Remaster e 4 GB para exportar os assets).

FFmpeg (LGPL), NVIDIA DLSS e AMD FSR são baixados pelo instalador das fontes oficiais e conferidos pelo
SHA-256 (veja [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)).

## Instalação

1. Baixe **`CrashRecomp-v1.0.zip`** nas [releases](https://github.com/VitoMiguelF/crashott-pc/releases) e
   descompacte (ou faça `git clone` deste repositório).
2. Abra o **`CrashSetup.exe`**. Escolha a pasta do jogo, confira os pré-requisitos, escolha as opções
   (legendas, HD AI Remaster, assets para modding, copiar o jogo para a pasta) e clique em **INSTALAR**.
   Ele recompila o código do jogo e compila tudo (de 5 a 20 minutos).

   Sem a interface, o mesmo num terminal:

   ```bat
   python tools\setup.py --game D:\Jogos\Crash --subtitles
   python tools\setup.py --game D:\Jogos\Crash --remaster --extract-assets --copy-game
   ```

   Zips da release colocados ao lado do `CrashSetup.exe` são usados em vez de baixar de novo.
3. Jogue pelo **`dist\CrashLauncher.exe`** (opções e depois JOGAR) ou pelo `dist\CrashRecomp.exe`.

## Jogando

`dist\` é a pasta do jogo: `CrashLauncher.exe`, `CrashRecomp.exe`, `game\` (o seu jogo, se copiado),
`mods\`, `userdata\` (saves, `settings.toml`, cache de shaders), `languages\` e `data\` (legendas, HD AI
Remaster). Nada depende de onde o projeto está. Atalhos no jogo: F1 overlay, F12 captura, Alt+Enter tela
cheia, F8 liga/desliga o remaster da iluminação.

As opções padrão são a experiência original (1280x720, 30 fps, sem upscaler, extras desligados). Ligue o
que quiser em **SETTINGS** (menu principal ou pausa > OPÇÕES) ou no launcher. Para o HD AI Remaster,
instale-o e ligue **DEFINITIVE EDITION > HD AI REMASTER**. Para jogar em português, IDIOMA = PORTUGUÊS (BR).

## Mods e exportação dos assets do jogo

Cada mod fica em `dist\mods\<nome>\` (`load_order.txt` opcional). Um mod pode trocar qualquer entrada dos
`.rcf`, acrescentar texturas HD sem mexer nos arquivos do jogo ou trocar sons.

Os assets do jogo **não** são distribuídos, mas você pode exportá-los da sua cópia (opção **ASSETS PARA
MODDING** do instalador, ou):

```bat
python tools\assets.py all          :: tudo para private\assets (cerca de 8 minutos, 4 GB)
python tools\textures.py export     :: ou por etapa: texturas -> PNG
python tools\models.py export       :: modelos, esqueletos, animações -> glTF (.glb)
python tools\audio.py export        :: sons e vozes -> WAV
python tools\audio.py videos        :: vídeos Bink -> MP4
```

Edite os arquivos exportados e monte o mod com `textures.py build`, `models.py build` ou
`audio.py build --mod <nome>`. O guia completo (formatos, índices, exemplos) está em
[docs/GUIA_DO_PROJETO.md](docs/GUIA_DO_PROJETO.md), seção "Extraindo e modificando assets". Mods feitos a
partir dos arquivos do jogo são conteúdo derivado: compartilhe com cuidado.

## Documentação

- [docs/GUIA_DO_PROJETO.md](docs/GUIA_DO_PROJETO.md): o guia completo do projeto: estrutura, compilação à
  mão, perfis de teste (`tools\run_game.py`), cada recurso e opção, desempenho, mods, leitura do código
  do jogo.
- [runtime/NOTES.md](runtime/NOTES.md): as notas de desenvolvimento do runtime, passo a passo.
- [docs/dev_features.md](docs/dev_features.md): console Lua, invencibilidade, teleporte, desbloqueios.

## Créditos e licenças

- Código deste projeto: MIT ([LICENSE](LICENSE)), por VitoMiguelF, escrito com o Claude Code.
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) da hedge-dev (MIT), com correções locais em
  [patches/XenonRecomp.patch](patches/XenonRecomp.patch).
- FFmpeg (LGPL), NVIDIA DLSS (licença RTX SDK), AMD FidelityFX FSR, SMAA (MIT), Dear ImGui (MIT), SIMDe,
  libmspack, tiny-AES-c: veja [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
- As legendas e o HD AI Remaster são assets opcionais da release, feitos por fãs e mantidos fora do git
  para poderem ser retirados se for pedido.
