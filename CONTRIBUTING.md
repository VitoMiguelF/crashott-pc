# Contributing to CrashRecomp

[Português do Brasil abaixo](#contribuindo-com-o-crashrecomp)

## 1. Set up from your own dump

You need your own legally obtained copy of Crash of the Titans for Xbox 360 (`default.xex`, `default.rcf`,
...). Nothing from the game is in this repository, and nothing from it may ever be added.

1. Install Visual Studio 2022+ with "Desktop development with C++", LLVM (`clang-cl`), CMake, Ninja, Git and
   Python 3.11+.
2. Clone your fork (or the repo, if you are a collaborator) and run the installer:
   ```bat
   git clone https://github.com/VitoMiguelF/crashott-pc CrashRecomp
   cd CrashRecomp
   python tools\setup.py --game <your game folder>
   ```
   It clones XenonRecomp at the pinned commit and applies `patches/XenonRecomp.patch`, downloads FFmpeg /
   DLSS / FSR from their official sources (checked by SHA-256), copies `default.xex` into `private\`,
   recompiles the game code into `generated\ppc`, builds and makes `dist\`. Your game folder is only read.
3. Enable the repository's pre-commit hook once (it refuses game-derived files):
   ```bat
   git config core.hooksPath .githooks
   ```
4. Machine-specific paths (another Visual Studio, LLVM, your game folder) go in `config/local.toml`
   (gitignored); see `tools/buildenv.py`.

Read `docs/GUIA_DO_PROJETO.md` (the full guide) and `docs/internals/` (English cheat sheets) before changing
an area.

## 2. Branches and pull requests

- Never push to `main`. Create a branch per change: `git switch -c fix/menu-mouse-hover`.
- Keep commits focused. Message: a short imperative summary line, a blank line, then what changed, why, and
  how it was verified (which test profiles). If an AI assistant co-wrote it, end with its
  `Co-Authored-By:` line.
- Update the docs in the same change: `runtime/NOTES.md` (engineering log, PT-BR), the GUIA section of the
  feature, and the matching `docs/internals/` file for new addresses, hooks or traps.
- Push your branch and open a pull request against `main`. The repo owner reviews and merges; releases and
  tags are made only by the owner.

## 3. Test before opening a PR

Run the profiles that exercise your change, not the whole suite:

```bat
python tools\run_game.py --list                   :: pick the relevant profiles
python tools\run_game.py fase_direta              :: e.g. anything in a level
python tools\run_game.py menu                     :: menus / menu music
python tools\run_game.py boot                     :: changes to the boot path, kernel, intro videos
```

- Avoid `--all` (about 10 minutes; profiles overlap). Run it only before a release, when asked.
- Tests run on a copy of `tests\userdata_base`, never on your real saves.
- Close other game instances first for anything timing-sensitive (benchmarks, input scripts); a running
  `CrashRecomp.exe` also blocks linking.
- New tests: copy a profile in `config/run.toml`, use `boot = "..."` and event anchors
  (`["@gameplay_ready"]`) instead of wall-clock button times, and add `expect` checks.
- List the profiles you ran and their result (`build\runs\...\result.json`) in the PR description.

## 4. No game-derived files in commits

Never commit: `default.xex`, `image.bin`, `.rcf` / `.bik` / `.rsd` / `.p3d` / `.xma` files, `private/`,
`generated/`, `dist/`, `build/`, `release/`, extracted assets or textures, decompiled scripts, shader caches,
texture dumps, subtitle transcripts, game screenshots outside the few in `docs/screenshots/`, or built
`.exe` / `.dll` files. The `.gitignore` covers these; `tools/check_game_files.py` checks the staged files
(pre-commit hook) and a GitHub Action checks every pull request. Mods and translations are fine when they
contain only your own work (e.g. the PT-BR pack stores translations by text hash, with no game text).

## 5. Reporting bugs

Open an issue with the **Bug report** template and include:
- version (release tag or commit), Windows version, CPU / GPU, controller(s);
- what you did and what happened (the level / menu, steps to reproduce);
- the log: the game writes it to the console, so reproduce the bug with
  `CrashRecomp.exe > crash_log.txt 2>&1` from a command prompt in `dist\` (test runs already have it in
  `build\runs\<run>\run.log`);
- for crashes, the `[crash]` / `[fatal]` block of the log;
- the **window title** (`... | run <folder> | session <id> | PID n`) or the `jogo: PID ...` line, so the
  instance can be matched to its log;
- your `settings.toml` options that differ from the defaults (or the whole file).

Never attach game files or save files containing game data you don't own.

---

# Contribuindo com o CrashRecomp

## 1. Configurando a partir do seu dump

Você precisa da sua própria cópia de Crash of the Titans para Xbox 360 (`default.xex`, `default.rcf`, ...).
Nada do jogo está neste repositório, e nada dele pode ser adicionado.

1. Instale o Visual Studio 2022+ com "Desenvolvimento para desktop com C++", LLVM (`clang-cl`), CMake, Ninja,
   Git e Python 3.11+.
2. Clone o seu fork (ou o repositório, se você for colaborador) e rode o instalador:
   ```bat
   git clone https://github.com/VitoMiguelF/crashott-pc CrashRecomp
   cd CrashRecomp
   python tools\setup.py --game <pasta do seu jogo>
   ```
   Ele clona o XenonRecomp no commit fixado e aplica `patches/XenonRecomp.patch`, baixa FFmpeg / DLSS / FSR das
   fontes oficiais (conferidos por SHA-256), copia o `default.xex` para `private\`, recompila o código do jogo
   em `generated\ppc`, compila e monta o `dist\`. A pasta do jogo só é lida.
3. Ligue uma vez o hook de pre-commit do repositório (ele recusa arquivos derivados do jogo):
   ```bat
   git config core.hooksPath .githooks
   ```
4. Caminhos da sua máquina (outro Visual Studio, LLVM, a pasta do jogo) vão em `config/local.toml` (fora do
   git); veja `tools/buildenv.py`.

Leia o `docs/GUIA_DO_PROJETO.md` (o guia completo) e `docs/internals/` (resumos em inglês) antes de mexer
numa parte.

## 2. Branches e pull requests

- Nunca dê push no `main`. Crie uma branch por mudança: `git switch -c fix/mouse-no-menu`.
- Commits focados. Mensagem: uma linha curta no imperativo, uma linha em branco, e depois o que mudou, por quê
  e como foi verificado (quais perfis de teste). Se uma IA ajudou a escrever, termine com a linha
  `Co-Authored-By:` dela.
- Atualize a documentação na mesma mudança: `runtime/NOTES.md`, a seção do recurso no GUIA e o arquivo
  correspondente em `docs/internals/` para endereços, hooks ou armadilhas novos.
- Dê push na sua branch e abra um pull request para o `main`. O dono do repositório revisa e faz o merge;
  releases e tags só o dono faz.

## 3. Teste antes de abrir o PR

Rode os perfis que exercitam a sua mudança, não todos:

```bat
python tools\run_game.py --list                   :: escolha os perfis certos
python tools\run_game.py fase_direta              :: ex.: qualquer coisa dentro de uma fase
python tools\run_game.py menu                     :: menus / música do menu
python tools\run_game.py boot                     :: boot, kernel, vídeos de abertura
```

- Evite o `--all` (uns 10 minutos; os perfis se repetem). Só antes de uma release, quando pedido.
- Os testes rodam numa cópia de `tests\userdata_base`, nunca nos seus saves.
- Feche outros jogos abertos antes de testes sensíveis a tempo (benchmarks, scripts de botões); um
  `CrashRecomp.exe` aberto também impede o link.
- Testes novos: copie um perfil do `config/run.toml`, use `boot = "..."` e âncoras de evento
  (`["@gameplay_ready"]`) em vez de horários fixos, e coloque checagens em `expect`.
- Liste no PR os perfis que rodou e o resultado (`build\runs\...\result.json`).

## 4. Nenhum arquivo derivado do jogo nos commits

Nunca faça commit de: `default.xex`, `image.bin`, arquivos `.rcf` / `.bik` / `.rsd` / `.p3d` / `.xma`,
`private/`, `generated/`, `dist/`, `build/`, `release/`, assets ou texturas extraídos, scripts descompilados,
caches de shader, dumps de textura, transcrições de legendas, capturas do jogo fora das poucas em
`docs/screenshots/`, ou `.exe` / `.dll` compilados. O `.gitignore` cobre isso; o `tools/check_game_files.py`
confere os arquivos do commit (hook de pre-commit) e uma GitHub Action confere cada pull request. Mods e
traduções podem, desde que tenham só trabalho seu (o pacote PT-BR guarda as traduções pelo hash do texto, sem
texto do jogo).

## 5. Reportando bugs

Abra uma issue com o modelo **Bug report** e inclua:
- versão (tag da release ou commit), versão do Windows, CPU / GPU, controle(s);
- o que você fez e o que aconteceu (fase / menu, passos para reproduzir);
- o log: o jogo escreve no console, então reproduza o bug com `CrashRecomp.exe > crash_log.txt 2>&1` num
  prompt de comando dentro de `dist\` (nos testes ele já está em `build\runs\<execução>\run.log`);
- em crashes, o bloco `[crash]` / `[fatal]` do log;
- o **título da janela** (`... | run <pasta> | session <id> | PID n`) ou a linha `jogo: PID ...`, para casar a
  instância com o log;
- as opções do seu `settings.toml` diferentes do padrão (ou o arquivo inteiro).

Nunca anexe arquivos do jogo.
