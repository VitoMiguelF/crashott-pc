# CrashRecomp: guia do projeto (PT-BR)

> O guia técnico completo: como tudo funciona, ferramentas, testes e recursos. Para instalar e jogar, veja o [README](../README.md).

Recompilação estática de **Crash of the Titans** (Xbox 360, title ID `565507E4`, `Crash007f.exe`) para código nativo x86-64 do Windows, usando [XenonRecomp](https://github.com/hedge-dev/XenonRecomp).

Nenhum arquivo do jogo é distribuído: tudo é gerado a partir da sua própria cópia.

**Estado (v1.04, 2026-10-24)**: o jogo roda da abertura até fases de vários episódios (boot direto testado em L1_E1, L3_E2 e L5_E3, também com o HD AI Remaster), com áudio, legendas, tradução PT-BR, controles de PC (teclado, mouse, DualSense), tela de configurações nativa (abas JOGO, ÁUDIO, VÍDEO, GRÁFICOS, CONTROLES e DEFINITIVE EDITION), DLSS/FSR, título "Definitive Edition", cores do mojo do PS2, HD AI Remaster, câmera livre experimental, cenas e vídeos que dá para pular (v1.01) e o console de desenvolvedor opcional com troca de skin (v1.01); o **co-op local** com dois controles e a escolha de JOGADOR 1 / JOGADOR 2 (v1.02) e o instalador instala sozinho os pacotes Python da exportação de assets (v1.02); o renderizador só Vulkan com DLSS e FSR 3.1 e o instalador que traz o próprio toolchain, sem instalar nada (v1.03); o **caminho de renderização nativo**: a cena numa passada só em vez das três faixas do tiling do Xbox 360, o flush de estado e o kick do D3D em código nativo, o vblank sem o teto de 500 fps e os shaders compilados na instalação e aquecidos em segundo plano, de ~180 para **~500 fps** na vila do L1_E1 (v1.04, veja `docs/internals/native-gpu-plan.md`), e o launcher com a vista simples e uma opção ILUMINAÇÃO única (v1.04). Para jogar, use a pasta `dist\` (veja "Jogando").

## Estrutura

| Pasta | Conteúdo |
|---|---|
| `dist\` | **A pasta para jogar**: `CrashLauncher.exe` (o launcher), `CrashRecomp.exe`, DLLs, `game\`, `mods\`, `userdata\` (seus saves e opções), `languages\`, `data\`, `licencas\` (veja "Jogando"). Montada por `tools\make_dist.py` |
| `runtime/` | Runtime que transforma o código gerado em `CrashRecomp.exe` (veja `runtime/NOTES.md`) |
| `launcher/` | O launcher (`CrashLauncher.exe`): as opções do `settings.toml` com a cara do jogo, antes de jogar (veja "Launcher") |
| `generated/ppc/` | C++ gerado pelo XenonRecomp (20 144 funções), feito na sua máquina pelo `tools\setup.py` (fora do git) |
| `config/` | `Crash.toml` (config do recompilador), `Crash_switch_tables.toml` (380 jump tables), `run.toml` (perfis de execução e teste) e `recorded\` (perfis gravados com F5) |
| `languages/` | Pacotes de idioma (fonte; o `make_dist` copia para `dist\languages`): `pt-br\` |
| `tests/` | `userdata_base\` (saves e `settings.toml` fixos dos testes) e `shadercache\` (cache de shaders dos testes) |
| `docs/capturas/` | Capturas que comprovam cada recurso (citadas neste README e no `runtime/NOTES.md`), reduzidas para JPEG |
| `private/` | Só seu, fora do git: `default.xex` (copiado do jogo pelo setup) e `image.bin` (ferramentas), `symbols\`, `subtitles\` e `remaster_full\` (instalados dos assets da release), `assets\` (exportados para modding) |
| `thirdparty/` | FFmpeg (LGPL), DLSS (NVIDIA), FidelityFX FSR 3.1 (AMD): o setup baixa das fontes oficiais e confere o SHA-256; SMAA e Dear ImGui (MIT) estão no git; licenças em cada pasta |
| `tools/XenonRecomp/` | XenonRecomp no commit fixado com `patches/XenonRecomp.patch` (veja abaixo): o zip da release já traz o código (sem `.git`, com `.crashrecomp_source`); num clone do repositório, o `tools\setup.py` o clona |
| `toolchain/` | O toolchain portátil (fora do git): llvm-mingw (clang + lld + headers/libs do mingw-w64), CMake, Ninja e o Python embutido com numpy e Pillow. Vem no zip da release; num clone, `python tools\toolchain.py fetch` |
| `tools/xexdump/` | Extrai a imagem descriptografada (`image.bin`) e a tabela de símbolos do XEX, para as ferramentas |
| `tools/run_game.py` | Compila, roda o jogo com um perfil de `config/run.toml` e confere o resultado (veja "Rodando e testando") |
| `tools/frontend_tree.py`, `tools/convert_recording.py` | A árvore do frontend com os números do jogo, e o conversor de gravações F5 antigas para o boot direto (veja "Boot direto e eventos") |
| `tools/make_dist.py` | Monta/atualiza a pasta `dist\` a partir do build |
| `launcher/installer.inc`, `launcher/installer_payload.inc`, `launcher/gpu_probe.inc` | O instalador gráfico (`CrashSetup.exe`): a cara do launcher na frente do `tools\setup.py`; os arquivos embutidos no EXE único e a extração; a placa de vídeo e o teste do DLSS (também no launcher) |
| `tools/setup.py`, `tools/buildenv.py`, `tools/toolchain.py`, `tools/make_installer.py`, `tools/make_release.py` | O instalador (do dump ao `dist\`), a escolha do toolchain (o de `toolchain\` ou Visual Studio + clang-cl), o download fixado do toolchain portátil, o EXE único do instalador e os assets da release |
| `runtime/gpu/dlss_module/` | O `CrashDlss.dll`: o DLSS (NGX) atrás de uma interface C, carregado pelo jogo, pelo launcher e pelo instalador |
| `tools/make_icon.py`, `launcher/crashrecomp.ico` | O ícone do launcher e do instalador (desenho nosso, nada do jogo) |
| `tools/perf/` | Benchmark e perfil de CPU: `bench.py`, `walk_window.py`, `report.py`, o amostrador `sampler.cpp` e `profile.py` (veja "Desempenho") |
| `tools/find_swallowed.py`, `tools/rtti_scan.py` e afins | Funções "engolidas" pelo config e nomes de classes/métodos pelo RTTI (veja "Lendo o código do jogo") |
| `tools/rcf.py`, `texts.py`, `assets.py`, `audio.py`, `textures.py`, `models.py`, `p3d.py`, `hd_textures.py`, `subtitles.py`, `assettool/` | Extraem os `.rcf`, textos, sons, vídeos, texturas e modelos para formatos abertos e geram mods (veja "Extraindo e modificando assets") |
| `build/` | Saída do CMake (`build\CrashRecomp.exe`) e `runs\` (uma pasta por execução do `run_game.py`) |

## Requisitos

Nada para instalar. O zip da release traz a pasta `toolchain\` com tudo o que compila o jogo: **llvm-mingw** (clang 23, lld, libc++ e os headers/bibliotecas de importação do mingw-w64 com a UCRT; só o alvo x86-64), **CMake**, **Ninja** e o **Python embutido** oficial com numpy e Pillow (`tools/toolchain.py` fixa as versões pelo SHA-256). Num clone do repositório, baixe a mesma pasta uma vez com `python tools\toolchain.py fetch` (precisa de um Python 3.11+ qualquer e do Git, que clona o XenonRecomp).

As ferramentas (`tools/buildenv.py`) põem as pastas de `toolchain\` na frente do `PATH` em toda compilação. Para compilar como antes, com o **Visual Studio 2022+ / Build Tools** ("Desenvolvimento para desktop com C++") + **LLVM `clang-cl`**, ponha `[build] toolchain = "msvc"` em `config/local.toml` (fora do git), use `CRASH_TOOLCHAIN=msvc`, `setup.py --toolchain msvc` ou FERRAMENTAS = VISUAL STUDIO no instalador; nesse modo o Visual Studio vem do `vswhere` e o resto do `PATH` e das pastas de instalação comuns. Cada pasta de build lembra o toolchain que a configurou (`.crashrecomp_toolchain`) e é reconfigurada sozinha quando ele muda.

O jeito simples é o instalador: `CrashSetup.exe` ou `toolchain\python\python.exe tools\setup.py --game <pasta do jogo>` faz todos os passos abaixo (veja o [README](../README.md)).

## Passo a passo (à mão; o `tools\setup.py` faz o mesmo)

```bat
:: (com o toolchain portátil no PATH)
set PATH=%CD%\toolchain\llvm-mingw\bin;%CD%\toolchain\cmake\bin;%CD%\toolchain\ninja;%CD%\toolchain\python;%PATH%

:: 0. XenonRecomp no commit fixado + as correções locais (o zip da release já traz isso), e as bibliotecas de terceiros
git clone https://github.com/hedge-dev/XenonRecomp tools\XenonRecomp
git -C tools\XenonRecomp checkout ddd128bcca99fe8bfbb99bea583c972351fa6ace
git -C tools\XenonRecomp submodule update --init --recursive
git -C tools\XenonRecomp apply --ignore-whitespace ..\..\patches\XenonRecomp.patch
:: (FFmpeg, DLSS e FSR: thirdparty\*\README.md; o setup.py baixa e confere)

:: 1. Compilar as ferramentas
cmake -S tools\XenonRecomp -B tools\XenonRecomp\build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ "-DCMAKE_CXX_FLAGS=-include cstdlib" -DCMAKE_EXE_LINKER_FLAGS=-static
cmake --build tools\XenonRecomp\build --target XenonAnalyse XenonRecomp

:: 2. Detectar jump tables
tools\XenonRecomp\build\XenonAnalyse\XenonAnalyse.exe private\default.xex config\Crash_switch_tables.toml

:: 3. Recompilar PPC -> C++ (a pasta generated\ppc precisa existir)
tools\XenonRecomp\build\XenonRecomp\XenonRecomp.exe config\Crash.toml tools\XenonRecomp\XenonUtils\ppc_context.h

:: 4. Compilar o código gerado (biblioteca CrashPPC), o runtime (build\CrashRecomp.exe) e o launcher (build\CrashLauncher.exe)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build

:: 5. Montar a pasta para jogar (dist\), dizendo onde está o seu jogo
python tools\make_dist.py --game D:\Jogos\Crash
```

Com o Visual Studio, os mesmos passos rodam num prompt `vcvars64` com `-DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl` (sem as duas flags extras do XenonRecomp). O `-include cstdlib` só existe porque o `fmt` do XenonRecomp usa `malloc` sem incluir `<cstdlib>`, o que a libc++ não perdoa; o XenonRecomp compilado pelos dois toolchains gera exatamente o mesmo `generated\ppc`.

Antes do passo 2, copie o `default.xex` do seu jogo para `private\` (o XenonRecomp e as ferramentas leem de lá). `private\image.bin` (a imagem descriptografada, para `find_swallowed.py` e `rtti_scan.py`) sai de `tools\xexdump\build\xexdump.exe private\default.xex private\image.bin private\symbols.txt`. O jogo em si não usa nenhum dos dois: o `CrashRecomp.exe` descriptografa o `default.xex` da pasta do jogo ao abrir (`runtime/xex_image/`, com o código do XenonUtils).

## Jogando (`dist\`)

`dist\CrashLauncher.exe` (duplo clique) abre o launcher, onde dá para mudar as opções e clicar em JOGAR; `dist\CrashRecomp.exe` abre o jogo direto. Tudo o que eles usam fica ao lado deles; nada depende do caminho do projeto:

```
dist\
  CrashLauncher.exe                o launcher (opções com a cara do jogo, depois JOGAR)
  CrashRecomp.exe                  o jogo
  avcodec-62.dll, avutil-60.dll,   FFmpeg (LGPL): som XMA
  swresample-6.dll
  nvngx_dlss.dll                   DLSS (NVIDIA)
  amd_fidelityfx_*.dll             FSR 3.1 (AMD)
  game\                            o seu jogo (default.xex, default.rcf, english.rcf, sound1.rcf...), ou nada (veja abaixo)
  mods\                            mods (um por pasta)
  userdata\                        seus saves (content\), settings.toml (as opções), cache de shaders
  languages\pt-br\                 tradução PT-BR
  data\                            opcional, derivado do SEU jogo: subtitles\english.txt (legendas),
                                   symbols\functions.csv (nomes nos relatórios de erro),
                                   remaster_full\ (HD AI Remaster)
  licencas\                        licenças de terceiros
  LEIA.txt
```

- **Pasta do jogo**: o exe procura, nesta ordem: `--game <pasta>`, `game\` ao lado do exe (se tiver `default.xex` e `default.rcf`), `[paths] game` do `userdata\settings.toml` e, na primeira vez sem nenhum dos três, uma janela para escolher a pasta (a escolha é gravada no `settings.toml`). O jogo pode ficar onde estiver (somente leitura, nunca é alterado nem copiado; o `make_dist --game` grava o caminho) ou dentro da pasta, em `dist\game\` (o instalador copia com COPIAR O JOGO PARA A PASTA).
- **Lógica do jogo**: o `default.xex` da pasta do jogo é descriptografado na memória a cada abertura (~0,1 s); não há `image.bin` nem `private\` na pasta do jogo.
- **Atualizar** depois de compilar: `python tools\make_dist.py` (ou `cmake --build build --target dist`). Ele troca os dois exes, as DLLs, `languages\`, `data\` e `licencas\`, e **nunca** mexe em `game\`, `mods\` e `userdata\` (só os cria se faltarem). Falha se o `dist\CrashRecomp.exe` estiver aberto. `--no-remaster` deixa o HD AI Remaster de fora; `--no-data`, toda a pasta `data\`. O HD AI Remaster entra em `data\` por *hard link* (os mesmos arquivos de `private\remaster_full`, sem ocupar espaço de novo).
- **Linha de comando** (opcional): `--game`, `--mods`, `--userdata`, `--languages`, `--data <pasta>`, `--xex <default.xex>` e `--image <image.bin>` (usa uma imagem pronta em vez de descriptografar). O log começa com as linhas `[paths]`, que dizem de onde veio cada pasta.
- **Exe do build**: `build\CrashRecomp.exe` aberto à mão (sem `languages\` ao lado e dentro do projeto) usa `languages\` e `private\` do projeto e `mods\`/`userdata\` de `dist\`, ou seja, os mesmos saves do jogador. Os testes passam todas as pastas pela linha de comando.

### O que vai para o GitHub (v1.0 a v1.04)

| No git (público) | Assets da release (opcionais, separados do código) | Nunca publicado |
|---|---|---|
| o código (`runtime/`, `launcher/`, `tools/`), `config/`, `patches/XenonRecomp.patch`, `languages/pt-br` (só a tradução por hash, sem textos do jogo), `tests/userdata_base` (saves de teste), `docs/` (sem `capturas/`) e as licenças | o instalador `CrashSetup-<versão>.exe` (até a v1.04, o zip do código + `CrashSetup.exe`), as legendas (transcrição em inglês + PT-BR) e o HD AI Remaster em partes (`tools/make_release.py`) | o jogo e tudo extraído dele: `private\`, `generated\ppc\`, `dist\` (o `CrashRecomp.exe` tem o código do jogo recompilado), `docs\capturas\`, `tests\shadercache\`, os assets exportados |

Da v1.01 à v1.04 as releases publicam só o zip do código + `CrashSetup.exe` (`python tools\make_release.py v1.04 --only source`): as legendas e o HD AI Remaster não mudaram, continuam como assets da v1.0, e o `tools/setup.py` os baixa de lá (`RELEASE_TAG`, que só muda quando esses assets forem refeitos).

A partir da v1.03 o zip do código também leva `toolchain\` (llvm-mingw, CMake, Ninja e o Python embutido com numpy e Pillow, ~480 MB descompactados; `python tools\toolchain.py fetch` antes do `make_release.py`) e o código do XenonRecomp já corrigido (`tools\XenonRecomp\`, sem `.git`, com o carimbo `.crashrecomp_source`): o jogador não instala nada nem precisa do Git. O zip fica com ~150 MB. São ferramentas de terceiros redistribuíveis (licenças no `THIRD_PARTY_NOTICES.md`), nenhuma coisa do jogo.

A partir da v1.05 o jogador baixa **um EXE só**, `CrashSetup-<versão>.exe` (~210 MB; `python tools\make_installer.py <tag>`, ou `make_release.py <tag> --only installer`): o `build\CrashSetup.exe` com tudo o que o zip levava (código no HEAD, XenonRecomp corrigido, `toolchain\`) e mais as DLLs que o `setup.py` baixava (FFmpeg, DXC, FSR, e o DLSS como `nvngx_dlss.dll` + o nosso `CrashDlss.dll` pré-compilado, sem headers nem `.lib` do SDK), comprimidos em LZMS (a Compression API do Windows, `cabinet.dll`) em blocos de 32 MB e anexados ao fim do exe. O instalador pergunta onde instalar, extrai lá em todos os núcleos (~10 s) e roda o `setup.py` dessa pasta: nada é baixado fora as legendas e o HD AI Remaster opcionais. Só vai o necessário para compilar e rodar (`INSTALL` / `MODDING` no `make_installer.py`: sem `docs\`, `tests\`, `.github\`, READMEs e ferramentas de desenvolvimento; as da exportação de assets e o numpy/Pillow só com ASSETS PARA MODDING). A pasta instalada (`Crash of the Titans Definitive Edition\`) tem só o que o jogador usa: `CrashLauncher.exe`, `CrashOTTDE.exe` (o `CrashRecomp.exe` renomeado), as DLLs, `game\` (o dump, sempre copiado), `userdata\`, `mods\`, `setup.log` e `.crashrecomp_files`; o projeto fica em `CrashRecomp\` dentro dela, com `languages\`, `data\` e `licencas\` (`make_dist.py --installed`, `setup.py --play-dir`; o runtime acha `languages\` e `data\` lá, `paths.h`). O formato está no começo do `tools/make_installer.py`; `--worktree` empacota a árvore de trabalho em vez do HEAD (testes). Nenhum arquivo do jogo e nenhum código recompilado entram no EXE.

O `.gitignore` cobre tudo isso e ainda recusa `*.xex`, `*.rcf`, `*.bik`, `*.rsd`, `*.p3d`, `image.bin` e qualquer pasta `game/`, para um dump nunca entrar por acidente.

### Launcher

`CrashLauncher.exe` mostra as opções do `userdata\settings.toml` antes de abrir o jogo, com a cara do menu do jogo: o tecido amarelo e as folhagens do menu principal, o logo, "Definitive Edition" em dourado, as fontes Titans (as de NEW GAME / LOAD GAME), as estrelas das barras de volume, a moldura das caixas de mensagem e os ícones dos botões. Nada disso está no launcher: ele lê tudo do `default.rcf` do seu jogo ao abrir, como a tela SETTINGS do jogo faz. Capturas em `docs\capturas\launcher\`.

| Parte | O que tem |
|---|---|
| Esquerda | Logo, **JOGAR** (salva o que mudou e abre o `CrashRecomp.exe` ao lado, com os mesmos argumentos de pasta), **SALVAR**, **PADRÕES** (volta tudo ao padrão, menos o idioma e a pasta do jogo), **SAIR** (pergunta se há mudanças não salvas); embaixo, onde está o jogo e se as opções estão salvas |
| Vista simples (o padrão) | Uma página só, **OPÇÕES**: IDIOMA, RESOLUÇÃO, LIMITE DE FPS, HD AI REMASTER e **ILUMINAÇÃO** (ORIGINAL / REMASTER). REMASTER liga todo o remaster da iluminação nos valores recomendados (SSAO ALTA, luz indireta a 100%, sombras de contato e suaves); ORIGINAL desliga tudo. Se as opções de iluminação do AVANÇADO forem uma mistura, a linha mostra **PERSONALIZADO** (RIGHT vai para REMASTER, LEFT para ORIGINAL) |
| **AVANÇADO** (canto inferior direito; Y, F2 ou clique) | As abas da tela SETTINGS (JOGO, ÁUDIO, VÍDEO, GRÁFICOS, CONTROLES, DEFINITIVE EDITION), com as mesmas linhas, ordem, valores, limites e textos de ajuda (menos ARMAZENAMENTO, que é só um aviso do menu principal), mais **INSTALAÇÃO**: pasta do jogo (escolher / trocar), mods (abre a pasta), saves e opções (abre a pasta) e os dados extras encontrados. **SIMPLES** no mesmo canto volta. A vista escolhida fica gravada na hora em `[launcher] view` (sem gravar as outras mudanças) |
| Diferenças da tela do jogo | O DLSS só é oferecido onde roda (NVIDIA RTX com o DLSS instalado, conferido pelo próprio NGX num processo filho `--probe-dlss`, ~4 s em segundo plano); nas outras placas aparece apagado "DLSS (REQUER NVIDIA RTX)" e o LEFT/RIGHT o pula, como na tela do jogo; legendas mostram "SEM TEXTO" quando o idioma não tem transcrição; a segunda linha da ajuda diz "Vale ao abrir o jogo." onde o jogo diz "Vale na hora." ou "O jogo reinicia..."; JOGADOR 1 / 2 lista os controles pelo número (o launcher não lê os controles) |

As duas telas leem as mesmas definições (`runtime/settings/options.{h,cpp}`): as abas e as linhas de cada uma, a chave do `settings.toml`, o nome, o valor mostrado, a ajuda em inglês e português, os limites das estrelas e o que LEFT / RIGHT fazem. Uma opção nova entra ali e aparece nas duas. O launcher só acrescenta as linhas dele (INSTALAÇÃO, ILUMINAÇÃO da vista simples, o título TECLADO E MOUSE sobre as teclas) e o que só ele sabe (`options::Context`). Os pacotes de texturas HD dos mods (`[graphics] hd_textures`, ligado por padrão) não têm linha em nenhuma das telas; o HD AI Remaster usa as texturas dele sempre que está ligado, mesmo com `hd_textures = false`.

- **Controles**: mouse (passar por cima seleciona, clique muda; nas estrelas, clicar ou arrastar define o valor; nas setas, clique para voltar ou avançar), teclado (setas, Enter, Q/E ou PgUp/PgDn trocam de aba, Y ou F2 alternam SIMPLES / AVANÇADO, F5 joga, Esc sai) e controle (D-pad/analógico, A muda, LB/RB trocam de aba, Y alterna a vista, START joga, B sai). Na aba CONTROLES, A / Enter / clique numa ação espera a nova tecla ou botão do mouse (Esc ou B cancela); X / Backspace volta ao padrão. A tecla nova sai das outras ações, como na tela do jogo.
- **Idioma**: em português quando o idioma do jogo é Português (BR), senão em inglês; muda junto com a opção IDIOMA.
- **Sem a pasta do jogo** (primeira vez, sem `game\` nem `[paths] game`): o launcher abre no AVANÇADO, na aba INSTALAÇÃO, com fundo liso e a fonte do Windows; ao escolher a pasta ele grava `[paths] game` e se reabre com a arte do jogo.
- **Arquivos**: o mesmo `settings.toml` do jogo (o jogo lê ao abrir). As pastas são resolvidas como no jogo (`runtime/paths.h`), então `--game`, `--userdata` etc. também valem para o launcher e são repassados ao jogo. O launcher escreve `launcher.log` ao lado dele.
- **Teste sem janela**: `CrashLauncher.exe --screenshot <arquivo.png> [--tab N] [--view simple|advanced]` desenha a vista simples ou a aba N do AVANÇADO (0 = JOGO ... 6 = INSTALAÇÃO) num PNG e sai, sem gravar nada.


## Rodando e testando (`config/run.toml`)

Tudo o que controla a execução fica em `config/run.toml`: caminhos (exe, pasta do jogo, mods, userdata), compilação automática e **perfis**. Cada perfil define tempo limite, áudio, renderização, botões apertados automaticamente, capturas de tela, gravação de áudio, logs extras e o que conferir no fim. O script `tools/run_game.py` lê esse arquivo, compila o que mudou (com o toolchain de `tools/buildenv.py`), roda o jogo e diz se o teste passou. Não precisa preparar nenhum prompt.

```bat
python tools\run_game.py --list                 :: lista os perfis
python tools\run_game.py jogar                  :: compila e abre o jogo com os seus saves (dist\userdata)
python tools\run_game.py boot                   :: um teste
python tools\run_game.py boot --dist            :: o mesmo teste com o dist\CrashRecomp.exe, como o jogador abre
python tools\run_game.py --all                  :: todos os perfis com test = true (demora; prefira os perfis do que mudou)
python tools\run_game.py menu --set timeout=90 --set audio=true --no-build
```

**Userdata dos testes**: todo perfil de teste (`userdata_mode = "copy"`, o padrão) roda numa **cópia** de `tests\userdata_base\` (saves com o primeiro slot no episódio 2 e um `settings.toml` com as opções padrão; veja o `LEIA.md` de lá) feita na pasta da execução, mais o cache de shaders dos testes (`tests\shadercache\`, que recebe os shaders novos de cada execução). Assim todo teste parte sempre da mesma userdata, pode sobrescrever slots e opções, e **nunca** toca nos seus saves. Só o perfil `jogar` (`userdata_mode = "shared"`) usa a sua userdata de verdade (`dist\userdata`). Opções diferentes num teste vão em `CRASH_SETTINGS` (veja "Configurações"), não na base.

**`--dist`**: roda `dist\CrashRecomp.exe` passando só `--userdata` (a cópia da base, com `[paths] game` gravado): o exe acha sozinho o jogo, os mods, os idiomas, `data\` e as DLLs, como no duplo clique. Use depois de `tools\make_dist.py` para conferir a pasta de jogar.

**Quem abriu esta janela?** O título da janela diz de onde veio cada instância do jogo, por exemplo `Crash of the Titans (CrashRecomp) | run 20261021-145044_boot | session d6124b29 | PID 16568`:
- `run`: a pasta da execução em `build\runs\` (ou `profile`, sem pasta).
- `session`: o começo do id da sessão do Claude Code que abriu o jogo (`CLAUDE_CODE_HOST_SESSION_ID`). Não aparece quando você abre o jogo por conta própria.
- `CRASH_WINDOW_TAG`: um texto livre, se tiver sido definido.
- `PID`: o processo, para achar no Gerenciador de Tarefas.

O `run_game.py` também imprime `jogo: PID ..., sessão ..., até N s` ao iniciar, e o `run.log` tem a linha `[gpu] window title:`.

| Perfil | O que faz | Duração |
|---|---|---|
| `jogar` | Abre o jogo sem limite de tempo, com os seus saves | — |
| `boot` | A abertura completa (logos, vídeos) e o loop de quadros; confere ≥ 480 quadros sem `[fatal]`/`[crash]` | 20 s |
| `menu` | Pula as intros (`skip_intros`) e chega ao menu; grava a música em `audio.wav` | 25 s |
| `menu_loaded` | `boot = "load:1:hub"`: LOAD GAME no primeiro slot até o menu do episódio | 30 s |
| `load_pelos_menus` | Ponta a ponta pela interface: LOAD GAME só com botões, cada um ancorado na tela em que cai, até a fase | 50 s |
| `primeira_fase` | Ponta a ponta pela interface: NEW GAME → nome → dificuldade → slot 1 pelo piloto automático de menus → cena de abertura e vila | 150 s |
| `primeiro_slot_load` | `boot = "load:1"`: LOAD GAME no primeiro slot e CONTINUE STORY até a fase (episódio 2) | 40 s |
| `fase_direta` | `boot = "level:L1_E3"`: direto numa fase sem save, com cena de abertura; anda depois dela | 40 s |
| `configuracoes` | Abre SETTINGS no menu (aba JOGO), muda volume (ÁUDIO) e anti-aliasing (VÍDEO), passa por todas as abas (RB) até DEFINITIVE EDITION, salva, fecha e faz LOAD GAME, com uma captura de cada aba (veja "Configurações") | 50 s |
| `iluminacao_tela` | Abre SETTINGS, vai à aba DEFINITIVE EDITION e liga a iluminação remaster pelo controle (SSAO ALTA, luz indireta 110%, contato, suaves), salva, faz LOAD GAME, continua a história e confere que os passes rodam na fase (veja "Iluminação (remaster)") | 70 s |
| `iluminacao` | Vila sem a cena de abertura com 8 capturas paradas e andando, sempre nas mesmas posições: rode com e sem `[lighting]` (e `CRASH_LIGHTING_SHOW`) para comparar | 48 s |
| `configuracoes_reinicio` | SETTINGS: idioma PT-BR, sai, aceita "Reiniciar agora?"; o runner reabre o jogo, que pula as intros sozinho, e faz LOAD GAME (veja "Aplicar sem fechar o jogo") | 30 + 30 s |
| `teclado` | Só teclado e mouse: LOAD GAME pelos menus e, na fase, WASD, Alt (andar devagar), Espaço, mouse 1/2, R (ataque giratório), Q + mouse 1 (girar + ataque leve), E, Shift e Esc (pausa e volta); confere no log o andar devagar, o giro e que o **próprio jogo** detectou a volta do analógico (veja "Controles de PC") | 65 s |
| `icones` | Ícones dos botões trocando entre teclado, PlayStation e Xbox no menu principal, nos saves, no episódio e na pausa (`DEVICE_*` no script) | 50 s |
| `mouse_menus` | Menus pelo mouse como num jogo de PC: de NEW GAME, passar em SETTINGS seleciona direto (o log mostra uma troca só, `selection 0 -> 3`), roda, teclado e D-pad continuam valendo, clique abre SETTINGS; lá, clique nas abas e no RB, passar nas linhas, clique e arrasto nas estrelas, clique na linha e na seta, roda (rola a aba CONTROLES), clique em VOLTAR; LOAD GAME, save e CONTINUE pelo clique, menu do episódio; na fase, pausa, passar em OPTIONS e clique em RESUME GAME. Capturas em `build\mouse_menus\` | 90 s |
| `controles_tela` | Aba CONTROLES da tela SETTINGS pelo teclado: troca PULAR para K, salva, e faz LOAD GAME | 55 s |
| `coop_p2` | Co-op com um segundo controle **virtual** (entradas `P2_`): vila sem a cena, B entra (JOIN GAME), Y desce da mochila, anda, pula e START pausa como P2 (veja "Co-op local") | 50 s |
| `coop_jogadores` | Só com **dois controles de verdade** (`test = false`): SETTINGS > CONTROLES, JOGADOR 1 = o segundo, JOGADOR 2 = o primeiro; confere a troca no log | 30 s |
| `coop_manual` | Para alguém jogar com dois controles (`test = false`): vila sem a cena por 90 s, uma captura a cada 6 s | 90 s |
| `configuracoes_resolucao` | SETTINGS: resolução 1920x1080 e DLSS Qualidade aplicados ao fechar a tela, sem reiniciar; LOAD GAME até a fase, com o overlay nas capturas | 55 s |
| `configuracoes_pausa` | `boot = "load:1"`; na fase: pausa > OPÇÕES abre a tela (a original fica escondida); MÚSICA ao vivo, COOPERATIVO / INVERTER EIXO / SALVAMENTO AUTOMÁTICO na aba JOGO, resolução ao fechar; reabre pela pausa "sem salvar"; volta a jogar | 65 s |
| `configuracoes_jogo` | Menu principal: COOPERATIVO = CAVALINHO e INVERTER EIXO na aba JOGO antes de entrar; LOAD GAME até a fase, e as opções continuam valendo por cima do save | 50 s |
| `sair_do_jogo` | Menu principal: desce até SAIR DO JOGO, aperta Y (não deve abrir o seletor de armazenamento) e A; confere a saída 0 (veja "Menu de PC") | ~15 s |
| `nome_teclado_controle` | NEW GAME: nome AQ1Z no teclado da tela pelo controle (D-pad, A, X apaga, Y pronto), NORMAL, sobrescreve o slot 3 e confere o nome no save (veja "Nome e saves no PC") | 50 s |
| `nome_teclado_fisico` | O mesmo pelo teclado físico: CRASH digitado, uma letra a mais com o nome cheio, Backspace, Enter; slot 4 | 50 s |
| `nome_teclado_mouse` | O mesmo pelo mouse: MOUSE clicando nas teclas, APAGAR e PRONTO; slot 5 | 50 s |
| `apagar_save` | LOAD GAME: X no slot 2; B cancela, NÃO cancela, SIM apaga; a lista é lida de novo (slot 2 vazio, os outros no lugar) e o slot 1 ainda carrega | 60 s |
| `apagar_save_mouse` | LOAD GAME pelo teclado e mouse: Delete no slot 3 e clique direito cancela; clique em X APAGAR no slot 4 e em SIM apaga | 60 s |
| `camera_livre` | `boot = "load:1"` com a câmera livre ligada: gira, anda relativo à câmera, pula, R3 recentra (veja "Câmera livre") | 45 s |
| `camera_livre_abertura` | NEW GAME (piloto automático) com a câmera livre ligada: a cena de abertura com a câmera do jogo, depois a câmera livre na vila | 165 s |
| `gravar` | Abre o jogo numa cópia dos saves gravando o controle desde o boot; F5 ou fechar a janela salva um perfil em `config/recorded` (veja "Gravando um perfil jogando") | — |
| `gravado_<data>` | Perfis gravados com F5 (`config/recorded/<nome>.toml`): repetem do boot o que foi jogado | o gravado + 1 s |
| `desempenho_vila` | Benchmark (a cena mais pesada): NEW GAME pelo piloto automático de menus, cena de abertura e ~35 s na vila andando, girando a câmera e atacando; rode com `tools\perf\bench.py` (veja "Desempenho") | ~3,5 min |
| `desempenho` | Benchmark mais leve: LOAD GAME no episódio 2 e ~48 s na fase | ~2 min |
| `desempenho_vila_direto` | Benchmark rápido: boot direto no L1_E1 pulando a cena de abertura e 22 s parado na vila logo que o jogador ganha o controle (o momento mais pesado); rode com `tools\perf\bench.py ... --profile desempenho_vila_direto` | ~45 s |

Os perfis `graficos`, `mojo`, `ptbr`, `ptbr_novo`, `legendas`, `legendas_ptbr`, `primeira_fase_load` e `primeira_fase_otimizado`, citados mais abaixo nas capturas antigas, saíram do `run.toml` (as definições não foram guardadas). Para refazer um deles, copie `primeiro_slot_load` (ou `fase_direta`), mude `env` (ex.: `CRASH_SETTINGS = "game.language=pt-br"`) e ancore as capturas em `["@gameplay_ready"]` (veja "Boot direto e eventos").

`python tools\run_game.py <perfil> --runs build\testes_auto` grava a execução em outra pasta no lugar de `build\runs`.

- **Saída do jogo**: `ok` (0, janela fechada ou SAIR DO JOGO no menu), `fatal` (1), `timeout` (2, o watchdog encerrou: é o normal num teste), `crash` (3) e `reinicio` (4, reinício pedido pela tela de configurações; veja abaixo).
- **Reinício pela tela de configurações**: o script roda o jogo com `CRASH_RESTART=exit`, então um reinício aceito na tela SETTINGS termina o processo com o código 4 em vez de o jogo se reabrir fora da vista do script. O script então abre o jogo de novo (mesmos argumentos + `--restarted`, mesma pasta de dados) e continua acompanhando: o `run.log` segue no mesmo arquivo (com uma linha `[run_game] ===== reinicio N`), e as capturas e o `frametimes.csv` da abertura N vão para `reinicio_N\`. No perfil, `restart_input` (botões da nova abertura, no relógio dela), `restart_env` e `restart_timeout` controlam a abertura seguinte (padrão: sem botões, o mesmo `timeout`); `max_restarts` (padrão 3) limita o laço, e `expect.restarts` confere quantos houve. O `CRASH_SETTINGS`, o `input`/`replay` e a gravação da primeira abertura não passam para as seguintes (os valores já estão no `settings.toml`). `env = { CRASH_RESTART = "relaunch" }` testa o caminho normal (o próprio jogo se reabre; o script só vê a saída 0 do primeiro processo).
- **Cada execução** ganha uma pasta em `build\runs\<data>_<perfil>\` com `run.log`, as capturas (`screenshot_NNNNN.png`, pelo número do quadro), `audio.wav`, `build.log`, `settings.json` (opções e variáveis usadas), `frametimes.csv` (o tempo de cada quadro), `marks.txt` (quadros marcados com F2), `replay.txt` (o `replay` do perfil, como o runtime lê) e `result.json` (quadros, saída, desempenho, marcas e cada checagem).
- **Saves**: veja "Userdata dos testes" acima. `userdata_from = "pasta"` (os perfis gravados usam) troca a base por outra pasta com `content\` e `settings.toml`.
- **Teclado e mouse no script**: `[t, "KEY_W", 2.5]` segura uma tecla (os nomes de `[controls]`: `KEY_SPACE`, `KEY_LSHIFT`, `KEY_MOUSE1`, `KEY_ESC`...), passando pelas mesmas ligações do teclado de verdade; `[t, "MOUSE", x, y]`, `[t, "CLICK", x, y]` e `[t, "RCLICK", x, y]` põem o cursor em (x, y) da imagem 1280x720 do jogo, com um clique, `[t, "LDOWN", x, y]` / `[t, "LUP", x, y]` apertam / soltam o botão esquerdo ali (arrastar) e `[t, "WHEEL", n]` gira a roda n dentes (+ = para cima) (com entradas de mouse no script, o mouse de verdade é ignorado sobre a janela); `[t, "DEVICE_PLAYSTATION"]` (ou `_XBOX`, `_KEYBOARD`) faz de conta que aquele aparelho foi o último usado (os ícones).
- **Botões**: `input = [[27, "A"], [29, "A"]]` aperta A aos 27 s e aos 29 s, contados a partir da primeira leitura do controle (é o relógio `INPUT` do overlay); depois de uma âncora `["@evento"]`, contados a partir daquele evento do jogo (o jeito preferido: veja "Boot direto e eventos"). Cada botão fica apertado 200 ms; um terceiro número segura por mais tempo: `[80, "RIGHT", 1.5]` segura a seta para a direita por 1,5 s. Botões válidos: `A B X Y START BACK UP DOWN LEFT RIGHT LB RB L3 R3`, e os analógicos inclinados até o fim: `LS_LEFT LS_RIGHT LS_UP LS_DOWN` (andar) e `RS_LEFT RS_RIGHT RS_UP RS_DOWN` (câmera), ex.: `[146, "LS_UP", 3.0]`. Analógico em qualquer posição: `[42, "LS", 0.45, 0.9, 2.5]` (x, y de -1 a 1, por 2,5 s) e gatilhos `[50, "RT", 1.0, 0.6]` (0 a 1). Esse relógio é o de parede: se o boot ou um carregamento demorar mais (vários jogos abertos), os apertos chegam cedo. Os perfis gravados com F5 usam `replay`, que não tem esse problema (veja "Gravando um perfil jogando").
- **Desempenho**: o resumo de cada execução mostra FPS médio, 1% low, frame time médio, p99 e máximo e quantos quadros passaram de 50 ms, calculados de `frametimes.csv` a partir do quadro `perf_from_frame` (padrão 0; use para deixar de fora o boot e os carregamentos). Os mesmos números vão para `result.json` (`perf`).
- **Checagens** (`expect`): `exit` (lista de saídas aceitas), `min_frames`, `log_absent` e `log_contains` (expressões regulares), as opcionais de desempenho `min_fps`, `max_frame_ms_p99` e `max_hitches` (quadros > 50 ms) e, automaticamente, as capturas pedidas para quadros que a execução alcançou. O script sai com código 0 se tudo passou e 1 se algo falhou.
- **Variáveis extras** do runtime (`CRASH_GPU_TRACE` etc., veja `runtime/NOTES.md`) vão em `env = { ... }` no perfil. O script ignora qualquer `CRASH_*` que já esteja no ambiente, para que os resultados dependam só do arquivo.

Para criar um teste novo, copie um perfil, ajuste `boot`, `input` (ancorado em eventos), `timeout` e `expect`, e rode `python tools\run_game.py <nome>`.

### Boot direto e eventos (testes sem relógio)

Os testes não dependem mais de apertar START, DOWN, A... em horários fixos (que se perdiam quando o carregamento demorava, com outros jogos abertos ou quando um menu mudava). O runtime entra direto na fase ou no save **pelo código do próprio jogo**, e cada botão de um teste pode ficar preso a um **evento** do jogo em vez do relógio.

**Pular as intros**: `skip_intros = true` no perfil (ou `CRASH_SKIP_INTROS=1`, ou `CrashRecomp.exe --skip-intros`). Sem os vídeos de logo (Dolby, Radical, Shadow), sem os 5 s do texto legal e sem a tela de título: o menu principal aparece em ~6 s (antes ~14 s). Um reinício pela tela SETTINGS usa o mesmo caminho.

**Boot direto** (`boot = "..."` no perfil, `CRASH_BOOT=...` ou `--boot ...`):

| `boot` | O que faz | Jogável em |
|---|---|---|
| `menu` | Pula as intros e para no menu principal | (menu em ~6 s) |
| `level:L1_E3` | Entra direto na fase, sem save (pelo título e 2 s no menu principal, e então o caminho `SkipToLevel` que os desenvolvedores deixaram na árvore do frontend) | ~11 s (+ a cena de abertura da fase, se tiver; ~14 s no L1_E1 com `skip_cinematics`) |
| `load:1` | LOAD GAME no slot 1 (a tela de saves, "load complete") e CONTINUE STORY no menu do episódio, com o progresso do save | ~17 s |
| `load:1:hub` | O mesmo, mas para no menu do episódio | (HUB em ~14 s) |
| `load:2:L1_E3` | Carrega o slot 2 e, no menu do episódio, começa a L1_E3 no lugar da próxima fase, com o progresso do save | ~17 s |

Nada é apertado por horário: o runtime toma, em cada tela, a saída que o jogo tomaria com o botão (as mesmas ações rodam: carregar o save, a música, o menu do episódio), e chama a função do jogo que começa a fase (`Frontend:StartLevel`). Em duas telas ele aperta o botão de verdade, assim que a tela aparece: START no título (o controle que aperta START vira o jogador; sem isso o LOAD GAME diz que não há perfil) e Y no menu do episódio. É que o jogo liga o jogador 1 ao controle depois de um tempinho nos menus: saindo deles na hora, a fase começa sem jogador 1, o primeiro START na fase entra como **jogador 2** e a pausa nunca abre. Por isso o `level:` também passa pelo título e fica 2 s no menu principal. Se algo der errado (save vazio, fase desconhecida) o log ganha `[test] event boot_failed` e o teste falha.

**Fases** (o runtime loga a lista: `[test] levels (21): ...`; `python tools\frontend_tree.py` mostra a árvore do frontend inteira com os números):

| Episódio | Fase | | Episódio | Fase |
|---|---|---|---|---|
| 1 | `L1_E1` (A New Hop: cena de abertura e vila) | | 11 | `L3_E2` |
| 2 | `L1_E2` (A Succumbing to Age Story; o slot 1 de `tests\userdata_base`) | | 12 | `L3_E3` |
| 3 | `L1_E3` (tem cena de abertura) | | 13 | `L3_E4` |
| 4 | `L1_E4` | | 14 | `L4_E1` |
| 5 | `L2_E1` | | 15 | `L4_E2` |
| 6 | `L2_E2` | | 16 | `L4_E3` |
| 7 | `L2_E4` | | 17 | `L4_E4` |
| 8 | `L3_E1` | | 18 | `L5_E1` |
| 9 | `L3_E1A` | | 19 | `L5_E2` |
| 10 | `L3_E1B` | | 20 | `L5_E3` |

(Há ainda `SoundTest`, índice 20, uma fase de teste de som que nenhum menu alcança; ver "Recursos DEV".)

**Eventos**: o runtime escreve `[test] event <nome> (frame F, input clock S s, tick T)` no `run.log`, e o resumo do `run_game.py` mostra quando cada um aconteceu (`eventos: main_menu 6.0 s | hub 14.4 s | ...`, também em `result.json`):

| Evento | Quando |
|---|---|
| `title` | Tela de título |
| `main_menu` | Menu principal (`main_menu#2`: a segunda vez) |
| `save_slots` | Tela de saves (do menu ou da pausa) |
| `load_complete` | "Load successful" depois do LOAD GAME |
| `hub` | Menu do episódio |
| `level_start` | Uma fase vai começar, com o nome (`level_start ...: L1_E2 (index 1)`), de qualquer jeito (boot, CONTINUE STORY, NEW GAME) |
| `level_ready` | A fase terminou de carregar |
| `cinematic` | Começou uma cena (a de abertura da fase ou uma no meio dela) |
| `skip_prompt` | Apareceu o aviso de pular de uma cena ou de um vídeo de abertura (detalhe `cutscene` / `video`) |
| `cinematic_skipped` | Uma cena foi pulada: pelo `skip_cinematics` (com o quadro em que ela estava) ou segurando o botão (`held, cutscene frame N`) |
| `video_skipped` | Um vídeo de abertura foi pulado segurando o botão |
| `cinematic_done` | Uma cena terminou (sozinha ou pulada): o jogo marca a cena como concluída |
| `gameplay_ready` | O jogador tem o controle (1,5 s de jogo seguido, depois da cena de abertura se tiver) |
| `pause`, `game_over` | Menu de pausa, tela de game over |
| `boot_done`, `boot_failed` | O boot terminou onde devia (`load:N:hub`) / deu errado |

**Âncoras no `input`**: depois de `["@evento"]`, os tempos contam a partir daquele evento, e cada entrada espera o evento acontecer. `["@evento#2"]` é a segunda vez; `["@start"]` volta ao relógio do input. `[t, "SHOT"]` tira uma captura. Exemplo (o `load_pelos_menus`, LOAD GAME só com botões):

```toml
skip_intros = true
input = [
    ["@main_menu"], [2, "DOWN"], [3, "A"],        # 2 s e 3 s depois de o menu aparecer
    ["@save_slots"], [1, "SHOT"], [1.5, "A"],     # na tela de saves, por mais que ela demore a vir
    ["@load_complete"], [1, "A"],
    ["@hub"], [2, "Y"],
    ["@gameplay_ready"], [1, "SHOT"],
]
expect = { exit = ["timeout"], events = ["hub", "gameplay_ready"] }
```

`expect.events` lista eventos que precisam acontecer (`"main_menu#2"` = pelo menos duas vezes) e `expect.events_absent` os que não podem; com `boot`, `boot_failed` sempre falha o teste. As mesmas âncoras valem em `CRASH_SCREENSHOT_TIMES` (`gameplay_ready+2.5`) e no `CRASH_INPUT_SCRIPT` (`gameplay_ready+2500:A`).

**No `replay`** (relógio do jogo, perfis gravados): `[t, "EVENT", "gameplay_ready"]` segura o replay até o evento e realinha a linha do tempo nele (mais cedo ou mais tarde, como o `SYNC` dos carregamentos), então o que vem depois mantém a distância até aquele momento do jogo. A gravação com F5 escreve sozinha os `EVENT` de `main_menu`, `hub`, `level_ready` e `gameplay_ready`, e o `boot`/`skip_intros` da execução gravada: grave com `python tools\run_game.py gravar --set 'boot="load:1"'` e o perfil gravado já começa na fase.

**Pular as cenas** (`skip_cinematics = true` no perfil, `CRASH_SKIP_CINEMATICS=1` ou `--skip-cinematics`): toda cena dentro da fase (NIS) termina assim que começa a tocar, pelo mesmo caminho do fim natural dela (o som para, fade, o jogador volta ao lugar, a cena fica marcada como concluída e o frontend segue sozinho). Junto com `boot = "level:L1_E1"` (o perfil `vila_sem_cena`), a vila do L1_E1 fica jogável em **~14 s** em vez de ~115 s, com a pausa funcionando. O jogador pula segurando um botão (veja "Pular cenas e vídeos"); no jogo original nenhum botão pula, e forçar a saída da cena na árvore do frontend deixa o jogo em LOADING para sempre. Detalhes em `runtime/NOTES.md`, "Pular as cenas".

**Gravações antigas**: `python tools\convert_recording.py gravado_20261020_221744 --boot load:1` escreve `config\recorded\gravado_20261020_221744_boot.toml` (a gravação original não é mexida): o mesmo jogo gravado, mas começando pelo boot direto e realinhando em `gameplay_ready` em vez de repetir o caminho pelos menus. Como as gravações antigas não têm eventos, o conversor roda a original uma vez para ver em que momento dela o jogador ganhou o controle (`--no-calibrate` usa o último carregamento antes do F5, menos preciso).

Continuam passando pela interface de verdade, como testes de ponta a ponta: `load_pelos_menus` (LOAD GAME só com botões, cada um ancorado na tela em que cai), `primeira_fase` (NEW GAME pelo piloto automático de menus, que aperta o botão de cada tela assim que ela aparece), e os testes da tela SETTINGS, do teclado, do mouse e dos ícones (botões ancorados em `@main_menu`, `@save_slots`, `@hub`, `@gameplay_ready`). O `boot` testa a abertura completa (logos e vídeos) e os benchmarks `desempenho*` continuam como estavam (o tempo deles é comparado entre execuções).

### Recursos DEV (Lua, invencibilidade, teleporte...)

O jogo de varejo ainda carrega bastante coisa dos desenvolvedores: o compilador Lua inteiro, a API que o C++ expõe aos scripts (gerentes, atores, estatísticas, objetivos), o banco de "tweaks" `SettingsDatabase`, os cheats originais e o GameLink (depurador remoto da Radical). O catálogo completo, com endereços, o que foi provado e os follow-ups, está em [docs/dev_features.md](docs/dev_features.md). O runtime expõe o mais útil para testes:

| Perfil (`run.toml`) | Ambiente | O que faz |
|---|---|---|
| `dev_lua = "<lua>"` | `CRASH_DEV_LUA` | Roda Lua dentro do jogo, no estado de scripts dele |
| `dev_lua_file = "arquivo.lua"` | `CRASH_DEV_LUA_FILE` | Um arquivo de blocos; uma linha `@<quando>` começa um bloco novo |
| `dev_lua_at = "gameplay_ready+2"` | `CRASH_DEV_LUA_AT` | Quando rodar: `<evento>[#n][+segundos]` ou segundos no relógio do input (padrão `gameplay_ready`) |
| `dev_invincible = true` | `CRASH_DEV_INVINCIBLE=1` | Os jogadores não perdem vida (todo dano passa pela mesma função do jogo) |
| `dev_teleport = [x, y, z]` | `CRASH_DEV_TELEPORT=x,y,z` | Leva o jogador 1 até a posição (`dev.pos()` loga a posição atual) |
| `dev_mojo = 500` | `CRASH_DEV_MOJO=500` | +500 de mojo (o HUD acompanha) |
| `dev_unlock_all = true` | `CRASH_DEV_UNLOCK_ALL=1` | As 20 skins do retail, as fases e os golpes/upgrades, pelos objetivos do próprio jogo (só em saves de teste) |
| `dev_shadow = true` | `CRASH_DEV_SHADOW=1` | O cheat Shadow Crash do jogo |
| `dev_gamelink = true` | `CRASH_DEV_GAMELINK=1` | Carrega o `GameLinkUtil` do jogo (`GetObjectChildren`, `DumpValues`, ...) |

Cada bloco loga `[dev] lua block N at <quando>: ok` (ou o erro) e o evento `dev_lua_<N>`, que serve de âncora no `input`. A saída vai para o log como `[lua] ...`: use `devprint(...)` (o jogo troca o `print` por uma função vazia). Erros de Lua aparecem como `[lua] erro: ...` e não derrubam o jogo; **chamar um método nativo com argumentos errados derruba** (os métodos não conferem nada), então confira a assinatura em `docs/dev_features.md`.

Helpers prontos (`dev.*`, definidos pelo runtime antes do primeiro bloco): `dev.player(i)`, `dev.controller(i)`, `dev.pos(i)`, `dev.teleport(x, y, z, i)`, `dev.damage(n, i)`, `dev.set_hitpoints(v, i)`, `dev.is_dead(i)`, `dev.mojo()`, `dev.give_mojo(n)`, `dev.setting("m_CheatShadowCrash", true)`, `dev.level()`, `dev.objective(nome, function(o) o:DO_... end)`, `dev.unlock_skin(ESkin_Crash_Hulk)`, `dev.unlock_all()` / `dev.unlock_all({ESkin_Crash_Hulk})` (as skins extras entram primeiro; a lista do jogo guarda 20), `dev.gamelink()`, `dev.show(rotulo, valor)`. O Lua do jogo é "5.1" com cara de 5.0: sem `#` (use `table.getn`) e sem `...` em expressões.

```toml
[profiles.meu_teste]
boot = "level:L1_E3"
skip_cinematics = true
dev_invincible = true
dev_lua_file = "tools/dev_lua/meu_teste.lua"
input = [["@dev_lua_1"], [1, "SHOT"]]
```

```lua
-- tools/dev_lua/meu_teste.lua (roda em gameplay_ready)
dev.give_mojo(1000)
dev.pos()
@gameplay_ready+3
dev.teleport(-43, 1.5, 55)
devprint("fase " .. dev.level() .. ", mojo " .. dev.mojo())
```

Provas (um perfil por recurso, capturas em `build\dev\`): `dev_api` (lista a API do jogo no log), `dev_invencivel`, `dev_mojo`, `dev_teleporte`, `dev_sombra`, `dev_cheat_cabecao` (o código original do X360, sem nada do runtime: pausa, segura RT, X X Y A = Big Head; Y X Y A = Shadow), `dev_desbloquear` e `dev_soundtest` (a fase escondida `SoundTest`, que precisa do mod de teste `private\assets\test_mods\dev`; não é `test`).

#### Console de desenvolvedor (comandos com o jogo aberto)

Desligado por padrão. Liga em **SETTINGS > DEFINITIVE EDITION > CONSOLE DE DESENVOLVEDOR** (ou no launcher; `[dev] console = true` no `settings.toml`, vale na hora), com `--dev-console` ou `CRASH_DEV_CONSOLE=1`. Aí a **tecla antes do 1** (`'` no ABNT2, `` ` `` no teclado americano; vale a posição, qualquer layout) ou **F10** abre o console por cima do jogo; Esc, F10 ou a mesma tecla com a linha vazia fecham. Aberto, ele fica com o teclado e o mouse e o jogo vê um controle parado (o jogo continua rodando).

Enter roda a linha; setas para cima/baixo andam no histórico (guardado em `userdata\console_history.txt`); Tab completa o comando, a skin, a fase ou on/off (Tab de novo passa para a próxima opção); PageUp/PageDown e a roda rolam a saída; Ctrl+V cola, Ctrl+L limpa a saída, Ctrl+C/Ctrl+U limpa a linha. O `devprint` do Lua e os erros de Lua aparecem no console (em vermelho) e no log (`[console] ...`).

| Comando | O que faz |
|---|---|
| `help [comando]` | lista os comandos ou explica um |
| `unlock all` | as 20 skins do jogo, fases e golpes (os objetivos do próprio jogo; o jogo grava no save quando salvar) + a Crash_Hulk no menu de skins |
| `unlock skin <nome>` | uma skin (`hulk` não ocupa vaga da lista de 20 do jogo: fica em `[dev] hulk_skin`) |
| `mojo <n>` | +n de mojo (o HUD acompanha) |
| `god [on\|off]` | invencibilidade (o HP não desce; poços ainda matam) |
| `tp <x> <y> <z>` / `pos` | teleporta o jogador 1 / mostra a posição (y = altura) |
| `shadow [on\|off]` / `bighead [on\|off]` | os cheats Shadow Crash e Big Head do jogo |
| `skin` / `skin <nome> [now]` | lista as skins e a atual / veste uma: no menu, na hora (o "vestir" da tela de skins); numa fase, na próxima carga, e `now` reinicia a fase com ela |
| `level` / `level <nome>` | lista as fases / vai para uma: numa fase, reinicia já na nova (`UberManager:SetLevelIndex` + `ForceReload`); no HUB ou no menu principal, pelo `Frontend:StartLevel` + a saída SkipToLevel da árvore (como o boot direto) |
| `reload` | reinicia a fase (`UberManager:ForceReload`) |
| `lua <código>` / `lua =<expressão>` | Lua no estado do jogo / mostra o valor |
| `clear` | limpa a saída |

Skins: `crash`, `hulk` (a Crash_Hulk verde, que o jogo nunca usou), `chiratta`, `koala`, `monkey`, `rabbit`, `bratgirl`, `spike`, `shurtle`, `sludge`, `ratcicle`, `battler`, `grizzly`, `roller`, `parafox`, `stinky`, `eelectric`, `shellephant`, `scorporilla`, `valentine`, `halloween`; também o nome do enum (`ESkin_Koala`) ou o número. `cortex` (sem modelo no disco), `yuktopus`, `ukauka` e `nina` (sem nome na tabela do jogo: derrubavam o jogo) são recusadas.

Nos perfis, `dev_console = [["@gameplay_ready+1", "!toggle"], ["@gameplay_ready+1.5", "god on"]]` digita cada entrada na janela (pelas mesmas mensagens de teclado de uma tecla de verdade; `!toggle`, `!esc`, `!tab`, `!up`, `!down`, `!enter`, `!type:<texto>` sem Enter) e liga o console; eventos `console_<n>`, `console_open`, `console_close` e `console_cmd`. Perfis (`test = true`): `console_dev` (abre, help, unlock all, mojo 500, god on, erro de Lua, Tab, `skin hulk now`, bighead, pos, `skin ratcicle now`, `level L1_E2`), `console_hulk` (a Crash_Hulk no chão e andando), `console_skin_menu` (a Crash_Hulk no menu de skins do jogo: HUB → RATNICIAN SKIN → direita → CRASH HULK → VESTIR → CONTINUE STORY) e `console_level_hub` (`level` a partir do HUB). Capturas em `build\console\`. Como a troca de skin e a Crash_Hulk funcionam por dentro: [docs/dev_features.md](docs/dev_features.md), "Skins".

**Ler os scripts do jogo** (`tools/luadec.py`): os `.lua`/`.blua` de `default.rcf` (objetivos, IA, HUD, árvores de luta, fases) são bytecode do "Lua 5.1 (work)" da Radical, que na prática é o formato do Lua 5.0 (números `float`, line info em u16) com o `FORPREP` e os laços `for` do 5.1. `python tools\luadec.py dec private\assets\raw\default\script\util.lua` mostra o script reconstruído em pseudo-Lua (`if/elseif`, `while`, `for`, `and/or`, tabelas, métodos, closures, `do ... end`), `dis` mostra funções, constantes, locais e instruções, e `python tools\luadec.py all` faz os 723 scripts compilados de uma vez em `private\scripts_dec\` (`*.dec.lua` e `*.dis.txt`; os 42 que já são texto ficam de fora). A saída usa os nomes de locais e parâmetros que vêm no bytecode e serve para ler e como base de mods (copie o trecho para um `dev_lua_file`); não é idêntica ao fonte original. Para conferir: `python tools\luadec.py check`, depois `python tools\run_game.py dev_luadec` e `python tools\luadec.py check --log build\runs\<pasta>\run.log`, que compila cada script reconstruído com o compilador do próprio jogo (`loadstring`, sem rodar) e lista os que não compilam (hoje: nenhum). Comparar o bytecode dentro do jogo não dá: o `string.dump` do retail derruba o jogo.

### Overlay de desempenho, marcas e gravação de perfis

O overlay tem quatro modos: **desligado**, **só FPS**, **FPS + frame time** (com o gráfico) e **completo**. O modo fica em `userdata\settings.toml` (`overlay = "off" | "fps" | "frametime" | "full"`) e muda pela tela SETTINGS ou pelo **F1**, que passa para o próximo modo e grava a escolha. Nos perfis, `overlay` em `run.toml` decide o modo daquela execução: `true` = completo (o padrão), `false` = o do `settings.toml`, ou o nome do modo (`"fps"`, `"frametime"`, `"full"`, `"off"`; também `"completo"` e `"desligado"`). Capturas de cada modo: `docs\capturas\antigas\overlay_modos\` (`off.jpg`, `fps.jpg`, `frametime.jpg`, `full.jpg`, `full_1080p.jpg` e `settings_tela.jpg`).

O modo completo, no canto superior esquerdo da janela:

```
QUADRO 2400    INPUT 81.3 S          <- número do quadro (o mesmo de screenshots = [...] e de screenshot_NNNNN.png)
FPS  30.0       FRAME  33.3 MS        <- quadros por segundo (último 1 s) e tempo do último quadro
MED  33.4 MAX  41.2 1%LOW  38.0       <- frame time médio, máximo e 1% low (ms) nos últimos 240 quadros
RENDER 2560X1440 (2X) -> 1920X1080    <- resolução interna (settings) e tamanho da janela
PERFIL MENU_LOADED                    <- só quando o jogo foi aberto pelo run_game.py com um perfil
BOTAO A @ 27.00 S Q810 SCRIPT         <- último botão: relógio do input, quadro, SCRIPT (input do perfil) ou JOGADOR
REPLAY 52.3 S  SYNC +1.1 S            <- só num perfil gravado: tempo do jogo e quanto os SYNC atrasaram
● REC 00:12  F5 PARA                  <- gravando (F5); aparece em qualquer modo, até com o overlay desligado
F1 MODO F2 MARCA F5 GRAVA F12 PRINT
[gráfico do frame time: verde até 34 ms, amarelo até 50 ms, vermelho acima; linhas em 16,7 e 33,3 ms]
```

As linhas `PERFIL` e `BOTAO` aparecem só nas execuções do `run_game.py`, que passa o nome do perfil em `CRASH_PROFILE`; abrindo o `CrashRecomp.exe` direto elas não aparecem. O tempo e o quadro do `BOTAO` são os mesmos de `input = [...]` e `screenshots = [...]`, então servem para montar perfis.

`INPUT` é o relógio dos botões do perfil: segundos desde a primeira leitura do controle (`--` antes disso). É o número que vai em `input = [[segundos, "BOTÃO"]]`.

| Tecla | O que faz |
|---|---|
| **F1** | Próximo modo do overlay (desligado → só FPS → FPS + frame time → completo → desligado), gravado no `settings.toml` |
| **F2** | Marca o momento: `[mark] frame N, input t=XX.X s` no `run.log` e uma linha em `marks.txt` na pasta da execução. Durante uma gravação (F5), também vira uma captura (`SHOT`) no perfil gravado. O resumo do `run_game.py` lista as marcas. |
| **F5** | Começa a gravar o controle / para e salva o perfil em `config/recorded` (veja "Gravando um perfil jogando") |
| **F8** | Liga / desliga a iluminação remaster inteira (SSAO, luz indireta, sombras de contato e suaves) na hora, sem mudar o `settings.toml`: para comparar antes e depois. Aparece um aviso no canto por 2,5 s, mesmo com o overlay desligado (veja "Iluminação (remaster)") |
| **F12** | Salva `screenshot_NNNNN.png` (número do quadro) na pasta da execução |

As capturas (F12 e `screenshots = [...]`) saem **sem** o overlay. Com `screenshot_overlay = true` ele aparece nelas também. A janela precisa estar em foco para as teclas (clique nela). F1, F2, F5, F8 e F12 não conflitam com as teclas de jogo (veja "Controles de PC").

### Gravando um perfil jogando (F5)

Em qualquer execução (`jogar`, `gravar`, um teste ou o `CrashRecomp.exe` direto), **F5** começa a gravar o controle 0 e **F5** de novo para. No canto aparece `● REC 00:12` (em qualquer modo do overlay, até desligado) e o log ganha `[input] recording started` / `stopped`. Ao parar, o runtime escreve um perfil pronto em `config/recorded/gravado_<data>_<hora>.toml`, e ele já roda:

```bat
python tools\run_game.py gravado_20261020_220035
```

- **O que entra**: tudo o que o jogo leu do controle (o físico e o teclado): botões com o tempo apertado, **os dois analógicos** com o valor real (centésimos, de -1 a 1) e **os dois gatilhos** (0 a 1). Um analógico só é escrito de novo quando anda 0,02, volta ao centro ou chega ao fim (abaixo de 0,05 conta como centro), então o arquivo fica legível. F2 durante a gravação vira uma captura.
- **De onde começa**: o perfil sempre roda **do boot**. O runtime guarda tudo desde a primeira leitura do controle, então o caminho até o ponto do F5 (menus, LOAD, a fase) entra no perfil antes da marca `# --- F5: inicio da gravacao ---`. Os saves e o `settings.toml` **como estavam no boot** vão para `config/recorded/<nome>/userdata`, que o perfil copia (`userdata_from`, com `userdata_mode = "copy"`): o replay não depende dos seus saves de agora e não mexe neles. Se a gravação começar muito depois do boot, o replay demora o mesmo tanto; para testes curtos, abra o jogo, vá direto ao ponto e aperte F5.
- **Capturas**: uma no F5, uma em cada F2 e uma no fim; o perfil fecha o jogo 1 s depois (`END`) e confere que o replay terminou (`expect`). A execução da gravação também salva capturas nesses mesmos momentos, para comparar.
- **Fechar a janela** durante a gravação também salva. O arquivo é reescrito a cada 2 s, então mesmo um processo encerrado à força deixa um perfil (marcado "gravação interrompida").
- `python tools\run_game.py gravar`: o mesmo, numa cópia dos saves e já gravando desde o boot (`record_input = true`).

Um perfil gravado (trecho):

```toml
[profiles.gravado_20261020_220035]
description = "Gravado com F5 em 2026-10-20 22:00:35 no perfil graficos: 16.07 s a partir de 48.30 s do jogo"
timeout = 184
userdata_mode = "copy"
userdata_from = "gravado_20261020_220035/userdata"   # relativo a este arquivo
replay = [
    [9.93, "SYNC", "0x4C800#1"],   # fim de carregamento
    [14.30, "START", 0.40], [16.00, "START"], [27.03, "DOWN"], [28.03, "A"], [40.53, "Y"],
    [41.27, "SYNC", "0x12530800#1"],   # fim de carregamento
    # --- F5: inicio da gravacao (48.30 s) ---
    [48.30, "SHOT"],   # F5: inicio da gravacao
    [50.17, "LS", 0.45, 0.90], [52.67, "LS", 0.00, 0.00], [52.97, "RS", 0.70, 0.00],
    [57.17, "X"], [58.17, "RT", 1.00], [58.77, "RT", 0.00], [59.67, "A"],
    [64.37, "SHOT"],   # fim da gravacao
    [65.37, "END"],
]
```

| Entrada de `replay` | O que faz |
|---|---|
| `[t, "A"]`, `[t, "A", 0.4]` | Aperta o botão por 0,2 s (ou pelo tempo dado). Botões: `A B X Y START BACK UP DOWN LEFT RIGHT LB RB L3 R3` |
| `[t, "LS", x, y]`, `[t, "RS", x, y]` | Põe o analógico em (x, y), de -1 a 1 (y = 1 é para cima); fica assim até a próxima entrada dele |
| `[t, "LT", v]`, `[t, "RT", v]` | Gatilho de 0 a 1, até a próxima entrada |
| `[t, "SYNC", "0xOFFSET#n"]` | Fim de um carregamento (gravado sozinho): o replay espera o jogo terminar de carregar e realinha o tempo |
| `[t, "WAIT", "texto"]` | Espera aparecer no log uma linha com o texto (ex.: `"nis01 starts"`) antes de seguir; o resto atrasa o tanto que esperou. Para pôr à mão |
| `[t, "SHOT"]` | Captura de tela |
| `[t, "END"]` | Fecha o jogo (saída `ok`) |

**O tempo `t` é o relógio do jogo**, não o de parede: o número de vezes que o jogo leu o controle, dividido por 30. O jogo lê o controle uma vez por passo de simulação (30 por segundo na fase; nos vídeos da abertura um pouco mais rápido, por isso `t` passa um pouco à frente do relógio `INPUT` no começo) e recupera os passos quando um quadro demora, então um analógico entregue na leitura N cai no mesmo passo de simulação em toda execução, com qualquer FPS. O que esse relógio não absorve são os carregamentos: a tela de carregamento continua contando. Medido com o `primeira_fase_load`: com 3 jogos abertos ao mesmo tempo, o carregamento da fase termina 40 a 80 leituras (1,5 a 2,7 s) mais tarde. Por isso a gravação marca o fim de cada carregamento com um `SYNC`: o jogo lê o `default.rcf` (fases e menus) em rajadas de megabytes quando carrega; uma rajada de 1 MB ou mais seguida de 1 s sem leituras é um carregamento, e o `SYNC` guarda o pedaço do arquivo lido por último. No replay, ao chegar num `SYNC`, o tempo para até o jogo ter lido aquele mesmo pedaço e ficar 5 leituras sem ler, e então o resto do perfil é deslocado para começar ali (mais tarde se o carregamento demorou mais, mais cedo se foi mais rápido). Nenhum `SYNC` ou `WAIT` segura mais de 60 s (aí o log diz `timed out` e o teste falha). O resumo do `run_game.py` mostra cada `SYNC` e o overlay, a linha `REPLAY ... SYNC +1.1 S`.

**Conferido** (capturas em `docs\capturas\gravacao\`): uma gravação com F5 a partir do LOAD GAME, andando e girando a câmera com valores parciais (LS 0,45/0,9, RS 0,7, RT), repetida sozinha duas vezes e com 3 replays ao mesmo tempo. Sozinho, as capturas do replay batem com as da gravação (`sozinho_gravacao_replay1_replay2.jpg`, diferença média de 0 a 1,8 em 255). Com 3 instâncias, os carregamentos terminaram 1,1 a 2,6 s mais tarde, os `SYNC` realinharam e o Crash termina no mesmo lugar, com diferenças pequenas de pose e câmera (`carga_3_instancias_replay.jpg`, até 7,7 em 255): o jogo calcula algumas coisas pelo relógio real, então o replay não é idêntico pixel a pixel, mas não dessincroniza.

Os perfis antigos (`input` em segundos de parede) continuam iguais. Para escrever um teste à mão, `input` serve; para algo que precisa passar por menus e carregamentos com o jogo lento, grave com F5 (ou copie as linhas de um `replay` gravado e edite: os tempos são legíveis e dá para apagar, mudar ou acrescentar entradas).

Também dá para usar sem o `run_game.py`, com variáveis de ambiente: `CRASH_OVERLAY=1` (completo) ou `CRASH_OVERLAY=fps|frametime|full|off`, `CRASH_PROFILE=<nome>`, `CRASH_SCREENSHOT_OVERLAY=1`, `CRASH_OVERLAY_CORNER=tl|tr|bl|br`, `CRASH_OVERLAY_SCALE=N`, `CRASH_RECORD_DIR=<pasta>` (onde o F5 grava; padrão `config\recorded` com o exe do build, `recorded\` ao lado do exe no `dist`), `CRASH_RECORD_INPUT=1` (grava desde o boot), `CRASH_REPLAY=<arquivo>` (o `replay.txt` que o `run_game.py` escreve), `CRASH_INPUT_TRACE=<arquivo.csv>` (cada leitura do controle com os bytes lidos do `default.rcf`) e `CRASH_FRAME_STATS=<arquivo.csv>` (veja `runtime/NOTES.md`, passos 18 e 21).

## Configurações

O item **CALIBRATION** do menu principal (NEW GAME / LOAD GAME / CREDITS / ...) virou **SETTINGS** (PARAMETRES, OPTIONEN, AJUSTES, IMPOSTAZIONI, OPTIES nos outros idiomas) e abre a tela de configurações do runtime. Ela é uma tela do próprio jogo: o SETTINGS leva, com a transição do jogo, à tela de opções da versão de PS2 (que ainda existe nos dados do Xbox, sem uso), com o fundo de tecido do frontend e as dicas **(B) VOLTAR** e **SELECIONAR (A)** desenhadas pelo jogo, como no LOAD GAME; as configurações são desenhadas por cima, com as fontes, os ícones e as molduras do jogo (veja "Visual" abaixo). Ao fechar, o jogo volta ao menu principal pela transição dele (com o cursor em NEW GAME). A tela de calibração de TV que o item abria não faz sentido no PC. A dica "Y STORAGE DEVICE" do menu foi escondida e o Y não faz mais nada (veja "Menu de PC" abaixo). NEW GAME, LOAD GAME e os saves não mudaram. O menu ganhou um quinto item, **EXIT GAME / SAIR DO JOGO**, que fecha o jogo.

Capturas da organização atual das abas em `docs\capturas\settings_abas\`: cada aba em inglês (`en_1_jogo.jpg` ... `en_6_definitive_edition.jpg`) e em PT-BR (`ptbr_*`), no jogo (perfil `configuracoes`, janela 1280x720), e o `--settings-preview` em `preview\` (1280x720, 1920x1080, 3840x2160 e 2560x1080, mais `*_pausa`, `*_reiniciar`, `*_legendas` e `*_camera`). Capturas mais antigas em `docs\capturas\settings_nativo\` (de quando as abas eram ÁUDIO, VÍDEO, GRÁFICOS, JOGO e LEGENDAS): cada aba em inglês (`en_1_audio.jpg` ... `en_5_legendas.jpg`, janela 1280x720) e em PT-BR (`ptbr_*_1080p_interna_4k_dlss.jpg`: janela 1920x1080 com resolução interna 3840x2160 e DLSS Qualidade), o menu depois de fechar, o LOAD GAME depois (`en_8_load_game.jpg`, `menu_loaded_load_game.jpg`) e a sequência de abrir e fechar (`transicao_abrir_e_fechar.jpg`). A versão anterior (lista única com a fonte do overlay): `docs\capturas\antigas\configuracoes_aberta.jpg`.

**Controles**: **LB / RB** (Q / E no teclado) trocam de aba (**JOGO, ÁUDIO, VÍDEO, GRÁFICOS, CONTROLES, DEFINITIVE EDITION**; em inglês GAME, AUDIO, VIDEO, GRAPHICS, CONTROLS, DEFINITIVE EDITION; a tela abre em JOGO); setas / D-pad / analógico esquerdo escolhem e mudam (segurar repete), A ou Enter muda o item selecionado (os que não são barras), B, Esc ou o botão direito do mouse salva e volta. O mouse funciona como num jogo de PC (passar seleciona a linha, clique nas abas, nas setas e nas estrelas, roda muda de linha; veja "Mouse nos menus"). Enquanto a tela está aberta o jogo não recebe os botões; ao fechar, o runtime aperta B para o jogo por 0,2 s (a saída "voltar" da tela de opções) e depois só devolve o controle quando tudo for solto. Alt+Enter alterna entre janela e sem bordas a qualquer momento.

| Opção | Valores | Quando vale |
|---|---|---|
| **Jogo** | | |
| Idioma `*` | English, Français, Deutsch, Español, Italiano, Nederlands (textos e vozes) e Português (BR) (textos; vozes em inglês, veja "Tradução PT-BR") | reinicia o jogo (`docs\capturas\settings_aplicar\reinicio_*.jpg`) |
| Cooperativo | pula-sela (padrão do jogo: os dois se revezam pulando pelos desafios) / cavalinho (um jogador fica na mochila do outro). A opção do menu de opções original | na hora |
| Inverter eixo | desligado (padrão) / ligado: inverte o eixo da câmera, como a opção original (a câmera lê a opção a cada quadro) | na hora |
| Salvamento automático | ligado (padrão) / desligado: o jogo não salva sozinho (o menu de pausa passa a mostrar SAIR DA FASE, como fazia a opção original). Ligado, só salva quando há um slot de save escolhido (NEW GAME ou LOAD GAME); continuar sem salvar continua sem salvar | na hora |
| Armazenamento | Hard Drive (informativo) | — |
| **Áudio** | | |
| Volume geral | 0–100% | na hora (mixagem final do runtime) |
| Música, efeitos, vozes | 0–100%. Substituem as estrelas DIÁLOGOS / MÚSICA / EFEITOS do menu de opções original do jogo (que fica escondido) | na hora (`docs\capturas\settings_aplicar\volume_musica_ao_vivo_*.wav`) |
| *Legendas* (sub-grupo da aba ÁUDIO, abaixo dos volumes) | | |
| Legendas | desligado (padrão) / ligado: legendas nas cenas e nas falas do Aku Aku, no idioma dos textos (veja "Legendas") | na hora |
| Tamanho da legenda | 50–200% (padrão 100%) | na hora |
| Fundo da legenda | 0–100%: opacidade da caixa atrás do texto (padrão 50%; 0% = só o contorno) | na hora |
| **Vídeo** | | |
| Modo de tela | janela, sem bordas (janela do tamanho do monitor), tela cheia (exclusiva; com Alt+Tab volta a sem bordas e retoma quando a janela volta ao foco) | na hora |
| Resolução | 1280x720 (original), 1600x900, 1920x1080, 2560x1440, 3840x2160: o tamanho em que o jogo é desenhado por dentro, depois ajustado à janela (`docs\capturas\antigas\resolucao_720_vs_1440.jpg`) | ao sair da tela, sem reiniciar (`docs\capturas\settings_aplicar\ao_vivo_*.jpg`) |
| Limite de FPS | 30 (original), 60, 120, sem limite | na hora |
| V-Sync | ligado / desligado (padrão: desligado); com ele ligado o FPS também fica limitado à taxa do monitor | na hora |
| Anti-aliasing | desligado / FXAA / SMAA: pós-processamento da imagem final, na resolução interna (o FXAA amolece também o HUD; o SMAA quase não) | na hora (`docs\capturas\antigas\graficos\aa_sem_fxaa_smaa.jpg`) |
| Overlay | desligado, só FPS, FPS + frame time, completo (o mesmo do F1; veja "Overlay de desempenho") | na hora |
| **Gráficos** | | |
| DLSS / FSR | desligado, DLSS (DLSS 4 / DLAA da NVIDIA; só placas RTX), FSR 3.1 (AMD, qualquer GPU): anti-aliasing temporal e upscaling da cena 3D. **Sem frame generation** | ao sair da tela, sem reiniciar (`docs\capturas\antigas\graficos\upscalers_720p.jpg`) |
| Modo do upscaler | nativo (DLAA / FSR Native AA: a cena na resolução escolhida, só o AA temporal), qualidade (cena a 67% por eixo), equilibrado (59%), desempenho (50%), ultra desempenho (33%) | ao sair da tela, sem reiniciar (`docs\capturas\antigas\graficos\dlss_desempenho_1440p.jpg`) |
| Nitidez (FSR) | 0–100%: o RCAS do FSR 3.1 depois do upscaling; o DLSS 4 não tem ajuste de nitidez | na hora |
| Motion blur | 0–100%: 0% desliga o motion blur do jogo, 100% é o original | na hora (`docs\capturas\antigas\graficos\motion_blur_100_vs_0.jpg`) |
| Profundidade de campo | 0–100%: o desfoque de distância das cenas; 0% desliga | na hora (`docs\capturas\antigas\graficos\profundidade_de_campo_100_vs_0.jpg`) |
| Filtro anisotrópico | desligado, 2x, 4x, 8x, 16x (o jogo já pede 16x na maioria das texturas da fase; a opção vale como mínimo) | na hora (`docs\capturas\antigas\graficos\mipmaps_antes_depois.jpg`) |
| Suavidade das sombras | 0–200%: raio do filtro da borda das sombras (0% = borda dura). Não há resolução de sombra para ajustar (veja abaixo) | na hora (`docs\capturas\antigas\graficos\sombra_suavidade_100_vs_0.jpg`) |
| **Controles** | ícones dos botões, câmera e sensibilidade do mouse, andar devagar, mouse nos menus e as teclas de cada ação (veja "Controles de PC") | na hora |
| **Definitive Edition** | os extras deste port | |
| Cores do mojo `*` | PS2 (padrão: mojo grande azul bem claro e mais brilhante, extra grande roxo lilás) / Xbox 360 (todos azuis, como no original) (veja "Cores do mojo") | reinicia o jogo (`docs\capturas\antigas\mojo\`) |
| HD AI Remaster `*` | desligado (padrão) / ligado: texturas e modelos remasterizados por IA do jogo todo (`data\remaster_full`), ou só da fase 1 (`data\remaster_l1`) se o completo não existir (veja "HD AI Remaster"). Sem esses arquivos no PC aparece **INDISPONÍVEL** e não muda | reinicia o jogo (`docs\capturas\hd_remaster\`) |
| *Iluminação (remaster)* (sub-grupo; tudo desligado por padrão; **F8** liga e desliga tudo na hora para comparar) | | |
| Oclusão de ambiente (SSAO) | desligado (padrão) / baixa (meia resolução) / alta (resolução cheia até 1440 linhas): escurece cantos, contatos e dobras da cena (veja "Iluminação (remaster)") | na hora (`docs\capturas\iluminacao\`) |
| Luz indireta (SSGI) | desligado (padrão) / ligado: a luz rebatida nas superfícies próximas tinge a cena com a cor delas (um rebote, espaço de tela) | na hora |
| Intensidade da luz indireta | 0–200% (padrão 100%) | na hora |
| Sombras de contato | desligado (padrão) / ligado: sombras curtas sob personagens, objetos e beiradas, onde encostam no chão | na hora |
| Sombras suaves | desligado (padrão) / ligado: as sombras do próprio jogo com um filtro mais largo e suave (a "Suavidade das sombras" da aba GRÁFICOS continua valendo como raio) | na hora |
| *Câmera em terceira pessoa* (sub-grupo) | | |
| Câmera livre (experimental) | desligado (padrão) / ligado: câmera de terceira pessoa que orbita o Crash (veja "Câmera livre") | na hora |
| Sensibilidade da câmera | 25–300% (padrão 100%): velocidade do analógico direito e do mouse | na hora |
| Inverter eixo Y | desligado (padrão) / ligado | na hora |
| Distância da câmera | 50–200% (padrão 100%) | na hora |

As opções marcadas com `*` (na tela, com o ícone de setas em círculo do jogo antes do nome) são lidas pelo jogo no boot e valem depois de um reinício, que o próprio jogo faz (veja "Aplicar sem fechar o jogo"). O rodapé explica o ícone: "REINICIA O JOGO"; quando uma delas muda, vira "O JOGO VAI REINICIAR". Embaixo, numa caixa de mensagem do jogo, duas linhas explicam o item selecionado.

### Durante o jogo e as opções do próprio jogo

A tela também abre **durante o jogo**: no menu de pausa, **OPÇÕES** (em todas as variantes da pausa: normal, "sem salvar" e de dois jogadores) leva à mesma tela, com as mesmas abas. A tela de opções original do jogo (COOPERATIVO, DIÁLOGOS, MÚSICA, EFEITOS, INVERTER EIXO, SALVAMENTO AUTOMÁTICO) fica escondida atrás dela, para não haver nada duplicado: os três volumes dela viraram os volumes da aba ÁUDIO, e as outras três opções estão na aba JOGO. Essas opções também aparecem quando a tela abre pelo **menu principal**, antes de entrar no jogo. Elas ficam no `settings.toml` e valem para qualquer save: o save guarda as opções dele e, ao ser carregado, o runtime volta a aplicar as da tela. **B** volta ao menu de pausa (o jogo continua pausado); START no menu de pausa volta a jogar. Mudar idioma, cores do mojo ou HD AI Remaster durante o jogo também oferece o reinício, e a caixa avisa que o progresso não salvo será perdido.

Capturas em `docs\capturas\settings_aplicar\`: `pausa_1_opcoes_abre_a_tela.jpg` (a tela aberta pela pausa), `pausa_2_jogo_cooperativo_inverter_salvamento.jpg`, `pausa_4_pausa_sem_salvar_depois_de_fechar.jpg` (com o salvamento automático desligado na tela, o próprio jogo passa à pausa "sem salvar", com SAIR DA FASE), `pausa_5_reaberta_pela_pausa_sem_salvar.jpg`, `pausa_6_fase_1600x900.jpg` (resolução trocada na pausa), `menu_principal_jogo_opcoes_do_jogo.jpg` e `menu_principal_opcoes_aplicadas_fase.jpg`.

### Aplicar sem fechar o jogo

Nenhuma opção pede mais para fechar o jogo à mão (capturas em `docs\capturas\settings_aplicar\`):

- **Na hora**: as opções marcadas "na hora" na tabela, inclusive música, efeitos e vozes: elas são aplicadas pelos próprios volumes de categoria do jogo (os das estrelas do menu original), que ele usa a cada atualização do som, então a mudança é ouvida na hora, no menu e na fase (`volume_musica_ao_vivo_menu.wav`: música a 0% e de volta a 60% com a tela aberta; `volume_musica_ao_vivo_pausa.wav`, na pausa).
- **Ao sair da tela, sem reiniciar**: resolução interna, DLSS / FSR e o modo do upscaler. Ao fechar a tela, o renderer espera a GPU, recria os render targets e resolve targets no tamanho novo, troca o upscaler (o NGX/FSR sobe na hora, se ainda não tinha subido) e, se a variante dos shaders muda (720p sem upscaler usa a original), retraduz os shaders (do cache: 0 compilações nos testes). Leva ~20 ms; o log mostra `[gpu] reconfigured: internal resolution ...`. Testado indo de 720p para 1080p + DLSS Qualidade e de 1440p + DLSS para 720p, com LOAD GAME até a fase depois (`ao_vivo_*.png`, perfil `configuracoes_resolucao`).
- **Reinício automático**: idioma, cores do mojo e HD AI Remaster. O jogo lê esses dados no boot (bíblias de texto e `.rcf` de voz, pacotes do mojo, lista de mods) e não há como recarregá-los com segurança com ele rodando (veja `runtime/NOTES.md`, "Aplicar as opções sem fechar o jogo"). Ao sair da tela com uma delas mudada, aparece numa caixa do jogo: "Algumas opções só valem depois de reiniciar: IDIOMA, ... Reiniciar o jogo agora? (A) REINICIAR (B) DEPOIS". **A** grava o `settings.toml`, mostra "Reiniciando..." e abre o jogo de novo com o mesmo executável, argumentos, pasta e ambiente, na mesma posição de janela; o processo novo espera o antigo terminar e **pula as intros sozinho** (aperta START até o menu principal, ou até você apertar qualquer botão), voltando ao menu principal em ~15 s já com as opções novas (`reinicio_3_menu_ptbr_sem_intros.png`). **B** sai normalmente e as opções ficam para a próxima abertura. Aberta pela pausa, a caixa ainda avisa "O progresso não salvo será perdido".
- `CRASH_RESTART` muda o reinício: `relaunch` (padrão), `exit` (sai com o código 4 para quem abriu o jogo reabrir; o `run_game.py` usa este, veja "Rodando e testando") ou `off` (sem reinício: as opções valem na próxima abertura e a tela volta aos avisos antigos "VALE AO REINICIAR" / "REINICIE O JOGO PARA APLICAR"). No reinício, `CRASH_SETTINGS`, `CRASH_INPUT_SCRIPT`, `CRASH_REPLAY` e `CRASH_RECORD_INPUT` não passam para o processo novo.

### Menu de PC: SAIR DO JOGO, sem Xbox 360 e sem o botão de armazenamento

O frontend se comporta como o de um jogo de PC (capturas em `docs\capturas\menus_pc\`, `en_*` e `ptbr_*`):

- **SAIR DO JOGO** (EXIT GAME, QUITTER JEU, SPIEL VERLASSEN, SALIR DEL JUEGO, ESCI DAL GIOCO, SPEL AFSLUITEN: o texto do próprio jogo, o mesmo do "(B) EXIT GAME" do menu do episódio) é o quinto item do menu principal, abaixo de SETTINGS; os cinco itens sobem um pouco para o menu continuar centralizado. A (ou Enter) toca o som de seleção e fecha o jogo na hora: grava o `settings.toml`, para uma gravação F5 em andamento, espera a GPU, sai da tela cheia exclusiva, libera o DLSS/FSR, para o áudio e termina com o código 0 (no `run_game.py`, saída `ok`). Não há confirmação: no menu principal não há jogo em andamento, então não há progresso a perder.
- **Menu de pausa**: não mudou. Ele não tem submenu de sair; "SALVAR E SAIR" salva e volta ao menu do episódio, de onde (B) SAIR DO JOGO volta ao menu principal. Fechar a janela (X / Alt+F4) continua fechando o jogo a qualquer momento, sem salvar, como antes.
- **Y (armazenamento)**: a dica "Y STORAGE DEVICE" sumiu do menu principal e das telas de escolher o slot (NEW GAME, LOAD GAME e salvar dentro do jogo), e apertar Y nessas telas não faz nada (antes voltava ao menu com o cursor em NEW GAME e abria o seletor de dispositivo). O seletor continua sendo chamado uma vez pelo próprio LOAD GAME / NEW GAME, como no Xbox, e escolhe o HD; os saves continuam em `userdata\`. O Y do menu do episódio (CONTINUAR HISTÓRIA) e o "pronto" da tela do nome não mudaram.
- **Textos sem Xbox 360**: os avisos de salvar/carregar dizem "Do not turn off your PC." / "Não desligue o PC.", as mensagens de dispositivo de armazenamento falam da pasta de jogos salvos, a de perfil de jogador não manda mais apertar o botão Guia Xbox e "reconecte o controle do Xbox 360" virou "reconecte o controle". Em inglês, francês, alemão, espanhol, italiano e holandês isso é um patch embutido das bíblias de texto (os arquivos do jogo não mudam; a lista completa está em `runtime/NOTES.md`, "Frontend de PC"); em PT-BR está no próprio pacote (`languages\pt-br\texts`). Os créditos ("CONSOLE MARKETING COORDINATOR"...) e os textos de PS2/Wii que o Xbox nunca mostra ficaram como estão.

Teste: perfil `sair_do_jogo` (desce até SAIR DO JOGO, aperta Y, que não deve abrir o seletor, e A; confere a saída 0). Em PT-BR: `--set 'env={CRASH_SETTINGS="game.language=pt-br",CRASH_SCREENSHOT_TIMES="16.5,23"}'`.

### Nome e saves no PC: teclado na tela do nome e apagar saves

**Digite seu nome (NEW GAME).** A roda de letras do console deu lugar a um **teclado QWERTY na tela**, desenhado com a fonte, as cores e o destaque dos menus do jogo, no lugar onde ficava a roda (o título, o nome digitado e as dicas do jogo continuam os dele):

```
1 2 3 4 5 6 7 8 9 0
Q W E R T Y U I O P
 A S D F G H J K L
   Z X C V B N M
[ APAGAR ] [ PRONTO ]
```

- **Controle**: D-pad / analógico andam entre as teclas (segurar repete; nas pontas dá a volta), **A** digita a tecla destacada, **X** apaga, **Y** ou **START** é PRONTO, **B** volta ao menu principal: as mesmas dicas que o jogo já mostra na tela (X APAGAR, B VOLTAR, A SELECIONAR, Y PRONTO), com os ícones do aparelho em uso.
- **Teclado físico**: as letras e os números (fileira de cima ou teclado numérico) digitam direto; **Enter** = PRONTO, **Backspace** / **Delete** = apagar, **Esc** = voltar; setas + Espaço usam o teclado da tela. Enquanto a tela do nome está aberta, as letras não são mais atalhos de menu (W/A/S/D, Q/E, P), e as dicas do jogo mostram ESPAÇO para selecionar e ENTER para pronto.
- **Mouse**: passar o mouse destaca a tecla, clique digita; APAGAR e PRONTO também são clicáveis; botão direito volta.
- **O que vale num nome é o que o jogo aceita**: até **5 caracteres**, **A–Z e 0–9** (os 36 da roda). Por isso o teclado não tem espaço, minúsculas, Shift/Caps nem acentos: o jogo só põe essas 36 letras no nome (a fonte até tem acentos, mas o código do nome não). Com o nome cheio as letras ficam apagadas e uma a mais não entra (com o som do jogo); PRONTO sem nome é recusado com o som de "não" do jogo, como na roda.
- O nome chega ao save **exatamente como pela roda**: cada tecla faz o que o A da roda faria (o mesmo som e a mesma função do jogo que acrescenta a letra), apagar e PRONTO também; o save mostra o nome no LOAD GAME (`Name: CRASH` no `.header`).

**Apagar saves (escolha do slot).** Na tela de escolher o slot (LOAD GAME e também NEW GAME), com o cursor num slot que tem save aparece **X APAGAR** entre VOLTAR e SELECIONAR. **X** (Delete ou Backspace no teclado, ou clique em X APAGAR) pergunta, numa caixa do jogo: "Tem certeza de que deseja apagar este jogo salvo?", com o espaço, o nome, a data e o tempo de jogo, "Não será possível recuperá-lo." e **NÃO / SIM**, com **NÃO** destacado, como nas perguntas do próprio jogo. **A** escolhe, **B** / Esc / botão direito cancela; o mouse aponta e clica em NÃO / SIM / VOLTAR / SELECIONAR.

- **SIM** apaga o save pelo próprio código de saves do jogo (a função de apagar do drive de saves dele, que o Xbox nunca usou porque os saves eram apagados no painel do console) e a lista é lida de novo na hora (o jogo passa pela tela rápida de "Acessando os arquivos salvos..." e volta) com o mesmo slot selecionado, agora VAZIO; os outros saves ficam como estavam. Apagando o último save no LOAD GAME, o jogo mostra a mensagem dele de "nenhum jogo salvo" e volta ao menu.
- **Não tem volta**: o save apagado sai de `userdata\content\...` (a pasta do pacote, o `.header` e a miniatura) e **não pode ser recuperado** pelo jogo. Se quiser guardar um save, copie a pasta `dist\userdata\content` antes.
- Nunca apaga sem a pergunta, e a pergunta só aparece num slot com save.

Textos em inglês e em PT-BR (segue o idioma escolhido em SETTINGS). Testes: perfis `nome_teclado_controle`, `nome_teclado_fisico`, `nome_teclado_mouse`, `apagar_save` e `apagar_save_mouse` (tabela em "Rodando e testando"); capturas em `build\nome_saves\` (`en_*` e `ptbr_*`, com `*_saves_depois_*.txt` listando os saves de cada etapa). Detalhes: `runtime/NOTES.md`, "Teclado do nome e apagar saves".

### Tela de título: "Definitive Edition"

Na tela do PRESS START, logo abaixo do logo do Crash of the Titans, aparece **Definitive Edition**, com a fonte dos menus do próprio jogo (Titans) em dourado; o logo sobe um pouco para dar espaço. É um texto do jogo (chave nova `frontend` / `FE_Definitive_Edition_Text`, igual em todos os idiomas, por ser o nome da edição), colocado por um patch embutido na hora em que o jogo abre o `default.rcf`: os arquivos do jogo não mudam. O menu principal continua como era. `CRASH_TITLE_LOGO=off` desliga. Capturas em `docs\capturas\logo\` (título em inglês e PT-BR, interna 1280x720 e 3840x2160 com DLSS; o menu sem mudanças). Detalhes: `runtime/NOTES.md`, "Logo Definitive Edition".

### Visual (fontes e arte do jogo)

A tela segue o estilo do frontend do jogo, com a arte lida na abertura do `default.rcf` da sua cópia (nada é copiado para o projeto; `runtime/settings/menu_art.*`):

- **Fontes**: os títulos (CONFIGURAÇÕES e as abas) usam a `Titans_Large` (a de NEW GAME / LOAD GAME) e o resto a `Titans_Small` (a dos avisos "Não desligue o console Xbox 360"), ambas de `package\b4c85fe7.p3d`. Cada letra tem o contorno embutido na textura (41% de opacidade), e o runtime pinta como o jogo: itens laranja com contorno marrom, o selecionado ciano com contorno azul-petróleo. As letras que as fontes não têm (ã õ Ã Õ Ê Ô) são compostas como no pacote PT-BR (veja "Tradução PT-BR"), então a tela tem todos os acentos.
- **Ícones e molduras**: LB / RB, A, B e as setas vermelhas são os ícones de botão das próprias fontes; o ícone de setas em círculo marca o que reinicia o jogo; as barras de volume e porcentagem são estrelas como as do menu de pausa (`FE_star_open`); a ajuda fica numa caixa de mensagem do jogo (`MSG_Background`), e o título entre os ornamentos da tela de nome (`FE_Name_Divider_Left/Right`).
- **Idioma da tela**: em inglês, ou em português quando o idioma escolhido é Português (BR); muda junto com a opção IDIOMA.
- **Resolução**: a tela é desenhada na CPU no tamanho da janela (não na resolução interna nem no tamanho da cena do DLSS/FSR), com as letras e imagens reamostradas para esse tamanho, numa área 16:9 centralizada; só é redesenhada quando algo muda. Para ver sem abrir o jogo: `build\CrashRecomp.exe --settings-preview <pasta>` grava cada aba em inglês e português em 1280x720, 1920x1080, 3840x2160 e 2560x1080 (sobre um fundo liso).
- **Mods**: se um mod substitui um desses pacotes, a tela usa o arquivo do mod. Se a tela de opções do PS2 não puder ser esvaziada (um mod mudou `package\cdd70a8c.p3d` a ponto de o patch não achar o que procura), as configurações são desenhadas com o fundo escurecido e as espirais do menu de pausa, e com o próprio (B) VOLTAR.

**DLSS / FSR**: com um upscaler ligado, a **Resolução** passa a ser a de saída: o HUD, o motion blur e a profundidade de campo são desenhados nela, e a cena 3D é desenhada no tamanho do modo (ex.: 3840x2160 + DLSS Qualidade = cena em 2560x1440) e reescalada pelo DLSS ou pelo FSR. O modo nativo (DLAA / FSR Native AA) é o que mais tira o serrilhado: a cena fica na resolução escolhida e o upscaler faz só o anti-aliasing temporal. Nesta RTX 5080 o melhor resultado é o DLSS (DLAA ou Qualidade). O FSR 4 só roda em Radeon RX 9000, por isso a opção é o FSR 3.1, que funciona em qualquer placa. O jogo continua a 30/60 fps como antes: o custo do upscaler é pequeno (~0,1–0,2 ms de CPU por quadro). Se o upscaler escolhido não existir na máquina (DLSS fora de uma RTX, DLL ausente), a tela mostra "(N/D)" e o jogo roda sem ele. As DLLs (`nvngx_dlss.dll`, `amd_fidelityfx_loader_dx12.dll`, `amd_fidelityfx_upscaler_dx12.dll`) vêm de `thirdparty\dlss` e `thirdparty\fsr` (SDKs oficiais da NVIDIA e da AMD, versões e licenças nos READMEs de lá) e o CMake as copia para o lado do exe.

**Serrilhado das texturas**: as texturas agora têm mipmaps e são filtradas com trilinear + anisotrópico, como no console (antes o runtime só usava o nível base, o que deixava o chão e a folhagem distantes "chiando").

**Por que não há resolução de sombra**: o jogo não usa mapas de sombra. As sombras são volumes de stencil desenhados na própria cena e depois suavizados por um filtro de 9 amostras na tela. A nitidez delas segue a resolução interna (e o DLSS/FSR); o que dá para ajustar é o raio desse filtro (**Suavidade das sombras**). FOV e distância de desenho não viraram opções: o FOV é definido por volume de câmera nos scripts de cada fase, e o jogo não expõe distância de desenho (detalhes em `runtime/NOTES.md`, passo 20).

**FPS**: o jogo original trava em 30 por dois motivos: a troca de quadro espera 2 vblanks e o laço principal só atualiza depois de 1/60 s. O runtime acelera o vblank emulado (60 Hz → 1000 Hz) e, acima de 60, baixa esse limite do laço; nos limites 60 e 120 quem segura o ritmo é um limitador no `VdSwap` (um quadro que atrasa custa só o que levou, em vez de esperar o próximo par de vblanks e cair para 30 naquele quadro); o limite 30 continua exatamente como no console. O jogo mede o tempo real, então a velocidade não muda (conferido numa cena: mesmo ponto no mesmo instante a 30 e a 55 fps). Medido aqui (Ryzen 7 5700X, vila a 1080p): 60 e 120 estáveis, ~125 sem limite (veja "Desempenho"). O jogo lê o controle a ~30 Hz fixos; nos testes automáticos os menus funcionaram em todos os limites. **Resolução**: na primeira vez com uma resolução nova os shaders são compilados de novo (alguns segundos de travadas no início); depois ficam no cache. 3840x2160 usa bem mais memória de vídeo. Os idiomas oferecidos são os que esta cópia tem completos (bíblias de texto e `.rcf` de voz); o jogo também conhece sueco, norueguês, finlandês, dinamarquês e russo, mas os `.rcf` de voz deles não existem aqui.

**Arquivo**: `userdata\settings.toml` (o do jogador fica em `dist\userdata\`), criado com os padrões na primeira execução, lido na abertura do jogo e regravado ao fechar a tela. Dá para editar à mão com o jogo fechado (valores inválidos voltam ao padrão e aparecem no log):

```toml
[paths]
game = "D:/Jogos/Crash"   # pasta do jogo, se não estiver em game\ ao lado do exe (veja "Jogando")
[audio]
master = 100     # 0-100, vale na hora
music = 100      # 0-100, vale na hora (como effects e voice)
effects = 100
voice = 100
[video]
window = "windowed"      # windowed | borderless | fullscreen
vsync = false
fps_limit = 30           # 30 (original) | 60 | 120 | 0 = sem limite
resolution = "1280x720"  # 1280x720 | 1600x900 | 1920x1080 | 2560x1440 | 3840x2160 (ao sair da tela)
overlay = "off"          # off | fps | frametime | full (F1 alterna)
antialiasing = "off"     # off | fxaa | smaa
[graphics]
upscaler = "off"         # off | dlss | fsr (ao sair da tela)
upscaler_quality = "native"   # native (DLAA / FSR Native AA) | quality | balanced | performance | ultra_performance (ao sair da tela)
sharpness = 0            # 0-100, nitidez do FSR (RCAS)
motion_blur = 100        # 0-100 %
depth_of_field = 100     # 0-100 %
anisotropic_filtering = 16    # 1 (desligado) | 2 | 4 | 8 | 16
shadow_softness = 100    # 0-200 %
hd_remaster = false      # HD AI Remaster (aba DEFINITIVE EDITION; reinicia o jogo)
[game]
language = "english"     # english | french | german | spanish | italian | dutch | pt-br
subtitles = false        # legendas (veja "Legendas")
subtitle_size = 100      # 50-200 %
subtitle_background = 50 # 0-100 %, opacidade da caixa
mojo_colors = "ps2"      # ps2 | xbox360 (reinicia o jogo; veja "Cores do mojo")
coop_mode = "leapfrog"   # leapfrog (pula-sela) | piggyback (cavalinho)
invert_axis = false      # inverter eixo da câmera (a opção do jogo)
autosave = true          # salvamento automático no slot escolhido; false = o jogo não salva sozinho
skip_cutscenes = true    # segurar B / Esc pula cenas e vídeos, com um aviso no canto (veja "Pular cenas e vídeos")
[camera]
free_camera = false      # camera livre em terceira pessoa (experimental; veja "Camera livre")
sensitivity = 100        # 25-300 %
invert_y = false
distance = 100           # 50-200 %
[lighting]
ssao = "off"             # off | low | high (veja "Iluminação (remaster)")
gi = false               # luz indireta
gi_intensity = 100       # 0-200 %
contact_shadows = false
soft_shadows = false     # as sombras do jogo com filtro mais suave
```

Nos testes, `CRASH_SETTINGS="audio.music=50;video.antialiasing=fxaa"` (em `env = { ... }` do perfil) muda valores só naquela execução. Os testes usam a cópia do `settings.toml` de `tests\userdata_base` na pasta da execução, então não mexem no seu.

**Como funciona** (detalhes em `runtime/NOTES.md`, passo 19 e "Tela de configurações nativa"): o menu do jogo é uma máquina de estados de dados (`fighttrees\Frontend.bfig`) com trechos Lua. O runtime aplica patches de mesmo tamanho nas entradas do `default.rcf` ao servi-las ao jogo (sem tocar nos arquivos e sem depender da pasta `mods`): a saída `ExitCalibration` passa a ir para a tela de opções do PS2 (o destino do `ExitOptions` do menu de PS2) e chama `PlaySoundSelect(4242)`, e o runtime abre a tela quando vê esse argumento; os textos, o menu e o logo dessa tela ficam transparentes (`package\cdd70a8c.p3d`); o rótulo é trocado nas bíblias de texto; a dica do Y fica invisível. Música/efeitos/vozes escalam o volume das categorias do mixer base do jogo (`sound\zscripts\mixers.p3d`). O idioma é o que o runtime responde ao jogo em `XGetLanguage`/XConfig (e o país, no caso do holandês). Se um mod substituir alguma dessas entradas, o patch é aplicado ao arquivo do mod; só fica de fora se o mod mudou o trecho que o patch procura (aparece no log).

### Cores do mojo

No Xbox 360 todo mojo é azul. Nos arquivos do jogo, os quatro tamanhos (pequeno, médio, grande e extra grande) usam o mesmo material: a mesma textura e as mesmas 4 paletas azuis/cianas, que o jogo alterna para fazer o brilho. Não era um bug do port: o renderer desenha o que os dados pedem. No PS2, na primeira fase, o mojo grande é de um azul bem claro, que brilha mais que o azul dos pequenos, e o extra grande é roxo lilás (o dourado aparece mais adiante no jogo e não precisa de correção). Com **SETTINGS > DEFINITIVE EDITION > CORES DO MOJO = PS2** (o padrão; `mojo_colors = "ps2"`), o runtime reconstrói em memória o `package\PermanentContent.p3d` do `default.rcf` ao abrir o jogo, com uma cópia do material (e do halo que brilha em volta do mojo) para cada um desses dois tamanhos, recolorida e com nomes próprios:

| Tamanho | Xbox 360 | PS2 |
|---|---|---|
| pequeno (S), médio (M) | azul | azul |
| grande (L) | azul | azul bem claro, mais brilhante (halo também) |
| extra grande (XL) | azul | roxo lilás (como no PS2) |

Os arquivos do jogo não mudam, e **XBOX 360** volta ao original. Vale depois do reinício que a tela oferece ao sair. Capturas de antes e depois em `docs\capturas\antigas\mojo\` (perfil `mojo` do `run.toml`). Detalhes e depuração (`CRASH_MOJO_COLORS`, `CRASH_MOJO_DUMP`) em `runtime/NOTES.md`, "Cores do mojo".

### Câmera livre (experimental)

**SETTINGS > DEFINITIVE EDITION > CÂMERA LIVRE (EXPERIMENTAL)** (`[camera] free_camera = true`, desligada por padrão) troca as câmeras do jogo, quase todas fixas por volume de câmera, por uma câmera de terceira pessoa como a dos jogos atuais. Vale na hora, inclusive no meio da fase.

| Controle | O quê |
|---|---|
| Analógico direito / mouse | gira a câmera em volta do Crash (horizontal e vertical, de −20° a 70°) |
| Analógico esquerdo | anda relativo à câmera: para cima = para onde a câmera olha |
| R3 (clique do analógico direito) | recentra a câmera atrás do Crash |

- **Sensibilidade**, **inverter eixo Y** e **distância** ficam na mesma aba, no sub-grupo CÂMERA EM TERCEIRA PESSOA (`sensitivity`, `invert_y`, `distance`). O mouse usa também a sensibilidade e o inverter de `[controls]`.
- **Colisão**: a câmera se aproxima quando uma parede ou o chão ficaria entre ela e o Crash, e sobe para olhar por cima quando isso dá mais espaço; volta para a distância escolhida aos poucos.
- **Titãs**: montado num titã, a câmera continua no Crash (que vai nos ombros dele) e se afasta 1,6×.
- **Cenas e momentos roteirizados**: as cinemáticas (cenas com faixas pretas) e as câmeras de script continuam com a câmera do jogo, e a câmera livre volta sozinha depois, partindo de onde a câmera do jogo estava (mistura de 0,35 s). Na pausa a imagem fica parada.
- **Como funciona**: a lógica de câmera do jogo continua rodando inteira (volumes, transições, tremor, FOV); o runtime troca só o resultado, a câmera de render do Pure3D, logo depois que o jogo a escreve. Como o próprio jogo converte o analógico esquerdo usando essa câmera, o andar fica relativo à câmera livre sem mais nada. Detalhes em `runtime/NOTES.md`, "Câmera livre em terceira pessoa".
- **Desligada**, o jogo é o mesmo de antes: o mesmo replay gravado (`gravado_20261020_221744`) dá o mesmo enquadramento de ontem (diferença média 0,5 em 255 no início).

**Limitações conhecidas** (é experimental): as fases foram feitas para as câmeras fixas, então olhando em volta aparecem **paredes invisíveis** (a câmera bate nelas como numa parede de verdade; atrás do ponto de partida do episódio 2 ela para a 1,4 do Crash), bordas e fundos sem acabamento, e objetos que surgem de repente (**culling/LOD** pensados para o enquadramento original). O **som** continua posicionado pela câmera do jogo e o **FOV** é o do volume da fase. **Perfis gravados** com a câmera do jogo desincronizam com a câmera livre (o analógico esquerdo vira outra direção); para testes, `CRASH_CAMERA_FREE_AFTER=<segundos>` liga a câmera livre só a partir desse ponto do replay. Com dois jogadores ela segue o primeiro. `CRASH_CAMERA_PROBE=1` registra no log o estado da câmera e o vetor de movimento.

Capturas em `docs\capturas\camera\` (veja o `LEIA.txt` de lá).

### Pular cenas e vídeos

O jogo original não deixa pular as cenas (nenhum botão faz nada durante elas). Aqui, quando uma **cena** começa (as cinemáticas com faixas pretas, na abertura das fases ou no meio delas) ou um dos **vídeos de abertura** (os logos e o teatro de sombras, em Bink), aparece um aviso pequeno no canto de baixo à direita, dentro da faixa preta: o botão e **PULAR CENA** / **SKIP SCENE** (**PULAR VÍDEO** / **SKIP VIDEO** nos vídeos), com a fonte e os ícones do próprio jogo. Ele some depois de 4 s e volta quando qualquer botão, tecla ou o mouse mexe.

- **Segure** por 0,8 s para pular (um anel em volta do botão enche enquanto isso; no teclado, a borda da tecla): **B** no controle Xbox (○ no PlayStation; START/Options e A/✕ também valem), **Esc**, **Espaço** ou **Enter** no teclado, ou o botão esquerdo do mouse em cima do aviso. O ícone segue o último aparelho usado (`prompts` em `[controls]`).
- Enquanto a cena ou o vídeo passa, esses botões não chegam ao jogo (nada no jogo os usa ali; nos vídeos, o toque que já pulava vira o "segurar", contra pulos sem querer), e depois do pulo continuam escondidos até serem soltos: o Esc segurado não abre a pausa, o A não faz o Crash pular.
- **Como pula**: a cena termina pelo **mesmo caminho do fim natural dela**, chamado pelo runtime (o som para, o fade, o jogador volta ao lugar, a cena fica marcada como concluída e o jogo segue sozinho); o vídeo recebe um START, o pulo do próprio jogo. A vila do L1_E1 depois do pulo fica igual à do fim da cena. As **legendas** da cena somem na hora.
- **Desligar**: **SETTINGS > JOGO > PULAR CENAS** (`skip_cutscenes = true` em `[game]`, ligado por padrão; vale na hora). Desligado, não há aviso e os botões passam para o jogo como no original. O vídeo "attract" (o que passa sozinho quando o título fica parado) continua saindo com qualquer botão, como no original.

Testes: `pular_cena` (Xbox, inglês, L1_E1: aviso, aviso apagado, B segurado, vila e pausa), `pular_cena_teclado` (Esc em PT-BR), `pular_cena_ps` (○, L1_E3), `pular_cena_chefe` (L4_E4), `pular_cena_fim` (L5_E1) e `pular_videos` (os vídeos de abertura até o título). Eventos: `skip_prompt`, `cinematic_skipped` (`held, cutscene frame N`), `video_skipped`, `cinematic_done`. `CRASH_SKIP_TRACE=1` registra cada leitura do controle enquanto o aviso está na tela. Capturas em `build\pular_cena\`. Detalhes em `runtime/NOTES.md`, "Pular as cenas".

## Desempenho

O jogo é limitado pela **CPU**, não pela placa de vídeo: uma thread do runtime (o *command processor*, que traduz os comandos de GPU do Xbox 360 para Vulkan) é o gargalo, e a thread do jogo espera por ela. Por isso **o DLSS/FSR e a resolução quase não mudam o FPS**: eles aliviam a GPU, que já está ociosa (espera de ~0 ms por quadro em todas as medições). Use o DLSS/FSR pela qualidade de imagem (DLAA, anti-aliasing temporal), não para ganhar FPS.

Medido num Ryzen 7 5700X + RTX 5080, na vila da primeira fase (a cena mais pesada), 1080p. Primeira rodada (2026-10-21, andando pela vila):

| | antes | agora |
|---|---|---|
| sem limite | 65 fps | **125 fps** |
| limite 60 | 57 fps (com quedas a 30 em ~1/3 dos quadros) | 60 fps estáveis |
| DLSS Qualidade, sem limite | 64 fps | 98 fps |

Segunda rodada (2026-10-22, parado logo depois da cena de abertura, o momento mais pesado, HD AI Remaster desligado, sem limite): **~94 → 176 fps**, com a janela fora de foco igual à em foco (antes ~100 contra ~125). O que mudou: as texturas que não mudam não são mais relidas a cada quadro (proteção de página), o processo não sofre mais o *power throttling* do Windows quando a janela perde o foco, um SRV só para os vértices, e o código do jogo regenerado com opções de otimização do XenonRecomp. Agora o command processor fica a maior parte do tempo ocioso e o limite passou a ser a thread do próprio jogo.

Num Ryzen 7 3700X (Zen 2, ~20–25% mais lento por núcleo) a estimativa é ~50 fps no original (o relatado) e ~140 fps agora nesse trecho, 60 estáveis com o limite 60. Detalhes, perfis por thread e o que mudou: `runtime/NOTES.md`, "Desempenho de CPU"; capturas, números e gráficos em `build\desempenho\` (`LEIA.md`).

**Dicas**

- **Limite de FPS**: 60 ou 120 dão o ritmo mais regular; "sem limite" dá o maior FPS médio, mas o tempo de cada quadro varia com a cena. 30 é o original.
- **V-Sync** desligado (o padrão) deixa passar da taxa do monitor; ligado, o FPS fica preso a ela. Em janela sem foco o jogo não fica mais preso aos Hz do monitor (o runtime pede *tearing* ao DXGI quando o V-Sync está desligado).
- **Núcleos**: o jogo usa duas threads pesadas (a do jogo e a do command processor); núcleos rápidos importam mais que muitos núcleos (com afinidade em 4 núcleos sem SMT o FPS foi o mesmo).
- **Janela fora de foco**: o mesmo FPS que em foco (o runtime pede ao Windows para não aplicar o *power throttling* de segundo plano e sobe a prioridade das duas threads pesadas).
- **DLSS/FSR**: escolha pela imagem (DLAA/Qualidade); o custo de CPU deles é pequeno, mas com o DLSS o command processor ficou ~25% mais lento por quadro nas medições (ainda não explicado), então sem upscaler o FPS é um pouco maior.
- **Overlay** (F1, modo completo) mostra FPS, frame time e o 1% low; a cada 300 quadros o `run.log` traz `[gpu] present pacing` (tempo esperando a GPU, que fica em ~0 quando o limite é a CPU) e `[gpu] CP thread ms/frame` / `CP work/frame` (o trabalho do command processor).

**Medindo** (para quem mexe no runtime): rode sozinho, sem outro `CrashRecomp.exe` aberto.

```bat
python tools\perf\bench.py meu_teste --profile desempenho_vila_direto  :: ~45 s: boot direto, cena pulada, 20 s parado na vila (o mais usado)
python tools\perf\bench.py meu_teste                               :: ~3,5 min: NEW GAME, cena e caminhada na vila
python tools\perf\bench.py meu_teste_dlss --settings "video.resolution=1920x1080;graphics.upscaler=dlss;graphics.upscaler_quality=quality"
python tools\perf\bench.py meu_teste_60 --fps 60
python tools\perf\bench.py meu_perfil --sample 1000                 :: + amostrador de CPU (build\perf\sampler.exe)
python tools\perf\walk_window.py meu_teste meu_teste_dlss            :: FPS dos primeiros 20 s jogáveis
python tools\perf\profile.py build\desempenho\meu_perfil.samples.txt --log <pasta da execução>\run.log
python tools\perf\system_callers.py build\desempenho\meu_perfil.samples.txt --lines   :: tempo em DLLs do sistema (Vulkan, driver, esperas) por linha do código
python tools\perf\hot_lines.py build\desempenho\meu_perfil.samples.txt GetTextureSrv ExecuteBuffer   :: tempo por linha dentro de funções do runtime
```

O `build\CrashRecomp.pdb` (tabelas de linha só do runtime, `-gline-tables-only`, sem mudar o código gerado) e o `build\CrashRecomp.map` dão nome e linha às amostras. O `bench.py` registra se a janela estava em foco (`janela em foco N/M`).

O perfil `desempenho_vila` não depende do relógio: `CRASH_MENU_AUTOPILOT=newgame` aperta o botão de cada menu assim que a tela aparece (logos, título, NEW GAME, nome, NORMAL, slot 1, CONTINUE; o mouse de verdade é ignorado), a cena de abertura roda a 30 fps (ela lê o controle uma vez por quadro e não pula), o replay espera o início da jogabilidade (`WAIT` pela linha `frontend node 638`) e `CRASH_FPS_LIMIT_AT` troca o limite de FPS ali. `bench.py --exe build\CrashRecomp_antes.exe --env CRASH_D3D_RING_MB=0` mede o exe de antes das otimizações (guardado em `build\desempenho\`). Variáveis para comparar: `CRASH_D3D_RING_MB` (buffer de comandos do D3D, padrão 32; 0 = os 6 MB do jogo), `CRASH_D3D_SEGMENT_KB` (tamanho dos segmentos dele; o do jogo, 192 KB, é o melhor), `CRASH_D3D_SPIN=busy` (a espera original do D3D), `CRASH_FRAME_LIMITER=vblank` (o limite de FPS antigo), `CRASH_TEXTURE_WATCH=0` (hasheia todas as texturas todo quadro, como antes), `CRASH_D3D_WAIT_STATS=1` (tempo nas esperas do D3D no log).

## Controles de PC

O jogo lê os quatro controles do Xbox 360 (o jogador 1 é o que apertou START na tela título; um segundo controle entra no co-op). O runtime põe em cada um deles um controle físico: **controles XInput** (Xbox One/Series e afins) ou **DualSense / DualSense Edge / DualShock 4** lidos direto por HID (USB e Bluetooth, sem programa extra), um por jogador (veja "Co-op local"). O **teclado** e o **mouse** vão sempre para o jogador 1, somados ao controle dele. Os ícones dos botões nos menus, nas dicas e na tela SETTINGS seguem o último aparelho usado.

### Co-op local (dois jogadores)

Ligue dois controles e jogue: na fase, o HUD do jogo mostra **JOIN GAME (B)** enquanto houver um segundo controle; **B** no segundo controle põe o jogador 2 na **mochila** do Crash (o modo do jogo; COOPERATIVO na aba JOGO escolhe PULA-SELA ou CAVALINHO), **Y** desce da mochila e o jogador 2 anda sozinho; **START** no controle dele pausa como **P2 PAUSED** (DROP OUT tira o jogador 2).

- **Quem é quem**: cada controle físico ganha um jogador na ordem em que aparece e fica com ele até ser desligado; **com um controle só, ele é o jogador 1, junto com o teclado**. Em **SETTINGS > CONTROLES** (e no launcher), **JOGADOR 1** e **JOGADOR 2** escolhem o controle de cada um: AUTO ou um dos conectados (a tela mostra o nome: CONTROLE 1 para XInput, DUALSENSE 1 (USB), DUALSENSE 2 (USB)...). Vale na hora. Em AUTO, o controle que apertou START na tela título vira o jogador 1 (como no console) e o teclado vai com ele; com um controle escolhido, ele é o jogador 1 mesmo se o outro apertar START.
- **DS4Windows / DSX**: um DualSense que esses programas também mostram como Xbox virtual (sem esconder o original) é um controle só: o runtime compara os primeiros apertos dos dois e junta. Até lá (os 1 ou 2 primeiros apertos) o DualSense não joga sozinho, só pelo Xbox virtual. Com HidHide (o original escondido) só o Xbox virtual existe.
- `settings.toml`: `[controls] player1 = "auto"`, `player2 = "auto"` (ou `xbox1`..`xbox4` = o controle XInput daquele número, `playstation1`..`playstation4` = o n-ésimo PlayStation lido por HID).
- **Log**: `[input] jogadores: P1 = DualSense 1 (USB) (controle 0 do jogo), P2 = DualSense 2 (USB) (controle 1 do jogo)` a cada mudança, `[input] o jogo pegou o controle N como jogador 1` na tela título, `[input] DualSense (USB): controle próprio` / `é o mesmo controle que o XInput 1`, e os primeiros botões dos outros controles (`[input] controle 1 do jogo (DualSense 2 (USB)): botões 0x2000`).
- **Testes**: `CrashRecomp.exe --pads-selftest` passa pelos casos (Xbox + DualSense, DS4Windows sem HidHide, DS4Windows aberto depois, DualSense e depois Xbox, P1 desconectado, escolhas da tela, START no título). No `CRASH_INPUT_SCRIPT` / `input` do `run.toml`, `P2_` antes de um botão, analógico ou gatilho aperta um **segundo controle virtual** (conectado desde o boot): `[2, "P2_B"]`, `[5, "P2_LS", 1, 0, 2]`; `P3_` e `P4_` também existem. Perfis `coop_p2` (virtual), `coop_jogadores` e `coop_manual` (controles de verdade) na tabela de "Rodando e testando".
- **Conferido** (2026-10-22, `docs\capturas\coop\`): com o controle virtual, o HUD mostra JOIN GAME, B põe o jogador 2 na mochila, Y solta, ele anda sozinho e START pausa como P2 PAUSED (capturas 1-4); com **dois DualSense de verdade por USB** (o DSX aberto segurando os dois pelo Bluetooth), uma pessoa jogou o P2 pelo segundo controle (`coop_manual`): o log tem `P1 = DualSense 1 (USB) (controle 0 do jogo), P2 = DualSense 2 (USB) (controle 1 do jogo)`, os botões do controle 1 e até o detector de giro do jogo no controle 1 (`coop_manual_input.log`), e o P2 quebra plantas e pega mojo sozinho (capturas 5-6); a troca pela tela (JOGADOR 1 = DUALSENSE 2) está na captura 7.

### Os controles do jogo

Tirados do próprio jogo: o mapa de entradas (`script\mixins\inputmap_methods.lua`, bytecode Lua 5.0) e as árvores de luta do Crash (`fighttrees\Crash.bfig` / `Crashlua.blua`). O jogo usa nomes de PlayStation (X, Círculo, Quadrado, Triângulo, R1...), aqui já convertidos para o Xbox 360:

| Botão | No jogo |
|---|---|
| A | pular (duas vezes: pulo duplo) |
| X | ataque leve; no ar, repetido, giro planando (SPIN HOVER) |
| Y | ataque pesado (segurar carrega); montado num titã, descer |
| B | ação: montar no inimigo atordoado (jack), contra-ataque, interagir |
| RT | bloquear; com o analógico, esquivar; no ar, deslizar |
| RB | golpe final |
| LB | modificador (LB + Y comemora, LB + A / X: combos) |
| Analógico esquerdo **girando** | os golpes de giro: **OLD SKOOL** (girar + ataque leve repetido, o giro clássico do Crash), **SPIN DISMOUNT** (girar + ação), **GYRO JACKHAMMER** (girar + ataque pesado) |
| Analógico esquerdo sacudindo | escapar de agarrões (contador de "sacudidas" do analógico) |
| START / BACK | pausa / voltar |

**Como o jogo vê o giro** (`sub_821EC3E0`): o analógico dividido em 8 octantes; cada leitura com o analógico além de 60% acende o octante por 0,75 s; com 7 dos 8 acesos o jogo considera "girando" (por 0,2 s, e precisa de outra volta para continuar). Voltar o analógico ao centro apaga tudo. A tecla de **girar** faz exatamente isso: o analógico esquerdo dá uma volta completa a cada 9 leituras do jogo (0,3 s a 30 Hz), sempre no máximo. O log mostra `[input] game: left stick rotation detected` quando o próprio detector do jogo reconhece a volta.

### Teclado e mouse (padrão)

| Ação | Teclas padrão | Vira |
|---|---|---|
| Andar | W A S D | analógico esquerdo |
| Andar devagar | Alt esquerdo (segurar) | analógico a 40% (`walk_speed`) |
| Câmera | mouse (na gameplay) / setas | analógico direito |
| Pular | Espaço | A |
| Ataque leve | botão esquerdo do mouse / J | X |
| Ataque pesado | botão direito do mouse / K | Y |
| Ação / montar / contra-ataque | E / L | B |
| Bloquear / esquivar / deslizar | Shift esquerdo / Ctrl esquerdo | RT |
| **Girar o analógico** (segurar) | Q | analógico esquerdo em círculos: Q + clique esquerdo repetido = OLD SKOOL, Q + E = SPIN DISMOUNT, Q + clique direito = GYRO JACKHAMMER |
| **Ataque giratório** (segurar) | R / botão do meio | girar + ataque leve repetido sozinho (o giro do Crash numa tecla) |
| Golpe final | F | RB |
| Modificador | C | LB |
| Gatilho esquerdo | Z | LT |
| Pausa | Esc | START |
| Voltar / mapa | Tab | BACK |

Nos **menus** as teclas são fixas: setas ou WASD navegam, **Enter / Espaço** confirmam (A), **Esc** volta (B; na pausa, Esc volta ao jogo), **Backspace / Delete** = X (apagar), **Tab** = Y, **P** = START, **Q / E** = LB / RB. O runtime sabe se está num menu pela árvore do frontend do jogo (`Frontend.bfig`): o nó "jogo rodando" e as cinemáticas que voltam a ele são gameplay; o resto é menu. Nos vídeos da abertura o frontend não roda, então lá vale a gameplay (Esc = START pula).

Na gameplay o cursor fica escondido e preso na janela, e o movimento do mouse gira a câmera (`mouse_camera`, `mouse_sensitivity`, `invert_y`). Nos menus o cursor aparece e some depois de 3 s parado. Alt sozinho e F10 não abrem mais o menu da janela (travava o jogo); Alt+Enter continua alternando janela e tela cheia sem bordas. O clique que traz a janela para a frente não chega ao jogo.

**Trocar as teclas**: SETTINGS > **CONTROLES** (A / Enter numa ação e aperte a tecla ou botão do mouse; Esc cancela; X / Backspace volta ao padrão; a tecla nova sai das outras ações) ou à mão em `userdata\settings.toml`:

```toml
[controls]
jump = "SPACE"
light_attack = "MOUSE1, J"     # até duas teclas por ação; "none" = nenhuma
rotate_stick = "Q"
mouse_sensitivity = 100        # 10-400 %
walk_speed = 40                # 10-90 %
mouse_camera = true
invert_y = false
mouse_menus = true
prompts = "auto"               # auto | xbox | playstation | keyboard
```

Nomes das teclas: `A`-`Z`, `0`-`9`, `F1`-`F24`, `SPACE`, `ENTER`, `ESC`, `TAB`, `BACKSPACE`, `LSHIFT`, `RSHIFT`, `LCTRL`, `RCTRL`, `LALT`, `RALT`, `UP`/`DOWN`/`LEFT`/`RIGHT`, `MOUSE1` (esquerdo), `MOUSE2` (direito), `MOUSE3` (meio), `MOUSE4`, `MOUSE5`, `WHEELUP`, `WHEELDOWN`, `NUM0`-`NUM9`, etc. A aba CONTROLES também tem os ícones, a câmera e a sensibilidade do mouse, o andar devagar e o mouse nos menus. Tudo vale na hora.

### Mouse nos menus

Como num jogo de PC. Nos menus do jogo (menu principal, saves, dificuldade, menu do episódio, pausa, confirmações...) **o item embaixo do cursor fica selecionado na hora**, sem passar pelos do meio (com o destaque e o som de mover do próprio jogo, uma vez por troca); **clique esquerdo** num item o escolhe; **botão direito** ou **Esc** volta; a **roda** anda um item por dente. Só o movimento do mouse conta: parado, ele não briga com o teclado nem com o controle. Clique fora dos itens não faz nada; numa tela sem menu (PRESS START, "Load successful") o clique é A.

Na tela **SETTINGS**: passar o mouse numa linha a seleciona; clique numa **aba** abre a aba (clique em LB / RB passa para a vizinha); clique nas **setas** ◄ ► muda o valor; clique numa **barra de estrelas** põe o valor ali (a 5ª estrela = 50%) e arrastar com o botão apertado ajusta; clique na linha é o A (liga/desliga, troca, ou na aba CONTROLES espera a tecla nova); a **roda** muda de linha (na aba CONTROLES a lista rola junto); clique em **(B) VOLTAR** / **SELECIONAR (A)** e nas respostas de "Reiniciar o jogo agora?" faz o mesmo que o botão; botão direito sai.

O cursor aparece nos menus e na tela SETTINGS e some depois de 3 s parado (volta ao mexer); na gameplay fica escondido (e preso na janela com `mouse_camera`). `mouse_menus = false` (MOUSE NOS MENUS, aba CONTROLES) desliga tudo isso.

Como funciona: o menu ativo é o da ação de menu da árvore do frontend (`CMenuAction::vf05`, `sub_820B2E98`, `this+24`); os itens e o índice selecionado vêm do objeto `Menu` do Scrooby, e o tamanho de cada item, das páginas dos pacotes do frontend. O cursor é levado da janela (com as barras pretas e a escala da saída) à imagem 1280x720 do jogo e à página 640x480. Passar por cima chama o `SetSelection` do próprio jogo (`sub_822F2E40`), e a roda o "anterior / próximo" dele. Telas sem menu do Scrooby (a fileira de personagens do episódio) e os botões de dica do jogo ("CONTINUE STORY (Y)") não são clicáveis; use as teclas. O teclado da tela do nome e o X APAGAR / a pergunta da tela de slots são do runtime e são clicáveis (veja "Nome e saves no PC").

### Ícones dos botões

O jogo desenha os botões com 14 ícones (A B X Y BACK START RT RB analógico < > girar LT LB), cada um uma textura paletizada nas fontes Titans (4 tamanhos) e KLEPTO. O renderer reconhece essas texturas pela chave do conteúdo e troca, na hora, por ícones **desenhados pelo próprio runtime** (formas + a fonte do jogo; nada copiado de outros jogos):

- **PlayStation**: ✕ ○ □ △ coloridos, L1 R1 L2 R2, Options, touchpad (o BACK).
- **Teclado**: a tecla ligada àquela ação (ESPAÇO, E, SHIFT, WASD...) ou um mouse com o botão aceso; nos menus, as teclas de menu (ENTER, ESC, TAB...).
- **Xbox**: os do jogo.

`prompts = "auto"` segue o último aparelho que mexeu (teclado/mouse, controle XInput ou PlayStation). Um DualSense usado pelo **DSX / DS4Windows** (com HidHide, que esconde o controle e cria um Xbox virtual) é reconhecido como PlayStation também. A tela SETTINGS usa os mesmos ícones. `CrashRecomp.exe --prompt-preview <pasta>` gera a folha de todos os ícones sem abrir o jogo; `CRASH_NO_PROMPT_ICONS=1` desliga a troca.

### Controles PlayStation (HID)

DualSense (inclusive Edge) e DualShock 4, por USB ou Bluetooth, são lidos direto pelo HID do Windows: ✕ = A, ○ = B, □ = X, △ = Y, L1/R1 = LB/RB, L2/R2 = LT/RT, Options = START, Create/Share e o clique do touchpad = BACK, L3/R3. Ligar e desligar com o jogo aberto funciona (o runtime procura de novo quando o Windows avisa e a cada 2 s). Cada um é um jogador (veja "Co-op local"). A **vibração** do jogo vai para o DualSense/DualShock do jogador, ou para o Xbox virtual quando o DSX/DS4Windows mostra o mesmo controle. Se outro programa estiver com o controle (DSX/DS4Windows), o runtime usa o Xbox virtual dele e mostra ícones de PlayStation.

- `CrashRecomp.exe --hid-monitor 30`: lista os controles PlayStation e mostra cada botão/analógico por 30 s (✕ segurado faz vibrar); `--hid-selftest` confere os formatos de relatório. `CRASH_HID_TRACE=1` registra os relatórios crus; `CRASH_NO_HID=1` desliga a leitura HID.
- **Conferido** com um DualSense de verdade por USB: os 14 botões, L2/R2 até o fim e os dois analógicos no curso todo (`docs\capturas\controles\hid_monitor_dualsense.txt`). Por Bluetooth, o DualSense desta máquina estava com o DSX (acesso negado, reconhecido como "em uso por outro programa").

### Capturas e testes

`docs\capturas\controles\`: `teclado_*` (ataque giratório e Q + ataque na fase), `icones_*` (menu, saves, episódio e pausa com teclado / PlayStation / Xbox), `mouse_*` (cada passo do `mouse_menus`), `settings_*` (aba CONTROLES, captura da tecla, tecla definida), `icones\icones_32/64/128.jpg` (folha dos ícones), `settings_preview\` (todas as abas) e `hid_monitor_dualsense.txt`. Perfis: `teclado`, `icones`, `mouse_menus`, `controles_tela` (tabela em "Rodando e testando"). `CRASH_MOUSE_TRACE=1` registra o menu ativo, os itens e o que o mouse fez.

**Para outros módulos** (a câmera livre usa): `input::pc::TakeMouseLook(true)` desliga mouse → analógico direito e `MouseLookDelta(dx, dy)` dá o movimento cru do mouse (`runtime/kernel/pc_input.h`).

## Editando os textos do jogo

Os textos (menus, HUD, legendas, telas de save) ficam em **text bibles** do Pure3D dentro dos pacotes `.p3d` do `default.rcf`. São 6 bíblias (`bootup`, `frontend`, `ingame`, `persistent`, `save_load`, `shared`) em até 13 idiomas (`E` inglês, `F` francês, `G` alemão, `S` espanhol, `I` italiano, `D` holandês e outras letras). A mesma bíblia aparece copiada em vários pacotes, às vezes em versões com subconjuntos diferentes.

```bat
:: 1. Exporta tudo para private\texts\<bíblia>\<idioma>.txt (uma vez)
python tools\texts.py export

:: 2. Edite os arquivos, por exemplo private\texts\frontend\E.txt:
::      0x00007e85 = NEW GAME     ->     0x00007e85 = NOVO JOGO

:: 3. Gera o mod (só os pacotes que mudaram) em dist\mods\meus_textos\
python tools\texts.py build --mod meus_textos

:: 4. Joga ou testa
python tools\run_game.py jogar
```

- **Formato**: uma linha `0xHASH = texto` por chave. O jogo procura o texto pelo hash, então não mude os hashes. Escapes: `\n` (quebra de linha), `\\` (barra) e `\uXXXX` (caracteres especiais, como os ícones de botão). Linhas apagadas mantêm o texto original.
- **Versões**: o arquivo junta todas as versões da bíblia. Uma linha editada vale para todos os pacotes que têm aquela chave (`build` diz em quantas cópias aplicou).
- **Reconstrução segura**: o buffer original é mantido. Um texto que cabe no próprio espaço é escrito no lugar; um maior é anexado ao fim, e só aquela chave é reapontada. Sem edições, cada bloco sai idêntico byte a byte ao original (verificado nos 239 blocos de idioma).
- **Opções**: `--src` (pasta dos textos editados), `--mods-dir` (onde gravar o mod) e `--game` (pasta do jogo). `python tools\texts.py scan` lista as bíblias, idiomas, versões e pacotes.
- Os textos exportados são dados do jogo: ficam em `private\` e não devem ser publicados. O mod vai para `dist\mods\` (a pasta do jogo).

## Tradução PT-BR

O jogo pode ser jogado com **todos os textos em português do Brasil e as vozes originais em inglês**. É um idioma a mais na tela de configurações (**SETTINGS > JOGO > IDIOMA > PORTUGUÊS (BR)**; ao sair, a tela reinicia o jogo); o inglês continua igual. No `settings.toml`: `language = "pt-br"`.

A tradução é um **pacote de idioma** em `languages\pt-br\`:

```
languages\pt-br\language.toml        id, nome na tela, idioma base (inglês), pacotes com textos, glifos compostos
languages\pt-br\texts\<bíblia>.txt   os textos traduzidos (bootup, frontend, ingame, persistent, save_load, shared)
languages\pt-br\GLOSSARIO.md         os termos usados (titãs, golpes, menus, saves)
```

Não há nada para gerar: o jogo roda em inglês por baixo (idioma do console, letra `E` das bíblias e `english.rcf` para as vozes) e, ao abrir o `default.rcf`, o runtime reconstrói em memória as bíblias em inglês dos pacotes listados com os textos do pacote. Para editar, mude a linha em `texts\<bíblia>.txt` e reinicie o jogo. Os arquivos de texto são só a tradução (nenhum dado do jogo é copiado para `languages\`).

```bat
:: confere o pacote: ícones de botão, códigos, chaves que não existem, letras que a fonte não desenha
python tools\texts.py lang-check pt-br
:: lista também os textos curtos que mais cresceram (candidatos a estourar botões)
python tools\texts.py lang-check pt-br --long 40
:: começa outro idioma a partir do inglês (cria languages\<id>\texts com o texto original)
python tools\texts.py lang-init <id>
```

- **Formato**: igual ao de `tools\texts.py export` (`0xHASH = texto`, escapes `\n`, `\\`, `\uXXXX`). Os ícones de botão são letras como `± ² ³ ´` e precisam ficar (o `lang-check` acusa). Uma chave sem linha fica em inglês.
- **Cópias diferentes da mesma bíblia**: algumas bíblias existem em versões antigas (de PS2/Wii ou de desenvolvimento) em que a mesma chave tem outro texto. As linhas do começo do arquivo valem para todas as cópias; depois de uma linha `[package\aabeaa37.p3d]` vêm só as chaves que nessa cópia têm texto diferente (`[]` volta para as linhas comuns). O `lang-init` já gera o arquivo assim.
- **Fonte**: as fontes do jogo (Titans_Large/Small) não desenham ã, Ã, Ê, Ô (são espaços vazios) nem têm õ/Õ. O runtime compõe essas letras de outras que a fonte tem, numa página nova da fonte (`glyphs = ["ã = a + ñ - n", ...]` no `language.toml`: o til do ñ menos o n, o circunflexo do Â menos o A). Na Titans_Small, o Ê e o Ô saíam sem o circunflexo (a célula do Â começa com um pedaço da letra da linha de cima, que era tomado pelo acento); corrigido. A tela SETTINGS usa as mesmas fontes, então o nome aparece como PORTUGUÊS (BR) e a ajuda do IDIOMA vem do `help` do `language.toml`.
- **Mods**: o pacote de idioma é aplicado por cima do que o jogo veria. Se um mod substitui um desses pacotes (por exemplo uma textura em `package\cdd70a8c.p3d`), as mudanças do mod ficam e só os textos em inglês das chaves traduzidas são trocados. Para as chaves que o pacote traduz, o pacote vence o mod.
- **Testes**: qualquer perfil com `--set 'env={CRASH_SETTINGS="game.language=pt-br"}'` (os antigos `ptbr` e `ptbr_novo` saíram do `run.toml`). Capturas em `docs\capturas\antigas\ptbr\`.
- **Outros idiomas**: qualquer pasta `languages\<id>\` com um `language.toml` vira mais uma opção na tela (base `english`, `french`, ... define as vozes). Pastas que começam com `_` são ignoradas.

### Mods de entradas de `.rcf`

O runtime aceita a substituição de um arquivo de dentro de um `.rcf` sem copiar o arquivo inteiro (`runtime/fs/rcf_overlay.cpp`):

```
mods\<mod>\rcf\<nome do .rcf sem extensão>\<caminho da entrada>
mods\meus_textos\rcf\default\package\7efdcd91.p3d   -> substitui package\7efdcd91.p3d do default.rcf
```

O jogo vê um `.rcf` virtual igual ao original, exceto que o diretório das entradas substituídas aponta para os dados novos (alinhados em 0x800). O arranjo é compacto: a entrada nova fica no lugar da original quando cabe; senão vai para o espaço liberado por outras entradas substituídas (as maiores primeiro); só o que sobra vai para depois do fim do arquivo. Vale o primeiro mod na ordem de carregamento. Só entradas que já existem podem ser substituídas, porque o diretório é indexado por um hash do nome. O log mostra cada substituição e onde ela ficou (`[fs] rcf overlay: D:\default.rcf package\... <- mod ... (in place / in freed space / appended at 0x...)`) e, no fim, o tamanho do arquivo virtual (`virtual size 2.98 GB ...`). O arquivo original nunca é alterado.

**Limite de tamanho**: o leitor de arquivos do jogo (`core::XenonDrive::Read`) posiciona o arquivo com `SetFilePointer(arquivo, offset, NULL, FILE_BEGIN)`, que trata o offset como número de 32 bits **com sinal**: sozinho, o jogo não lê nada depois de 2 GB (mostra o erro de "disco sujo" e trava). O runtime corrige isso (`runtime/fs/xenon_drive_hooks.cpp`: o mesmo `SetFilePointer`, com a parte alta zerada), então o limite agora é o do formato `.rcf`, com offsets de 32 bits **sem sinal**: 4 GB de arquivo virtual. Quando passa de 2 GB o log avisa (`WARNING: ... does not fit in 2 GB`); entradas que não cabem nem em 4 GB ficam de fora com um aviso, e o jogo recebe a original. Testado com 2,39 GB de entradas regravadas (arquivo virtual de 2,98 GB): o jogo chega à primeira fase.

Para ver e extrair o que há num `.rcf`:

```bat
python tools\rcf.py list dist\game\default.rcf "package\*"
python tools\rcf.py extract dist\game\default.rcf "script\*.lua" private\extracted
```

O `default.rcf` tem 2 904 entradas: 2 100 pacotes `.p3d`, 414 scripts `.lua` em texto, 351 `.blua` (Lua compilado), configurações e os vídeos `.bik`. Qualquer uma delas pode ser substituída por um mod do mesmo jeito.

## Legendas

O jogo original não tem legendas. O runtime desenha legendas nas **cenas** (as 22 cenas com voz, incluindo a abertura) e nas **falas avulsas** que o jogo toca durante a fase (dicas e piadas do Aku Aku, títulos dos episódios, vilões, créditos), em inglês ou em português (BR). Ligue em **SETTINGS > ÁUDIO > LEGENDAS** (o sub-grupo abaixo dos volumes; vale na hora); o tamanho e a opacidade da caixa também estão lá. No `settings.toml`: `subtitles = true`, `subtitle_size`, `subtitle_background`.

| Inglês | Português (BR) |
|---|---|
| `docs\capturas\antigas\legendas\en_*.jpg` | `docs\capturas\antigas\legendas\ptbr_*.jpg` |

As capturas `01`–`08` são falas da cena de abertura e `09`–`10` a primeira dica do Aku Aku na vila (subindo para não cobrir a dica escrita do jogo); `settings_legendas.png` mostra as opções na tela SETTINGS, e `load_game_depois.png` o LOAD GAME funcionando depois de ligar as legendas.

**De onde vem o texto.** As bíblias de texto do jogo não têm as falas (só menus, HUD, dicas escritas e os títulos dos episódios). Cada cena tem um único áudio 5.1 com as vozes no canal central (`english.rcf`, `vo_nis\6_channel\nisNN`), e cada fala avulsa é um clipe próprio. Por isso o texto em inglês é **transcrito das vozes**, uma vez, no seu PC, por um modelo local de reconhecimento de fala (Whisper large-v3 via [faster-whisper](https://github.com/SYSTRAN/faster-whisper)). O resultado é dado derivado do jogo: fica em `private\subtitles\` e não deve ser publicado.

```bat
:: 1. (uma vez) o ambiente do Whisper, separado do Python do sistema
:: (o ambiente antigo foi apagado na limpeza de 2026-10-21; refaça-o só para transcrever de novo)
python -m venv private\stt\venv
private\stt\venv\Scripts\python -m pip install faster-whisper
:: opcional, para usar a GPU NVIDIA (sem isso roda na CPU, mais devagar):
private\stt\venv\Scripts\python -m pip install nvidia-cublas-cu12 "nvidia-cudnn-cu12>=9,<10"

:: 2. Exporta os sons, se ainda não exportou (veja "Extraindo e modificando assets")
python tools\assets.py all

:: 3. Transcreve as cenas e as falas avulsas (~5 min numa RTX 5080; o modelo, ~3 GB, é baixado na primeira vez)
python tools\subtitles.py transcribe --dialogue

:: 4. Gera private\subtitles\english.txt (o make_dist copia para dist\data\subtitles; edite à vontade)
python tools\subtitles.py draft

:: conferir: lista as falas por som / compara a tradução com o inglês
python tools\subtitles.py list
python tools\subtitles.py check pt-br
```

- **Transcrição**: o canal central de cada cena é cortado nos silêncios, e cada trecho é transcrito separado (numa passada só, o modelo pulava falas). O `draft` tira ruídos que o modelo escreve como texto ("Unh!", "Thanks for watching!"), divide falas longas por frase, corrige a grafia dos nomes do jogo (Doominator, N. Gin...) e garante pelo menos 1,2 s por legenda. Os títulos dos episódios usam o texto do próprio jogo. A transcrição é boa, mas não é perfeita: algumas falas rápidas ou gritadas podem estar erradas; corrija direto no `english.txt` (o `draft` não sobrescreve sons que já estão no arquivo; `--force` refaz tudo).
- **Formato** (UTF-8; vale para o inglês e para as traduções):
  ```
  [nis01]                                         chave do som (nome do WAV exportado)
  sound = english sound\rsd\english\c\c268899e25ed5279.rsd   arquivo e entrada do .rsd
  seconds = 103.363                               duração do som
  7.26 9.62 Crash! Help me get this gizmo working!   início e fim (s no som) e texto; \n quebra a linha
  ```
- **Tradução**: `languages\<id>\subtitles.txt`, com as mesmas chaves e tempos. O pacote PT-BR (`languages\pt-br\subtitles.txt`) traduz todos os 858 sons (1 598 falas): as 22 cenas, o Aku Aku (títulos de episódio, dicas, tutorial, apresentações dos inimigos, piadas), Cortex, Nina, N. Gin, Uka Uka, Tiny, Crunch, os avisos de alto-falante e os créditos falados, seguindo o `GLOSSARIO.md` (nomes de pessoas nos créditos ficam como estão). Uma chave que a tradução não tem mostra o texto em inglês. As falas são legendadas no idioma do **texto**: com Português (BR) as vozes continuam em inglês e as legendas saem em português. Os outros idiomas do jogo (francês, alemão...) têm vozes próprias e ainda não têm transcrição: com eles a legenda fica desligada (dá para transcrever com `--model` e um `<idioma>.txt`, não feito).
- **Desenho**: texto branco com contorno preto e caixa translúcida, centralizado embaixo (nas cenas, logo acima da faixa preta; nas falas durante o jogo, um pouco mais alto, para não cobrir as dicas escritas do próprio jogo), quebrando em até duas linhas (82% da largura), na resolução da janela, com a fonte do Windows (Segoe UI), então todos os acentos aparecem. A legenda entra nas capturas (F12 e `screenshots`), o overlay de desempenho não.

**Sincronia.** O runtime reconhece cada som quando o jogo entrega os primeiros pacotes XMA dele ao decodificador: na abertura, ele lê o começo de cada `.rsd` legendado e guarda uma impressão digital (hash) dos primeiros pacotes de cada stream e de cada bloco de 64 KB, e o decodificador compara cada buffer novo com elas. A partir daí a legenda segue o relógio de áudio do próprio jogo (quadros enviados à saída de som), com o atraso medido entre o jogo receber o áudio e ele soar (~0,28 s, conferido contra a gravação do áudio da cena de abertura). Pausa e cena pulada: se o som para de ser decodificado, a legenda some; se o jogo passa a tocar outra coisa naquele canal, ela acaba. Não depende de nenhum código do jogo, então vale para qualquer cena ou fala que tenha texto no arquivo. Ajustes: `CRASH_SUBTITLE_OFFSET_MS` (atraso), `CRASH_SUBTITLE_TRACE=1` (loga cada som reconhecido e cada legenda), `CRASH_SUBTITLE_FILE=<arquivo>` (outro arquivo de legendas), `CRASH_SUBTITLE_TEST=<texto>` (mostra um texto fixo, para testar o visual).

**O que fica de fora**: os sons de combate e provocações dos inimigos comuns (labrat, monkey, kooala, voodoo, brat girl: milhares de grunhidos) não foram transcritos; os vídeos Bink (logos e o teatro de sombras da introdução) não passam pelo XMA e não têm legenda; um som legendado que um mod substitui (`mods\...\audio`) não é reconhecido.

**Testes**: `primeira_fase` com `--set 'env={CRASH_SETTINGS="game.subtitles=true"}'` (NEW GAME até a cena de abertura e a primeira dica do Aku Aku na vila; os antigos `legendas` e `legendas_ptbr` saíram do `run.toml`).

## Extraindo e modificando assets

Tudo o que os `.rcf` do jogo têm pode ser extraído para formatos abertos: sons, vozes e áudio das cenas em WAV, vídeos em MP4, texturas em PNG e modelos 3D com esqueleto e animações em glTF. Sons, texturas e modelos também voltam ao jogo por um mod. O que é extraído é dado do jogo: fica em `private\assets\` e **não deve ser publicado**. A pasta do jogo nunca é escrita.

A exportação é **um arquivo por conteúdo**. O jogo repete muita coisa: o mesmo Crash está copiado em centenas de pacotes de cena, as fases repetem texturas, e há gritos iguais nos seis idiomas. Cada imagem, malha ou som diferente sai uma vez só, numa pasta pela categoria (personagem, fase, objeto...). Um índice lista onde cada arquivo é usado. Ao montar o mod, a edição vai para **todos** os pacotes que usam aquele conteúdo.

As ferramentas de textura e modelo usam numpy e Pillow (`tools/requirements.txt`). O instalador os instala pelo pip no mesmo Python que roda o `setup.py` (no começo, quando ASSETS PARA MODDING está ligado, e de novo antes dessa etapa); se o pip falhar (sem internet, Python sem pip), só a etapa 9 falha, com o comando para rodar à mão, e o jogo fica instalado. `python tools\setup.py --game <pasta> --assets-only` (ou TENTAR DE NOVO no `CrashSetup.exe`) refaz só essa etapa.

```bat
:: Uma vez: os pacotes Python (numpy, Pillow)
python -m pip install -r tools\requirements.txt

:: Tudo de uma vez (extração incremental). ~8 min na primeira vez.
python tools\assets.py all

:: Ou por etapa
python tools\assets.py extract       :: entradas cruas de todos os .rcf + índice
python tools\audio.py export         :: .rsd -> WAV, um por som diferente
python tools\audio.py videos         :: .bik -> MP4 (+ um WAV por faixa de áudio)
python tools\textures.py export      :: texturas dos .p3d -> PNG, uma por imagem diferente
python tools\models.py export        :: malhas, esqueletos, skins e animações -> .glb

:: Procurar (por nome do arquivo, nome original no jogo ou pacote)
python tools\audio.py find nis01
python tools\textures.py find crash_normal
python tools\models.py find crash
python tools\assets.py status              :: contagens por arquivo/tipo
```

| Pasta (em `private\assets\`) | Conteúdo |
|---|---|
| `textures\<categoria>\<nome>.png` | **1 553 PNG**: 1 182 texturas, 285 sprites do menu montados e 86 tiles soltos, tiradas de 5 980 texturas/sprites em 1 763 pacotes. Categorias: `personagens\<quem>` (`crash`, `grizzly`...; o Crash montado num monstro fica na pasta do monstro), `fases\<fase>` (`L1_E1`...), `objetos`, `efeitos`, `interface` (menus e HUD), `cinematicas` (o que só as cenas usam) e `outros`. O que várias fases ou vários personagens usam vai para `fases\compartilhado` ou `personagens\compartilhado`. `indice.csv` traz, por arquivo, tamanho, mips, formato original, quantos usos, nomes no jogo e pacotes; `textures.json` tem os mesmos dados com cada uso detalhado |
| `models\<categoria>\<pacote>.glb` | **732 .glb** com as 13 443 malhas diferentes (16 594 contando as cópias), com texturas embutidas, cores de vértice, esqueleto e skin. Cada malha sai no pacote mais representativo que a tem: o do próprio personagem antes das cópias das cenas. Mesmas categorias das texturas |
| `models\animacoes\<esqueleto>.glb` | **58 .glb**, um por esqueleto (`crash.glb`, `crash_skel.glb` das cenas, `tiny.glb`...), com o modelo do personagem e todas as animações diferentes dele, de gameplay e de cena (967 animações; `crash.glb` tem as 313 do Crash, incluindo as dos monstros montados). `models\indice.csv` lista malhas e arquivos de animação; `models.json` é o índice para o `build` |
| `audio\vozes\<idioma>\<personagem>\*.wav` | vozes e diálogos, uma pasta por idioma (`english`, `french`, `german`, `spanish`, `italian`, `dutch`; ~6 750 cada). Os das cenas ficam em `vo_nis\` (5.1, voz no centro) |
| `audio\vozes\todos_idiomas\...` | 172 sons de voz idênticos nos seis idiomas (gritos, gemidos), guardados uma vez |
| `audio\sons\<pasta>\*.wav` | 831 efeitos e músicas (`music`, `enemies`, `props`, `ambience`, `nis`...), vindos de `sound1.rcf` e `sound2.rcf` juntos |
| `audio\nao_usados\...` | 57 sons de `crash_old_sounds`, `crash_temp_sounds` e `crash_temp_vo`: sobras com nomes de "antigo" ou "temporário", separadas para não misturar |
| `audio\indice.csv`, `audio\names.csv` | `indice.csv` tem uma linha por arquivo: canais, taxa, duração, as entradas dos `.rcf` que tocam esse som e os nomes originais. `names.csv` tem uma linha por entrada (42 560), com o arquivo exportado e o nome original. Ao todo são **41 550 WAV**: sons idênticos com nomes diferentes saem uma vez |
| `video\*.mp4` | os 4 vídeos Bink (1280x720, H.264 + AAC 5.1) e `*.trackN.wav` com cada faixa de áudio |
| `raw\<arquivo>\<entrada>`, `index.json`, `index.csv` | as 45 525 entradas dos 9 `.rcf` exatamente como estão (3,1 GB, nomes de hash; `assets.py verify` confere byte a byte) e o índice de entradas, com o nome original e os arquivos exportados de cada uma. É a fonte das outras ferramentas |

**Mips não são LODs.** No jogo, cada textura pode vir com *mipmaps*: cópias menores da mesma imagem (metade, um quarto... até 8 px) que a GPU usa em superfícies distantes, para não cintilar. A exportação antiga gravava cada nível como um PNG (`x.png`, `x_1.png` ... `x_5.png`). Agora sai só a imagem principal, e o `build` refaz os mips a partir dela. Isso é diferente do LOD de modelos (versões com menos polígonos para longe), que este jogo não tem.

**Uma imagem, vários formatos.** Muitas texturas de personagem existem em duas cópias quase iguais: uma de 256 cores (paleta) para o gameplay e uma de cores cheias para as cenas, às vezes com outro nome. A pele do Grizzly, por exemplo, é `crash_normal` no pacote do Crash montado e `C_grizzly` nas cenas. A exportação compara os pixels e junta essas cópias num arquivo só (coluna `versoes` do índice), exportado da melhor cópia. Ao editar, cada pacote recebe a imagem no formato que já usava. Imagens diferentes com o mesmo nome continuam separadas, com o tamanho ou o pacote no nome: `FE_Title_Logo (470x345).png`, `crash_normal (Characters_Crash_Koala).png`.

Os pacotes `.p3d` com nome de hash ganham o nome original pelo `hashdictionary.txt` do próprio `default.rcf` (`package\b06ecd92.p3d` = `package\Characters_Crash.p3d`). Scripts `.lua` e `.txt` já são texto (ficam em `raw\`); `.blua` é Lua 5.1 compilado.

**Como os formatos foram lidos** (detalhes no cabeçalho de cada ferramenta):

- **Som** (`.rsd`, "RSD6XMA "): cabeçalho de 0x800 bytes (canais, taxa, caminho original), formato XMA2 (streams, duração, blocos de 64 KB, tabela de busca) e pacotes XMA2. Arquivos 5.1 são 3 streams estéreo intercalados pelo campo *skip* dos pacotes. A decodificação é a mesma do runtime (`runtime/audio/xma.cpp`: quadro a quadro pelo WMA Pro do FFmpeg), feita por `tools/assettool`. Ele é escrito em C++, compila sozinho na primeira vez com o toolchain do `tools/buildenv.py` e usa as DLLs de `thirdparty\ffmpeg`. Dois sons são o mesmo quando os dados XMA são iguais byte a byte; comparar o áudio decodificado dá o mesmo resultado (41 550 nos dois casos).
- **Vídeo** (`.bik`, Bink 1): o FFmpeg incluído tem os decodificadores Bink, mas não o libavformat, então o `assettool` lê o contêiner Bink e grava o MP4 com um muxer próprio (H.264 via libopenh264, AAC).
- **Texturas**: no 360 as imagens dos `.p3d` **não** estão no formato da GPU (sem tiling Xenos). O chunk de imagem (0x19001) guarda um PNG comum (quase todas), um DDS (DXT1/3/5) ou um TGA com paleta, um por nível de mip, e o jogo converte ao carregar. As texturas de 256 cores são desenhadas com a **paleta do chunk 0x19006/0x19007 de mesmo nome**, não com a paleta do próprio arquivo. Outras paletas pintam os mesmos índices de outro jeito: `Crash_Player2.bmp` é o Crash do jogador 2 em `crash_normal.bmp`, os `*_jacked.bmp` são os monstros montados, e `x.tga.1..3` são paletas animadas.
- **Modelos**: Pure3D clássico em little-endian. Malha 0x10000 / skin 0x10001 → grupos 0x10020 com posições, normais, UVs, cores, índices (só listas de triângulos), matrix list, pesos e palette. Esqueleto 0x23000 (joints com pai e pose local 4x4). Shader 0x11000 → textura. Animações 0x121000 com canais `TRAN` (float3, ou 1/2 eixos + constantes) e `ROT` (quaternion comprimido: 3 × int16, w implícito). O Pure3D é mão esquerda; no glTF o z é invertido e cada triângulo sai com os vértices na ordem inversa (no jogo a frente é a face horária, como dizem as normais; o glTF usa a anti-horária). O `build` não lê os índices do .glb. `python tools\models.py preview x.glb` faz um render rápido (PNG) sem precisar do Blender.

### Colocando de volta (mods)

Copie o arquivo exportado para `dist\mods\<mod>\` com **o mesmo caminho** (`textures\...`, `models\...`, `audio\...`), edite e rode o `build` correspondente. Cada `build` escreve só os pacotes que mudam em `dist\mods\<mod>\rcf\<arquivo>\<entrada>` (o overlay de entradas da seção anterior); `--mods-dir` escolhe outra pasta de mods.

**Texturas**:

```bat
:: 1. Copie o PNG e edite
::    private\assets\textures\personagens\crash\crash_normal.png
::    -> mods\crash_azul\textures\personagens\crash\crash_normal.png
:: 2. Gera os pacotes
python tools\textures.py build --mod crash_azul
```

O PNG pode mudar de tamanho e ser RGB, RGBA ou com paleta. O `build` faz o seguinte:
- grava a imagem em todos os pacotes que usam aquele arquivo;
- refaz todos os níveis de mip a partir dela, cada um no formato original do nível;
- recorta de novo os tiles dos sprites, com a borda de 1 px que o jogo usa;
- regrava DDS como DXT e TGA com paleta como TGA.

Numa textura de 256 cores, o `build` gera uma paleta nova, usada por todos os níveis, e a grava também no chunk de paleta. As outras paletas da mesma textura (jogador 2, monstro montado) são remapeadas: cada índice novo recebe a cor que aquela paleta tinha no mesmo ponto da imagem. Por isso o Crash do jogador 2 continua com as cores dele. Sem edições, `textures.py verify` confere o índice e reconstrói os 1 763 pacotes **idênticos byte a byte**.

**Texturas HD sem regravar pacotes** (`tools\hd_textures.py`, `runtime/gpu/hd_textures.*`): o `textures.py build` põe a imagem nova dentro dos pacotes, então ela ocupa a memória do console emulado (o jogo reserva no boot dois blocos fixos, 135 MB + 368 MB) e uma textura usada por centenas de pacotes regrava todos eles. A alternativa é um pacote de texturas HD, que o runtime troca **só na placa de vídeo**: o jogo continua carregando e usando a textura original (memória do jogo igual), e o renderer liga a versão HD no lugar dela.

```bat
:: 1. (opcional) grava as texturas que o jogo usa: jogue com texture_dump = true
::    (settings.toml, [graphics]) e passe pelo que o pacote deve cobrir
:: 2. confere os dumps com as exportações
python tools\hd_textures.py match
:: 3. monta o pacote a partir de uma pasta com as imagens editadas (mesmos caminhos de private\assets\textures)
python tools\hd_textures.py build --mod meu_hd --src pasta\com\as\texturas
```

O `build` grava `mods\<mod>\hd_textures\<chave>.dds` (BC7 com todos os mips; `--format rgba8` sem compressão) e um `manifest.json`. A chave é um hash da textura como o jogo a tem na memória. Para as texturas PNG do jogo (quase todas: RGBA 8 bits, de cabeça para baixo e em BGRA na memória) ela é calculada direto da exportação, sem dump; o `match` confere essa previsão com todo dump (82 de 82 iguais). Texturas DXT ou guardadas menores pelo jogo precisam de dump. A imagem editada tem de ser maior que a do jogo e com a mesma proporção. O jogo continua usando o tamanho original para as coordenadas (UV normalizado). Render targets e cópias de resolve nunca são trocados. Cada troca aparece no log (`[gpu] hd texture <chave>: 256x256 -> 1024x1024 ...`), e `hd_textures = false` em `[graphics]` desliga tudo.

Testado em 2026-10-21 com as 170 texturas do level 1 ampliadas pelo 4x-UltraSharp (`private\remaster_l1\mods_hd`, pasta já apagada, até 2048 px, 87 MB em BC7): 48 trocas na abertura e no começo da vila, sem mudança na memória do jogo e com o mesmo FPS.

Qual arquivo editar: o jogo procura texturas pelo nome, e um mesmo personagem pode ter artes diferentes em gameplay e em cena. O Crash de gameplay é `personagens\crash\crash_normal.png`. O das cenas é `cinematicas\Crash_512.png`, e `personagens\crash\Crash_V2.png` está em 582 pacotes de cena e no `PermanentContent`. Use `textures.py find` e a coluna `pacotes` do índice.

Testado no jogo com o mod `private\assets\test_mods\textura\teste_textura` (6 imagens, 590 pacotes):
- logo da tela de título editado: `docs\capturas\assets\textura_menu_logo.jpg`;
- Crash de gameplay azul, textura de 256 cores com a paleta nova: `docs\capturas\assets\textura_crash_gameplay_zoom.jpg`, antes e depois;
- chão da vila com xadrez magenta (`grass_10_512` e `drt_11_512`, 7 níveis de mip cada): o xadrez aparece igual perto e longe, então os mips foram refeitos (`docs\capturas\assets\textura_fase_crash_azul_chao_xadrez.jpg`);
- `Crash_V2.png` gravado nos 582 usos, com os níveis 1 a 5 refeitos e conferidos fora do jogo.

**Modelos** (mesma topologia):

```bat
:: 1. Importe o .glb no Blender (File > Import > glTF 2.0), mova/esculpa vértices ou edite UVs
::    (sem criar nem apagar vértices ou faces) e exporte glTF 2.0 (.glb) mantendo os dois mapas UV
::    em mods\<mod>\models\personagens\crash\Characters_Crash.glb
:: 2. Gera os pacotes
python tools\models.py build --mod meu_crash
```

O segundo mapa UV (`TEXCOORD_1`) de cada vértice exportado guarda (índice do vértice, índice do grupo). É por ele que o `build` devolve cada posição, normal e UV ao seu lugar, mesmo que o Blender reordene ou duplique vértices na exportação. A edição vai para todos os pacotes que têm a mesma malha. Índices, skin, materiais e o resto do pacote não mudam; caixas e esferas envolventes são recalculadas. Testes:
- no jogo: Crash de gameplay com a cabeça 1,6× maior, mod `test_mods\modelo\teste_modelo`; o pacote gerado é idêntico ao do teste anterior, `docs\capturas\antigas\assets_modelo_comparacao.jpg`;
- fora do jogo: a língua do Crash das cenas (`crash_tongueShape`, em 570 pacotes) aumentada 1,25×; nos 570 pacotes só ela mudou, com a escala exata.

**Modelos com outra topologia** (subdividir, refazer a malha, criar faces): no Blender, apague o segundo mapa UV da malha (Object Data > UV Maps) e exporte como acima. Sem o `TEXCOORD_1`, o `build` reconstrói os grupos de primitivas da malha a partir do glTF:
- posições, normais, UVs, cores e triângulos do glTF; nos skins, `JOINTS_0`/`WEIGHTS_0` contra o mesmo esqueleto (os ossos são achados pelo nome; ficam as 4 influências mais fortes);
- cada primitiva vai para o grupo do shader com o nome do material (o nome que a exportação dá); se os nomes não baterem e o número de primitivas for o de grupos, vale a ordem. Um shader sem primitiva fica como estava;
- um grupo com mais de 65 535 vértices ou mais de 51 ossos (a maior paleta do jogo; o skinning é feito na CPU) é dividido em vários;
- o que o glTF não traz (normais, UVs, cores, pesos), o código de normal por vértice (chunk 0x10010, um byte que aponta numa tabela de 256 direções) e os blend shapes dos rostos das cenas (0x121305: deslocamento de posição ou UV por vértice, por alvo) vêm do ponto mais próximo da superfície original. Uma malha com blend shapes precisa caber num grupo só, e blend shapes de cor (`CLR0`) não são transferidos (a malha não é trocada).

Uma malha que ainda tem o segundo mapa UV segue o caminho da mesma topologia, byte a byte como antes. Testado fora do jogo (mod `subdiv_test`, Catmull-Clark nível 1 no Blender): `CrashShape` (1 995 → 15 021 vértices), Aku Aku, `mojoSShape` e, no `nis_01`, coco, crunch e akuaku (com blend shapes) e o dirigível do Cortex (6 grupos, 48 511 vértices), em 39 pacotes. Os pacotes foram lidos de volta: as contagens, os índices, as paletas e os blend shapes estão coerentes, e nada fora dessas malhas mudou. Com poses aleatórias do esqueleto e com cada blend shape aplicado, a superfície nova fica a 0,1% (média) do tamanho da malha da original, a mesma distância que em repouso. O build leva ~35 s.

**Texturas e modelos no mesmo mod**: rode `textures.py build --mod X` e depois `models.py build --mod X --chain`. Com `--chain`, o pacote que já está em `mods\X\rcf` vira a base: as malhas e skins dele voltam às do jogo e recebem as edições dos modelos, e o resto (as texturas) fica como está. Rodar de novo dá os mesmos bytes. Sem `--chain`, o `build` parte dos pacotes do jogo e sobrescreve o que houver. Testado com o mod de texturas `remaster_l1_personagens` (683 pacotes) e os 8 .glb subdivididos do `remaster_l1_modelos`: nos 77 pacotes com as duas edições, as malhas são as do build de modelos e todo o resto é do build de texturas.

**Sons** (WAV ou Ogg Vorbis, qualquer taxa e número de canais):

```bat
:: 1. Copie o som exportado para o mod, com o mesmo caminho, e edite (ou troque por outro arquivo)
::    private\assets\audio\sons\music\crash_idea43_part3_wave1.wav
::    -> mods\minha_musica\audio\sons\music\crash_idea43_part3_wave1.wav   (ou .ogg)
:: 2. Gera os .rsd substitutos e mods\minha_musica\audio\overrides.txt
python tools\audio.py build --mod minha_musica
```

Não existe encoder XMA aberto, então o `.rsd` substituto leva "quadros marcadores" no lugar do áudio. O decodificador XMA do runtime, ao encontrá-los, toca as amostras do arquivo solto, convertido para os canais e a taxa do original. Vale para qualquer som: efeitos, vozes, músicas e áudio 5.1 das cenas. Um arquivo de `vozes\todos_idiomas\` ou um som com vários nomes gera um `.rsd` para cada entrada que o usa. Evite um som muito mais longo que o original (o jogo pode guardar a duração em outro lugar). Detalhes em `runtime/NOTES.md`, "Mods de áudio". Testado no jogo: a música do menu trocada por um tom de teste (440/660 Hz no `audio.wav` do perfil `menu`), em `docs\capturas\assets\audio_mod_menu.wav`.

**O que ainda não volta**: malhas com topologia nova (vértices ou faces a mais ou a menos, outro esqueleto ou outros pesos), animações editadas, vídeos (seria preciso um encoder Bink) e os `.blua`. Os `.lua` em texto, os `.txt` e qualquer entrada crua podem ser substituídos direto pelo overlay de `.rcf`. Também não saem as animações de objetos rígidos das cenas e dos efeitos (portas, plataformas, peças de efeito: 340 animações cujo esqueleto não tem skin) nem outros chunks dos `.p3d` (sombras, morphs 0x121305, partículas, câmeras).

### HD AI Remaster

Opção **HD AI REMASTER** da aba DEFINITIVE EDITION (`hd_remaster` em `[graphics]` do `settings.toml`, onde já estava), **desligada por padrão**. Mudá-la reinicia o jogo (a tela pergunta ao sair; veja "Aplicar sem fechar o jogo"). Ligada, ela carrega no boot o mod de remaster por IA da pasta `data\` ao lado do exe (`dist\data\remaster_full`, que o `make_dist` liga por hard link a `private\remaster_full`; o exe do build usa `private\` direto). Esse mod vem do jogo e não deve ser distribuído, por isso **não** fica em `mods\`. O runtime procura, nesta ordem:
1. **o jogo todo** (`<data>\remaster_full\mods\remaster_full`, montado em 2026-10-21; detalhes, riscos e como testar em `private\remaster_full\README.md`): 919 texturas de todos os episódios (o pacote HD e as de 256 cores) e os modelos de todas as fases, personagens, props e cenas subdivididos. A ajuda da linha diz "(jogo todo)";
2. **a fase 1** (`<data>\remaster_l1\mods_final\remaster_l1_final`), se o completo não existir. Essa pasta foi apagada na limpeza de 2026-10-21 (o completo a substitui); o runtime ainda a aceita se ela voltar. O `make_dist` só põe em `data\` o que o runtime escolheria. A ajuda diz "(fase 1)".

O mod da fase 1 tem três partes (o completo tem as mesmas):
- `hd_textures\`: 133 texturas da fase (mapa, casa, chão, pedras, plantas, água, céu, objetos, elenco da abertura, dirigível) em 4x-UltraSharp até 2048 px, trocadas só na placa de vídeo (veja "Texturas HD sem regravar pacotes");
- `rcf\`: as texturas de 256 cores (Crash, Mojos, rato de laboratório...) e os modelos subdivididos (personagens, salas e props), pelo overlay de `.rcf` (700 entradas, arquivo virtual de 2,26 GB);
- `textures\` e `models\`: a fonte dos dois builds (o runtime não lê).

Desligada, nada do mod é aberto: o jogo é o mesmo de antes (o log não tem nenhuma linha `[remaster]`). Ligada, o log mostra `[remaster] HD AI Remaster on: mod ...`, o mod aparece na lista `[fs]` (depois dos mods de `mods\`, que têm prioridade) e as trocas aparecem como `[gpu] hd texture ...`. As texturas HD do remaster valem mesmo com `hd_textures = false` (essa chave, sem linha nas telas, só desliga os pacotes dos outros mods). Sem os arquivos (outro PC, pasta apagada), a linha mostra INDISPONÍVEL com uma explicação, e com `hd_remaster = true` o jogo roda sem o mod (`[remaster] ... has no remaster data: playing without it`). `CRASH_HD_REMASTER_DIR=<pasta>` aponta para outra cópia do mod (testes).

Não há níveis na opção. As versões anteriores da fase 1, só por pacotes (`mods_minimo`/`mods_personagens`/`mods_completo`), foram apagadas na limpeza de 2026-10-21. Não existe variante sem modelos: os modelos são gravados (`--chain`) nos mesmos pacotes das texturas de 256 cores. Um mod com `remaster` no nome dentro de `mods\` é carregado sempre (o log avisa com `[remaster] WARNING`); para testar outra pasta use `CRASH_HD_REMASTER_DIR=<pasta>` ou `--set mods_dir=<pasta>`.

Capturas em `docs\capturas\hd_remaster\`: a linha na tela em EN e PT-BR (`settings_*_jogo_*.jpg` no jogo, `settings_*_previa_*.jpg` pelo `--settings-preview`, `settings_*_sem_dados_indisponivel.jpg`), a fase 1 desligado × ligado no mesmo quadro (`fase1_<quadro>_desligado_vs_ligado.jpg`, perfil `primeira_fase` com `--set 'env={CRASH_SETTINGS="graphics.hd_remaster=true"}'`) e a comparação com a captura da sessão do remaster (`fase1_*_referencia_vs_setting.jpg`). Testado em 2026-10-21: 67 texturas HD trocadas na abertura e na vila, 30,2 fps contra 30,6 desligado, sem falhas.

### Iluminação (remaster)

Um remaster da iluminação **por pós-processamento da cena 3D**, sem mexer nos dados nem na jogabilidade, ao lado do HD AI Remaster (texturas e modelos). Fica no sub-grupo **ILUMINAÇÃO (REMASTER)** da aba DEFINITIVE EDITION (`[lighting]` no `settings.toml`), **tudo desligado por padrão**, e vale na hora. **F8** desliga e religa tudo durante o jogo (sem gravar nada) para comparar antes e depois; um aviso `ILUMINACAO REMASTER: LIGADA/DESLIGADA (F8)` aparece no canto.

Onde entra: no mesmo ponto do DLSS/FSR (veja "Desempenho" e `runtime/NOTES.md`, passo 20): depois que as três faixas do tiling predicado terminaram a cena e antes do primeiro desenho 2D que lê a cor da cena, ou seja, **antes do HUD, do motion blur e da profundidade de campo do jogo**, na resolução da cena e antes do upscaler (que então acumula o resultado como qualquer outro detalhe da cena). Como trabalha no quadro inteiro já montado, não há emenda nas faixas x = 416 / 832. Menus, logos, tela de carregamento e o HUD não passam por ele (o menu principal e o HUB de episódios não têm cena 3D). As cenas (cutscenes) são cenas 3D do motor e recebem o efeito do mesmo jeito.

| Efeito | Como funciona |
|---|---|
| **SSAO** | Oclusão por *bitmask de visibilidade* (Therrien et al. 2023): por pixel, 2–3 fatias da semiesfera em direções girando por pixel e por quadro, marcando quais dos 32 setores a profundidade esconde. A normal vem da própria profundidade. Acumulado no tempo com os motion vectors (camera + mapa de velocidade do jogo) e filtrado (bilateral por profundidade e normal). **BAIXA**: meia resolução, 2 fatias × 6 passos; **ALTA**: resolução cheia até 1440 linhas (acima disso, em 4K, meia resolução com 3 fatias × 8 passos) e upsampling bilateral. O resultado multiplica a cor da cena, com proteção para pixels muito claros (céu, emissivos, sol numa parede), sem efeito no céu e sumindo com a distância (60–90 unidades) |
| **Luz indireta** | No mesmo passo do SSAO: cada setor que um ponto próximo esconde pela primeira vez traz a cor daquele ponto (um rebote de luz; a grama tinge a base das plantas, o Crash tinge o chão perto dele). Soma-se à cor da cena (cor × luz indireta × intensidade), só perto de geometria: áreas abertas não mudam |
| **Sombras de contato** | Um raio curto (1,2 unidade) **para cima** em cada pixel, na profundidade da cena: escurece o chão sob personagens, props e beiradas. É vertical de propósito: as sombras do jogo são volumes de stencil extrudados na CPU a partir de uma luz que nenhuma constante de shader traz, e as luzes direcionais dos shaders iluminados (`LightDirection0..3`) ficam desligadas na maioria dos desenhos e não batem com essas sombras (um raio na direção delas desenhava a sombra do lado oposto ao da sombra do jogo) |
| **Sombras suaves** | Substitui o filtro de 9 amostras da sombra do jogo (`shadowshaderfp`, o mesmo patch da "Suavidade das sombras") por um disco girado de 16 amostras, 1,35× mais largo, cada uma testando 4 texels do stencil como o original. A sombra fica com a mesma força e borda mais macia |

Para não sujar a arte: a soma de AO e sombra de contato nunca escurece um pixel abaixo de 50% do original; frestas de 1–2 pixels entre malhas (as emendas do terreno da vila) são preenchidas antes do AO; degraus muito baixos (decalques, emendas) não contam como oclusores.

**Custo** (RTX 5080 + Ryzen 7 5700X, vila, perfil `desempenho_vila_direto`, FPS sem limite, sozinho na máquina; "GPU" é o tempo dos passes medido com timestamps, linha `[lighting] N frames, X ms of GPU per frame` do log):

| Configuração | Desligado | ALTA + luz indireta + contato + suaves | BAIXA + o resto |
|---|---|---|---|
| 1920x1080 nativo | 175 fps | 168 fps (0,75 ms de GPU) | 178 fps (0,17 ms) |
| 3840x2160 nativo | 148–153 fps | 119 fps (1,09 ms; antes do limite de 1440 linhas: 91 fps, 3,8 ms) | 122 fps (0,83 ms) |
| 3840x2160 DLSS desempenho (cena 1920x1080) | 150 fps | 131 fps (0,85 ms) | — |

Sombras suaves sozinhas em 4K: 148 → 141 fps. **Custo de CPU: zero na prática** (o thread do command processor, o gargalo do jogo, ficou em 2,88 × 2,83 ms/quadro a 1080p): são 6 passes de tela cheia gravados uma vez por quadro, sem nada por desenho do jogo.

**Opção (c), shadow maps de verdade**: não implementado. Exigiria redesenhar a geometria da cena a partir da luz: guardar, para cada desenho opaco da cena (~1.600 por quadro na vila, ×3 por causa das faixas), os buffers de vértice/índice e as constantes, gerar uma variante só-profundidade de cada vertex shader traduzido com a matriz da luz no lugar de c4–c7 (os personagens usam shaders de skinning à parte), e repetir esses desenhos num mapa de sombra. Dois problemas de fundo: (1) **não existe luz confiável** para projetar (veja acima: a luz das sombras do jogo só existe na CPU), e as sombras novas brigariam com as de stencil e com a iluminação já "assada" nas cores dos vértices; (2) **dobraria o trabalho do thread do command processor**, que é exatamente o gargalo do jogo. Estimativa: uma a duas semanas, com perda de FPS na CPU. O SSAO + contato + sombras suaves dão o "aterramento" esperado sem esse custo.

**Diagnóstico**: `CRASH_LIGHTING_SHOW=ao | gi | shadow | split` mostra o termo de AO, a luz indireta, as sombras de contato ou a tela dividida (esquerda original, direita remaster); `CRASH_LIGHTING_TUNE="radius=1.6;thickness=0.5;gi=3;shadow_length=1.2;shadow=0.5;fade=90"` ajusta os parâmetros sem recompilar; `CRASH_LIGHTING_NO_TEMPORAL=1` desliga a acumulação. Perfis: `iluminacao` (vila, capturas paradas e andando; compare com `--set 'env={ CRASH_SETTINGS = "lighting.ssao=high;lighting.gi=true;lighting.contact_shadows=true;lighting.soft_shadows=true" }'`) e `iluminacao_tela` (liga tudo pela tela SETTINGS com o controle, salva e entra na fase).

Capturas em `docs\capturas\iluminacao\` (antes × depois na vila em detalhe, BAIXA × ALTA, os três termos separados, episódio 2, a cena de abertura e DLSS Qualidade 1440p em tela dividida, a tela em EN e PT-BR); o conjunto completo, com todas as posições e o `execucoes.txt` (pasta de cada execução), em `build\iluminacao\`. Verificado: vila (L1_E1), episódio 2 (save do slot 1), cena de abertura, menu principal e HUB sem mudança, DLSS e FSR ligados e desligados, sem emendas de faixa, e estável parado (diferença média de 0,05/255 entre quadros seguidos no termo de AO).

## Endereços encontrados

| Símbolo | Endereço | Observação |
|---|---|---|
| `__savegprlr_14` / `__restgprlr_14` | `0x823CEBE0` / `0x823CEC30` | padrão de bytes, 1 ocorrência cada |
| `__savefpr_14` / `__restfpr_14` | `0x823CFED0` / `0x823CFF1C` | padrão de bytes, 1 ocorrência cada |
| `__savevmx_*` / `__restvmx_*` | — | não existem neste binário |
| `longjmp` | `0x823D2EA0` | restaura f14–f31, r13–r31, LR, CR, r1; chama `RtlUnwind` se `jmp_buf+0x138` ≠ 0 |
| `setjmp` | `0x823D2FC0` | função folha sem `.pdata`, mesmo layout do `jmp_buf` |

O `Crash.toml` também declara 68 funções explícitas: funções folha sem entrada em `.pdata` que a análise linear cortava no meio, deixando as jump tables "fora da função". Cada faixa explícita termina antes da próxima função que só é alcançada por ponteiro (veja a próxima seção).

## Funções engolidas (`tools/find_swallowed.py`)

O XenonRecomp só gera uma função (e uma entrada na tabela de chamadas indiretas) para endereços que ele conhece como início: entradas do `.pdata`, alvos de `bl`, funções explícitas do config e o que a varredura linear acha depois do fim da função anterior. Uma função folha pequena que só é chamada por ponteiro (vtable, tabela de callbacks nos dados ou endereço montado no código com `lis`/`addi`) e que fica logo depois de uma faixa explícita grande demais some dentro dela. Em tempo de execução isso vira `[fatal] indirect call to unmapped guest address ...`. Foi o caso de `0x82289948` (passo 8 do `runtime/NOTES.md`) e de `0x8210EB98` (passo 16).

O script procura todos os casos de uma vez:

```bat
python tools\find_swallowed.py --suggest
```

1. Junta os candidatos a ponto de entrada: palavras de 32 bits alinhadas nas seções de dados que apontam para código, endereços montados no código com `lis rX` + `addi`/`ori` (acompanhando quais registradores são sobrescritos no caminho) e alvos de `bl`.
2. Lê as funções que o XenonRecomp gerou de verdade (`generated/ppc/ppc_func_mapping.cpp`, com o tamanho contado pelos comentários de instrução em `ppc_recomp.*.cpp`) e as faixas explícitas do config.
3. Classifica cada candidato que não é início de função: **swallowed** (a palavra anterior é `blr`/`bctr`/`b`/padding e nenhum desvio da função que o contém aponta para ele: é uma função escondida), **label** (alvo de desvio interno ou label de `Crash_switch_tables.toml`), **table** (a própria jump table, logo depois do `bctr`), **fallthru** (a instrução anterior cai nele: em geral tabelas de escopo de SEH/C++ EH) e **gap** (fora de qualquer função). Só `swallowed` precisa de correção; `--verbose` lista o resto.
4. Com `--suggest`, imprime as faixas novas: divide a faixa que contém cada entrada, acrescenta como entrada também os alvos de `b` (tail calls) que passariam a cair numa faixa vizinha e confere que a base, os labels e o default de cada jump table continuam na mesma faixa.

O código de saída é 1 quando há algum `swallowed`, então dá para usar como verificação depois de mexer no config e regenerar. Hoje o resultado é `swallowed: 0`. Os casos encontrados e corrigidos na primeira execução foram `0x8210EB98` (em `0x8210EB08`), `0x82134778` (em `0x82134760`, apontada por uma vtable em `0x82013BE8`), `0x822D4D28` e `0x822D4D50` (em `0x822D4928`, callbacks guardados em estruturas), mais `0x822D4B88`, a cauda comum que essas duas chamam com `b`.

O runtime faz uma versão mais simples da mesma checagem ao iniciar (só ponteiros nos dados): `[loader] N data words point into code; M point at unmapped code after blr/b/padding`. Se uma chamada indireta cair num endereço sem função, o `[fatal]` diz em qual função gerada o endereço está e se ele vem logo depois de `blr`/`b`/padding.

Depois de mudar o config:

```bat
tools\XenonRecomp\build\XenonRecomp\XenonRecomp.exe config\Crash.toml tools\XenonRecomp\XenonUtils\ppc_context.h
python tools\find_swallowed.py
cmake --build build
```

A saída do XenonRecomp (`private\recomp.log`) deve ter 0 `Unrecognized instruction` e 0 `Switch case ... outside function`.

## Lendo o código do jogo

As 20 144 funções geradas se chamam `sub_XXXXXXXX`. O primeiro passo para ler a lógica do jogo é dar nome a elas. O executável foi compilado com MSVC e guarda o **RTTI** de cada classe polimórfica (o que o `typeid`/`dynamic_cast` usa): nome da classe, hierarquia e, por tabela, as vtables. `tools/rtti_scan.py` lê essas estruturas (big-endian, ponteiros absolutos de 32 bits) e dá nome a toda função que aparece numa vtable.

```bat
python tools\rtti_scan.py
```

Roda em ~1 s e escreve, em `private\symbols\` (são dados derivados do jogo: não publique):

- `rtti.json`: as classes (nome, nome decorado, `TypeDescriptor`, bases diretas e todas as bases com deslocamentos), as vtables de cada classe (endereço, COL, deslocamento do subobjeto) com cada slot, e, por função, a lista de classes/vtables/slots que a usam;
- `functions.csv`: `address,name,source,note`, uma linha por função com nome.

Como os nomes são formados:

| `source` | Nome | Quando |
|---|---|---|
| `rtti` | `CLifeDisplay::vf03` | a função só aparece nas vtables de uma classe; `vfNN` é o slot (deslocamento `4*NN` na vtable). Numa vtable secundária (herança múltipla): `Classe::Base::vfNN` |
| `rtti-inherited` | `CFrontendElement::vf05` | várias classes usam e todas derivam de uma delas: o slot é da base, as outras herdam. A nota diz quantas herdam, ou se a própria base tem outra função no slot (a implementação vem de uma classe intermediária sem RTTI, por exemplo o `AddRef`/`Release` de `core::IRefCount`) |
| `rtti-shared` | `nullsub`, `return_0`, `_purecall` | classes sem base comum: corpos idênticos que o linker juntou (`blr`, `li r3,N; blr`) ou o `_purecall`. Corpos não triviais levam o nome da primeira classe e a nota diz quantas classes usam |
| `rtti-dtor` | ``CLifeDisplay::`scalar deleting destructor'`` | o slot testa `flags & 1` (r4) e chama `operator delete` (`vector deleting destructor` se também testa `& 2`) |
| `heuristic` | `CLifeDisplay::~CLifeDisplay`, `operator delete`, `_amsg_exit` | achados a partir dos anteriores: a única chamada antes do teste no deleting destructor é o destrutor da classe (quando várias classes chamam o mesmo, é o da base comum mais derivada, porque os destrutores derivados foram inline); a chamada depois do teste é o `operator delete` |
| `import` | `XamShowSigninUI` | thunks de import de `private\symbols.txt` |

Quando dois endereços terminariam com o mesmo nome, o que é dono do slot (ou o mais usado) fica com ele e os outros ganham o endereço no fim (`CFrontendElement::vf06_821E2F08`). A desdecoração usa `llvm-undname` se existir, senão o `undname.exe` do Visual Studio (o LLVM instalado aqui não traz o `llvm-undname`); sem nenhum dos dois, só os nomes sem template saem legíveis.

Cobertura atual: 2 300 `TypeDescriptor`s (2 186 classes com vtable, 40 com herança múltipla, nenhuma com herança virtual), 2 247 vtables com 15 686 entradas e 5 007 funções distintas. Com nome: **5 608 das 20 144 funções recompiladas (27,8%)** mais os 152 imports, sendo 3 324 `rtti`, 512 `rtti-inherited`, 1 088 `rtti-dtor`, 83 `rtti-shared` e 601 `heuristic`. Os namespaces com mais classes são `script` (698), global (626), `pure3d` (302), `fight` (223), `audio` (138), `om` (123), `core` (71) e `content` (62). As funções que não são virtuais (código chamado só com `bl`) continuam `sub_XXXXXXXX`; os próximos passos são propagar nomes por chamadas e strings e decompilar no Ghidra.

Onde os nomes aparecem:

- **Runtime**: `runtime/symbols.cpp` carrega `<data>\symbols\functions.csv` ao iniciar (`dist\data\symbols`, ou `private\symbols` no exe do build) (`[symbols] 5760 function names ...`) e os relatórios `[fatal]`, `[crash]` e `[watchdog]` passam a mostrar `CLifeDisplay::vf03+0x1C (sub_821E2C78+0x1C)`, com um backtrace guest simbolizado (`guest lr in ...`, `#0 ...`). Sem o arquivo tudo continua funcionando, só com `sub_`. `CRASH_SYMBOLS=<arquivo>` usa outro CSV e `CRASH_SYMBOLS=0` desliga.
- **Cópia anotada do código gerado**: `python tools\annotate_ppc.py` copia `generated\ppc` para `private\ppc_annotated` com `// CLifeDisplay::vf03  [rtti]` acima de cada `PPC_FUNC_IMPL` com nome e `// Nome` no fim de cada chamada direta `sub_XXXXXXXX(ctx, base);`. O `generated\ppc` compilado não muda.
- **Ghidra** (XEX aberto com o [XEXLoaderWV](https://github.com/zeroKilo/XEXLoaderWV), base `0x82000000`): depois da análise automática, rode `tools\ghidra_apply_symbols.py` no Script Manager e escolha `private\symbols\functions.csv`. Ele cria a função se faltar, põe cada método no namespace/classe da sua classe, adiciona um comentário com a origem do nome e, com o `rtti.json` ao lado, rotula vtables, COLs e `TypeDescriptor`s. Nomes que você deu à mão são mantidos. Headless: `analyzeHeadless <projeto> <nome> -process default.xex -postScript ghidra_apply_symbols.py <caminho\functions.csv>`.
- **IDA**: `File > Script file...` → `tools\ida_apply_symbols.py` (IDAPython), mesmo CSV. O IDA troca os caracteres inválidos por `_`; o nome original vai no comentário da função.

Depois de regenerar o código (config novo, XenonRecomp atualizado) rode `rtti_scan.py` de novo: ele usa `generated\ppc\ppc_func_mapping.cpp` para saber onde cada função termina e avisa se alguma entrada de vtable não tem função gerada (função engolida, veja a seção anterior).

## Correções locais no XenonRecomp

1. **`XenonUtils/xex.cpp`**: com compressão básica, o buffer da imagem era menor que `security->imageSize`, causando leitura fora dos limites. Agora aloca o maior dos dois.
2. **`XenonAnalyse/function.cpp`**: em `bclr`/`bcctr` condicional, `SearchBlock` recebia um endereço relativo em vez de absoluto. O bloco nunca era encontrado e a análise entrava em loop infinito, consumindo memória (travava em `0x8237FF6C`).
3. **`XenonRecomp/recompiler.cpp`**:
   - pula funções de tamanho zero em vez de entrar em loop;
   - variantes "LRU" de VMX (`lvxl`, `stvlxl`, … e suas formas `128`) usam os mesmos handlers das instruções base;
   - novos handlers: `vandc`, `dcbst` (no-op), `fcmpo` (= `fcmpu`), `frsqrte`, `mulhdu`, `mcrf`;
   - o `switch` das jump tables usa `.u32` em vez de `.u64` (o jogo limita e escala o índice com operações de 32 bits, então a metade de cima do registrador pode ser lixo, por exemplo −1 + 1 = `0x1_00000000`);
   - marcadores de progresso `[phase]`/`[analyse]` no stderr.

**Opções de otimização** (2026-10-22, `config/Crash.toml`, sem mudança no código do XenonRecomp): `skip_lr`, `skip_msr`, `ctr_as_local`, `xer_as_local`, `reserved_as_local` e `cr_as_local` ligadas (esses registradores viram variáveis locais do C++; o `generated\ppc` foi regenerado e o anterior está em `backups\generated_ppc_2026-10-22_sem_otimizacoes`, com o `Crash.toml` de antes). O runtime lê LR/CTR só em logs (`GuestLR`/`GuestCTR`, que dão 0). `non_argument_as_local` e `non_volatile_as_local` **não** podem ser ligadas: os blocos `__finally` do SEH recebem o quadro do pai em r12 e usam os registradores não voláteis dele, e o jogo quebra na inicialização do CRT (`runtime/NOTES.md`, "Desempenho de CPU").

## Próximas etapas

O histórico de cada etapa está em `runtime/NOTES.md` (o começo tem um resumo do estado atual). O que falta, em resumo:

1. GPU: cache de render targets com EDRAM real (aliasing geral entre formatos e pitches; hoje só os clears por profundidade e as superfícies 4x são tratados), cubemaps e resolves escritos na memória guest.
2. Áudio: interrupções do XMA, subframes e os vídeos Bink com legenda.
3. Câmera livre: paredes invisíveis e culling pensados para as câmeras fixas.
4. Código legível: propagar nomes pelas chamadas e strings, decompilar no Ghidra e reescrever em C++ as partes de interesse (base de uma API de mods).
5. Testes: recriar perfis de regressão para gráficos, legendas e PT-BR sobre a base `tests\userdata_base`.
