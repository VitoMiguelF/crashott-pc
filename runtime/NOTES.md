# Runtime: notas

O runtime (`runtime/`) transforma o `CrashPPC.lib` (código recompilado) em `CrashRecomp.exe` e executa `_xstart`, o entry point do XEX (`0x82264F40`). Este arquivo é o histórico técnico, passo a passo; o README tem o uso.

## Estado atual (2026-10-21)

- **Jogo**: roda do boot ao menu, LOAD/NEW GAME, cenas e fases, com áudio XMA (FFmpeg WMA Pro), saves em `userdata\content`, 30–120+ fps.
- **Pasta para jogar**: `dist\` (`tools/make_dist.py`): `CrashLauncher.exe` (launcher com a arte do jogo, "Launcher" abaixo), exe + DLLs, `game\`, `mods\`, `userdata\`, `languages\`, `data\`. Nenhum caminho é embutido no build: `paths.cpp` resolve tudo a partir da pasta do exe (veja "Caminhos" abaixo), e a imagem é descriptografada do `default.xex` da pasta do jogo a cada abertura (`xex_image/`), sem `image.bin`.
- **Renderer D3D12** (passos 10, 16, 20): shaders traduzidos do microcode, resolves, tiling predicado, motion blur, DLSS/DLAA e FSR 3.1, FXAA/SMAA, mipmaps e anisotrópico, overlay de desempenho, texturas HD sem regravar pacotes.
- **PC**: tela SETTINGS nativa com as abas JOGO, ÁUDIO, VÍDEO, GRÁFICOS, CONTROLES e DEFINITIVE EDITION ("Abas da tela SETTINGS"); opções aplicadas na hora, ao sair da tela ou com reinício automático ("Aplicar as opções sem fechar o jogo"); menu de PC com SAIR DO JOGO ("Frontend de PC"); teclado QWERTY na tela do nome e apagar saves ("Teclado do nome e apagar saves"); teclado, mouse, DualSense e ícones ("Controles de PC"); título "Definitive Edition" ("Logo Definitive Edition").
- **Conteúdo**: tradução PT-BR ("Pacotes de idioma"), legendas ("Legendas"), cores do mojo do PS2, HD AI Remaster, câmera livre experimental, mods de `.rcf` até 4 GB, mods de áudio.
- **Testes**: `tools/run_game.py` com perfis em `config/run.toml`; todo teste roda numa cópia de `tests\userdata_base` (README, "Rodando e testando"). Capturas de cada recurso em `docs\capturas\`.
- **Desempenho** (2026-10-21/22, "Desempenho de CPU"): limitado pela CPU, não pela GPU; na vila a 1080p, num Ryzen 7 5700X, 65 → 125 fps (primeira rodada) e ~94 → 176 fps logo depois da cena de abertura (segunda); limite 60 sem quantização em 30; o gargalo agora é a thread principal do jogo.
- **Em aberto**: EDRAM geral (aliasing entre formatos), interrupções do XMA, Bink com legenda, limites da câmera livre (veja "Próximos passos sugeridos", no fim).

As seções "Passo 1" a "Passo 16" registram como cada problema do boot foi achado e resolvido; os "Ponto de falha" e "Execução atual" de cada passo descrevem o estado **daquela época**, não o de hoje. Capturas citadas como "não guardada" foram apagadas nas limpezas de 2026-10-21.

## Compilar e rodar

Com as pastas do toolchain portátil (`toolchain\`, seção "Toolchain portátil" no fim) na frente do `PATH` (o `tools/run_game.py` faz isso sozinho; com `[build] toolchain = "msvc"` ele usa um prompt `vcvars64` + `clang-cl`):

```bat
set PATH=%CD%\toolchain\llvm-mingw\bin;%CD%\toolchain\cmake\bin;%CD%\toolchain\ninja;%CD%\toolchain\python;%PATH%
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build --target CrashRecomp
python tools\make_dist.py
set CRASH_TIMEOUT=20
dist\CrashRecomp.exe > run.log
```

### Caminhos (`paths.*`)

Nada é embutido no build (antes eram as macros `CRASH_PRIVATE_DIR`, `CRASH_ROOT_DIR` e `CRASH_GAME_DIR` do CMake, com caminhos absolutos). `paths::Init`, a primeira coisa do `main`, resolve tudo a partir da pasta do exe:

| Pasta | Padrão | Argumento |
|---|---|---|
| jogo (montado como `D:\`, só leitura) | `game\` ao lado do exe se tiver `default.xex` e `default.rcf`; senão `[paths] game` de `<userdata>\settings.toml`; senão uma janela de escolher pasta (a escolha vai para o `settings.toml`) | `--game` |
| mods | `mods\` | `--mods` |
| userdata (saves, `settings.toml`, caches) | `userdata\` | `--userdata` |
| idiomas | `languages\` | `--languages` |
| dados opcionais derivados do jogo (`subtitles\english.txt`, `symbols\functions.csv`, `remaster_full\` / `remaster_l1\`) | `data\` | `--data` |
| XEX | `<jogo>\default.xex` | `--xex` |
| imagem | nenhuma: descriptografada do XEX (`xex_image/`) | `--image <image.bin>` |

**Exe do build**: em `build\CrashRecomp.exe` (sem `languages\` ao lado e com o `CMakeLists.txt` do projeto um nível acima), `languages\` vem do projeto, `data` é `private\` e `mods\`/`userdata\` são os de `dist\` (os do jogador). O `run_game.py` passa todas as pastas pela linha de comando. O log começa com as linhas `[paths]`.

**Descriptografia** (`xex_image/xex_image.cpp`, alvo CMake `XexImage`): usa o `Image::ParseImage` do XenonUtils (AES da chave de retail, compressão básica e LZX), o mesmo código do `tools/xexdump` que gerava o `image.bin`; fica numa biblioteca à parte porque os headers do XenonUtils (`ppc_context.h`) colidem com os do runtime. O loader confere base e tamanho contra `PPC_IMAGE_BASE`/`PPC_IMAGE_SIZE` (outro `default.xex` é recusado com uma mensagem). As DLLs do FFmpeg são carregadas só de ao lado do exe (o CMake as copia para `build\`, o `make_dist` para `dist\`).

O alvo `CrashRecomp` usa as mesmas flags do `CrashPPC` (`-mavx` no llvm-mingw / `/arch:AVX` no clang-cl, `-fno-strict-aliasing ...`) e runtime estático. O link termina sem nenhum símbolo não resolvido.

## Estrutura

| Arquivo | Função |
|---|---|
| `main.cpp` | Inicialização e execução de `_xstart` |
| `paths.*` | Onde fica cada pasta (jogo, mods, userdata, idiomas, data, XEX), relativo ao exe; janela para escolher a pasta do jogo na primeira vez (veja "Caminhos") |
| `xex_image/` | Descriptografa e descomprime o `default.xex` com o XenonUtils (alvo `XexImage`, compilado à parte) |
| `kernel/memory.*` | Reserva 4 GB (preferencialmente em `0x100000000`, como no UnleashedRecomp) com placeholders: a memória física (512 MB) é uma seção mapeada em `0xA0000000` e em `0xC0000000`, e o resto é reserva comum. Commit explícito para imagem, tabela de funções e heap do runtime; os demais acessos fazem commit sob demanda via vectored exception handler, exceto nos primeiros 64 KB (null) e no heap do runtime (guarda de pilha) |
| `kernel/xex_loader.*` | Copia a imagem (descriptografada do XEX, ou um `image.bin` dado com `--image`) para `0x82000000`, lê do cabeçalho o entry point, o stack size e a tabela de imports, resolve a IAT e preenche a tabela de chamadas indiretas (`PPC_LOOKUP_FUNC`, em `0x825D0000`) a partir de `PPCFuncMappings`. Endereços sem função apontam para um handler que reporta o alvo (`ctr`) |
| `kernel/imports.cpp` + `import_list.inc` | Um stub **weak** por import: registra nome, r3–r6 e LR, e retorna 0. Para implementar de verdade, basta definir `PPC_FUNC(__imp__Nome)` em outro arquivo do runtime: a definição forte substitui o stub no link |
| `kernel/kernel.h` | Helpers das implementações reais: códigos `X_STATUS_*` (com prefixo, porque `windows.h` define `STATUS_*` como macros) e `KERNEL_IMPORT_ENTER`/`KERNEL_LOG` (marca o último import para o relatório de crash e loga as 16 primeiras chamadas de cada import). Imports reais não entram no resumo `[imports]`, que só conta stubs |
| `kernel/page_heap.*` | Alocador por páginas de 4 KB com semântica de reserve/commit/decommit/release do Windows sobre um intervalo guest. O commit/decommit acompanha a memória host, então páginas recém-commitadas vêm zeradas |
| `kernel/memory_imports.cpp` | `NtAllocateVirtualMemory`/`NtFreeVirtualMemory`/`NtQueryVirtualMemory` (heap virtual `0x40000000–0x7F000000`) e `MmAllocatePhysicalMemoryEx`/`MmFreePhysicalMemory`/`MmGetPhysicalAddress`/`MmQueryAddressProtect`/`MmQueryStatistics` (heap físico `0xA0000000–0xC0000000`) |
| `kernel/sync_imports.cpp` | `RtlInitializeCriticalSection`/`RtlEnterCriticalSection`/`RtlLeaveCriticalSection` sobre o `RTL_CRITICAL_SECTION` guest |
| `kernel/objects.*` | Objetos de dispatcher (eventos, semáforos, mutants, threads), lock e condition variable únicos do dispatcher, tabela de handles, conversão de timeouts e a espera (`WaitForObjects`, any/all) |
| `kernel/threads.*` | Threads guest: thread host (64 MB de pilha) + `GuestThreadContext`, sempre criada suspensa até o bloco guest existir; saída por retorno ou `ExTerminateThread` sinaliza o KTHREAD |
| `kernel/thread_imports.cpp` | `ExCreateThread`/`ExTerminateThread`/`NtResumeThread`/`KeResumeThread`, `ObReferenceObjectByHandle`/`ObDereferenceObject`/`NtClose`, `MmCreateKernelStack`/`MmDeleteKernelStack`, `KeDelayExecutionThread`, `KeQueryPerformanceFrequency`, `KeTls*` e `KeGetCurrentProcessType` (= 1) |
| `kernel/dispatcher_imports.cpp` | `NtCreateEvent`/`NtSetEvent`/`KeSetEvent`/`KeResetEvent`, `NtCreateSemaphore`/`NtReleaseSemaphore`/`KeInitializeSemaphore`/`KeReleaseSemaphore`, `NtCreateMutant`/`NtReleaseMutant`, `NtWaitForSingleObjectEx`/`NtWaitForMultipleObjectsEx`/`KeWaitForSingleObject`/`KeWaitForMultipleObjects` e spin locks (`KfAcquireSpinLock` & cia.) |
| `fs/vfs.*` | Sistema de arquivos virtual: montagens (`D:`/`game:`/`\Device\Cdrom0` → pasta do jogo, só leitura; `hdd:`/`E:` e `cache:`/`Z:` → `userdata\`), normalização de caminhos e overlay de mods |
| `kernel/file_imports.cpp` | `NtCreateFile`/`NtOpenFile`, `NtReadFile`/`NtWriteFile`, `NtQueryInformationFile`/`NtSetInformationFile`, `NtQueryDirectoryFile`, `NtQueryVolumeInformationFile`, `NtFlushBuffersFile` e `RtlInitAnsiString`, com handles de arquivo na tabela de handles do kernel |
| `gpu/gpu.*` | GPU, primeira etapa: janela de registradores (`0x7FC80000`, primeira página com trap de escrita), thread de interrupções (vblank a 60 Hz + `PM4_INTERRUPT`) e contadores |
| `gpu/command_processor.cpp` | Thread host que percorre o PM4 do ring buffer e dos indirect buffers, aplica escritas de registrador, faz as escritas em memória/esperas/interrupções e grava o read pointer de volta. Shaders, draws e swaps vão para o renderer (passo 10) |
| `gpu/video_imports.cpp` | `Vd*` e `XGetVideoMode`: modo fixo 1280x720 60 Hz widescreen, ring buffer/write-back/callback de interrupção para a GPU, `VdSwap` |
| `gpu/shader_translator.*`, `gpu/xenos_renderer.cpp` (antes `d3d12_renderer.cpp`), `gpu/rhi*.{h,cpp}`, `gpu/texture_decode.*`, `gpu/window.*`, `gpu/xenos.h` | Renderização (passo 10): Direct3D 12 e Vulkan (seção "Vulkan") |
| `gpu/overlay.*`, `gpu/overlay_font.h`, `kernel/input.h` | Overlay de desempenho (quadro, relógio do input, FPS, frame time), marcas F2, `frametimes.csv` e gravação de botões (passo 18) |
| `settings/*` | Tela de configurações (SETTINGS no menu principal), `userdata\settings.toml`, patches embutidos do menu/textos/mixers e o gancho Lua que abre a tela (passo 19); `restart.cpp`: o reinício pela tela e o pulo das intros ("Aplicar as opções sem fechar o jogo") |
| `camera/free_camera.*` | Câmera livre em terceira pessoa (experimental, `[camera] free_camera`): órbita sobre a câmera de render do jogo, colisão, recentrar ("Câmera livre em terceira pessoa") |
| `kernel/rtl_imports.cpp` | `RtlFillMemoryUlong` e `RtlCompareMemoryUlong` |
| `kernel/xam_notify_imports.cpp`, `kernel/xam_user_imports.cpp`, `kernel/xam_input_imports.cpp` | Listeners de notificação, sign-in/perfil, controle (XInput + teclado), XConfig (passo 10) |
| `kernel/xam_content_imports.cpp` | `XamShowDeviceSelectorUI`, `XamShowMessageBoxUIEx`, `XamContent*`, `XamEnumerate`: dispositivo único "Hard Drive", pacotes de conteúdo em `userdata\content` montados no VFS (passo 15) |
| `kernel/xmsg_imports.cpp` | `XMsgInProcessCall`/`XMsgStartIORequest(Ex)`: Xbox Music Player (app `0xFA`) sempre parado, título no controle da música (passo 14) |
| `kernel/audio_imports.cpp` | `XAudio*` (driver de render: clientes, callback a cada 256 amostras, frames submetidos) e `XMACreateContext`/`XMAReleaseContext` (passo 12) |
| `audio/xma.*` | Hardware XMA: 320 contextos em memória física, janela de registradores em `0x7FEA0000` (kick/lock/clear), parser de pacotes/frames XMA2 e thread decodificadora (passo 12) |
| `audio/ffmpeg.*` | Carrega em tempo de execução as DLLs do FFmpeg (LGPL), copiadas de `thirdparty/ffmpeg` para o lado do exe, e usa o decodificador WMA Pro (passo 12) |
| `audio/audio_override.*` | Mods de som: quadros XMA "marcadores" de um `.rsd` substituto viram as amostras de um WAV/Ogg solto do mod (veja "Mods de áudio" abaixo) |
| `audio/audio_output.*`, `audio/wav_writer.*` | Saída de som: downmix 5.1 → estéreo, XAudio2, relógio guiado pelo dispositivo, `CRASH_NOAUDIO` e dumps WAV (passo 12) |
| `kernel/fibers.cpp` | Fibers do XAPI (`CreateFiber`/`ConvertThreadToFiber`/`SwitchToFiber`/`DeleteFiber`, linkados estaticamente no título) sobre fibers do host, cada um com o seu `PPCContext` (passo 11) |
| `cpu/guest_thread.*` | Bloco da thread guest (pilha + PCR/r13 + TLS + KTHREAD), no layout do UnleashedRecomp, mas com a pilha embaixo e 64 KB de guarda não commitada. O KTHREAD aponta (`+0x84`) para um KPROCESS com o stack size padrão em `+0x1C` |
| `diagnostics.*` | Log, relatório de crash (exceção, função guest mais próxima do RIP, registradores guest da thread que falhou, último import) e watchdog, que lista todas as threads guest |
| `symbols.*` | Nomes das funções guest lidos de `<data>\symbols\functions.csv` (`tools/rtti_scan.py`, passo 17), usados nos relatórios `[fatal]`/`[crash]`/`[watchdog]` |
| `tools/gen_import_stubs.py` | Gera `kernel/import_list.inc` a partir do `llvm-nm` de `CrashPPC.lib` |

O mapa de memória guest fica assim: `0x00000000–0x0000FFFF` inacessível, `0x40000000–0x7F000000` heap virtual do título (`NtAllocateVirtualMemory`), `0x7F000000–0x7FC00000` heap do runtime (variáveis do kernel, objetos de kernel, blocos e pilhas das threads), `0x7FC80000–0x7FD00000` registradores da GPU (passo 7), `0x7FEA0000–0x7FEB0000` registradores do XMA (passo 12), `0x82000000–0x825D0000` imagem, `0x825D0000–0x82D30238` tabela de funções, `0xA0000000–0xC0000000` memória física (`MmAllocatePhysicalMemoryEx`) e `0xC0000000–0xE0000000`, uma segunda visão da mesma memória física (passo 9). `0xE0000000+` fica sem mapeamento.

## Passo 1: símbolos que o CrashPPC.lib usa e não define

Lista obtida com `llvm-nm -u` menos `llvm-nm --defined-only` (`runtime/tools/gen_import_stubs.py`):

- **152 imports `__imp__*`**: 118 do xboxkrnl.exe e 34 do xam.xex. Batem com as 152 thunks de `private/symbols.txt`. O XenonRecomp os declara com linkage C++ (`?__imp__Nome@@YAXAEIAUPPCContext@@PEAE@Z`), por isso os stubs também usam linkage C++.
- **Nenhum mid-asm hook** (o `Crash.toml` não declara nenhum).
- **CRT**: `memcpy`, `memset`, `sqrt`, `sqrtf`, `exp2f`, `log2f`, `_setjmp`, `longjmp`, `__security_check_cookie`, `__security_cookie` e `_fltused`, todos resolvidos pelo CRT estático.

Imports (ordem alfabética): DbgBreakPoint, DbgPrint, ExCreateThread, ExGetXConfigSetting, ExRegisterTitleTerminateNotification, ExTerminateThread, HalReturnToFirmware, KeAcquireSpinLockAtRaisedIrql, KeBugCheck, KeBugCheckEx, KeDelayExecutionThread, KeEnterCriticalRegion, KeGetCurrentProcessType, KeInitializeSemaphore, KeLeaveCriticalRegion, KeLockL2, KeQueryPerformanceFrequency, KeQuerySystemTime, KeRaiseIrqlToDpcLevel, KeReleaseSemaphore, KeReleaseSpinLockFromRaisedIrql, KeResetEvent, KeResumeThread, KeSetAffinityThread, KeSetBasePriorityThread, KeSetCurrentStackPointers, KeSetEvent, KeTlsAlloc, KeTlsFree, KeTlsGetValue, KeTlsSetValue, KeUnlockL2, KeWaitForMultipleObjects, KeWaitForSingleObject, KfAcquireSpinLock, KfLowerIrql, KfReleaseSpinLock, KiApcNormalRoutineNop, MmAllocatePhysicalMemoryEx, MmCreateKernelStack, MmDeleteKernelStack, MmFreePhysicalMemory, MmGetPhysicalAddress, MmQueryAddressProtect, MmQueryStatistics, NetDll_WSAStartup, NtAllocateVirtualMemory, NtClose, NtCreateEvent, NtCreateFile, NtCreateMutant, NtCreateSemaphore, NtFlushBuffersFile, NtFreeVirtualMemory, NtOpenFile, NtQueryDirectoryFile, NtQueryInformationFile, NtQueryVirtualMemory, NtQueryVolumeInformationFile, NtReadFile, NtReleaseMutant, NtReleaseSemaphore, NtResumeThread, NtSetEvent, NtSetInformationFile, NtWaitForMultipleObjectsEx, NtWaitForSingleObjectEx, NtWriteFile, ObCreateSymbolicLink, ObDeleteSymbolicLink, ObDereferenceObject, ObReferenceObjectByHandle, RtlCaptureContext, RtlCompareMemoryUlong, RtlEnterCriticalSection, RtlFillMemoryUlong, RtlImageXexHeaderField, RtlInitAnsiString, RtlInitializeCriticalSection, RtlLeaveCriticalSection, RtlMultiByteToUnicodeN, RtlNtStatusToDosError, RtlRaiseException, RtlTimeFieldsToTime, RtlTimeToTimeFields, RtlUnwind, VdCallGraphicsNotificationRoutines, VdEnableDisableClockGating, VdEnableRingBufferRPtrWriteBack, VdGetCurrentDisplayGamma, VdGetCurrentDisplayInformation, VdGetSystemCommandBuffer, VdInitializeEngines, VdInitializeRingBuffer, VdInitializeScalerCommandBuffer, VdIsHSIOTrainingSucceeded, VdPersistDisplay, VdQueryVideoFlags, VdQueryVideoMode, VdRetrainEDRAM, VdRetrainEDRAMWorker, VdSetDisplayMode, VdSetGraphicsInterruptCallback, VdSetSystemCommandBufferGpuIdentifierAddress, VdShutdownEngines, VdSwap, XAudioGetVoiceCategoryVolume, XAudioGetVoiceCategoryVolumeChangeMask, XAudioRegisterRenderDriverClient, XAudioSubmitRenderDriverFrame, XAudioUnregisterRenderDriverClient, XGetAVPack, XGetGameRegion, XGetLanguage, XGetVideoMode, XMACreateContext, XMAReleaseContext, XMsgInProcessCall, XMsgStartIORequest, XMsgStartIORequestEx, XNotifyGetNext, XamContentClose, XamContentCreateEnumerator, XamContentCreateEx, XamContentDelete, XamContentFlush, XamContentGetCreator, XamContentGetDeviceData, XamContentGetDeviceState, XamContentSetThumbnail, XamEnumerate, XamGetExecutionId, XamGetSystemVersion, XamInputGetCapabilities, XamInputGetState, XamInputSetState, XamLoaderLaunchTitle, XamLoaderTerminateTitle, XamNotifyCreateListener, XamShowDeviceSelectorUI, XamShowDirtyDiscErrorUI, XamShowMessageBoxUIEx, XamShowSigninUI, XamUserGetSigninState, XamUserReadProfileSettings, XamWriteGamerTile, XexCheckExecutablePrivilege, XexGetModuleHandle, XexGetProcedureAddress, \_\_C_specific_handler, \_vsnprintf, sprintf.

### Imports de variável (não aparecem como símbolos)

Além das thunks, o xboxkrnl exporta **10 variáveis** que o código lê pela IAT. O XenonRecomp não cria símbolos para elas, e em `image.bin` o slot ainda guarda o registro original de import (byte-swapped pelo XenonUtils). O loader aloca memória guest para cada uma e grava o ponteiro no slot:

| Variável | Ordinal | IAT | Valor inicial |
|---|---|---|---|
| VdHSIOCalibrationLock | 0x1C1 | 0x82000490 | zeros |
| VdGpuClockInMHz | 0x1C0 | 0x820004C0 | 500 |
| XboxKrnlVersion | 0x158 | 0x82000504 | 2.0.17559.0 |
| KeCertMonitorData | 0x266 | 0x8200050C | zeros |
| VdGlobalDevice | 0x1BE | 0x82000538 | 0 |
| XexExecutableModuleHandle | 0x193 | 0x82000564 | → LDR entry cujo +0x58 aponta para uma cópia do cabeçalho XEX |
| KeDebugMonitorData | 0x59 | 0x8200056C | zeros |
| ExThreadObjectType | 0x1B | 0x82000574 | zeros |
| KeTimeStampBundle | 0xAD | 0x820005AC | relógio atualizado a cada 1 ms por uma thread host (passo 10) |
| ExLoadedCommandLine | 0x1AE | 0x820005B0 | `"default.xex"` |

Os slots IAT das 152 funções passam a apontar para a thunk correspondente (que está na tabela de funções), para que leituras da IAT resultem em um endereço chamável.

## Passo 4: até onde a execução chegava só com stubs

(Histórico: resolvido no passo 5.) Execução de `build/CrashRecomp.exe` em 19/10/2026, antes dos imports reais:

```
[thread] starting guest 0x82264F40: r1=0x7F052F00 r13=0x7F053000
[import #1] RtlImageXexHeaderField(r3=0x7F000150, r4=0x00020401, ...) lr=0x8226588C -> 0 (stub)
[import #2] NtAllocateVirtualMemory(r3=0x7F052B50, r4=0x7F052C34, r5=0x60002000, r6=0x00000004) lr=0x8226748C -> 0 (stub)
[import #3] NtAllocateVirtualMemory(r3=0x7F052B54, r4=0x7F052C3C, r5=0x60001000, r6=0x00000004) lr=0x822674DC -> 0 (stub)
[crash] access violation (0xC0000005) at host rip ..., nearest guest function sub_82267158
[crash] write of guest address 0x0000004C
guest lr=0x822674DC r1=0x7F052B00 r3=0x00000000 ...
```

### Imports chamados, em ordem

1. `RtlImageXexHeaderField(header, 0x00020401)`, chamado em `sub_82265850`. Pede o campo `XEX_HEADER_DEFAULT_HEAP_SIZE`, que este XEX não tem, então retornar 0 está correto. O CRT segue com o tamanho padrão.
2. `NtAllocateVirtualMemory(&base, &size, 0x60002000, PAGE_READWRITE)`: reserva o heap do processo (0x2000 = `MEM_RESERVE`, mais flags de página do Xbox 0x60000000).
3. `NtAllocateVirtualMemory(&base, &size, 0x60001000, PAGE_READWRITE)`: commit (0x1000 = `MEM_COMMIT`) da primeira parte desse heap.

### Ponto de falha (resolvido)

Em `sub_82267158`, a criação do heap do processo. Cadeia de chamadas: `_xstart` → `sub_82265928` (a primeira chamada de `_xstart`) → `sub_82265850` (lê `DEFAULT_HEAP_SIZE` e cria o heap) → `sub_82267158`. O stub de `NtAllocateVirtualMemory` retorna `STATUS_SUCCESS` sem gravar o endereço em `*BaseAddress`. O heap então usa base 0 e escreve o cabeçalho em `0x0000004C`, que cai na página nula protegida. Ou seja, a execução passa pelo prólogo de `_xstart` e falha ainda dentro da primeira chamada, antes de qualquer arquivo, thread ou gráfico.

Observação: `sub_822671F0` (entrada do `.pdata`) é um segundo ponto de entrada no meio de `sub_82267158`. O código gerado de `sub_82267158` cobre o corpo inteiro, inclusive a chamada em `0x822674D8`, por isso o relatório aponta `sub_82267158`.

### Experimento: com um `NtAllocateVirtualMemory` mínimo

Para testar o mecanismo de override, compilei temporariamente uma implementação forte de `NtAllocateVirtualMemory`. Ela aloca de forma sequencial a partir de `0x40000000`, faz commit e devolve base e tamanho. O arquivo foi removido depois e não faz parte do runtime. Com ela, a execução avança:

```
RtlImageXexHeaderField → NtAllocateVirtualMemory x2 → KeGetCurrentProcessType → RtlInitializeCriticalSection
→ RtlEnter/LeaveCriticalSection → XexCheckExecutablePrivilege(0xA) → KeTlsAlloc → KeTlsSetValue(0, ...)
→ ~1000 pares RtlEnterCriticalSection/RtlLeaveCriticalSection no heap (lr 0x822677B0 / 0x82267E38)
```

Depois disso, uma recursão no alocador do heap (`sub_82267F9C`, LR `0x82267F68`) estoura a pilha guest de 256 KB e bate na guarda. Na primeira tentativa, antes da guarda existir, a pilha crescia por commit sob demanda até ~120 MB e só estourava a pilha host. A causa foi investigada no passo 5: eram os stubs de TLS, não o heap.

## Passo 5: primeiros imports reais do kernel

Implementados em `runtime/kernel/`. Eles substituem os stubs weak no link; `import_list.inc` e o código gerado não mudaram.

- **Memória virtual**: `NtAllocateVirtualMemory(&base, &size, type, protect)` tem reserve/commit de verdade. Respeita `*BaseAddress` quando não é zero, arredonda para páginas de 4 KB ou de 64 KB (flag `0x20000000`, o `MEM_LARGE_PAGES` do Xbox) e alinha reservas em 64 KB. `MEM_COMMIT` sem base reserva e faz commit de uma vez, como no Windows, e `MEM_TOP_DOWN` procura de cima para baixo. `NtFreeVirtualMemory` faz decommit (tamanho 0 vai até o fim da reserva) e release. `NtQueryVirtualMemory` preenche o `MEMORY_BASIC_INFORMATION` de 7 dwords; fora dos heaps, descreve o que o host tem mapeado.
- **Memória física**: `MmAllocatePhysicalMemoryEx(flags, size, protect, min, max, align)` usa páginas de 4 KB, 64 KB ou 16 MB, conforme o `protect`, e respeita `min`/`max`/`align`. Por enquanto tudo vai para a visão em `0xA0000000`; o console também mapeia a memória física em `0xC0000000` e `0xE0000000`. `MmGetPhysicalAddress` devolve `addr & 0x1FFFFFFF`, com +0x1000 acima de `0xE0000000`, como no Xenia. `MmQueryAddressProtect` devolve a proteção da página, e `MmQueryStatistics` preenche a estrutura de 104 bytes com os valores do Xenia.
- **Critical sections**: o layout é `Header (16 bytes) / LockCount +0x10 / RecursionCount +0x14 / OwningThread +0x18`, como no UnleashedRecomp. `LockCount` segue o comportamento do console: começa em -1 e é incrementado a cada Enter. O dono é o KTHREAD (`PCR+0x100`), que é o valor usado pelo console; o UnleashedRecomp usa r13. Quando há contenção, a espera usa `std::atomic_ref::wait/notify` em `OwningThread`.
- **TLS**: os slots saem de um bitmap de 64 por processo. Os valores ficam no array de TLS do bloco da thread (`[PCR+0]`), então leituras diretas por r13 batem com `KeTlsGetValue`. `KeGetCurrentProcessType` retorna 1.

### A recursão em `sub_82267F9C` (causa)

`sub_82267F9C` não é recursiva. É o funclet `__finally` de `sub_82267708` (o `RtlAllocateHeap` do CRT) e só faz `RtlLeaveCriticalSection` no lock do heap (`r12` = frame do pai). O LR `0x82267F68` era só o frame mais profundo no momento em que a pilha estourou. A recursão de verdade vinha do CRT:

- `sub_823D1C58` é o `_getptd_noexit`. Ele lê a TLS (`KeTlsGetValue(1)`, por ponteiro de função). Se o valor for 0, chama `_calloc_crt(1, 0xC4)` e depois `KeTlsSetValue`; se o set falhar, libera o bloco e retorna NULL.
- Com os stubs, `KeTlsGetValue` sempre retornava 0 e `KeTlsSetValue` retornava FALSE. Então `_getptd_noexit` falhava sempre, mas só depois de um calloc + free. Esses são os ~1000 pares Enter/Leave no lock do heap.
- O chamador, `_getptd` (a chamada logo depois de `0x823D1D10`), então chama `sub_823D18E0(16)`, que é `_amsg_exit(_RT_THREAD)`. O caminho de erro dele volta a chamar `_getptd`, e o ciclo se repete até bater na guarda da pilha.

Com o TLS real, `_getptd` encontra o bloco da thread em `KeTlsGetValue(1)` e a recursão some. O heap do processo é criado normalmente: reserva de 1 MB em `0x40000000` e commit de 64 KB.

### Execução no fim do passo 5

Execução de `build/CrashRecomp.exe` em 19/10/2026 com `CRASH_TIMEOUT=20` (log não guardado):

```
RtlImageXexHeaderField(0x20401) -> 0 (stub)
NtAllocateVirtualMemory(base=0, size=0x100000, type=0x60002000) -> 0x40000000      heap do processo: reserve
NtAllocateVirtualMemory(base=0x40000000, size=0x10000, type=0x60001000) -> ok       commit
KeGetCurrentProcessType -> 1; RtlInitializeCriticalSection(0x40000610)              lock do heap
XexCheckExecutablePrivilege(0xA) -> 0 (stub)
KeTlsAlloc -> 0; KeTlsSetValue(0, 0x8242FBF4)                                       FLS do CRT
RtlInitializeCriticalSection x14 (0x8252B2F0...)                                    locks do CRT
KeTlsAlloc -> 1; KeTlsSetValue(1, 0x40000690)                                       _ptd da thread principal
MmAllocatePhysicalMemoryEx(size=0x8700000, protect=4) -> 0xA0000000                 135 MB
MmQueryStatistics -> ok
MmAllocatePhysicalMemoryEx(size=0x17000000) -> 0xA8700000, depois MmFreePhysicalMemory   sonda de memória livre
KeQueryPerformanceFrequency (stub)
ObReferenceObjectByHandle(-2) / KeSetBasePriorityThread / ObDereferenceObject (stubs)
MmCreateKernelStack(0x5000) (stub); NtCreateSemaphore x2 (stubs)
ExCreateThread(&handle, 0x2000, 0xA001492C, XapiThreadStartup 0x823EA7E8, ...) (stub)
ObReferenceObjectByHandle / KeSetBasePriorityThread / ObDereferenceObject; NtResumeThread (stubs)
NtReleaseSemaphore (stub)
KeDelayExecutionThread(1, 0, ...) em loop: ~228 milhões de chamadas em 20 s
[watchdog] still running after 20 s ... nearest guest function sub_823E9A68, guest lr=0x822930DC
```

### Ponto de falha (resolvido no passo 6)

A execução não quebra; ela fica presa num loop de espera. `sub_82293090` (código do jogo) faz `while (!obj->vfunc[23]()) { obj->m_0C->vfunc[3](); Sleep(0); }`. `sub_822886D8(0)` leva a `sub_823E9A68`, o `SleepEx` do XAPI, que faz uma chamada a `KeDelayExecutionThread(UserMode, alertable, &interval)` e só repete se for alertável e voltar `STATUS_ALERTED`. A condição esperada depende do trabalho de uma thread que o jogo acabou de criar com `ExCreateThread` (startup `0x823EA7E8`). Como esse import é um stub, a thread nunca roda, e os semáforos (`NtCreateSemaphore`/`NtReleaseSemaphore`) também são stubs. Além disso, o stub de `KeDelayExecutionThread` retorna na hora, então o loop ocupa 100% da CPU.

Detalhes que podem importar mais adiante:
- A sonda de 0x17000000 bytes (368 MB) de memória física dá certo aqui, mas no console provavelmente falharia, porque sobram só ~512 MB − 135 MB − kernel. Se o jogo dimensionar algo pelo resultado, vai ser preciso limitar o heap físico ao total do console.
- O stub de `MmCreateKernelStack` retornava 0, e com isso a criação de fiber do XAPI (`sub_823E8C30`) falhava com `ERROR_NOT_ENOUGH_MEMORY`. O jogo seguia mesmo assim. Implementado no passo 6.

## Passo 6: threads e sincronização

Implementados em `runtime/kernel/`:

- **Threads**: `ExCreateThread(&handle, stackSize, &threadId, XapiThreadStartup, start, context, flags)` cria uma thread host com seu próprio `GuestThreadContext` e roda `XapiThreadStartup(start, context)` pela tabela de funções. O `0x823EA7E8` chama `start(context)` e depois `ExTerminateThread(resultado)`. O bit 0 de `flags` é `CREATE_SUSPENDED` e os bits 24–31 são a máscara de processador, que vira o byte de CPU no PCR. A thread host é sempre criada suspensa e só é retomada depois que o handle e o id foram gravados (ou por `NtResumeThread`/`KeResumeThread`). A pilha guest é `max(stackSize, 64 KB)`; com tamanho 0, usa o padrão do processo. `ExTerminateThread` marca a thread como encerrada e chama `ExitThread`. A thread principal (`_xstart`) usa o mesmo caminho.
- **KTHREAD como objeto**: o KTHREAD de cada thread tem um `DISPATCHER_HEADER` (Type 6) e é sinalizado quando a thread termina. `ObReferenceObjectByHandle` devolve esse endereço, inclusive para o pseudo-handle `0xFFFFFFFE` (thread atual). O KTHREAD aponta (`+0x84`) para um KPROCESS com o stack size padrão em `+0x1C`, que o XAPI lê quando recebe stack size 0.
- **Objetos**: eventos (notification/synchronization), semáforos e mutants, criados por `Nt*` (com handle; objetos com nome no `OBJECT_ATTRIBUTES` são reaproveitados) ou inicializados pelo guest e usados por ponteiro nos `Ke*`. Como no `QueryKernelObject` do UnleashedRecomp, o `WaitListHead` do header guarda uma assinatura (`'RCMP'`) e o índice do objeto host. Um header inicializado inline pelo jogo (sem assinatura) vira um objeto novo, criado a partir de `Type`/`SignalState`. O estado host é a referência, e o `SignalState` guest é atualizado junto.
- **Esperas**: as quatro funções de espera usam um lock e uma condition variable únicos do dispatcher, o que deixa `WaitAll` simples e sem corridas. O timeout está em unidades de 100 ns (negativo = relativo, positivo = tempo absoluto do sistema, ponteiro nulo = infinito). As esperas ainda não são alertáveis (não há APCs).
- **`KeDelayExecutionThread`** dorme de verdade, e um intervalo de 0 faz `SwitchToThread`.
- **Spin locks** (`KfAcquireSpinLock`/`KfReleaseSpinLock`/`KeAcquireSpinLockAtRaisedIrql`/`KeReleaseSpinLockFromRaisedIrql`): CAS no u32 guest com o PCR do dono, como no Xenia. IRQL não é modelado.
- **`KeQueryPerformanceFrequency`**: o `mftb` recompilado lê `__rdtsc()`, então a frequência devolvida é a do TSC, medida uma vez (~3,4 GHz nesta máquina).
- **`MmCreateKernelStack`**: pilha no heap do runtime, com guarda; `MmDeleteKernelStack` não libera nada.
- **Thread safety**: o heap de páginas, a tabela de handles, o bitmap de TLS e a lista de threads têm mutex; `KeTlsAlloc` zera o slot novo em todas as threads vivas. O diagnóstico virou por thread: o crash mostra os registradores da thread que falhou, e o watchdog lista todas.

Limitações conhecidas: objetos e blocos de thread nunca são liberados (`NtClose` só remove o handle); `KeSetBasePriorityThread`/`KeSetAffinityThread` continuam stubs; não há APCs, timers nem `NtSuspendThread`.

### Execução no fim do passo 6

Execução de `build/CrashRecomp.exe` em 19/10/2026 com `CRASH_TIMEOUT=20` (log não guardado). Tudo até `MmFreePhysicalMemory` continua igual ao passo 5. Depois:

```
KeQueryPerformanceFrequency -> 3400003621
ObReferenceObjectByHandle(0xFFFFFFFE) -> KTHREAD 0x7F053BB0; KeSetBasePriorityThread (stub)
MmCreateKernelStack(0x5000) -> 0x7F069000                                         fiber do XAPI
NtCreateSemaphore(0, 32000) x2 -> 0xF8000004, 0xF8000008
ExCreateThread(stack 0x2000, start 0x82288BB0, ctx 0xA00148E8, CREATE_SUSPENDED) -> 0xF800000C
NtResumeThread(0xF800000C) -> previous count 1; NtReleaseSemaphore(0xF8000004, 1)
KeDelayExecutionThread(interval 0) x16, até a thread nova rodar             o loop de sub_82293090 termina
[thread 0x82288BB0] NtWaitForSingleObjectEx(semáforo) -> 0
[thread 0x82288BB0] RtlInitAnsiString / NtOpenFile / NtQueryVolumeInformationFile (stubs); NtClose(0) -> invalid handle
[thread 0x82288BB0] NtWaitForSingleObjectEx(semáforo), espera infinita              thread de I/O ociosa
XGetVideoMode (stub)
MmAllocatePhysicalMemoryEx(0x12C0, 0x8000, 0x600000 [64 KB pages], 0x60, 0x20, 0xC8)     estruturas da GPU
VdInitializeEngines, ExRegisterTitleTerminateNotification, VdSetGraphicsInterruptCallback(0x8226BF38)
ExGetXConfigSetting(3, 0xA), VdSetSystemCommandBufferGpuIdentifierAddress
MmGetPhysicalAddress(0xA8702000) -> 0x08702000
VdInitializeRingBuffer(0x08702000, 12), VdEnableRingBufferRPtrWriteBack(0x0870A03C, 6), KiApcNormalRoutineNop (stubs)
ExCreateThread(stack 0x10000, start 0x82281B38) x2 -> duas threads de trabalho (afinidade 4 e 8)
[workers] KeSetAffinityThread (stub); KeWaitForSingleObject(0x40009344, infinito) / (0x400092F4, 30 ms)
VdQueryVideoMode, VdQueryVideoFlags, VdCallGraphicsNotificationRoutines, VdGetCurrentDisplayGamma (stubs)
[crash] access violation in sub_8226C3F0: write of guest address 0x7FC80714 (thread principal)
```

### Ponto de falha (resolvido no passo 7)

A thread principal passou do loop de espera, inicializou a GPU com os `Vd*` (stubs) e falha no primeiro envio de comandos para a GPU. `sub_8226C3F0` escreve pacotes no ring buffer (base em `[r26+14744]`, máscara em `[r26+14748]`), atualiza o ponteiro de escrita em `[r26+10824]` e faz:

```
lis r11, 0x7FC8
stw r29, 0x714(r11)    ; GPU CP_RB_WPTR = r29
```

`0x7FC80000` é a janela MMIO dos registradores da GPU Xenos (`0x714` = registrador `0x1C5`, `CP_RB_WPTR`). No nosso mapa de memória, esse endereço cai dentro do intervalo do heap do runtime (`0x7F000000–0x7FFF0000`), que não faz commit sob demanda. Por isso o relatório fala em "runtime heap guard", mas não é estouro de pilha. O heap do runtime em si ainda está em `~0x7F0CD000`, bem longe dessa região.

O próximo passo é a GPU: tirar a janela MMIO `0x7FC80000–0x7FCFFFFF` do heap do runtime (baixar `KERNEL_HEAP_END`) e tratar as escritas nela. Se fosse só memória comum, o jogo seguiria, mas como nada consome o ring buffer, o read pointer (write-back em `0x0870A03C` físico = `0xA870A03C`) nunca andaria e o jogo provavelmente travaria esperando a GPU. Também seria preciso responder ao `VdSetGraphicsInterruptCallback` (`0x8226BF38`) com interrupções de vsync.


## Passo 7: GPU, primeira etapa (sem renderização)

Objetivo: passar pela inicialização de vídeo e chegar ao loop de frames. Código em `runtime/gpu/`.

- **Mapa de memória**: `KERNEL_HEAP_END` caiu para `0x7FC00000`, e `0x7FC80000–0x7FD00000` passou a ser a janela de registradores da Xenos (registrador N em `+N*4`, big-endian como o guest escreve). O handler de commit sob demanda ignora a janela.
- **Trap de escrita**: o XenonRecomp marca as stores seguidas de `eieio` como `PPC_MM_STORE_U32`, mas a macro cai em uma store comum e já está compilada no `CrashPPC.lib`. Por isso o trap fica no runtime. A primeira página da janela (registradores `0x000–0x3FF`, onde está `CP_RB_WPTR` = `0x1C5`) fica read-only. A store do guest falha, o handler vetorado libera a página, faz single-step da instrução (trap flag), protege a página de novo e repassa o valor. Escrever em `CP_RB_WPTR` acorda o command processor. Os registradores dessa página ficam num array host; os demais ficam na própria memória da janela, compartilhada entre guest e CP. Outra thread que escreva durante o single-step não é vista, por isso o CP também lê `CP_RB_WPTR` a cada 2 ms. Valores iniciais iguais aos do Xenia: `RB_EDRAM_TIMING`, `RB_BC_CONTROL`, `D1MODE_V_COUNTER = 0x2D0`, status de interrupção (`0x1951`) = 1 (vblank) e viewport 1280x720.
- **Command processor** (`command_processor.cpp`, thread host): lê o ring de `VdInitializeRingBuffer(endereço, log2)` (tamanho `2^(log2+3)` bytes; aqui `0x08702000`, 32 KB) e trata os pacotes:
  - Tipos 0, 1 e 2: escritas de registrador e NOP.
  - Tipo 3 executado: `INDIRECT_BUFFER` (recursivo), `WAIT_REG_MEM` (espera de verdade, com aviso após 2 s), `REG_RMW`, `REG_TO_MEM`, `MEM_WRITE`, `COND_WRITE`, `EVENT_WRITE`/`_SHD`/`_EXT`/`_ZPD`, `SET_CONSTANT`/`SET_CONSTANT2`/`LOAD_ALU_CONSTANT`/`SET_SHADER_CONSTANTS` (vão para o arquivo de registradores), `INTERRUPT` (fila para a thread de interrupções) e `XE_SWAP`.
  - Os demais (draws, shaders) só entram no histograma.
  - Os registradores de scratch são espelhados em memória conforme `SCRATCH_UMSK`. Escritas da GPU em memória respeitam o swap indicado nos 2 bits baixos do endereço.
  - Ao fim de cada lote, o read pointer (em dwords) é gravado big-endian no endereço de `VdEnableRingBufferRPtrWriteBack` (aqui `0xA870A03C`) e em `CP_RB_RPTR`.
- **Interrupções**: uma thread de runtime com contexto guest próprio (PCR/KTHREAD/pilha, via `CreateRuntimeGuestThread`) chama o callback de `VdSetGraphicsInterruptCallback` (`0x8226BF38`, user data `0x40006680`). A chamada leva `r3 = 0` (vblank, ~60 Hz, com o byte de CPU do PCR = 2) ou `r3 = 1` (`PM4_INTERRUPT`, uma vez por CPU da máscara), e `r4` = user data.
- **Vídeo** (`video_imports.cpp`): `VdQueryVideoMode`/`XGetVideoMode` (1280x720, 60 Hz, widescreen, HD, NTSC), `VdGetCurrentDisplayInformation` (estrutura de 0x58 bytes do Xenia), `VdQueryVideoFlags` = 1, gama tipo 2 / 2,22, `VdInitializeEngines` = 1, `VdIsHSIOTrainingSucceeded` = 1, `VdGetSystemCommandBuffer` (`0xBEEF0000/1`), `VdPersistDisplay` (64 bytes físicos). `VdInitializeScalerCommandBuffer` retorna 0 dwords. Os argumentos 11 e 12 dele, e o 9 e o 10 de `VdSwap`, vêm da pilha, em `r1+0x54+(n−8)*8`.
- **`VdSwap`**: o jogo reserva 64 dwords no command buffer e passa o segundo. O bloco vira `[NOP] [XE_SWAP 'SWAP' front width height] [NOPs]`, como no Xenia. `VdSwap` conta frames (`[gpu] VdSwap #N`), e o CP conta os `XE_SWAP` executados.
- **Diagnóstico**: crash/timeout/fatal imprimem os contadores da GPU e o histograma de opcodes (`AddDiagnosticReporter`). A cada 300 vblanks sai um resumo.
- **Arquivos** (provisório, `kernel/file_imports.cpp`): `RtlInitAnsiString` real, e `NtCreateFile`/`NtOpenFile` registram o caminho e retornam `STATUS_OBJECT_NAME_NOT_FOUND`. Antes, o stub retornava sucesso sem handle.

### Execução no fim do passo 7

Execução em 19/10/2026 com `CRASH_TIMEOUT=20`. O processo termina com crash ~0,1 s depois do início:

```
[gpu] register window 0x7FC80000-0x7FD00000 ready (first page trapped)
[I/O thread] NtOpenFile("<root 0xFFFFFFFD>D:\") -> NOT_FOUND
XGetVideoMode -> 1280x720; VdInitializeEngines -> 1; VdSetGraphicsInterruptCallback(0x8226BF38, 0x40006680)
[gpu] ring buffer at 0x08702000 (guest 0xA8702000), 0x8000 bytes
[gpu] read pointer write-back at 0x0870A03C (guest 0xA870A03C), block 2^6
2 worker threads (0x82281B38)
VdQueryVideoMode, VdQueryVideoFlags, VdCallGraphicsNotificationRoutines, VdGetCurrentDisplayGamma
[gpu] executing ring 0x0..0x16; [gpu] executing ring 0x16..0x19        CP_RB_WPTR trap → CP
MmAllocatePhysicalMemoryEx(0x398000) x2                                    front/back buffers 1280x720
[I/O thread] NtCreateFile("<root 0xFFFFFFFD>D:\shaders\simpleshaderlitvp.out") -> NOT_FOUND
[crash] null read in sub_822765C0 (main thread)
[gpu] vblank 3: 0 VdSwap call(s), 0 swap packet(s) executed, 0 interrupt(s) delivered
```

Histograma de pacotes até o crash (read pointer `0x19`, 33 pacotes tipo 0, 5 indirect buffers):

| Opcode | Nome | Quantidade |
|---|---|---|
| 0x36 | DRAW_INDX_2 | 24 |
| 0x3F | INDIRECT_BUFFER | 5 |
| 0x21 | REG_RMW | 2 |
| 0x2B | IM_LOAD_IMMEDIATE | 2 |
| 0x3B | INVALIDATE_STATE | 2 |
| 0x58 | EVENT_WRITE_SHD | 2 |
| 0x3C | WAIT_REG_MEM | 1 (satisfeito na hora) |
| 0x48 | ME_INIT | 1 |

Os 24 `DRAW_INDX_2` vêm da inicialização do device D3D (provavelmente limpeza da EDRAM). Não foram executados.

### Ponto de falha (resolvido no passo 8)

Agora é o **sistema de arquivos**, não a GPU. A thread principal pede à thread de I/O (`0x82288BB0`, que recebe pedidos por semáforo) para carregar `D:\shaders\simpleshaderlitvp.out`. `NtCreateFile` falha, `NtQueryInformationFile` (stub) não preenche tamanho, e o buffer do arquivo fica nulo. `sub_822765C0`, chamado por `sub_8239A1B0` (retorno em `0x8239A25C`) logo depois da leitura, faz `lwz r11,0(r3)` com `r3 = 0`. O jogo não verifica o erro.

Os arquivos existem na cópia do usuário: `<pasta do jogo>\shaders\simpleshaderlitvp.out` (além de `default.rcf`, `english.rcf`, `sound1.rcf` etc. na raiz). `RootDirectory = 0xFFFFFFFD` é o diretório de dispositivos DOS, então os caminhos são `D:\...` diretos para a raiz do jogo.

Ainda não verificado nesta etapa, porque a execução para antes:
- **Interrupções de vblank**: o callback foi registrado, mas só 3 vblanks passaram antes do crash e nenhuma interrupção chegou a ser entregue.
- **`VdSwap` e `XE_SWAP`**: o jogo ainda não chegou ao primeiro frame.

## Passo 8: sistema de arquivos e overlay de mods

### Como funciona

- **Montagens** (`runtime/fs/vfs.cpp`):

  | Dispositivo guest | Destino | Escrita |
  |---|---|---|
  | `D:`, `game:`, `dvd:`, `\Device\Cdrom0` | pasta do jogo (`--game`), com overlay de mods | não |
  | `hdd:`, `E:`, `\Device\Harddisk0\Partition1` | `userdata\hdd` | sim |
  | `cache:`, `Z:`, `\Device\Harddisk0\Cache0` | `userdata\cache` | sim |

  O prefixo `\??\` e `RootDirectory = 0xFFFFFFFD` (diretório de dispositivos DOS) são tratados como caminho absoluto. Um `RootDirectory` que é handle de diretório aberto resolve o caminho relativo a ele. `/` e `\` são equivalentes, `.` e `..` são resolvidos, e a busca não diferencia maiúsculas de minúsculas (como no host).
- **Overlay de mods**: cada subpasta de `mods\` é um mod com a mesma árvore do jogo. Um arquivo vem do primeiro mod, na ordem de carga, que o tenha; se nenhum tiver, vem da pasta do jogo. As listagens de diretório são a união (o primeiro vence). Ordem de carga: os nomes listados em `mods\load_order.txt` (um por linha, `#` comenta), depois as demais pastas em ordem alfabética. Pastas que começam com `_` ou `.` ficam desativadas. O log mostra a ordem na partida e a origem de cada arquivo aberto, por exemplo `-> mod zz_second (...)` ou `-> game (...)`. Por enquanto a substituição é de arquivos inteiros; entradas dentro dos `.rcf` ficam para depois.
- **A pasta do jogo nunca é escrita**: na montagem do jogo, criar, sobrescrever ou truncar retorna `STATUS_MEDIA_WRITE_PROTECTED`, e `NtWriteFile`, `FileEndOfFile`/`FileAllocation`/`FileDisposition` retornam `STATUS_ACCESS_DENIED`. Uma abertura com acesso de escrita a um arquivo existente é aceita como só leitura (o log avisa) e a escrita falha depois. O host abre esses arquivos só com `GENERIC_READ`. As montagens graváveis criam a raiz em `userdata\` sob demanda.
- **Imports** (`runtime/kernel/file_imports.cpp`):
  - Arquivos são `FileObject` na tabela de handles; `NtClose` fecha o handle host.
  - `NtCreateFile` recebe `CreateOptions` como 9º argumento, na pilha. Dispositions, `FILE_DIRECTORY_FILE`/`FILE_NON_DIRECTORY_FILE` e o `Information` (`FILE_OPENED`/`CREATED`/`OVERWRITTEN`) seguem o NT. `NtOpenFile` usa `OpenOptions` em r7.
  - `NtReadFile`/`NtWriteFile` usam `ByteOffset`, ou a posição do arquivo quando o ponteiro é nulo ou vale `FILE_USE_FILE_POINTER_POSITION`. São síncronas: preenchem o `IO_STATUS_BLOCK`, sinalizam o evento opcional e nunca retornam `STATUS_PENDING`. Isso cobre o caminho com `OVERLAPPED` do `ReadFile` do XAPI. Rotinas APC não são chamadas (o jogo não usa).
  - `NtQueryInformationFile`: classes 4, 5, 6, 14, 20 e 34. `NtSetInformationFile`: 14 (posição), 20, 19, 13 e 4. `NtQueryVolumeInformationFile`: classes 1, 3, 4 e 5. `NtQueryDirectoryFile` devolve uma entrada por chamada, com curinga `*`/`?` e `RestartScan` (9º argumento, na pilha).
  - Leituras para páginas guest ainda não commitadas usam um buffer host intermediário, porque o kernel retorna `ERROR_NOACCESS` em vez de gerar a falha que faria o commit sob demanda.
  - `XamContent*` continuam stubs: o jogo ainda não chegou a chamá-los.

### Execução no fim do passo 8

Execução em 19/10/2026 com `CRASH_TIMEOUT=20`. O processo termina com `[fatal]` (código 1) depois de ~0,2 s.

Arquivos abertos, todos da pasta do jogo: `D:\` (diretório, 2x, para `GetDiskFreeSpace`: `NtQueryVolumeInformationFile` classe 3), `D:\shaders\` `simpleshaderlitvp.out`, `simpleshaderunlitvp.out`, `simpleshaderlitfp.out`, `furshaderlitfp.out`, `simpleshaderunlitfp.out`, `palsimpleshaderlitfp.out`, `palsimpleshaderunlitfp.out`, `palfurshaderlitfp.out`, `passthru.out`, `rtrestore.out`, `framebuffervp.out` (2x), `motionblurfp.out`, `colourmatrixfp.out`, `heatshimmervp.out`, `heatshimmerfp.out`, `DOFvp.out` e `DOF.out`. Cada um passa por `NtQueryInformationFile` (classe 34, tamanho), `NtSetInformationFile` (classe 14) e `NtReadFile` do arquivo inteiro. Nenhum `.rcf` foi aberto ainda, e nada foi escrito (`userdata\` não chegou a ser criada).

Teste do overlay, com uma pasta de mods temporária em `build\` e `--mods`, apagada depois:
- `zz_second` listado em `load_order.txt`, mais `aa_first` e `_off`, cada um com uma cópia idêntica de um shader do jogo.
- Ordem de carga: 1. `zz_second`, 2. `aa_first`; `_off` foi ignorado.
- `D:\shaders\passthru.out` veio de `mod zz_second` (o `PASSTHRU.OUT` de `aa_first` perdeu pela ordem), e `rtrestore.out` veio de `game`.
- Nenhum arquivo da pasta do jogo foi modificado.

Depois dos shaders, a thread principal segue:

```
NtOpenFile D:\ + NtQueryVolumeInformationFile        GetDiskFreeSpace
NtCreateSemaphore x2 + ExCreateThread(0x82288BB0) x2  mais duas threads de I/O (contextos 0xA1E7ABC8, 0xA1E7DC78)
ExGetXConfigSetting(3, 9), XGetGameRegion, ExGetXConfigSetting(3, 0xE)   (stubs)
XamNotifyCreateListener(1, 1) (stub); [thread de I/O] XNotifyGetNext (stub)
XamInputGetCapabilities(0) / XamInputGetState(0) (stubs)                 leitura do controle
[fatal] indirect call to unmapped guest address 0x82289948 (ctr), lr=0x822893A4
[gpu] vblank 8: 0 VdSwap call(s), 0 swap packet(s) executed, 5 interrupt(s) delivered
```

As interrupções de vblank agora chegam ao callback (5 entregues).

### Ponto de falha (resolvido pela correção do config, antes do passo 9)

Logo depois de ler o controle, `0x822893A4` chama por ponteiro de função (`bctrl`) o endereço `0x82289948`, que não está na tabela de funções. A causa está na recompilação, não no runtime:

- `0x822897E0` é uma das 52 funções explícitas de `config/Crash.toml`, com `size = 0x208`, ou seja, até `0x822899E8`.
- Esse intervalo engoliu três funções folha que ficam logo depois dela: `0x82289948`, `0x82289978` e `0x822899B8`. As três vêm depois de um `blr` (`0x82289944`, `0x82289974`, `0x822899B4`) e são apontadas por uma tabela de ponteiros de função em `0x8202E468`/`46C`/`470`. Os alvos `0x82289868`/`87C`/`8AC` são da jump table da própria `0x822897E0` (em `0x82289858`) e estão corretos.
- Correção: em `config/Crash.toml`, reduzir `0x822897E0` para `size = 0x168` e adicionar `{ address = 0x82289948, size = 0x30 }`, `{ address = 0x82289978, size = 0x40 }` e `{ address = 0x822899B8, size = 0x30 }`. Depois, regenerar `generated/ppc` e recompilar o `CrashPPC`. Não fiz isso porque `config/` e `generated/` estavam fora do escopo desta etapa.

Outras funções explícitas com valores no meio do intervalo que aparecem fora delas na imagem (possíveis casos iguais; precisam ser conferidos, porque podem ser só dados que coincidem): `0x8210A338` (→ `0x8210A348`), `0x8210EAE0` (→ `0x8210EB08`), `0x82134760` (→ `0x82134778`), `0x82149160` (→ `0x82149248`), `0x82316E50` (→ `0x82316E58`), `0x8231F668` (→ `0x8231F6F8`), `0x82340D70` (→ `0x82340DD0`, `0x82340E40`) e `0x823449F8` (→ `0x82344A60`).

## Passo 9: do primeiro frame ao loop de frames

Contexto: a sessão principal corrigiu `config/Crash.toml` (63 funções explícitas: a divisão de `0x822897E0` e mais 7 intervalos que engoliam funções alcançáveis só por tabelas de ponteiros), regenerou `generated/ppc` (20.139 funções) e recompilou. Com isso a execução passava de `0x82289948`, fazia 2 `VdSwap` e quebrava com divisão inteira por zero em `sub_8227CBC0`. A suspeita inicial era `VdQueryVideoFlags`, mas a causa estava em outro lugar. Os 7 bugs do runtime abaixo apareceram em sequência. Todos foram corrigidos sem nenhuma proteção no runtime contra os sintomas.

1. **`VdGetSystemCommandBuffer` deixava 0x90 bytes sem escrever** (causa da divisão por zero). O swap do D3D (`sub_82274080`) passa um buffer de pilha de 0x94 bytes e depois lê de volta `+4` → `device+21512` e `+8` → `device+22132`. Só o primeiro dword era escrito, então o resto era lixo de pilha. No frame 1 ele era zero por acaso; no frame 2 veio `0x820A1C00`/`0x00007FF6`, e o retângulo de origem do scaler virou `{0,0,0,0x13}`, que chega a `sub_8227CBC0` como `r5 = (0 << 16) | 0x13`. Agora o buffer é zerado inteiro, como no Xenia. Sobre as suspeitas iniciais: `VdQueryVideoFlags` só é testado no bit 0 (widescreen) em `sub_8227B5F8`, e o divisor de `sub_8227CBC0` vem de `X_VIDEO_MODE+0x08` (interlaced: 1 ou 2), não de `+0x1C`. O layout de `VdGetCurrentDisplayInformation` foi confirmado pelo uso em `sub_82274BF8`: largura `+0x48`, altura `+0x4A` e largura real `+0x56`.
2. **`KeSetAffinityThread` era stub**. O jogo indexa dados por processador com o byte do PCR (`lbz r,268(r13)`), e todas as threads ficavam com CPU 0. A versão nova grava a afinidade anterior no ponteiro, como no Xenia, e põe no PCR o processador mais baixo da máscara (workers do job system em 2 e 3; threads de áudio em 5 e 2). Isso não era a causa do crash seguinte, mas estava errado.
3. **Visões da memória física** (causa do null em `sub_82281820`, num worker do job system). O jogo acessa memória física pela visão `0xC0000000` (páginas de 16 MB, por exemplo `0xC8770300` = físico `0x08770300`), que no console é a mesma RAM que `0xA0000000`. Aqui `0xC…` caía no commit sob demanda e lia zeros, então as listas de jobs saíam nulas. Agora a memória física é uma seção de 512 MB (`CreateFileMapping` com `SEC_COMMIT`) mapeada em `0xA0000000` e `0xC0000000` (`VirtualAlloc2`/`MapViewOfFile3` com placeholders, carregados por `GetProcAddress`). O heap físico zera, no commit, as páginas que já foram usadas, porque não dá para fazer decommit numa visão mapeada.
   - `0xE0000000` continua sem mapeamento. Lá o console desloca 0x1000: o D3D do jogo converte endereços com `(addr & 0x1FFFFFFF) + (addr >= 0xE0000000 ? 0x1000 : 0)`. Esse deslocamento não cabe na granularidade de 64 KB das visões no host, então acessos a `0xE…` agora falham alto em vez de ler zeros.
4. **Interrupções do CP assíncronas**. A sequência de callback do D3D (`sub_8226CA20`) é:
   - `SCRATCH_REG4/5` = função/dado, `SCRATCH_REG0` = máscara de CPU (write-back em memória via `SCRATCH_ADDR`/`UMSK`);
   - `WAIT mem == máscara`, `INTERRUPT`;
   - às vezes, `WAIT mem == 0`.

   O callback (fonte 1) limpa seu bit nessa palavra. Como a thread de interrupções rodava o callback mais tarde, essa limpeza podia apagar o write-back da sequência seguinte, e o `WAIT` dela travava. Agora `PM4_INTERRUPT` espera o callback terminar, como no Xenia.
5. **Predicated tiling**. O D3D reexecuta o mesmo command buffer uma vez por tile e marca os pacotes específicos de tile com o bit 0 do cabeçalho. O CP ignorava isso, executava tudo em todos os tiles (inclusive swaps e interrupções) e chegou a 218 swaps para 5 `VdSwap`. Agora `SET_BIN_MASK`/`SET_BIN_SELECT` (`0x50`/`0x51`/`0x60`–`0x63`) mantêm a máscara e a seleção de 64 bits. Pacotes tipo 3 predicados rodam só quando `select & mask != 0`, e swaps predicados nunca rodam (Xenia).
6. **Layout do bloco do `VdSwap`** (causa do último travamento). O escritor de comandos do D3D usa `stwu` com pré-incremento: o cursor aponta para o último dword escrito, então o bloco reservado vai de `cursor+4` a `cursor+256`, e `VdSwap` recebe `cursor+4`. A versão anterior escrevia um NOP em `buffer-4` (o último dword do pacote anterior) e só 63 dwords. O dword `cursor+256` ficava com o que já estava lá (0 = cabeçalho tipo 0), e o CP perdia a sincronia do stream. O endereço do front buffer (`0x08D10000`) era lido como cabeçalho tipo 0 e zerava ~2250 registradores, inclusive `SCRATCH_UMSK`/`SCRATCH_ADDR`, e o `WAIT` seguinte travava. Agora são exatamente 64 dwords a partir de `buffer`, como no Xenia.
7. `RtlFillMemoryUlong`/`RtlCompareMemoryUlong` reais (o D3D usa o primeiro para encher o buffer do scaler de NOPs). `RtlRaiseException` passou a registrar o código: hoje só aparece `0x406D1388` (nome de thread), que é tratado retornando, como faria um `__except(EXCEPTION_CONTINUE_EXECUTION)`.

Diagnósticos adicionados no caminho e mantidos: um `WAIT_REG_MEM` travado por 2 s imprime os últimos 64 pacotes executados (endereço, cabeçalho e dois dwords), e o resumo da GPU inclui os pacotes predicados que foram pulados.

### Execução atual

Execução em 19/10/2026 com `CRASH_TIMEOUT=20`: **nenhuma falha em 20 s**. O jogo fica no loop de frames, e o watchdog encerra o processo.

- **598 `VdSwap` em 20 s** (~30 fps: um swap a cada 2 vblanks, alternando os front buffers `0xA8D10000`/`0xA90A8000`, 1280x720). O CP executa todos os 598 swaps, e ~2990 interrupções são entregues (1200 de vblank e o resto de `PM4_INTERRUPT`).
- **Arquivos abertos** (todos da pasta do jogo, nada escrito): `D:\` (2x), os 20 shaders `D:\shaders\*.out` (incluindo `shadowshadervp.out`/`shadowshaderfp.out`), `D:\default.rcf`, `D:\sound1.rcf`, `D:\sound2.rcf` e `D:\english.rcf`.
- **Threads** (nomes vindos do `RtlRaiseException` 0x406D1388): principal, 3 de I/O (`0x82288BB0`), 2 workers do job system (CPUs 2 e 3), `audio::hal::sw::RendererThread` (CPU 5), `audio::IOLoadTaskQueue`, `audio::Streamer::DvdLoadThread`, `audio::Streamer::MemLoadThread` e `ClipLoadThread`. No fim dos 20 s, a principal está ativa (`sub_821ED308`) e as demais estão em espera.
- **Histograma do CP em 20 s** (read pointer `0x6B3`, 517.887 pacotes tipo 0, 14.330 indirect buffers, 89.550 pacotes predicados pulados):

  | Opcode | Nome | Quantidade |
  |---|---|---|
  | 0x60 | SET_BIN_MASK_LO | 154.027 |
  | 0x36 | DRAW_INDX_2 | 97.311 |
  | 0x2D | SET_CONSTANT | 91.341 |
  | 0x61 | SET_BIN_MASK_HI | 61.492 |
  | 0x2B | IM_LOAD_IMMEDIATE | 18.507 |
  | 0x3C | WAIT_REG_MEM | 14.334 |
  | 0x3F | INDIRECT_BUFFER | 14.330 |
  | 0x3B | INVALIDATE_STATE | 13.135 |
  | 0x46 | EVENT_WRITE | 7.164 |
  | 0x54 | INTERRUPT | 2.985 |
  | 0x62 | SET_BIN_SELECT_LO | 2.985 |
  | 0x58 | EVENT_WRITE_SHD | 2.390 |
  | 0x21 | REG_RMW | 600 |
  | 0x64 | XE_SWAP | 597 |
  | 0x63 | SET_BIN_SELECT_HI | 598 |
  | 0x45 | COND_WRITE | 256 |
  | 0x48 | ME_INIT | 1 |

### Próximo ponto de falha

Não há falha nos 20 s. O que limita o avanço agora, em ordem:

- **Nada é desenhado**: os ~97 mil `DRAW_INDX_2` só são contados. O jogo provavelmente está numa tela inicial ou de carregamento, e ainda não dá para ver qual.
- **`XNotifyGetNext` em loop**: 46 milhões de chamadas em 20 s, uma thread girando sobre o stub. Precisa retornar FALSE (sem notificação), como faz, mas o jogo provavelmente espera um listener de verdade (`XamNotifyCreateListener` devolve um handle que pode ser esperado). Isso consome um núcleo.
- **Áudio**: `XAudioRegisterRenderDriverClient` é stub, então a thread de render de áudio nunca recebe callbacks.
- **Entrada**: `XamInputGetState`/`GetCapabilities` retornam sucesso sem preencher nada (controle "conectado" e parado). `XamUserReadProfileSettings`, `XamGetSystemVersion` e `NetDll_WSAStartup` também são stubs.
- **`0x7FEA1000`**: uma leitura em `0x7FEA1800` cai no commit sob demanda (outra janela MMIO do console, provavelmente de áudio/XMA). Por enquanto lê zero.

## Passo 10: renderização (Direct3D 12), primeira versão

Objetivo: desenhar numa janela o que o jogo manda para a GPU. Código em `runtime/gpu/`. A escolha foi Direct3D 12: o projeto é só Windows, o HLSL gerado compila com o `d3dcompiler_47.dll` que já vem no sistema (FXC, Shader Model 5.1, sem dependência extra) e a referência principal (o backend D3D12 do Xenia) usa a mesma API.

### Como funciona

| Arquivo | Função |
|---|---|
| `gpu/xenos.h` | Índices de registradores, enums (formatos de RT/textura/vértice, blend, primitivas), fetch constants de textura/vértice e o endereçamento tiled 2D (`Tiled2D`), seguindo o `register_table.inc`/`registers.h`/`xenos.h`/`texture_address.h` do Xenia (BSD) |
| `gpu/shader_translator.*` | Microcode Xenos → HLSL. Fluxo de controle (exec/cond exec por bool ou predicado, loops, call/return, jump), as 30 ops vetoriais e 51 escalares (com as regras do D3D9: `0 * x = 0` em mul/mad/dp), exports (posição, 16 interpoladores, oC0–3, oDepth, máscaras de constante 0/1), `vfetch` (todos os formatos de vértice, mini fetch, exp_adjust) e `tfetch` 1D/2D (getWeights, offsets, coordenadas não normalizadas). Shaders sem loop/jump viram código linear; os demais, um laço com `switch` sobre o contador de programa |
| `gpu/d3d12_renderer.cpp` (hoje `xenos_renderer.cpp` + `rhi_vulkan.cpp`; o backend D3D12 saiu em 2026-10-23, ver "Vulkan") | Dispositivo, swap chain, anel de upload (256 MB) e de descritores com reaproveitamento por fence, cache de PSOs, render targets, resolves, texturas, samplers, draws e apresentação |
| `gpu/texture_decode.*` | Textura guest → dados host: formatos, untiling 2D, swap de endian, nível base no mip tail empacotado, conversões em CPU (565/1555/4444/CTX1/DXT3A/profundidade) |
| `gpu/window.*` | Janela Win32 numa thread própria (fechar encerra o processo, F12 tira screenshot) |
| `gpu/png_writer.*` | PNG sem compressão para os screenshots |
| `gpu/renderer.h` | Interface chamada pelo command processor |

- **Command processor**: `IM_LOAD_IMMEDIATE`/`IM_LOAD` entregam o microcode (big-endian → host) ao renderer, `DRAW_INDX`/`DRAW_INDX_2` viram draws (DMA, auto-índice e índices imediatos) e `XE_SWAP` apresenta. O `VdSwap` agora inclui no pacote a fetch constant do front buffer (r4), como o Xenia.
- **Shaders**: traduzidos e compilados no primeiro draw que os usa; o DXBC fica em `userdata\shadercache\` (chave = hash do HLSL). Interface fixa: b0 = constantes do sistema (transformação de viewport, flags, alpha test, bool/loop constants, bases de vertex buffer, swizzles de textura), b1/b2 = 256 float4 do VS/PS (a partir de `SQ_VS_CONST`/`SQ_PS_CONST`), t0–t31/s0–s31 = fetch constants de textura, `t0 space1` = dados de vértice. VS e PS trocam sempre 16 interpoladores + posição. `r0.x` do VS recebe o índice do vértice; o PS recebe os interpoladores em r0–r15 e, com `param_gen`, a posição em `r[param_gen_pos]`.
- **Vértices e índices**: o VS lê os vértices com `ByteAddressBuffer` de um buffer por draw, com o intervalo usado de cada fetch constant (limitado pelos índices mínimo/máximo quando o índice é o `r0.x` original) já com o swap de endian aplicado na cópia. Os índices são expandidos para 32 bits na CPU com `VGT_INDX_OFFSET`, clamp `MIN/MAX_VTX_INDX` e reset de primitiva; fans/polígonos e quad lists viram listas, line loops viram strips fechadas, rect lists passam por um geometry shader que gera o 4º canto.
- **Viewport**: feito no VS (como o Xenia faz para draws sem clipping): viewport host = RT inteiro, `ndc_scale/offset` a partir de `PA_CL_VPORT_*`/`PA_CL_VTE_CNTL`, window offset e meio pixel do D3D9; `vtx_xy_fmt`/`vtx_z_fmt`/`vtx_w0_fmt` tratados. Scissor = `PA_SC_WINDOW_SCISSOR` (+ window offset) ∩ screen scissor.
- **EDRAM**: não é emulada como memória. Cada (base em tiles, pitch, formato) usado como cor ou profundidade vira um render target host 1x (sem MSAA) de `pitch × 2048`. Formatos: 8888 (e gamma como sRGB), 2_10_10_10, float/fixos de 16 e 32 bits; profundidade D24S8 e D24FS8 (como D32S8).
- **Resolves** (`RB_MODECONTROL` = copy): retângulo a partir dos 3 vértices em `vf0` (meio pixel, window offset, scissor, alinhamento de 8, como o `GetResolveInfo` do Xenia), cópia por shader do RT para uma textura host registrada pelo endereço físico de destino ("resolve target"), com exp bias e troca R/B. Um destino dentro de uma textura já registrada é posicionado por macro tiles de 32×32 (o D3D desloca `RB_COPY_DEST_BASE` com `XGAddress2DTiledOffset`); um destino novo assume que o canto (alinhado a 32) do retângulo é a origem. A memória guest não é escrita. Clears de cor/profundidade pedidos no resolve usam `ClearRenderTargetView`/`ClearDepthStencilView`.
- **Texturas**: o fetch procura primeiro um resolve target no endereço base; senão decodifica da memória guest (só o nível base) e reenvia quando o hash do conteúdo muda (checado uma vez por frame). Samplers vêm da fetch constant, com os overrides de filtro da instrução.
- **Estado**: blend independente por RT (fatores/ops do Xenos), máscara de cor, profundidade/stencil (frente e verso, uma referência e um par de máscaras, limitação do D3D12), culling (`face` = 0 → frente CCW), depth bias, depth clip conforme `clip_disable`, alpha test no PS (`RB_COLORCONTROL`/`RB_ALPHA_REF`), exp bias por RT.
- **Apresentação**: no `XE_SWAP`, o resolve target do front buffer é desenhado (filtro linear) no back buffer da janela, `Present(0)`, até 2 frames em voo.

Variáveis de ambiente novas: `CRASH_NO_RENDER` (desliga), `CRASH_D3D12_DEBUG` (debug layer, mensagens no log), `CRASH_SCREENSHOTS=60,300` (números de swap) e `CRASH_SCREENSHOT_DIR`, `CRASH_DUMP_SHADERS` (HLSL em `userdata\shaderdump\`; shaders que falham são sempre gravados lá), `CRASH_GPU_TRACE=N` e `CRASH_GPU_TRACE_FROM=M` (registradores e dados dos N draws a partir do draw M), `CRASH_PRESENT_SWAP_RB`. Teste do tradutor sem rodar o jogo: `CrashRecomp.exe --test-shaders <pasta shaders do jogo>`.

### Validação

- **Tradutor**: os 38 containers `shaders\*.out` do jogo (o microcode fica em `bloco físico + definição[0]`, tamanho em `definição[1]`; a definição é apontada pelo dword `0x18` do cabeçalho) traduzem e compilam sem erro com `--test-shaders`, incluindo os 9 com loops (`DOF`, `furshaderlitfp`, `waterspecularfp` com 154 instruções e 7 texturas etc.). A decodificação foi conferida à mão contra o disassembly do XDK em `shaders\*.asm` (exec/alloc/tfetch/`mulsc` de `simpleshadertexfp`).
- **No jogo** (20 s, `CRASH_TIMEOUT=20`): 587 swaps, 586 apresentando o front buffer resolvido, ~69 mil draws, 1761 resolves, 3522 clears, sem erro de D3D12 nem falha. O jogo usa *predicated tiling* com 3 tiles verticais (`x` = 0, 416, 832) e MSAA 2x num RT de pitch 480; os três resolves de cada frame caem nos lugares certos do front buffer (`0x08D10000`/`0x090A8000`, alternados). Screenshot em `docs\capturas\antigas\screenshot_00300.jpg`.

### O que aparecia: tela preta (resolvido no passo 11)

Até aqui o jogo só emite clears: cada frame limpa o RT principal (EDRAM 0) para preto, limpa um RT secundário (EDRAM 1080) para cinza 0,5 e resolve. Nenhum draw usa textura e só 4 shaders (2 VS + 2 PS de cor sólida) chegam ao CP. A janela mostra os frames resolvidos, que são pretos — não há "frames reconhecíveis" ainda, e o motivo não é a GPU:

O laço principal (`sub_821FCFB8`, chamado por `main` = `sub_821F9920`) é a máquina de estados do jogo, singleton em `0x824A7A24` (vtable `0x8201EC88`): estado atual em `+8`, próximo em `+20`, subsistemas em `+(índice + 7) * 4`, flag "pronto" por subsistema em `+112 + índice`. O jogo fica no **estado 2**: ao entrar, cria `UberManager`/`GameDatabase` e pede `levels/leveldata.p3d`; a transição para o estado 3 (`sub_821FB218`, vtable+8) exige que os 19 subsistemas da tabela `0x8201EAD0` respondam "pronto" ao `vtable[1](estado, tentativa)`. Pelos nomes RTTI, 15 respondem e **4 não**: `CFrontendManager` (6), `CRenderManager` (12), `CScriptManager` (14) e `CSoundManager` (15). Os quatro registram um pedido no `CScriptManager` (`sub_82230EB0` → despachante de mensagens `sub_822D2320`) e esperam a conclusão, que chamaria a máquina de estados de volta; isso nunca acontece. Nenhum dado dos `.rcf` é lido depois dos diretórios (só os 0x3C bytes de cabeçalho + diretório de cada um). A fila do gerenciador de carga (`sub_822530D0` enfileira, `sub_82252F88` processa) recebe os pedidos, mas o processamento não volta a rodar. Ou seja: falta algum serviço de sistema/kernel que destrava a carga assíncrona; a próxima investigação é nesse despachante e no que alimenta as threads de I/O (`sub_82288BB0`, ociosas nos semáforos).

Coisas descartadas no caminho (não destravaram, mas eram defeitos reais e foram corrigidos, em `runtime/kernel/`):

- `KeTimeStampBundle` agora avança (thread host a cada 1 ms, como o timer do Xenia); antes o `GetTickCount` do XAPI ficava congelado.
- `XamNotifyCreateListener`/`XNotifyGetNext` reais (`xam_notify_imports.cpp`): listener é um evento esperável; todo listener de sistema recebe XN_SYS_UI (1, 0), XN_SYS_SIGNINCHANGED ×2 e XN_SYS_STORAGEDEVICESCHANGED. O jogo cria dois listeners (`0x822957F4`, que só consulta o id 0xB, e o do jogo em `0x821FA784`) e consome as notificações. Isso também acabou com o busy loop de 46 milhões de chamadas ao stub.
- `XamUserGetSigninState`: usuário 0 logado localmente (1), os outros não; `XamUserReadProfileSettings` real (consulta de tamanho e configurações "não definidas", XOVERLAPPED completado na hora).
- `XamInputGetState`/`GetCapabilities`/`SetState` via XInput, com teclado para o controle 0 quando a janela tem foco (Enter = START, Z/Espaço = A, X = B, C = X, V = Y, setas = D-pad, Q/E = ombros); os outros controles respondem "não conectado" se não houver XInput.
- `ExGetXConfigSetting` (categoria usuário: idioma inglês, vídeo widescreen, país EUA etc., valores do Xenia), `XGetGameRegion` = 0xFFFF, `XGetLanguage` = 1.
- Relógio do driver de áudio (`audio_imports.cpp`): o callback de `XAudioRegisterRenderDriverClient` é chamado a cada 256 amostras a 48 kHz (~187/s, confirmado); os frames submetidos são descartados (sem som, sem XMA).

Diagnóstico novo no runtime: o relatório de crash/timeout traz um backtrace guest (`endereço de retorno@frame do chamador`, pela cadeia de r1), `CRASH_DUMP=endereço,bytes;...` imprime memória guest no relatório, `CRASH_LOG_IMPORTS=Nome1,Nome2` loga todas as chamadas desses imports e `CRASH_LOG_ALL_IO` loga toda leitura/escrita de arquivo.

### Faltando no renderer / bugs visuais esperados quando houver conteúdo

- **Só o nível base das texturas** (sem mipmaps: amostragem sempre com LOD 0), sem texturas 3D, arrays e cubemaps (cubemap amostra a face 0 com coordenadas aproximadas), sem formatos de vídeo (YUV) — o Bink usa texturas próprias.
- **EDRAM sem aliasing**: dados escritos num RT não aparecem em outro RT que compartilhe os mesmos tiles (por exemplo, profundidade relida como cor, ou um RT reaproveitado com outro formato). MSAA é ignorado (tudo 1x). (Passo 16: superfícies 4x agora caem no RT da largura equivalente em amostras, e os clears do D3D feitos por profundidade aparecem no RT de cor.) `k_16_16`/`k_16_16_16_16` de ponto fixo usam float, sem a faixa −32..32.
- **Resolves não escrevem na memória guest**: o que o jogo ler da CPU, ou texturas que voltem a ser carregadas no mesmo endereço depois de um resolve, vão aparecer erradas (o resolve target "esconde" a textura nova). Formatos de destino 565/1555/4444 viram RGBA8.
- Sem memexport, point sprites / tamanho de ponto, polygon mode (wireframe), clip planes do usuário, alpha-to-coverage, hi-Z, occlusion queries (`viz query` só pula draws com `kill_pix_post_hi_z`).
- Um par de máscaras e uma referência de stencil para as duas faces; constantes de blend "alpha" usam o fator de cor do D3D12.
- Upload de vértices é por draw (cópia do intervalo usado); texturas são re-hasheadas uma vez por frame em que são usadas. Pode ficar pesado em cenas grandes.
- Present sem vsync (`Present(0)`), sem gamma do display (`VdGetCurrentDisplayGamma` informa 2,2 mas a saída não aplica rampa).

### Bug do tiling predicado: só a primeira faixa recebia conteúdo (corrigido depois do passo 11)

Com o boot destravado (passo 11), a tela de carregamento aparecia cortada: as pegadas e o texto "LOADING" sumiam em `x` ≥ 416, a borda da primeira faixa. O trace dos pacotes PM4 de um frame mostrou o mecanismo do D3D:

- cada frame é reexecutado 3 vezes, com `BIN_SELECT` = `0x80000003`, `0xC` e `0x30` (2 bits por faixa; o bit 31 só na primeira passada). Window offset e scissor de cada faixa vêm de `SET_CONSTANT` predicados com máscara `0x3`/`0xC`/`0x30`, e isso funcionava (`PA_SC_WINDOW_OFFSET` = `0x7E60`, ou seja −416, na segunda faixa);
- na primeira passada, cada draw de conteúdo é seguido de um `EVENT_WRITE_EXT` predicado com máscara `0x80000000`, em que a GPU grava na memória a área da tela tocada pelo draw. No fim da passada vem um `INTERRUPT` e um `WAIT_REG_MEM`: a CPU lê essas áreas e reescreve os `SET_BIN_MASK_LO` dos draws para as passadas seguintes;
- o enum do command processor tinha `PM4_EVENT_WRITE_EXT = 0x59` (que é o `EVENT_WRITE_CFL`); o certo é `0x5A`. O pacote caía no `default` e as áreas nunca eram gravadas, então o D3D marcava todo draw como "só faixa 1" (máscara `0x80000003`) e as passadas 2 e 3 só tinham clears.

Correção em `gpu/command_processor.cpp`: opcode `0x5A`, e a área fixa "tela inteira" do Xenia (`max z` = 1; antes ia 0). Os demais opcodes do enum foram conferidos contra o `xenos.h` do Xenia.

### Cores trocadas: vermelho e azul invertidos na tela (corrigido depois do passo 12)

Quando a intro da Radical passou a rodar (passo 12), a tela saía com vermelho e azul trocados: o logo da Radical marrom com o boneco azul (o certo é azul com o "i" vermelho) e o Crash azul em vez de laranja. Antes disso só havia telas brancas e cinza, onde a troca não aparece. Causa:

- O D3D resolve o front buffer com `RB_COPY_DEST_INFO.copy_dest_swap` = 1 (log: `resolve ... swap 1`): na memória o frame fica em ordem BGRA, como um `D3DFMT_A8R8G8B8`. O resolve target replica isso (o shader de resolve troca R/B).
- Quem desfaz a troca é a fetch constant que o `VdSwap` recebe para o front buffer: `k_8_8_8_8` com swizzle `0xA0A`, ou seja R←Z, G←Y, B←X, A←1. Os fetches de textura nos shaders já aplicavam o swizzle (`xe_swizzle`), mas a apresentação lia o resolve target direto, sem ele.

Correção (`gpu/d3d12_renderer.cpp`, `Swap` e `PRESENT_PS`): a apresentação aplica o swizzle da fetch constant do front buffer, como o `RequestSwapTexture` do Xenia. O formato, o endian e o swizzle vistos são registrados no log uma vez (`[gpu] front buffer fetch constant: ...`). `CRASH_PRESENT_SWAP_RB` continua existindo, só como chave de depuração. Os outros suspeitos foram conferidos e estão certos: o mapeamento k_8_8_8_8 → `R8G8B8A8` com o swizzle aplicado no shader, o endian por textura e a troca R/B do resolve. A troca acontecia só no último passo.

Antes e depois em `docs\capturas\antigas\color_before_00600.jpg`/`00760.jpg` e `docs\capturas\antigas\color_after_00600.jpg`/`00680.jpg`/`00760.jpg`. Os números de frame não coincidem exatamente entre execuções, porque o ritmo da intro varia.

### Menu e tela de título: retângulo central, fundo preto e texto transparente (corrigido depois do passo 13)

Sintomas, comparados com uma captura do jogo no Xbox 360: na tela de título e no menu, o fundo (tecido em trama, ornamentos nos cantos, brilho no meio) aparecia só num retângulo opaco de 320×180 no centro, com o resto preto. O texto do menu saía quase transparente: o item selecionado num ciano lavado, os outros num marrom fraco. Na referência, o selecionado é ciano com contorno escuro e os outros são marrom-escuros opacos (a cor ciana do item selecionado é correta). Eram dois bugs do tradutor de shaders.

1. **W da posição invertido** (`shader_translator.cpp`, epílogo do VS). `PA_CL_VTE_CNTL.vtx_w0_fmt` = 1 quer dizer que o shader já escreveu W; com 0, ele escreveu 1/W e é preciso inverter (`kSysFlag_WNotReciprocal` do Xenia). O código fazia o contrário: invertia W quando o bit estava ligado. Os quads 2D comuns têm W = 1 e não mudavam. Os quads do fundo usam uma matriz que leva x ∈ [0, 640] a x' ∈ [−0,5, 0,5] com W = 0,5 (`c4` = 1/640, `c7` = (−0,5, −0,5, 0,40, 0,5)). Com W virando 2, o NDC caía em ±0,25: exatamente o retângulo de 1/4 da tela, centralizado. Teste de causa: com só essa correção revertida (e a do passo 14 presente), o retângulo volta (`docs\capturas\antigas\menu_only_w_reverted.jpg`).
2. **Operando constante de `MULSC`/`ADDSC`/`SUBSC` lido como registrador** (opcodes escalares 42–47). Nessas operações o bit `src3_sel` faz parte do índice do registrador temporário (`(op & 1) | (sel << 1) | (swizzle & 0x3C)`); o operando constante é sempre `c[src3_reg]`. O tradutor usava a fonte 3 normal e, com `sel` = temporário, lia `r[src3_reg]`. No shader de texto do menu (`ps f0c6741f811b92dd`), a última instrução é `alpha = c2.w × alpha`; ela virava `alpha = r2.x × alpha`, ou seja vermelho × alfa. O ciano (R ≈ 0,58) ficava com metade da opacidade, e o marrom-escuro (R baixo) quase sumia. O mesmo erro apagava a trama do fundo.

Como o texto é desenhado, para referência: cada glifo vem de um atlas `k_8` 256×256 (`tf0`) e o shader faz o bilinear à mão (4 fetches point nos cantos dos texels, offsets de 1 texel), passando cada índice por uma rampa de cor 256×1 `k_8_8_8_8` (`tf8`, D3DCOLOR ARGB com swizzle ZYXW). As rampas do texto: contorno marrom-escuro, preenchimento ciano (`E194DEEB` no fim) e laranja para BACK/SELECT.

Ferramentas novas de depuração (ficam no código):

- `CRASH_GPU_TRACE_SWAP=N` registra todos os draws e resolves do frame apresentado no swap N: estado, texturas (formato, tamanho, filtros da fetch constant e da instrução, se é resolve target, conteúdo cru das texturas 1D de até 256 texels) e os dois primeiros vértices em hex.
- `CRASH_GPU_SKIP_PS=hash1,hash2` descarta os draws desses pixel shaders, para achar qual passe desenha o quê.
- `CRASH_INPUT_SCRIPT=4000:A,60000:START,...` aperta botões do controle 0 nesses instantes (ms desde a primeira leitura do controle, 200 ms cada; `80000:RIGHT:1500` segura 1,5 s, passo 18). Serve para chegar ao menu sem ninguém na janela (`kernel/xam_input_imports.cpp`).

Antes e depois: `docs\capturas\antigas\title_before.jpg` / `docs\capturas\antigas\title_after.jpg` (swap 1750) e `docs\capturas\antigas\menu_before.jpg` (captura do passo 13) / `docs\capturas\antigas\menu_after.jpg` (swap 2250, com `CRASH_INPUT_SCRIPT=4000:A,8000:A,12000:A,16000:A,20000:A,60000:START`). Diferença que ainda resta em relação à referência: as bordas do fundo saem um pouco mais claras (o escurecimento das bordas é mais forte no console). Suspeito do passe que multiplica pelo destino (`ps fdf408078ea760bb`). Ele amostra como `k_8_8_8_8` um resolve de **profundidade** (`0x0AD72000`), que aqui vira um resolve target `R32_FLOAT` em vez dos bytes D24S8 crus. Não investigado.

### Execução atual

19/10/2026, `CRASH_TIMEOUT=20` (com o boot do passo 11): janela 1280×720, nenhuma falha, 648 swaps apresentados, ~106 mil draws (62 mil pulados: pontos com máscara de cor 0, sem RT ligado, que não desenham nada), 9498 resolves, 49 texturas, 12 shaders, 15 pipelines. A sequência é: aviso de copyright da Sierra/Radical (texto com textura, frames 1–~230) e depois a animação de carregamento ("LOADING" + pegadas atravessando a tela). Screenshots em `docs\capturas\antigas\screenshot_00300.jpg`, `00440` e `00600`. O watchdog encerra a execução (código 2) com o jogo ainda na tela de carregamento (ver passo 11).

Bugs visuais vistos até aqui: nenhum nessas duas telas (texto, alfa das pegadas e as três faixas conferidos). As limitações da lista acima ainda não foram exercitadas, porque o jogo não chegou a cenas 3D.

## Passo 11: fibers (o boot sai do estado 2)

### Causa: `SwitchToFiber` virou uma função vazia

As "threads de I/O" do jogo (objetos de `sub_822889E8`, corpo `sub_82288BB0`) não são só threads: o loader roda num **fiber do XAPI** sobre a thread principal. `sub_82288BB0` chama `ConvertThreadToFiber` (`sub_823E8B50`), o gerenciador de carga (`*0x824FBFC4`, `sub_822D9380`/`sub_822D8D60`) cria o fiber do loader com `CreateFiber` (`sub_823E8C30`, rotina `0x82288D18`, parâmetro `0x824FBF9C`) e passa a vez para ele com `sub_82288CE0` → `SwitchToFiber` (`sub_823E8D70`).

`sub_823E8D70` é só `b 0x823E9C10`, e `0x823E9C10` é o `SwitchToFiber` de verdade: assembly escrito à mão que grava r1/LR/CR/FPSCR, r14–r31, f14–f31 e v64–v127 no fiber atual (`KTHREAD+0x164`), carrega os do fiber destino e termina com um tail call para `KeSetCurrentStackPointers`, ou seja, "retorna" no meio de outra função. O XenonRecomp não tem como expressar isso: o código ficou dentro de `sub_823E9C00` (depois do `KeBugCheck` do stub de início de fiber) e `sub_823E8D70` saiu como `// ERROR 823E9C10` + `return`. Toda troca de fiber era um no-op. Por isso:

- o fiber do loader nunca rodava, e nenhum pedido da fila do gerenciador de carga era processado (`levels/leveldata.p3d`, pedido no `Enter` do estado 2, e todos os outros);
- o retry da transição também não rodava. `sub_821FB0E0` reenfileira, no mesmo gerenciador, um objeto de 16 bytes que chama `vtable[1](estado, tentativa)` de cada subsistema que ainda não respondeu. Esse objeto fica atrás dos pedidos de arquivo, então `CFrontendManager`/`CRenderManager`/`CScriptManager`/`CSoundManager` nunca eram consultados de novo e o estado 2 nunca terminava;
- nada dos `.rcf` era lido além dos diretórios, e o laço principal girava sem nada para fazer. Um efeito colateral visível: ~100 mil `XMsgStartIORequest` (XUserSetContext) por segundo. O timer de presença de `sub_821FC760` desconta o tempo desde o último frame a cada iteração do laço, não por frame, então isso é comportamento do jogo.

Layout do fiber guest (0xA50 bytes, preenchido pelo próprio `CreateFiber`):

- `+0x00` parâmetro, `+0x04` base da alocação da pilha, `+0x08` base, `+0x0C` limite;
- `+0x1C` LR salvo. Num fiber que nunca rodou é `0x823E9C00` (`mr r3,r31; bl sub_823E8D80`, que chama a rotina com o parâmetro e dá `KeBugCheck` se ela retornar);
- `+0x30` r1 (64 bits), `+0x98` em diante r14–r31 (r31 em `+0x120` = rotina de início), `+0x128` CR, `+0x130` FPSCR.

`KTHREAD+0x164` aponta para o fiber atual. `ConvertThreadToFiber` copia `KTHREAD+0xD0/+0x5C/+0x60` (base da alocação, base e limite da pilha), e o `KeSetCurrentStackPointers` do switch grava os do destino de volta.

### Correção (`kernel/fibers.cpp`)

Cada fiber guest vira um fiber do host (`CreateFiberEx` com 64 MB reservados, como as threads, e `FIBER_FLAG_FLOAT_SWITCH`), com o seu próprio `PPCContext`. Os frames recompilados de um fiber suspenso ficam na pilha host dele e continuam usando o próprio contexto. A troca é um `SwitchToFiber` do host mais a contabilidade visível para o guest: `KTHREAD+0x164`, os campos de pilha do KTHREAD e o r1 salvo no fiber de origem.

- A thread que faz a primeira troca é convertida (`ConvertThreadToFiberEx`), e o fiber dela usa o contexto da própria thread.
- Um fiber retomado por outra thread pega o r13 (PCR) dessa thread.
- Funções sobrescritas (definições fortes no lugar dos aliases fracos gerados): `sub_823E8D70` (`SwitchToFiber`), `sub_823E8C30` (`CreateFiber`, descarta um fiber host antigo no mesmo endereço), `sub_823E8BE0` (`ConvertFiberToThread`) e `sub_823E8D10` (`DeleteFiber`, apaga o fiber host).
- O relatório do watchdog acompanha o contexto do fiber que está rodando (`UpdateDiagnosticContext`).

### Dois problemas que apareceram logo em seguida

1. **Bug do XenonRecomp nas jump tables** (crash em `sub_822AA070`, na thread principal, logo que o loader começa a ler). O recompilador emite `switch (ctx.rN.u64)` com `default: __builtin_unreachable()` (`tools/XenonRecomp/XenonRecomp/recompiler.cpp`, linha do `switch ({}.u64)`). Só que o registrador do índice nem sempre tem a metade de cima zerada. Em `sub_822AA070` o índice é `lwz r3` + `addi r11,r3,1`, e com o estado -1 (`0xFFFFFFFF`) dá `r11 = 0x1_0000_0000`. O console só usa a palavra de baixo (o `cmplwi` de limite e o `rlwinm` do deslocamento na tabela), então o caso certo é o 0. O `switch` de 64 bits cai no default inalcançável e o host salta para lixo.
   - A correção certa é gerar `switch (...u32)` e regenerar `generated/` (as 380 jump tables).
   - Como esta sessão não podia mexer em `tools/`/`generated/`, `cpu/jump_table_workaround.cpp` teve por um tempo uma cópia da função gerada com essa única mudança. O recompilador já foi corrigido (`switch` no `.u32`, ver README) e o arquivo foi apagado.
2. **`XMACreateContext` era stub** e não devolvia contexto (escrita no endereço 0 numa thread de áudio). Agora `kernel/audio_imports.cpp` entrega um dos 320 contextos de 64 bytes do decodificador XMA, em memória física, como o `XmaDecoder::AllocateContext` do Xenia, e `XMAReleaseContext` devolve. Não há decodificador: contextos "chutados" pelos registradores XMA (`0x7FEA0000+`, escritas que caem no commit sob demanda) nunca produzem amostras.

### Execução atual

19/10/2026, `CRASH_TIMEOUT=30` e `60`, sem falha:

- **Fibers**: o fiber do loader roda (`[fiber] fiber 0x400051B0 starts: routine 0x82288D18(0x824FBF9C)`), e a thread principal alterna entre ele e o fiber principal a cada serviço do gerenciador de carga.
- **Dados lidos** (com `CRASH_LOG_ALL_IO`): 99 leituras de `default.rcf` (até 0x23800 bytes cada, offsets até ~0x4A0000), 17 de `sound1.rcf`, 5 de `sound2.rcf` e 2 de `english.rcf`, mais 10 shaders (`reflect*`, `refract*`, `particle*`, `palreflectfp`, `palparticleshaderfp`).
- **Máquina de estados**: estado 2 → 0 (transição em `sub_821FB218`, porque `sub_82233140()+176` = 0) → 3 (`lr=0x820F5D40`).
- **Frames com textura**: 49 texturas, 12 shaders e 15 pipelines. A tela é a animação de carregamento, com pegadas andando sobre fundo preto (capturas não guardadas), a ~30 fps.

### Próximo ponto de travamento (não investigado a fundo)

(Resolvido no passo 12: era o XMA.) Depois de passar ao estado 3, a leitura de `default.rcf` para (as últimas leituras acontecem nos primeiros ~10 s) e o jogo fica na tela de carregamento. O candidato principal é o **áudio XMA**. Uma thread de áudio (`r13=0x7F1C2000`, backtrace `822CC6D4 ← 822CC848 ← 822CC87C ← 822B6C40 ← 822C4724 ← 822B4D6C`) fica em `sub_822CC530`, num laço que espera 50 µs (`sub_822AD4E8`) até haver amostras decodificadas suficientes nos buffers de saída do contexto XMA. Sem decodificador, isso nunca acontece. Se o carregamento do estado 3 depende dos bancos de som, o próximo passo é um decodificador XMA. Um primeiro passo possível é um decodificador que só consome a entrada e produz silêncio, avançando os ponteiros de leitura/escrita do contexto como o hardware faz.

## Passo 12: áudio (decodificador XMA e driver de render do XAudio)

### Como o jogo usa o XMA

O jogo não usa o XAudio2 do XDK para tocar XMA: o motor de som da Radical (`audio::hal::sw`, mixagem em software) usa a biblioteca de playback XMA do XDK (`sub_823EDF50` e vizinhas), que fala direto com o hardware:

- `XMACreateContext` devolve um contexto de 64 bytes. Uma vez, na inicialização, o jogo lê o endereço físico do array de contextos no **registrador `0x600`** (`sub_823ED260`) e calcula o índice de cada contexto a partir dele: `(MmGetPhysicalAddress(ctx) - base) / 64`. Antes este registrador lia 0 (commit sob demanda), e os índices saíam gigantes.
- Os registradores ficam em `0x7FEA0000 + reg*4` e são **little-endian** (`lwbrx`/`stwbrx`). Um bitmask em `0x650+n` "chuta" (habilita) os contextos `32n..32n+31`, `0x690+n` trava (desabilita) e `0x6A0+n` limpa. O `0x606` informa "ocupado" como `índice | 0x200`; aqui fica 0, porque a trava espera o decodificador terminar.
- Para mexer num contexto, o jogo:
  1. trava (`sub_823EE020`);
  2. espera (`sub_823EE0A0`/`sub_823EE168`, que copia o contexto para uma sombra);
  3. altera a sombra: `sub_823ED580` submete buffers de entrada, `sub_823ED7D8` consome a saída (avança o read offset e religa "output valid") e `sub_823EDAC0` reinicia o stream;
  4. copia a sombra de volta e chuta (`sub_823EE2A8`).
- O streamer da Radical (`sub_822CC530`, chamado por `sub_822CC860` para ler N amostras de um clipe) espera em laço de 50 µs até haver amostras decodificadas. Era aqui que o carregamento travava. Os clipes são decodificados inteiros para a memória, um por vez (cria contexto → decodifica → libera): 106 clipes nos primeiros ~25 s.

Layout do contexto: dwords big-endian, campos contados a partir do bit 0. Segue o `XMA_CONTEXT_DATA` do Xenia e foi conferido contra as funções acima.

| Dword | Campos |
|---|---|
| 0 | pacotes do buffer 0 :12, loop count :8, buffer 0 válido :1, buffer 1 válido :1, blocos de saída :5, write offset :5 |
| 1 | pacotes do buffer 1 :12, subframes de loop :8, subframe decode count :4, padding :3, sample rate :2 (24/32/44,1/48 kHz), estéreo :1, ? :1, output válido :1 |
| 2 | read offset de entrada :26, em bits a partir do início do buffer atual, cabeçalhos incluídos |
| 3 | início do loop :26 |
| 4 | fim do loop :26, metadata :5, buffer atual :1 |
| 5, 6, 7 | buffers de entrada 0 e 1 e buffer de saída (endereços físicos) |
| 9 | read offset de saída :5 |

### Implementação (`runtime/audio/`)

- **Registradores**: a página `0x7FEA1000` (registradores `0x400–0x7FF`) fica só de leitura. As stores do jogo nela compilam para `mov [mem], r32` (ou `mov [mem], imm32`). O handler vetorado decodifica a instrução, pega o valor do registrador x86, avança o RIP e trata a escrita, **sem nunca deixar a página gravável**. Assim, escritas de outras threads não escapam (o trap da GPU, ao contrário, faz single-step). Se aparecer outra forma de instrução, o handler cai no single-step. Em 30 s foram ~11.700 stores emuladas e nenhuma por single-step.
  - A janela fica fora do commit sob demanda (`memory.h`/`memory.cpp`).
  - O array de contextos é alocado no topo do heap físico (`AllocatePhysicalMemory(..., topDown)`, `0xBFFFB000`), longe das alocações do jogo.
- **Kick/lock/clear**: o kick marca o contexto como habilitado e acorda a thread decodificadora; o contexto fica habilitado até o lock. O lock pega o mutex do contexto (espera um passe de decodificação em andamento) e desabilita. O clear zera os bits de válido e os offsets de saída, como o `XmaContext::Clear` do Xenia, e reinicia o estado do decodificador.
- **Parser**: segue o stream do jeito que o hardware o descreve.
  - Pacotes de 2048 bytes com cabeçalho de 32 bits: frames que começam no pacote :6, offset em bits do primeiro frame :15, metadata :3, pacotes a pular até o próximo deste stream :8.
  - Frames que atravessam pacotes (pulando cabeçalhos e pacotes de outros streams) e passam de um buffer de entrada para o outro.
  - Um buffer consumido é liberado (bit de válido zerado, `current buffer` troca) assim que o último frame dele é lido, para o jogo reabastecer. Loops (`loop count`, início e fim) são tratados.
  - O fim dos frames de um pacote vem da **contagem de frames do cabeçalho**, e não de "sobram menos de 15 bits": o campo de tamanho de um frame pode atravessar a borda do pacote. Esse era o último bug. Sem a contagem, um clipe de 245 frames saía com 240, o jogo esperava as amostras que faltavam e travava de novo.
- **Decodificação**: cada frame extraído vai para o decodificador **WMA Pro** do FFmpeg (frames XMA são frames WMA Pro com decode flags `0x10D6`), dentro de um pacote WMA Pro sintético de 2048 bytes: sequência :4, :2, bits do frame anterior :15, o frame e padding com 1s. Frames maiores que um pacote são divididos em dois.
  - Não usei o decodificador `xma2` do FFmpeg porque ele segura 4096 amostras e corta amostras no início e no fim. Isso quebra a contabilidade de 512 amostras por frame que o jogo espera.
  - Cada frame vira exatamente 512 amostras por canal. Dois casos são completados com silêncio: o primeiro frame de um stream (que o WMA Pro não emite, por causa do overlap) e o último frame, quando tem `trim_end`.
  - A saída é PCM de 16 bits big-endian, intercalado em estéreo, escrito em blocos de 256 bytes no anel de saída. Quando o write offset alcança o read offset, "output valid" é zerado, e o jogo passa a tratar o anel como cheio (`sub_823ED760`).
- **FFmpeg**: build LGPL shared do BtbN, FFmpeg 8.1 (`ffmpeg-n8.1-latest-win64-lgpl-shared-8.1`, o link "Windows builds by BtbN" do ffmpeg.org), em `thirdparty/ffmpeg`. Só ficaram `include/libavcodec`, `include/libavutil`, `avcodec-62.dll`, `avutil-60.dll`, `swresample-6.dll` e a licença (`thirdparty/ffmpeg/README.md` tem a origem).
  - As DLLs são carregadas com `LoadLibrary`, de ao lado do exe (o CMake as copia para `build\`, o `make_dist` para `dist\`). A versão é conferida contra os headers.
  - Sem as DLLs, o runtime compila e roda, e o XMA vira silêncio: o jogo continua avançando, só que mudo.
- **Driver de render** (`kernel/audio_imports.cpp` + `audio/audio_output.cpp`):
  - Os frames de `XAudioSubmitRenderDriverFrame` têm 256 amostras × 6 canais, **planares**, em floats big-endian (FL, FR, C, LFE, SL, SR).
  - Downmix para estéreo: `L = FL + 0,707·C + 0,707·SL` (idem à direita), LFE descartado, com clamp.
  - Saída por XAudio2: voz estéreo float a 48 kHz; o XAudio2 converte para o formato do dispositivo (aqui, 8 canais).
  - O relógio continua chamando o callback a cada 256 amostras (~187/s), mas ajusta o passo para manter ~8 frames (~43 ms) na fila do dispositivo. Se o dispositivo parar de consumir por 1 s, o jogo segue sem som.
  - `XAudioGetVoiceCategoryVolume` devolve 1,0, e a máscara de mudança é 0.

Variáveis de ambiente novas:

| Variável | Efeito |
|---|---|
| `CRASH_NOAUDIO=1` | Não abre dispositivo de áudio (para testes automáticos). O relógio fica em tempo real e a decodificação XMA continua |
| `CRASH_AUDIO_DUMP=<arquivo.wav>` | Grava a mixagem final (estéreo, 16 bits, 48 kHz), por no máximo `CRASH_AUDIO_DUMP_SECONDS` segundos (padrão 60) |
| `CRASH_XMA_DUMP=<pasta>` | Grava cada stream XMA decodificado (`xma_<contexto>_<n>.wav`, na taxa e no número de canais do stream) |
| `CRASH_XMA_TRACE=1` | Loga cada buffer de entrada enviado a um contexto, com os cabeçalhos dos primeiros pacotes (passo 20) |

O cabeçalho dos WAVs é reescrito a cada segundo de áudio, então eles continuam válidos quando o watchdog encerra o processo. Os WAVs ficam só em `build\`: são dados do jogo e não devem ser distribuídos.

O relatório de crash/timeout agora traz o estado do XMA e do áudio:

- XMA: contadores e, para cada contexto alocado, buffers, offsets, formato e amostras pendentes;
- áudio: callbacks, frames submetidos, frames com som, pico e fila do dispositivo.

### Execução atual

19/10/2026, `CRASH_TIMEOUT=30 CRASH_NOAUDIO=1` (log não guardado). Nenhuma falha; o código 2 é o do watchdog.

- **XMA**: 106 clipes decodificados (mono e estéreo, 24/32/44,1/48 kHz), 5342 frames, 21.792 blocos de saída e **nenhum erro do decodificador**. 80 frames foram completados com silêncio, todos o último frame de um clipe com `trim_end`. Os clipes em `build\xma_dump\` têm conteúdo de verdade: RMS de ~3.500 a ~8.000, picos perto do fundo de escala e 512 amostras de silêncio no início, como esperado.
- **Mixagem**: o jogo submeteu 5545 frames, 3273 com som (pico 0,98). `docs\capturas\antigas\audio_mix.wav` (30 s): silêncio até ~5 s, som de ~5 a ~24 s e silêncio depois (o jogo passa para o vídeo; ver abaixo).
- **Com dispositivo** (sem `CRASH_NOAUDIO`): `XAudio2 output: device 8 ch at 48000 Hz`, ~188 callbacks/s, fila estável em ~6 frames e nenhum frame descartado.
- **Tela**: o carregamento termina. Depois da tela de LOADING, o jogo roda a **abertura da Radical em 3D** (fliperama com o Crash, swap ~600) e o logo da Radical Entertainment (swap ~760), com som. (Capturas não guardadas.)

### Próximo ponto de falha: os vídeos não existem nesta cópia

Logo depois do logo (~25 s), a thread de I/O tenta abrir `D:\movies\sierra_360.bik` e repete para sempre, com a tela preta: ~22 mil `NtCreateFile` em 5 s, todos com `STATUS_OBJECT_PATH_NOT_FOUND`. A pasta `movies\` não existe na cópia do usuário, que só tem os `.rcf`, `shaders\` e o `default.xex`.

Caminho do erro: `sub_82297A44` → `sub_8228F154` → `sub_82296E6C` (`CreateFile` do XAPI). Na falha, `sub_82296A90` converte o `GetLastError` num código interno. Os erros 2 (`FILE_NOT_FOUND`) e 3 (`PATH_NOT_FOUND`) dão o mesmo código, então trocar o NTSTATUS não muda nada.

Caminhos possíveis:

- copiar a pasta `movies\` do disco do usuário para a pasta do jogo (ou para um mod);
- descobrir no código o que faz o jogo pular um vídeo.

Depois disso vem a reprodução de Bink: texturas YUV e o áudio do Bink, que usa o mesmo driver de render.

Pendências do áudio:

- o XMA não gera interrupções (o jogo faz polling, então elas não fizeram falta);
- `subframe decode count` e os subframes de loop são ignorados (o decodificador sempre trabalha com frames inteiros);
- o anel de 8 KB por canal do streamer recebe blocos de 256 bytes, e não conferi contra o hardware real se a granularidade bate;
- não ouvi o áudio nesta sessão: a validação foi só pelos números e pelos WAVs.

## Passo 13: o vídeo da Sierra que falta (`RtlNtStatusToDosError`)

### O sintoma

Depois do logo da Radical, a thread de I/O tentava abrir `D:\movies\sierra_360.bik` milhares de vezes por segundo, com a tela preta. O executável referencia 5 vídeos: `movies\sierra_360` (sem `_51`), `radical_360_51`, `dolby_360_51`, `attract_360_51` e `shadow_360_51`. O `default.rcf` tem só os 4 `_51`, e `sierra_360.bik` não está em nenhum `.rcf` nem na pasta do jogo. **A cópia do usuário não tem esse arquivo**: provavelmente era um arquivo solto no disco. Se ele aparecer, basta colocá-lo num mod (`mods\<mod>\movies\sierra_360.bik`): o overlay procura arquivo por arquivo, então funciona mesmo sem `movies\` na pasta do jogo.

### A causa: `GetLastError` voltava 0

Como a camada de arquivos da Radical trata uma falha de abertura (drive `"DVD"`, objeto `0xA00147B0`):

1. A thread do drive (`sub_82297940`) executa o pedido de abertura (`sub_8228F0C0`). Ela procura o nome nos `.rcf` montados e, se não acha, chama o `CreateFile` do XAPI (`sub_82296E6C`). Em caso de falha, `sub_82296A90` converte o `GetLastError()` num código interno, guardado em `drive+208`: 0 → 0 (sem erro), 2/3/15 → 1 (não encontrado), 21 → 2, 1005 → 6, 1167 → 4, os demais → 8.
2. O modo de erro do drive (`drive+92`) é 2: em vez de falhar o pedido, a thread do drive levanta o erro (`sub_82293CA0`: `drive+112` = pendente, `drive+116` = pedido) e espera uma resposta.
3. O serviço do drive (`sub_82294008`) chama o callback de erro que o jogo registrou (`drive+104`) com o código de `drive+208` e o nome do arquivo, e passa a resposta adiante com `sub_82293BD8`: 1 = tentar de novo (o pedido volta para a fila), 0 = falhar.

O XAPI preenche o `GetLastError` com `RtlNtStatusToDosError`, que ainda era um stub e devolvia 0. O código interno ficava 0 ("sem erro"), o callback do jogo respondia **1** e o pedido voltava para a fila para sempre. Os erros 2 e 3 dão o mesmo código interno, então `STATUS_OBJECT_NAME_NOT_FOUND` e `STATUS_OBJECT_PATH_NOT_FOUND` têm o mesmo efeito. Por isso não mudei o VFS para fingir que `D:\movies\` existe: não teria efeito, e não há como saber se o disco tem essa pasta. Se o arquivo não está nos `.rcf`, provavelmente nem a pasta existe no disco, e `PATH_NOT_FOUND` é a resposta certa.

### A correção

`RtlNtStatusToDosError` real (`kernel/rtl_imports.cpp`), com a tabela do Windows para os status que o kernel do runtime devolve. Status `FACILITY_WIN32` voltam o código embutido; os não mapeados voltam `ERROR_MR_MID_NOT_FOUND` (317), como no Windows. Com `STATUS_OBJECT_PATH_NOT_FOUND` → 3 → código interno 1, o callback responde **0**: o jogo tenta abrir uma vez só, desiste do vídeo e segue.

Para a investigação, usei funções sobrescritas temporárias (definições fortes de `sub_82293CA0`/`sub_82293C38`/`sub_82293BD8`/`sub_82297188` que logavam e chamavam o original). Já foram removidas.

### Execução atual

19/10/2026, `CRASH_NOAUDIO=1`, com o usuário na janela:

- **O vídeo da Sierra é pulado**: uma única tentativa, `RtlNtStatusToDosError(0xC000003A) -> 3`, e o callback responde 0. Em seguida, o jogo cria uma thread nova (`0x82433BA0`, provavelmente o player do Bink) e toca a **abertura em teatro de sombras** (`shadow_360_51.bik`, do `default.rcf`) pelo Bink. A imagem anda (palmeira, Cortex, Nina) e o som também: a mixagem tem áudio contínuo de ~25 s em diante. (Capturas não guardadas) (swap 1100).
- **Menu principal**: com Espaço (= A) o usuário pulou as intros e chegou ao menu (NEW GAME / LOAD GAME / CREDITS / CALIBRATION, "STORAGE DEVICE", botões B/Y). Screenshots dos swaps 2000 e 3000. A execução terminou quando o usuário fechou a janela (código 0).
- Em ~127 s: 15.734 frames XMA, nenhum erro do decodificador, 3788 swaps.

### Próximo ponto de falha / pendências

- ~~Bug visual no menu~~ (resolvido): o retângulo amarelo opaco no centro era o fundo desenhado com 1/4 do tamanho, por causa do W invertido no tradutor de shaders. Ver "Menu e tela de título" no passo 10. O passo 14 atribuiu isso ao XMP, mas o teste com só a correção de W revertida mostra o retângulo de volta.
- **Bink**: os vídeos tocam; falta conferir cores e sincronia.
- **Próximo teste**: entrar em NEW GAME, que deve exercitar `XamShowDeviceSelectorUI`/`XamContent*` ("STORAGE DEVICE"; os `XamContent*` ainda são stubs) e o carregamento da primeira fase.

## Passo 14: música do menu (XMP, o Xbox Music Player)

### O sintoma

No menu principal, os efeitos sonoros tocavam (cliques), mas a música não. O decodificador XMA decodificava a música normalmente: um stream estéreo de 48 kHz contínuo, com conteúdo real no `CRASH_XMA_DUMP`, consumido pelo jogo. Mesmo assim, a mixagem ficava em silêncio. Ou seja, não era a decodificação: o jogo calava a própria trilha.

### A causa

No Xbox 360, se o usuário toca a própria música pela dashboard, o jogo tem que silenciar a trilha dele (regra de certificação). O jogo consulta o Xbox Music Player (app `0xFA` do XAM) por `XMsgInProcessCall`:

| Mensagem | Wrapper | O que faz |
|---|---|---|
| `0x00070009` `XMPGetStatus` | `sub_822642C8`, via `sub_822644A0` | estado do player, `{ client, u32* state }`; consultado a cada frame |
| `0x0007001B` `XMPGetPlaybackController` | `sub_822643E0` | `{ client, u32* controller, u32* locked }` |
| `0x0007001A` `XMPSetPlaybackController` | `sub_82264338` | assíncrono, via `XMsgStartIORequestEx` |

`sub_822644B0` e `sub_82264588` são o `XMPOverrideBackgroundMusic` e o `XMPRestoreBackgroundMusic`.

`sub_82247818` devolve "o player está parado" (`state == 0`). Com o stub, nada era escrito, `state` ficava com o lixo da pilha (≠ 0), e o jogo concluía que havia música do usuário tocando.

### A correção (`kernel/xmsg_imports.cpp`)

`XMsgInProcessCall`, `XMsgStartIORequest` e `XMsgStartIORequestEx` reais para o XMP, seguindo o `xmp_app.cc` do Xenia: o player está sempre parado (estado 0), o título tem o controle (controller 0, sem lock) e o volume é 1,0. As demais mensagens do XMP (playlist, set controller/volume) dão sucesso sem fazer nada. Pedidos assíncronos com `XOVERLAPPED` terminam na hora e devolvem `ERROR_IO_PENDING`. Mensagens de outros apps (por exemplo o `XUserSetContext`, app `0xFB`, chamado ~100 mil vezes por segundo) continuam devolvendo sucesso sem efeito. O `CompleteOverlapped` de `xam_user_imports.cpp` virou `CompleteXOverlapped`, declarado em `kernel.h`, para ser compartilhado.

### Execução atual

19/10/2026, `CRASH_NOAUDIO=1 CRASH_INPUT_SCRIPT="27000:A,29000:A"` (pula as intros) com `CRASH_AUDIO_DUMP`:

- A mixagem tem som contínuo no menu, de ~28 s até o fim da execução (RMS ~4.000–6.500 por segundo). Antes, ficava em zero a partir de ~29 s.
- O menu mudou junto: o "retângulo amarelo" do passo 13 sumiu. Agora o fundo amarelo com ornamentos cobre a tela inteira, com as opções legíveis (captura não guardada). Pelo jeito, a animação do fundo segue a música e ficava parada num quadro intermediário. **Correção posterior:** o retângulo não tinha relação com o XMP. A execução já incluía a correção do W da posição feita ao mesmo tempo no tradutor de shaders (passo 10, "Menu e tela de título"); com só ela revertida, o retângulo volta mesmo com o XMP corrigido.

Sobre "fazer dump de todos os áudios do jogo": não resolveria nada aqui, porque o áudio já era decodificado certo. Para conferir a decodificação, `CRASH_XMA_DUMP=<pasta>` grava todo stream XMA que o jogo toca, já em WAV. Extrair os bancos de som dos `.rcf` offline exigiria um extrator do formato RCF/bancos da Radical (ainda não existe no projeto).

## Passo 15: seletor de dispositivo, saves (XamContent*) e relógio

### O sintoma

O jogo congelava em dois lugares:

- ao apertar Y ("STORAGE DEVICE") no menu;
- em NEW GAME → nome → dificuldade, quando ia para a escolha de slot.

Nos dois casos ele chamava `XamShowDeviceSelectorUI`, ainda um stub, e esperava para sempre pelo `XOVERLAPPED`. Os `XamContent*` e o `XamEnumerate` também eram stubs.

### O que foi feito

- **`kernel/xam_content_imports.cpp`** (novo):
  - **`XamShowDeviceSelectorUI`**: escolhe na hora o único dispositivo, um "Hard Drive" (id 1, tipo HDD, 20 GB, 16 GB livres; o dummy HDD do Xenia). Grava o id no ponteiro de saída, envia `XN_SYS_UI` 1/0 e completa o `XOVERLAPPED`.
  - **`XamShowMessageBoxUIEx`**: loga título, texto e botões, responde com o botão em foco e envia `XN_SYS_UI` 1/0.
  - **`XamContentGetDeviceState` / `XamContentGetDeviceData`**: a estrutura de 0x50 bytes do dispositivo.
  - **`XamContentCreateEx`**: as 5 disposições (create new / create always / open existing / open always / truncate existing), saída 1 = criado e 2 = aberto. O pacote é montado no VFS com o nome de raiz pedido pelo jogo (aqui `s0:`).
  - **`XamContentClose`** (desmonta), **`XamContentFlush`**, **`XamContentDelete`**, **`XamContentGetCreator`** (o perfil local criou tudo) e **`XamContentSetThumbnail`**.
  - **`XamContentCreateEnumerator` + `XamEnumerate`**: lista os pacotes do tipo pedido e devolve `ERROR_NO_MORE_FILES` no fim. Com `XOVERLAPPED`, o número de itens vai no campo de tamanho, via `CompleteXOverlappedEx`.
  - Todas as variantes assíncronas completam na hora (`ERROR_IO_PENDING` + overlapped preenchido + evento sinalizado), como o `CompleteOverlappedImmediate` do Xenia.
- **VFS** (`fs/vfs.*`): montagens dinâmicas `MountContent(raiz, pasta)` e `UnmountContent(raiz)`. `"<raiz>:\..."` resolve para a pasta do pacote (gravável) enquanto ele está aberto. As montagens nunca são liberadas: handles abertos guardam o ponteiro.
- **Notificações** (`xam_notify_imports.cpp`): `BroadcastXNotification(id, param)` entrega a todos os listeners cuja máscara cobre a área da notificação (bits 25–30 do id; bit 0 = sistema). O `XNotifyGetNext` já devolvia FALSE com a fila vazia, sem bloquear, como no console, então não havia busy loop dele aqui.
- **Title ID**: o loader lê o campo de execução (`0x00040006`) do cabeçalho XEX. `CurrentTitleId()` = `565507E4`.
- **Relógio**: `KeQuerySystemTime` (hora UTC do host em FILETIME), `RtlTimeToTimeFields` e `RtlTimeFieldsToTime` reais (`kernel/rtl_imports.cpp`). Com os stubs, a data do save saía como "Last Save: 255.2.35".
- `objects.h`: tipo `OBJECT_ENUMERATOR` (0x81) para os handles de enumerador. `kernel.h`: `CompleteXOverlappedEx` e `BroadcastXNotification`.

### Onde ficam os saves

```
userdata\content\<XUID>\<title id>\<tipo>\<nome do arquivo>\          arquivos do pacote
userdata\content\<XUID>\<title id>\<tipo>\<nome do arquivo>.header    XCONTENT_DATA (0x134 bytes, como o jogo passou)
userdata\content\<XUID>\<title id>\<tipo>\<nome do arquivo>.thumbnail imagem (se o jogo definir)
```

- XUID = `B13EBABEBABEBABE` (o perfil local, usuário 0; o mesmo default do Xenia) para saves (tipo 1); `0000000000000000` para os outros tipos.
- Exemplo real: `userdata\content\B13EBABEBABEBABE\565507E4\00000001\CrashOTT GameSlot 1\CrashOTT GameSlot 1` (1509 bytes). Nome de exibição: "Slot: 1  Name: AA  Last Save: …  Time: 00:00". O jogo cria com flags `0x12` (create always) e grava 4 + 0x5E1 bytes, reescrevendo depois os 4 primeiros.

### Execução atual

19/10/2026, `CRASH_NOAUDIO=1` com `CRASH_INPUT_SCRIPT` (START, START para pular os dois filmes; A, A, A, Y para NEW GAME e o nome "AA"; A para a dificuldade NORMAL; A para o slot):

- **STORAGE DEVICE (Y)**: abre e fecha o "seletor" na hora, e o menu continua respondendo.
- **NEW GAME**: nome → dificuldade → "CHOOSE GAME SLOT", com os 5 slots "EMPTY". A tela re-escaneia os saves a cada frame (`sub_822959D8`: `XamContentGetDeviceData` → `XamContentCreateEnumerator` → `XamEnumerate` → `CloseHandle`), o que é comportamento do jogo, não travamento. Escolhendo o slot 1, o save é criado e gravado em `userdata\` (acima).
- Com o relógio corrigido, um segundo save (slot 2) sai com "Last Save: 10.19.26" (MM.DD.AA). Antes de criar o slot 2, o jogo abre o slot 1 existente (flags `0x13`, open always).
- **LOAD GAME**: lista o save (slot 1 "AA", com a miniatura do nível) (captura não guardada).

### Ponto de falha (resolvido no passo 16): função engolida no config (`0x8210EB98`)

Depois de salvar, o jogo começa a carregar a primeira fase e cai em `[fatal] indirect call to unmapped guest address 0x8210EB98 (ctr), lr=0x822E4660`.

`0x8210EB98` (`stfs f1,48(r3)` / `blr`, 8 bytes) é uma função folha usada como callback: o endereço é montado no código com `lis 0x8211` / `addi -5224` em pelo menos 5 lugares, como em `ppc_recomp.10/11/12/13/20`. Ela ficou dentro da função explícita `{ address = 0x8210EB08, size = 0x98 }` do `config/Crash.toml`.

`0x8210EB08` tem uma jump table (em `0x8210EB40`) com os casos `0x8210EB68`/`74`/`80`/`8C`, que pertencem a ela. Só `0x8210EB98` vem depois dos casos e não é alvo da tabela.

Correção proposta aqui (aplicada no passo 16, junto com os outros casos que o script novo encontrou):

- trocar a entrada por `{ address = 0x8210EB08, size = 0x90 }`;
- acrescentar `{ address = 0x8210EB98, size = 0x8 }`;
- regenerar `generated/ppc` e recompilar o `CrashPPC`.

Vale procurar outros casos iguais: endereços montados com `lis`/`addi` que caem dentro de funções explícitas e não são início de função. (Feito no passo 16 com `tools/find_swallowed.py`.)

## Passo 16: funções engolidas, primeira fase e 3D da fase

### Funções engolidas: correção sistemática (`tools/find_swallowed.py`)

O `[fatal] indirect call to unmapped guest address 0x8210EB98` do passo 15 era o mesmo tipo de bug do `0x82289948` (passo 8). Em vez de corrigir um por um, o script `tools/find_swallowed.py` (Python puro, sem dependências; detalhes no README, seção "Funções engolidas") procura todos:

- **Candidatos** (19.610): palavras de 32 bits alinhadas nas seções de dados (`.rdata`, `.data`, `BINK*`, `.XBMOVIE`) que apontam para código; endereços montados no código com `lis rX,hi` + `addi`/`ori rY,rX,lo` (acompanhando quais registradores cada instrução sobrescreve, para não juntar um `lis` antigo com um `addi` que usa outro valor); alvos de `bl`. O `.reloc` da imagem não ajuda: o XEX não traz relocações e a seção contém outros dados.
- **Funções geradas**: nomes e endereços do `ppc_func_mapping.cpp`; o tamanho de cada uma é o número de comentários de instrução (`\t// ...`) no corpo em `ppc_recomp.*.cpp` × 4.
- **Classificação** dos 632 candidatos que não são início de função: `table` (366, o endereço da própria jump table logo depois do `bctr`), `fallthru` (221, quase todos em tabelas de escopo de SEH/C++ EH em `0x8206C2xx` e `0x8203xxxx`, em que a instrução anterior continua no endereço), `label` (45, alvos de desvios internos ou labels do `Crash_switch_tables.toml`) e `swallowed` (os casos reais: a palavra anterior é `blr`/`bctr`/`b`/padding e nenhum desvio interno chega ali).
- Casos reais encontrados (todos em faixas explícitas do config): `0x8210EB98` (em `0x8210EB08`, callback montado com `lis`/`addi` em 5 lugares), `0x82134778` (em `0x82134760`: `0x82134760` é um thunk de 6 instruções que termina em `bctr`; `0x82134778` está na vtable `0x82013BE8`), `0x822D4D28` e `0x822D4D50` (em `0x822D4928`, callbacks guardados em estruturas). Os dois últimos terminam com `b 0x822D4B88`, a cauda comum de outra função que também estava dentro da mesma faixa. O `--suggest` acha essas caudas sozinho (alvos de `b` que passariam a ficar fora da faixa) e confere que a base, os labels e o default de cada jump table continuam na mesma faixa.
- Um candidato fica de fora de propósito: `0x823E6344`, a continuação passada ao `RtlUnwind` (`0x8242FC24`) em `sub_823E62E0`. É uma entrada secundária legítima (vem depois de um `bl`), e o `RtlUnwind` do runtime não salta para ela.

`config/Crash.toml` passou de 63 para **68 funções explícitas**: `0x8210EB08` (0x90) + `0x8210EB98` (0x8); `0x82134760` (0x18) + `0x82134778` (0x94); `0x822D4928` (0x25C) + `0x822D4B88` (0x19C) + `0x822D4D28` (0x28) + `0x822D4D50` (0xC). Regenerado: **20.144 funções** (eram 20.139), 0 `Unrecognized instruction`, 0 `Switch case ... outside function`, os mesmos 71 `// ERROR` de antes, e `find_swallowed.py` termina com `swallowed: 0`.

**Runtime** (`kernel/xex_loader.cpp`):

- O `[fatal]` de chamada indireta agora diz onde o alvo caiu: fora do código ("bad or uninitialized function pointer") ou dentro de qual função gerada, com o deslocamento, e se vem logo depois de `blr`/`b`/padding ("a function swallowed by the recompiler config"). A mensagem aponta para o script.
- Na carga aparece `[loader] N data words point into code; M point at unmapped code after blr/b/padding` (hoje 18.235 e 0). É a parte "ponteiros nos dados" do script, sem os endereços montados no código; se M > 0, a lista vem em seguida. Não testei com um build quebrado de propósito; a lógica é a mesma do script, que encontrou `0x82134778` por esse caminho.

### Até a primeira fase

Com a regeneração, o carregamento passa. Roteiro de entrada usado (os 5 slots já existem em `userdata\content`, então a escolha do slot pede confirmação: DOWN + A = "YES"):

```bat
set CRASH_NOAUDIO=1
set CRASH_INPUT_SCRIPT=20000:START,24000:START,28000:START,32000:A,35000:A,37000:A,39000:Y,43000:A,55000:A,58000:DOWN,59500:A,62000:A,65000:A
```

(START ×3 pula as intros e o título; A = NEW GAME; A, A = nome "AA"; Y = pronto; A = NORMAL; A = slot 1; DOWN, A = sobrescrever; A = "Save successful / CONTINUE".) Com slots livres, troque `58000:DOWN,59500:A` por `RIGHT`s antes do `55000:A`. Pelo LOAD GAME também funciona, mas precisa de mais um "continuar história" e cai na tela de estatísticas.

Resultado: a fase carrega, toca a cena de abertura em tempo real ("Episode 1 — A New Hop": Crash, Coco, Uka Uka, a nave do Neo Cortex), e depois o jogo fica controlável na vila, com HUD. Execuções de 230–330 s sem falha; o watchdog encerra. Antes/depois: `docs\capturas\antigas\level_before.jpg` (último frame antes do fatal, "Saving..."), `docs\capturas\antigas\level_after_intro.jpg` e `docs\capturas\antigas\level_after_gameplay.jpg`.

### 3D da fase: sombras, faixas verticais e escurecimento

Sintomas (vistos pelo usuário e nos screenshots): faixas verticais mais claras em `x` = 400–448, 816–864 e 1232–1280 (as emendas das 3 faixas do tiling predicado), a tela inteira escurecida e a "sombra por cima dos personagens". Antes: `docs\capturas\antigas\level3d_before_bands.jpg`; depois: `docs\capturas\antigas\level3d_after.jpg`.

O passe responsável é `ps fdf408078ea760bb` (`shaders\shadowshaderfp.out`): sombras por stencil. Em cada faixa, o jogo desenha volumes de sombra que só mexem no stencil (`depthctl 0x1C7187B3`: frente `INCR_WRAP`, verso `DECR_WRAP` no z-pass, cor desligada), resolve depth+stencil para `0x0AD72000` e desenha um quad de tela inteira que multiplica a cor (`blend 0x00080008` = src × dst) por `1 − Σ peso × (1 − cor)` sobre 9 amostras (PCF) cujo **stencil** é > 0. O stencil é lido amostrando o resolve de profundidade como `k_8_8_8_8` e pegando o byte baixo (`t.z` depois do swizzle `0x60A`), porque no Xenos a palavra é `(profundidade24 << 8) | stencil`. Havia dois bugs:

1. **Resolve de profundidade sem stencil**. O renderer guardava resolves de profundidade só como `R32_FLOAT`. Agora cada resolve de profundidade também escreve uma cópia "empacotada" em RGBA8 (R = stencil; G/B/A = os 24 bits de profundidade, em 20e4 para `D24FS8`, como o `PreClampedFloat32To20e4` do Xenia, ou em unorm24). Um fetch desse endereço com formato `k_24_8`/`k_24_8_FLOAT` continua lendo a profundidade em float (água e refração usam), e qualquer outro formato lê a cópia empacotada. Para isso, o render target de profundidade ganhou uma SRV do plano de stencil, e há uma root signature de blit com 2 SRVs.
2. **Orientação da face frontal invertida** (a causa das faixas e do escurecimento). `PA_SU_SC_MODE_CNTL.face` = 0 quer dizer que triângulos em sentido **horário** são a frente (o padrão do D3D9, `CULL_CCW`), em coordenadas de janela com y para baixo, a mesma regra do D3D12. O código fazia `FrontCounterClockwise = (face == 0)`. Ninguém tinha notado porque nenhuma cena até aqui usa culling (`mode 0x18000`). O quad de tela inteira (horário) virava face traseira, cujo teste de stencil é `ALWAYS` com `DECR_WRAP`: o passe desenhava na tela toda e deixava o stencil em 255. Logo depois, um overlay de tela inteira com `stencil LESS ref 0` (`ps bc5a024f96d50e67`, máscara `0xF7`) escurecia tudo por ×0,75. As faixas eram onde os retângulos de "conserto" das emendas (`ps 3c908fa28db6ec90`, x 400–448) zeravam o stencil.

   **Correção posterior** (seção "Chiratta de capacete"): o bit `face` só orienta o **culling**, com 0 = frente anti-horária. O stencil de duas faces continua com horário = frente, que é o que o quad acima precisa.

Como foi achado (as ferramentas ficaram no código): o trace de um frame (`CRASH_GPU_TRACE_SWAP`) agora mostra também `RB_STENCILREFMASK`/`_BF`, as constantes de PS não nulas e se o resolve tem cópia empacotada; `CRASH_GPU_SHOW_RESOLVE=<endereço físico>` apresenta um resolve no lugar do front buffer (empacotado: stencil ×64 no vermelho; cor: texels crus); e `userdata\shaderoverride\<ps|vs>_<hash>.hlsl` substitui o HLSL traduzido daquele shader (mesmo formato dos arquivos de `shaderdump\`; serve para instrumentar um passe e também para mods de shader). Com um override que escrevia R = 1 no passe de sombra, o R final ainda saía ×0,75 fora das faixas. Isso provou que o escurecimento vinha de outro draw que dependia do stencil.

Medido no mesmo frame (6000) contra uma execução com o passe desligado: antes, ×0,50 fora das faixas e ×0,67 nelas; depois, ×1,00 em tudo, menos na sombra sob o Crash (×0,89 na borda suave do PCF).

### Engasgos (stutter)

O ritmo de apresentação agora é medido. A cada 300 frames saem `[gpu] present pacing: ... avg / max / N over 50 ms; waiting for the GPU ...` e `[gpu] CP thread ms/frame: ...` (tempo de CPU da thread do command processor em cada etapa do draw). Na fase, antes: média de 33,4 ms, mas 20–40 frames a cada 300 levavam ~62 ms (um frame perdido seguido de um curto). Sem renderer (`CRASH_NO_RENDER=1`), o jogo fazia exatamente 2 vblanks por frame; com renderer, perdia alguns por segundo. Havia duas causas:

1. **Resolução do timer do Windows**. O vblank guest (`gpu.cpp`, `wait_until`) e as esperas do command processor (`WAIT_REG_MEM` com `sleep_for(100 µs)`) eram arredondados para o tick padrão de 15,6 ms. `main.cpp` agora chama `timeBeginPeriod(1)` (link com `winmm`).
2. **Custo de CPU do renderer**: as constantes de VS/PS (2 × 1024 dwords por draw, ~1.400 draws por frame) eram lidas registrador por registrador, por uma função de outro arquivo. Agora são copiadas direto da janela MMIO com byte swap, e `Reg()` lê inline os registradores fora da primeira página (a única com trap): de 6,5 para 0,5 ms por frame.

Depois: média de 33,33 ms, máximo de ~35 ms e **0 frames acima de 50 ms** na fase. Os picos restantes ocorrem só durante leituras de disco (intros, carregamento da fase) e no frame em que o screenshot é gravado. A GPU não é o gargalo (espera por fence ≈ 0). Em monitores que não são de 60 Hz (o desta máquina é 3840×2160 a 144 Hz), 30 fps sempre têm uma cadência irregular de 4/5 atualizações por frame. Sincronizar o vblank guest com o vblank real só resolveria isso em telas de 60 Hz.

### Motion blur exagerado: clears por aliasing de EDRAM

Sintoma: blur forte na tela inteira mesmo com a câmera parada, sempre que algo se mexia (cachoeira, árvores e pedras borradas depois dos ataques do Crash). O passe é `ps 1577d3c304364818` (`shaders\motionblurfp.out`; o HLSL original está em `shaders\motionblurfp.updb`). Ele lê dois mapas de velocidade (`0x097E2000` e `0x0944A000`, alternados a cada frame), resolvidos do RT 1 (EDRAM 1080, `k_8_8_8_8`, pitch 480, 2x MSAA) de cada faixa do tiling predicado. O valor neutro é `0x80` (0,5 menos meio passo de 8 bits = velocidade 0). Só o Crash e objetos que se mexem escrevem nesse RT; o resto da tela depende do clear.

Como o D3D limpa esse RT, visto no trace (`CRASH_GPU_TRACE_SWAP`, que agora mostra também `RB_COLOR1..3_INFO` e quais RTs o PS escreve), em cada faixa:

1. um retângulo de cor normal em x 400–448 (superfície 2x, pitch 480, cor `0x438`, valor (0,504; 0,504; 0,504));
2. um retângulo **só de profundidade** em x 0–200 numa superfície de **pitch 240 com 4x MSAA** e `RB_DEPTH_INFO` = 1080 (`D24S8`): `ZFUNC ALWAYS` com escrita, z = `0x3B007FF1` (= `0x8080` / `0xFFFFFF`), stencil `ALWAYS`/`REPLACE` com ref `0x80` (`depthctl 0x8777`, `refmask 0x00FF0080`).

No Xenos, a palavra de 32 bits que o D24S8 grava na EDRAM é `(profundidade24 << 8) | stencil` = `0x00808080`, e a mesma EDRAM lida como `k_8_8_8_8` dá R = `0x80` (stencil), G/B = `0x80`, A = 0: o clear neutro. O 4x é porque cada pixel de 4x cobre 2×2 amostras: 200 pixels de pitch 240 em 4x são as amostras 0–400 da linha, as mesmas de 400 pixels de pitch 480 em 2x. O mesmo par (2x em 400–448 + 4x em 0–200) também limpa o RT 0 e a profundidade 540, mas esses também são limpos pelo resolve no fim de cada faixa.

O renderer criava um RT host por (base, pitch, formato): o retângulo 4x ia para um RT próprio e o de profundidade para um depth buffer próprio, e o RT 1 só era limpo em 400–448. Fora dessa coluna, o mapa da faixa 1 nunca era limpo e acumulava os rastros dos objetos frame a frame. Na faixa 2 o RT continuava com o conteúdo da faixa 1 (daí as cópias em +416 e +832). O motion blur borrava tudo por onde algo tinha passado.

Correção (`d3d12_renderer.cpp`):

- **MSAA na chave dos RTs**: o RT host é identificado pela largura da linha em amostras (`pitch × 2` para 4x; 1x e 2x continuam um pixel por pixel host, como antes). Uma superfície 4x de pitch 240 cai no mesmo RT host da superfície 2x de pitch 480, com dois pixels host por pixel guest em x: o viewport host cobre a largura em amostras, o scissor é multiplicado e os resolves de uma fonte 4x (flag no shader) e os clears de resolve também usam esse fator. Na fase, os RTs host caíram de 9 para 6.
- **Clears por aliasing profundidade→cor**: um draw só de profundidade com `ZFUNC ALWAYS` e escrita de z (stencil desligado, ou `ALWAYS`/`REPLACE` com máscara de escrita `0xFF`) numa base em que existe um RT de cor `k_8_8_8_8` com a mesma largura é desenhado também nesse RT de cor. Um PS do renderer (`DEPTH_ALIAS_PS`) grava os bytes da palavra da EDRAM: R = stencil (só se o stencil é escrito), G/B/A = profundidade em unorm24 ou 20e4 (`D24FS8`). A profundidade vem de `SV_Position.z`, o mesmo valor que vai para o depth buffer, e o stencil vem de `xe_flags.w`. O draw de profundidade continua indo para o depth buffer. A troca das metades de 40 amostras dos tiles de profundidade na EDRAM não é emulada; não importa para clears com valor constante. O contador `clears (N depth-alias)` aparece no resumo `[gpu] renderer:`, e `CRASH_GPU_NO_DEPTH_ALIAS=1` desliga isso (o comportamento antigo, para comparar).

Verificação (mesmo roteiro de entrada, com X/A alternados a cada 600 ms a partir de 180 s para o Crash atacar e pular; frame 5900; `CRASH_GPU_SHOW_RESOLVE=097E2000` para os mapas):

- `docs\capturas\antigas\velocity_before.jpg`: rastros acumulados dos ataques do Crash na faixa 1, repetidos nas três faixas, e a coluna cinza em 400–416 e 1232–1280. `docs\capturas\antigas\velocity_after.jpg`: cinza neutro (`0x80`) em tudo, menos o Crash e um objeto pequeno que se mexe.
- `docs\capturas\antigas\motionblur_before.jpg`: cachoeira, árvores e pedras borradas com a câmera parada. `docs\capturas\antigas\motionblur_after.jpg`: cena nítida; só o Crash, que está se mexendo, tem blur.
- Sem erros do D3D12 nem fatais; o jogo continua chegando à fase com o mesmo roteiro.

Os retângulos translúcidos alaranjados nos cantos de cima são o HUD dos jogadores 1 e 2, não artefatos.

### Ícones cortados no menu depois do load: superfície 4x sobre um RT criado em 1x (corrigido)

Sintoma (perfil `menu_loaded`, tela "EPISODE 1" com CRASH'S STATS / EXIT GAME / CONTINUE STORY): os bonecos e os "?" do carrossel apareciam só em faixas verticais (x < 480, 400–480, 800–880). O centro, 480–800, ficava vazio, e havia um risco solto perto de (560, 358). O ícone redondo do episódio, desenhado como 2D, saía inteiro.

O que o trace do frame mostrou (`CRASH_GPU_TRACE_SWAP`):

- os ícones são modelos 3D (`prim 4`, ~1130 índices, textura DXT1, `ps bc5a024f96d50e67`) desenhados no passe sem tiling (superfície pitch 1280, 1x, profundidade na EDRAM 720). O scissor é a tela inteira; não é corte por scissor;
- o teste de profundidade é `GEQUAL` com profundidade invertida (viewport z escala −1, offset 1), então qualquer profundidade velha na frente reprova o modelo;
- o D3D limpa essa profundidade (EDRAM 720) com retângulos `edram 5` alternados entre a superfície **1280 em 1x** (colunas nas bordas das faixas, como x 400–448, 416–480, 832–880) e uma superfície **640 em 4x MSAA** na mesma base, que cobre o resto. Na EDRAM, um pixel 4x são 2×2 amostras, então 640×360 em 4x são exatamente os bytes de 1280×720 em 1x.

A chave dos RTs do passo 16 já levava essa superfície 4x ao mesmo RT host de largura 1280. Mas o grid host trata 1x e 2x como o mesmo grid de pixels, e para 4x só multiplicava x por 2. Num RT de 1x, cada linha host é uma linha de amostras, e uma linha de pixels 4x são duas: os clears pela superfície 4x só alcançavam a metade de cima (y < 360) do depth buffer de 1280×720. Os ícones ficam em y ≈ 375–520 e reprovavam contra a profundidade velha. Só apareciam onde o clear vinha da superfície 1x (as colunas acima).

Correção (`d3d12_renderer.cpp`): cada RT host guarda `rowSamples`, quantas linhas de amostras da EDRAM cada linha host representa (1 se o RT foi criado por uma superfície 1x, 2 se por 2x ou 4x, que continuam no grid de antes). Uma superfície com `msaa` usa `HostRowScale` = (2 se 2x/4x, 1 se 1x) / `rowSamples` linhas host por linha de pixel. Isso vale para o viewport do draw (altura × escala), o scissor, o retângulo dos clears de resolve e a leitura da fonte nos resolves (flags 8/16 em `RESOLVE_PS`/`RESOLVE_DEPTH_PACKED_PS`). Resultado: 4x sobre RT de 1x → ×2 (este caso); 4x sobre RT de 2x → ×1 (os clears do mapa de velocidade continuam iguais); 1x/2x sobre o próprio RT → ×1. Um draw que misture RTs com `rowSamples` diferentes usa a escala do depth (ou do primeiro RT) e registra um aviso no log.

Antes e depois: `docs\capturas\antigas\episode_menu_before.jpg` (a captura do problema, swap 1000) e `docs\capturas\antigas\episode_menu_after.jpg` (swap 1060): os quatro bonecos, o item central e os "?" laterais inteiros, sem o risco. `python tools\run_game.py --all` continua passando.

### Faixa no gelo na emenda x = 832: a refração lia o resultado final da faixa anterior (corrigido)

Sintoma (perfil `graficos`, o personagem congelado no cristal de gelo da vila, ~150–158 s): uma faixa vertical de 6 px, x 832–838, onde o gelo fica mais opaco (mais claro ou mais azulado que o resto). É a emenda das faixas 2 e 3 do tiling predicado. Já existia antes das opções gráficas do passo 20.

O que o trace do frame mostrou (`CRASH_GPU_TRACE_SWAP`): as três faixas têm 448 px e se sobrepõem em 32 px (scissor 0–448, 416–864 e 832–1280, window offset 0, −416 e −832). Em cada faixa, o jogo copia a cena para `0x0A642000` (resolves sem clear, `c0`, retângulo da faixa inteira) antes dos passes que leem a cena. Um desses passes é a refração do gelo (`ps af71cf6947f3c235` = `shaders\refractfp`, `vs 17b062591448a553`): ela mistura `0,55 × textura + 0,45 × cena[posição deslocada pela normal]` (blend ONE/ZERO). No fim, cada faixa copia a cena **final** para a mesma textura (resolve com clear, `c1 d1`), que o pós-processamento em 1x lê depois das três faixas. Quando a faixa 3 desenha o gelo, a textura em x < 832 já tem o final da faixa 2, com o gelo. Os pixels da faixa 3 logo à direita da emenda refratam para a esquerda, leem esse gelo e o aplicam de novo. A faixa 2 não tem o problema: até 864 ela lê a própria cópia.

No console a sequência de cópias é a mesma: mesmos retângulos, na mesma ordem, e a faixa 3 não tem nada da EDRAM à esquerda de 832. Então o console (e o Xenia) provavelmente mostram a mesma faixa, só que em 720p no console ela some mais fácil. Limitar os resolves à área "própria" de cada faixa não resolve, porque x < 832 continua com o final da faixa 2. A correção é uma melhoria sobre o console.

Correção (`d3d12_renderer.cpp`, `TileStashBeforeResolve`, chamada por `Resolve()` para resolves de cor): cada superfície de resolve que recebe cópias da cena e finais de faixa no mesmo frame ganha duas texturas só de cópia:

- `pre`: no resolve final de uma faixa (com clear de cor), antes de escrevê-lo, guarda a última cópia sem clear dessa faixa (a cena antes do gelo) no retângulo da faixa;
- `final`: na primeira cópia sem clear de uma faixa seguinte (retângulo diferente do último final, mesmo frame), guarda a textura inteira (os finais das faixas anteriores) e põe `pre` no lugar. A refração da faixa 3 lê então, à esquerda de 832, a cena da faixa 2 antes do gelo, que é o que a faixa 3 teria se a EDRAM dela fosse mais larga;
- no resolve final dessa faixa, `final` volta para a textura antes do resolve. Depois da faixa 3, o pós-processamento vê os três finais, como antes.

O custo são ~9 cópias de 1280×720 por frame, só nas superfícies que recebem cópias e finais de faixa no mesmo frame (`0x0A642000` na fase; uma cena de `0x09B87000` também cai no caso). Funciona com a cena em escala menor (FSR/DLSS): os retângulos são os da superfície da escala do resolve. `CRASH_GPU_NO_TILE_STASH=1` volta ao comportamento antigo. Se uma faixa fizer uma cópia e nunca chegar ao final com clear (não acontece aqui), os finais voltam na próxima cópia para a textura.

Medido no mesmo build, mesmos quadros (perfil `graficos`, capturas em 150,5–158 s, com e sem `CRASH_GPU_NO_TILE_STASH`): o gradiente horizontal médio das colunas tinha picos de 13–15 em x = 832 e x = 838 (fundo ~2–4). Agora não há pico, e com FSR qualidade (cena em 854×480) também não. Antes e depois: `docs\capturas\antigas\graficos\gelo_emenda_antes_depois.jpg` (quadros 04887 e 04662; a captura original do problema é `docs\capturas\antigas\graficos\gelo_emenda_tiling.jpg`).

### Próximo ponto de falha / pendências

- **EDRAM ainda sem transferência geral entre RTs**: só existe o caso profundidade→cor dos clears acima. Cor relida como profundidade, RTs com outro formato ou outro pitch sobre os mesmos tiles, e 1x × 2x na mesma base (que dividem o grid de pixels sem converter as linhas de amostras) continuam sem tratamento. (4x sobre um RT de 1x já converte as linhas: ver "Ícones cortados no menu depois do load".) Se aparecer outro passe que dependa disso, o caminho é um cache de RTs com posse por tile, como o do Xenia.
- **Data dos saves em UTC**: `KeQuerySystemTime` devolve UTC, então o jogo grava "Last Save: 10.20.26" às 21h de 19/10 no horário de Brasília. Falta o fuso (`ExGetXConfigSetting`, bias do usuário).
- Picos de 60 ms durante leituras de disco (intros, carregamento): I/O síncrono na thread que espera. Não afeta o jogo dentro da fase.

## Passo 17: nomes de classes e métodos virtuais (RTTI)

Primeiro passo para ter a lógica do jogo legível (RTTI → funções virtuais com nome → decompilação no Ghidra → C++ reescrito à mão para as partes de interesse → API de mods). Uso e tabela de nomes no README, seção "Lendo o código do jogo".

### O que tem no executável

O RTTI do MSVC está inteiro, em big-endian com ponteiros absolutos de 32 bits:

- 2 300 `TypeDescriptor`s (`.?AVCLifeDisplay@@`, `.?AUSVectorData@CActionAnimation@@`, templates como `.?AV?$Constructor1@VCActor@@I@script@@`), todos com o mesmo ponteiro de vtable do `type_info` (`0x8203B168`), no `.data`;
- 2 247 `CompleteObjectLocator`s no `.rdata`, cada um seguido (em outro lugar do `.rdata`) pela sua vtable: a palavra antes da vtable aponta para o COL. As entradas vão até a primeira palavra que não aponta para `.text`/`BINK`. Muitas vtables de uma entrada só (`CActionAI` e afins: só um "get type") são seguidas direto por constantes `float`, o que confirma o critério de parada;
- `ClassHierarchyDescriptor` com atributos 0, 1 (herança múltipla) e 5 (múltipla + ambígua: `core::CementLibrary`); nenhuma herança virtual. O `BaseClassArray` está em pré-ordem, com `numContainedBases` para pular as sub-árvores (é assim que o script acha as bases diretas). Vtables secundárias (`offset` ≠ 0 no COL; 61 delas) são atribuídas à base cujo `mdisp` é igual ao `offset`.

Todas as 15 686 entradas de vtable são inícios de funções geradas (o `find_swallowed.py` já tinha garantido isso).

### Padrões reconhecidos

- **Deleting destructor** (1 088): `mr r30,r4` … `bl ~Classe` … `clrlwi r11,r30,31` / `cmplwi r11,0` / `beq` / `bl operator delete`. O script acompanha as cópias de r4 (só registradores não voláteis sobrevivem a uma chamada) e aceita `clrlwi`, `rlwinm` com máscara de 1 bit ou `andi.`. Teste de `& 2` = `vector deleting destructor`. A chamada depois do teste mais votada (458 vezes) é o `operator delete` (`0x821FDB28`). O slot do destrutor varia: 0 na maioria, 2 nas classes que começam com `AddRef`/`Release` (`core::IRefCount`).
- **Destrutores** (`Classe::~Classe`, ~600): a única chamada antes do teste. Quando o mesmo destrutor é chamado pelos deleting destructors de várias classes (os destrutores derivados foram inline e só sobrou a chamada ao da base), o nome é o da base comum mais derivada: `core::IRefCount::~IRefCount` (82 classes), `fight::ParamValue::~ParamValue` (133), `pure3d::Light::~Light` (5 luzes).
- **Corpos idênticos juntados pelo linker**: `nullsub` (`blr`, em 511 classes), `return_0` (540), `return_1` (480) e outros `return_N`.
- **`_purecall`** (`0x823CF1F0`, nas vtables de 101 classes abstratas): chama `__pPurecall` se não for nulo e depois `_amsg_exit(25)` (`_RT_PUREVIRT`, `0x823D6698`).
- **Métodos herdados**: o dono do slot é a classe mais derivada cujo layout de vtable todas as usuárias compartilham naquele deslocamento. Exemplo: `0x820A1EE0` (`lwz r11,4(r3); addi r11,r11,1; stw` = `AddRef`) aparece em 603 classes e vira `core::IRefCount::vf00`, embora o slot 0 do próprio `IRefCount` seja `_purecall` (a implementação está numa classe sem RTTI, provavelmente um template de refcount).

### Cobertura

5 608 das 20 144 funções recompiladas têm nome (27,8%), mais os 152 imports: 3 324 `rtti`, 512 `rtti-inherited`, 1 088 `rtti-dtor`, 83 `rtti-shared`, 601 `heuristic`. O resto são funções chamadas só com `bl` (métodos não virtuais, funções livres, CRT) e continua `sub_XXXXXXXX`.

### Runtime

`symbols.cpp` lê o CSV em `InstallCrashHandler` (`CRASH_SYMBOLS=<arquivo>` ou `0`), e `DescribeGuestCode(endereço)` devolve `Nome+0xOFF (sub_XXXXXXXX+0xOFF)`, `Nome (import)` ou só `sub_XXXXXXXX+0xOFF`. Usado em:

- `[crash]`/`[watchdog]`: "nearest guest function" com nome, e depois do backtrace bruto uma linha `guest lr in ...` e um frame por linha (`#0 ...`);
- `[fatal] indirect call to unmapped guest address`: a função onde está o alvo e a do `lr`;
- o aviso de inicialização `[loader] ... point at unmapped code`.

Exemplo real (`CRASH_TIMEOUT=12`, durante o vídeo de abertura):

```
[symbols] 5760 function names from .../private/symbols/functions.csv
guest lr in sub_824117C0+0x14
  #0  sub_824300F0+0x368
  #1  sub_824319F0+0x84
  #2  sub_822DA660+0x44
  #3  CMoviePlayer::vf07+0x1E4 (sub_82230198+0x1E4)
  #4  CRenderManager::vf10+0x66C (sub_82225AD0+0x66C)
  #5  CUberManager::vf10+0x134 (sub_821FC198+0x134)
```

e nas threads de trabalho `om::TaskThread::vf00`, `audio::hal::sw::RendererImpl::RenderThread::vf00`, `audio::hal::hw::StreamerDvdLoadThread::vf00`.

### Ferramentas

| Arquivo | Função |
|---|---|
| `tools/rtti_scan.py` | Varre o RTTI e escreve `private/symbols/rtti.json` e `functions.csv` |
| `tools/annotate_ppc.py` | Cópia de leitura de `generated/ppc` em `private/ppc_annotated` com os nomes em comentários (função e chamadas diretas) |
| `tools/ghidra_apply_symbols.py` | Script do Ghidra (Jython/PyGhidra): nomes, namespaces/classes, comentários, rótulos de vtable/COL/`TypeDescriptor` |
| `tools/ida_apply_symbols.py` | O mesmo para o IDA (IDAPython) |

Os scripts do Ghidra e do IDA não foram testados aqui (nenhum dos dois está instalado nesta máquina).

### Próximos passos

- Propagar nomes: chamadas feitas só por um método com nome (`Classe::vf03` → `Classe::vf03_helper`), funções com strings de log/assert, construtores (gravam o endereço da vtable em `0(r3)`: dão o nome `Classe::Classe` e ligam cada classe ao código que a cria).
- Ler os slots no Ghidra para dar nomes de verdade aos `vfNN` (por exemplo, os métodos de `core::IRefCount`, `pure3d::Entity` e `om::Entity`, que se repetem em centenas de classes) e guardar esses nomes num arquivo versionável de "nomes manuais" que o `rtti_scan.py` aplica por cima.

## Passo 18: overlay de desempenho, marcas e gravação de botões

Para escolher em que quadro tirar um print, montar perfis do `config/run.toml` e acompanhar FPS e frame time. Código em `gpu/overlay.*` (desenho e contadores), `gpu/overlay_font.h` (fonte), `gpu/window.cpp` (teclas) e `kernel/xam_input_imports.cpp` (relógio e gravação, interface em `kernel/input.h`). No renderer, só três pontos de contato em `d3d12_renderer.cpp`: `overlay::Init` no fim do `Init`, `overlay::BeginFrame` no começo do `Swap` e `overlay::Draw` depois do passe de apresentação.

**O que aparece** (canto superior esquerdo, fundo preto translúcido, escala 2× em 720p/1080p):

- `QUADRO N`: o `g_stats.swaps` do renderer naquele `Swap`, exatamente o número que `CRASH_SCREENSHOTS` compara e que vai no nome `screenshot_NNNNN.png`. (O `VdSwap #N` do log é contado no `VdSwap` da CPU e pode estar alguns quadros à frente, porque o CP executa o swap depois.)
- `INPUT t S`: segundos desde a primeira leitura do controle 0, com uma casa decimal. É o zero do `CRASH_INPUT_SCRIPT`; antes da primeira leitura aparece `--`. O relógio agora existe mesmo sem script (`g_clockStart`, marcado na primeira chamada de `ReadController(0)`, que é onde o script já começava a contar). Usa `GetTickCount64`, como o script.
- `FPS`: swaps por segundo no último segundo, atualizado duas vezes por segundo. `FRAME`: tempo do último quadro (QPC entre dois `Swap`, a mesma medida do `[gpu] present pacing`).
- `MED / MAX / 1%LOW`: frame time médio, máximo e percentil 99 (ms) dos últimos 240 quadros.
- Gráfico com um quadro por coluna (os últimos 204), escala de 0 a 66,7 ms, guias tracejadas em 16,7 e 33,3 ms. Verde até 34,5 ms, amarelo até 50 ms, vermelho acima.
- Linha de status: as teclas, `MARCA n: QUADRO N` / `PRINT: QUADRO N` por 2,5 s depois de F2/F12 e `REC` em vermelho quando a gravação está ligada.

**Como é desenhado**: a imagem (204×88) é rasterizada na CPU a cada quadro com uma fonte 5×7 própria (`overlay_font.h`, desenhada para o projeto, sem dados de terceiros; minúsculas usam as maiúsculas, por isso o texto é em caixa alta e sem acentos). Ela é copiada para uma de 4 fatias de um buffer de upload mapeado (o `Swap` deixa no máximo 2 quadros na fila). Um pixel shader lê essa imagem como `StructuredBuffer<uint>` por SRV de raiz, sem descriptor heap, e a desenha com blend alfa sobre o back buffer, num triângulo de tela cheia limitado pelo scissor ao retângulo do overlay. Tem root signature e PSO próprios, então não mexe no estado do renderer (o `DrawImpl` reconfigura root signature e heaps a cada draw). O custo é desprezível: ~19 mil pixels na CPU e um draw pequeno.

**Capturas**: por padrão, o print é tirado antes do overlay ser desenhado (back buffer `RENDER_TARGET → PRESENT`, cópia, volta para `RENDER_TARGET`, overlay). Com `CRASH_SCREENSHOT_OVERLAY=1`, a captura é feita depois e inclui o overlay. Com o overlay escondido, o caminho é o mesmo de antes.

**Teclas** (no `WindowProc`, só com a janela em foco; F1/F2 ignoram auto-repeat): F1 mostra/esconde, F2 marca o quadro (`[mark] frame N, input t=XX.X s` no log, mesma linha em `<CRASH_SCREENSHOT_DIR>\marks.txt` e `input::RecordMark`), F12 salva o print (já existia). Nenhuma delas é usada pelo teclado do controle 0.

**Gravação** (substituída no passo 21 pela gravação com F5, analógicos e relógio do jogo; o texto abaixo é o histórico) (`CRASH_RECORD_INPUT=<arquivo>`): em `ReadController(0)`, o estado do controle físico (XInput + teclado, **antes** de somar o script) passa por `Recorder::Update`, que detecta a borda de descida e de subida de cada botão digital (A B X Y START BACK, D-pad, LB RB). Ao soltar um botão, entra `[início, "BOTÃO"]`, com a duração como terceiro campo se ficou apertado ≥ 300 ms (arredondada a 50 ms). Toques curtos voltam como o aperto padrão de 200 ms. O arquivo é reescrito inteiro (via `.tmp` + `MoveFileEx`) a cada botão solto e a cada marca, então fica completo seja qual for o jeito em que o processo terminar (janela fechada, `TerminateProcess` do watchdog). Os apertos do `CRASH_INPUT_SCRIPT` entram no trecho com o tempo exato do script, ordenados junto com os gravados, e as marcas viram `screenshots = [...]`. Analógicos e gatilhos não são gravados. O script ganhou o terceiro campo opcional (`ms:BOTÃO:ms_apertado`) e os botões `LB`/`RB`.

**Estatísticas** (`CRASH_FRAME_STATS=<arquivo>`): `frame,ms,input_s` por quadro, com `fflush` a cada 30 quadros. O `tools/run_game.py` sempre liga isso (`frametimes.csv` na pasta da execução) e calcula FPS médio (quadros / tempo), frame time médio, máximo e p99, 1% low (FPS médio do 1% de quadros mais lentos) e quadros acima de 50 ms, a partir de `perf_from_frame`. O resultado aparece no resumo, vai para `result.json` (`perf`) e pode ser conferido com `expect.min_fps`, `expect.max_frame_ms_p99` e `expect.max_hitches`.

Outras variáveis: `CRASH_OVERLAY=1` (começa visível; sem ela começa escondido, e F1 mostra), `CRASH_OVERLAY_CORNER=tl|tr|bl|br`, `CRASH_OVERLAY_SCALE=N`. Opções do `run.toml`: `overlay` (padrão `true`), `screenshot_overlay`, `record_input` e `perf_from_frame`. O perfil `gravar` junta tudo para montar um teste.

## Passo 19: tela de configurações (SETTINGS no menu principal)

Pedido: uma tela de configurações aberta pelo menu principal sem mexer nos fluxos de NEW GAME e LOAD GAME. Ficou no item CALIBRATION (a tela de calibração de TV é inútil no PC), renomeado para SETTINGS; a dica "Y STORAGE DEVICE" foi escondida. Código novo em `settings/` (`settings.*`, `settings_menu.*`, `frontend_hooks.cpp`, `frontend_patches.cpp`); pontos de contato: `fs/rcf_overlay.*` (patches embutidos), `gpu/d3d12_renderer.cpp` (FXAA, desenho da tela, V-Sync, tela cheia), `gpu/overlay.*` (`DrawImage`, modos), `gpu/window.*` (modos de janela, Alt+Enter), `kernel/xam_input_imports.cpp` (entrada da tela, controle neutro para o jogo, idioma/país), `audio/audio_output.cpp` (volume geral) e `main.cpp` (`settings::Load` e registro dos patches logo depois do `vfs::Init`).

**Por que não o Y.** O Y do menu e o LOAD GAME (sem dispositivo escolhido) chamam o mesmo `XamShowDeviceSelectorUI`, do mesmo lugar (`sub_821CF420`, lr `0x821CF488`) e com a mesma pilha guest inteira: os dois vêm do Lua `Managers.SaveGameManager:ShowDeviceSelector()` (o método passa pelo thunk genérico `sub_822E29D8` e pela VM Lua, `CLuaFunctionAction::vf04` → `CScriptBehaviourController::vf09` → ...). Os protótipos Lua deste jogo não guardam nome de fonte, então nem a pilha Lua diferencia. O NEW GAME só chama o seletor depois, ao salvar.

**O menu é dado.** `fighttrees\Frontend.bfig` (no `default.rcf`) é a máquina de estados do frontend, com os trechos Lua em texto (`[u32 tamanho][texto]`, compilados pelo jogo ao carregar). Cada saída é `[0xBC27359D][tamanho][nome, NUL, padding a 4][u32][u32][u32][f32 -1][u32][f32 -1][u32 nó destino]...`. No menu do Xbox, `ExitCalibration` vai para o nó 0x23B (`VideoCalibrationScreen`) e roda `Managers.FrontendManager:PlaySoundSelect()\r\n\r\n`; `ExitShowXenonDeviceSelect` (Y) volta ao próprio menu, nó 0x46. Visibilidade de elementos: `[0x7B6567EE]["GameStart_Start"]["GameStart_DeviceSelector"][u32 visível]`, no bloco `ContentXenonWithProfile`. As condições ficam em `fighttrees\Frontendlua.blua` (Lua 5.1 modificado: `lineinfo` em u16, `lua_Number` float).

**Patches embutidos** (`rcf_overlay::RegisterBuiltinPatch`): o runtime lê a entrada como o jogo a veria (a original ou, se um mod a substitui, o arquivo do mod, servido depois do fim do arquivo), o patcher edita uma cópia (mesmo tamanho) e as diferenças viram sobreposições de bytes na leitura do `.rcf` virtual (sem mudar diretório nem tamanho). Assim um mod de textura que reconstrói `package\cdd70a8c.p3d` continua com o rótulo SETTINGS (log `package\cdd70a8c.p3d (mod <nome>): built-in patch "rotulo SETTINGS" ...`). Se o patcher não acha o que procura (outra versão do jogo, ou um mod que mudou aquele trecho) a entrada fica como está (`not applied`). Os quatro patches (`frontend_patches.cpp`), procurados pelo conteúdo, não por offsets fixos:

1. `Frontend.bfig`: as três `ExitCalibration` (PS2, Wii, Xbox) passam a ir para o nó do menu (o destino de `ExitShowXenonDeviceSelect`), e o trecho vira `PlaySoundSelect(4242)` (mesmo tamanho: o `\r\n\r\n` some). O método ignora o argumento a mais.
2. Mesmo arquivo: a visibilidade `GameStart_DeviceSelector` de `ContentXenonWithProfile` passa de 1 para 0. O Y continuava ativo (desde o "Frontend de PC", abaixo, não faz nada).
3. Bíblias `frontend` de `package\7efdcd91.p3d` e `package\cdd70a8c.p3d` (as únicas que o frontend lê; conferido com `CRASH_LOG_ALL_IO`): as chaves 0x1E96 e 0xE1F3 (as duas CALIBRATION em inglês; em russo "brilho" e "ajuste de imagem") recebem SETTINGS / PARAMETRES / OPTIONEN / AJUSTES / IMPOSTAZIONI / OPTIES / ... por cima do texto original (só se couber e se nenhuma outra chave apontar para dentro da string). 0x1E96 sozinha não bastou: o item do menu usa a 0xE1F3.
4. `sound\zscripts\mixers.p3d` (`sound1.rcf`): veja "Volumes" abaixo.

**O gancho.** `PPC_FUNC(sub_822E29D8)` (o thunk Lua → C++ das bindings; r3 = `lua_State*`) olha a pilha Lua antes de chamar o original: neste Lua, `L->top` fica em +8 e `L->base` em +12, e cada slot é `{u32 tipo, u32 valor}` (tipo 3 = número, float). Com exatamente `self` + o número 4242, chama `settings::menu::RequestOpen()`. Como a saída foi redirecionada, o jogo reentra no estado do menu (o cursor volta para NEW GAME) e a tela abre por cima.

**A tela** (`settings_menu.cpp`; desde a versão nativa o desenho, a tela do jogo por trás e o fechamento mudaram, veja "Tela de configurações nativa" abaixo; o resto deste parágrafo é o histórico): imagem de 320×180 rasterizada na CPU com a fonte do overlay (sem acentos), desenhada pelo presenter com escala inteira centralizada (4× em 720p), por `overlay::DrawImage` (mesmo PSO do overlay, buffer de upload próprio com 4 fatias de 512×512). Roda no thread do CP, no `Swap`, depois do passe de apresentação/FXAA e antes do overlay, então aparece nas capturas. Entrada: `input::MenuButtons()` (XInput 0, analógico como D-pad, teclado com Esc = B e o `CRASH_INPUT_SCRIPT`). Enquanto `BlocksGameInput()` (pedido pendente, tela aberta ou botões ainda apertados depois de fechar), `ReadController(0)` zera o `Gamepad` que o jogo vê; o número do pacote acompanha o estado zerado. Sem renderer (`CRASH_NO_RENDER`) o pedido é ignorado, para a entrada nunca ficar presa numa tela invisível. Fechar grava `settings.toml` (via `.tmp` + rename).

**Volumes.** O jogo mistura tudo sozinho e entrega só a mixagem final (`XAudioSubmitRenderDriverFrame`); não usa `XAudioGetVoiceCategoryVolume`. O volume geral multiplica essa mixagem em `audio_output.cpp` (o `CRASH_AUDIO_DUMP` grava o que se ouve). As categorias vêm dos dados de som: `categories.p3d` define a árvore (raízes MUSIC, SFX, ambience e Dialogue; NISDialogue e Loudspeaker ficam sob SFX) e `mixers.p3d` tem um snapshot por chunk (`default`, `combat`, `ingame_mute`, `low_health`, `vo_duck`, `user_control`, `xmp`, `NIS`), com `[nome][f32 volume][f32 pitch][f32][f32][u16]` por categoria (`vo_duck` baixa MUSIC para 0,7; `xmp`, a música do usuário, zera MUSIC; o segundo float varia por personagem, é o pitch). Medido no menu com o dump de áudio: MUSIC a 0,5 em todos os snapshots saiu a 0,06 da amplitude (os snapshots ativos se multiplicam), só no `default` saiu a 0,25 (o ganho vai com o quadrado), e com a raiz quadrada no `default` saiu a 0,49. Então: música → MUSIC, efeitos → SFX e ambience, vozes → Dialogue e NISDialogue (que também segue os efeitos, por estar sob SFX), aplicados como √(porcentagem) no snapshot `default`. O jogo lê os mixers no boot, por isso valem ao reiniciar. 0% é silêncio.

**Idioma.** `sub_823E8A80` lê o idioma em `ExGetXConfigSetting(3, 9)`; `sub_82288230` o converte em letra da bíblia (1 E, 2 J, 3 G, 4 F, 5 S, 6 I, 7 K, 8 C, 9 P); `sub_821D80C0` só aceita F/G/I/S, usa D quando o país (`ExGetXConfigSetting(3, 14)`, via a tabela de `sub_82264670`) é a Holanda (74) e U para o Reino Unido (35), senão E; `sub_821F9498` monta o `.rcf` de voz pela letra. O runtime responde ao jogo com o idioma de `settings.toml`: inglês (1, EUA), francês (4, FR 34), alemão (3, DE 24), espanhol (5, ES 31), italiano (6, IT 50) e holandês (1, NL 74). Conferido: francês e holandês mudam o menu e montam `french.rcf`/`dutch.rcf`. `sub_821D8170(this, letra)` é a troca ao vivo (a tela de idioma nórdica `BootUp_LanguageSelect` usa por `FrontendManager:SetLanguage`, com `ILanguageChangeCallback`), mas trocaria também o `.rcf` de voz montado; ficou para o próximo boot.

**Vídeo.** FXAA (`FXAA_PS`, escrito para o projeto no esquema clássico "FXAA PC console": teste de contraste de luma na vizinhança diagonal, blend de 2 ou 4 amostras na direção da borda, áreas planas intocadas): com ele ligado o passe de apresentação desenha num alvo RGBA8 do tamanho do back buffer (`g_postTarget`, recriado ao redimensionar) e o FXAA copia para o back buffer. V-Sync: `Present(1, 0)`; o padrão continua `Present(0, 0)` (o limite de 30 fps do jogo não vem daí; veja "Limite de FPS"). Modo de tela: `window::SetMode` posta uma mensagem para o thread da janela, que troca o estilo (`WS_OVERLAPPEDWINDOW` ↔ `WS_POPUP` do tamanho do monitor) e o `WM_SIZE` leva ao `ResizeBuffers`; "tela cheia" ainda chama `SetFullscreenState(TRUE)` quando a janela está em primeiro plano (tentativa a cada 2 s; no Alt+Tab o DXGI sai e fica a janela sem bordas). Overlay = `overlay::SetMode` (veja o fim do passo 19; ao abrir, a tela mostra o modo atual, que o F1 pode ter mudado).

**Teste**: perfil `configuracoes` em `config/run.toml` (abre SETTINGS, volume geral 90%, FXAA, fecha, LOAD GAME até o menu do episódio; confere as linhas `[settings]` e o `XamContentCreateEx ... (opened)` do load). `CRASH_SETTINGS="secao.chave=valor;..."` muda valores só numa execução. Capturas: `docs\capturas\antigas\configuracoes_aberta.jpg`, `docs\capturas\antigas\configuracoes_alterada.jpg`, `docs\capturas\antigas\configuracoes_menu_depois.jpg`, `docs\capturas\antigas\configuracoes_load_depois.jpg`, `docs\capturas\antigas\menu_antes_settings.jpg` / `docs\capturas\antigas\menu_settings.jpg`, `docs\capturas\antigas\fxaa_off.jpg` / `docs\capturas\antigas\fxaa_on.jpg` / `docs\capturas\antigas\fxaa_off_on.jpg` (mesmo ponto da cena, quadros um pouco diferentes) e `docs\capturas\antigas\idioma_frances_holandes.jpg`.

**Resolução interna** (`video.resolution`, ao reiniciar): 1280x720 (original), 1600x900, 1920x1080, 2560x1440 ou 3840x2160, isto é, um fator `s` = altura / 720 (1, 1,25, 1,5, 2, 3) aplicado no `d3d12_renderer.cpp`. Os tamanhos lógicos de `RenderTarget` e `ResolveTarget` continuam em unidades guest (as chaves de EDRAM por base/pitch e a procura de destino de resolve por macro tiles de 32 não mudam); `hostWidth/hostHeight` são os recursos, `s` vezes maiores. Escalados: viewport e scissor de cada draw (o NDC continua calculado em pixels guest, o viewport faz o resto), retângulos de clear, viewport/scissor dos resolves e o deslocamento destino − origem passado ao shader de resolve (múltiplo de 32 × `s`, sempre inteiro). O shader de resolve já trabalha em pixels host, então lê a origem certa sem mudança. No tradutor (`SetRenderScaled`, só quando `s` ≠ 1, para o HLSL de 720p continuar idêntico e o cache de shaders valer), `xe_system` ganha `xe_render_scale` (1/`s`) e `xe_tex_size_scale[fc]` (1/`s` para resolve targets, 1 para texturas): o `GetDimensions` de cada fetch é corrigido para o tamanho guest (coordenadas não normalizadas, offsets de texel e getWeights), e a posição do param gen (`vPos`) volta para pixels guest. A apresentação usa a razão guest/guest, então amostra o front buffer maior sem mudança. Conferido a 2560x1440 até a cena de abertura: sombras, profundidade de campo e cores certas, bordas suavizadas (`docs\capturas\antigas\resolucao_720p.jpg`, `docs\capturas\antigas\resolucao_1440p.jpg`, `docs\capturas\antigas\resolucao_720_vs_1440.jpg`). Limites: a primeira execução numa resolução nova compila todos os shaders (~2,5 s de travadas no início, o que atrasa um roteiro de botões); as passagens de pós que reduzem a imagem com amostragem bilinear (bloom) amostram só 1/`s`² da área; 3840x2160 usa ~1 GB de VRAM a mais (render targets de 2048 linhas × 3).

**Limite de FPS** (`video.fps_limit`, na hora): o jogo trava em 30 em dois lugares. (1) `pure3d::xnDisplay::vf15` (`sub_8238EFE8`, a troca de quadro) espera o contador de vblank do D3D (`0x82526368`, copiado por `sub_8238EE28` no callback de vblank) andar 1 e apresenta com intervalo 1: 2 vblanks por quadro. (2) `CUberManager::vf10` (`sub_821FC198`, o laço principal) soma o dt e só atualiza e desenha quando passou o float de `0x8201EF70` (1/60 s, lido só ali). `gpu::SetFrameLimit` muda a frequência do vblank emulado (30 → 60 Hz, a original; 60 → 120 Hz; 120 → 240 Hz; sem limite → 1000 Hz) e, acima de 60, grava 1/fps nesse float (1/1000 sem limite), conferindo antes que o valor original é 1/60. O jogo usa o tempo real para o dt (a 120 Hz de vblank a cena de abertura está no mesmo ponto no mesmo instante que a 30 fps), então só o FPS muda. Medido (`primeira_fase_load`): 60 → 60 estáveis; 120 → 100–120; sem limite → 140–490 conforme a cena (às vezes 144, a taxa do monitor, por causa do flip model sem tearing). O jogo lê o controle a ~30 Hz fixos (~30 `XamInputGetState`/s em qualquer limite); nos testes com roteiro (toques de 100 ms, segurar 1 s, o LOAD GAME inteiro) os menus responderam certo em 30, 120 e sem limite, mas houve um relato de toque contado duas vezes no menu sem limite (a investigar). `CRASH_VBLANK_HZ` força a frequência. `CRASH_SCREENSHOT_TIMES=19.5,30` tira capturas por segundo do relógio do input (para testes em FPS variável). O log `[gpu] vblank ...` agora sai a cada 5 s, não a cada 300 vblanks.

**Modos do overlay** (`video.overlay` = `off` | `fps` | `frametime` | `full`; o antigo `show_fps = true/false` ainda é lido como completo/desligado). `overlay.cpp` rasteriza só as linhas do modo, e a imagem tem o tamanho delas (no máximo 40 colunas × 7 linhas + o gráfico; o gráfico tem no mínimo 34 colunas): só FPS = uma linha; FPS + frame time = FPS/frame, médio/máximo/1% low e o gráfico; completo = quadro e relógio do input, FPS/frame, médio/máximo/1% low, `RENDER <interna> (<fator>X) -> <janela>`, nas execuções de perfil `PERFIL <nome>` e `BOTAO <nome> @ <s> S Q<quadro> SCRIPT|JOGADOR`, a linha das teclas/avisos e o gráfico. O F1 (`window::ConsumeOverlayToggles`, que agora conta os toques) passa ao próximo modo e grava `settings.toml`. Prioridade no início: `CRASH_OVERLAY` (1/true = completo, ou o nome do modo; `run_game.py` o define pelo `overlay` do perfil: `true` → `full`, texto → aquele modo, `false` → não define) e, sem ele, o `settings.toml`. O nome do perfil vem de `CRASH_PROFILE`, que o `run_game.py` sempre exporta. O último botão vem de `ReadController(0)` em `xam_input_imports.cpp`: as bordas de descida do controle físico + teclado (antes de somar o roteiro) contam como `JOGADOR`, as do `CRASH_INPUT_SCRIPT` sozinho como `SCRIPT`, com o relógio do input e o quadro que o overlay passa em `input::SetFrame` a cada `BeginFrame` (`input::LastPress`). A fonte ganhou o `@`.

## Passo 20: opções gráficas de PC (motion blur, mipmaps, SMAA, DLSS/DLAA e FSR 3.1)

Pedido: controle do motion blur (desligado ou 0–100%), menos serrilhado, opções típicas de PC (resolução de sombra etc.) e upscaling temporal DLSS/DLAA e FSR, **só o upscaling, sem frame generation**. Tudo na tela SETTINGS (nova seção GRÁFICOS; a lista agora rola e mostra duas linhas de ajuda do item selecionado) e em `[graphics]` do `settings.toml`. Código novo: `gpu/graphics_options.*`, `gpu/smaa.*`, `gpu/upscaler.*`; mudanças em `gpu/d3d12_renderer.cpp`, `gpu/shader_translator.cpp`, `gpu/texture_decode.*`, `gpu/overlay.cpp`, `settings/*`, `diagnostics.cpp`, `kernel/xam_input_imports.cpp`, `CMakeLists.txt`, `tools/run_game.py`. Capturas em `docs\capturas\antigas\graficos\`.

### Constantes por draw e patches de shader (`gpu/graphics_options.*`)

A root signature dos draws ganhou o parâmetro 6: 8 constantes de 32 bits em `b3` (`cbuffer xe_effects`, declarado em todo shader traduzido): `xe_effect` = (escala do motion blur, da profundidade de campo, da suavidade da sombra, escala de LOD das texturas) e `xe_jitter` (jitter do upscaler). O renderer as atualiza a cada `Swap` a partir de `settings::Get()`, então essas opções valem na hora. Os patches são edições de texto no HLSL traduzido de três shaders do jogo (pelo hash; aplicados antes do dump, então `shaderdump\` mostra o código que roda; um `shaderoverride\` substitui tudo, sem patch). Se o código não estiver onde se espera, o patch não é aplicado e o log avisa. `--test-shaders` agora imprime o hash de cada `.out` (é como os hashes abaixo foram ligados aos arquivos).

| Opção | Shader | O que muda |
|---|---|---|
| Motion blur 0–100% | `ps 1577d3c304364818` (`motionblurfp`) | as duas leituras do mapa de velocidade (t2, t3): `v.xy = 128/255 + (v.xy − 128/255)·k` (o neutro do mapa é 0x80); `Params.zw` (c0, o blur de câmera para pixels parados) × k. Com 0% o passe devolve o quadro sem blur (o passe continua rodando: é a âncora do upscaler) |
| Profundidade de campo 0–100% | `ps 45775928e9ebd556` (`DOF`) | `MaxRadius` (c5) × k: raio das 8 amostras; 0 = todas no centro |
| Suavidade das sombras 0–200% | `ps fdf408078ea760bb` (`shadowshaderfp`) | os 9 offsets UV do PCF 3×3 (c2–c10, ~1,5 px a 720p) × k |

**Por que não existe "resolução das sombras"**: o jogo não usa shadow maps. As sombras são volumes de stencil (passo 16): os volumes só marcam o stencil, o depth+stencil é resolvido e um quad de tela inteira escurece onde o stencil é > 0, com o PCF acima. Não há textura de sombra com resolução própria: a borda segue a resolução interna (e o DLSS/FSR). O que dá para ajustar é o raio do filtro; a tela mostra "RESOLUCAO DAS SOMBRAS = RESOLUCAO" como item informativo.

Não implementados (e por quê): **FOV**: o campo de visão é dado por volume de câmera nos scripts de cada fase (`levels\*\cameraoverrides.lua`: `Managers.CameraManager:SetCameraVolumeFOV(volume, 65)` em alguns volumes, o padrão do motor nos outros; 50,5° vertical medido na vila). Uma opção global precisaria de um gancho no `CameraManager` (ou na projeção, que também é usada pelo enquadramento automático da câmera e pelas cenas), e ficou para depois. **LOD/distância de desenho**: nada nos scripts nem nos `fighttrees\*.bfig` controla distância de desenho; as ocorrências de "lod" são de props. **Profundidade de campo** foi implementada (acima); ela aparece nas cenas, não na jogabilidade.

### Mipmaps e filtro anisotrópico

Antes só o nível base das texturas era enviado e todos os fetches usavam `SampleLevel(..., 0.0)`: nada de trilinear e nada de anisotrópico, embora o jogo peça 16× AF (fetch constant `aniso_filter = 5`, mip linear) em quase todas as texturas da fase. Isso causava o "chiado" das texturas ao longe.

- `texture_decode`: `TextureLevelCount` e `ComputeMipLayout` (regras do `GetGuestTextureLayout`/`GetPackedMipOffset` do Xenia, BSD): os mips começam no `mip_address`, cada nível com o pitch de uma textura potência de 2 alinhado a 32×32 blocos e alinhado a 4 KB; a partir do primeiro nível com lado menor ≤ 16 texels, todos ficam numa cauda empacotada com offsets fixos. O renderer cria a cadeia inteira, decodifica cada nível no seu footprint (`GetCopyableFootprints`) e o hash de mudança inclui os mips. `CRASH_GPU_NO_MIPS=1` volta ao nível base. Conferido nas capturas: nenhuma textura corrompida, folhagem e troncos distantes sem o ruído (`docs\capturas\antigas\graficos\mipmaps_antes_depois.jpg`).
- Tradutor (`FindGradientFetches`): o HLSL não aceita gradientes dentro do laço da máquina de estados, então as derivadas são tiradas no `main()` (fluxo uniforme) dos registradores de entrada (interpoladores ou param gen). Um fetch 2D com LOD calculado usa `SampleGrad` com elas quando as componentes da coordenada não são escritas por nenhuma instrução que possa rodar antes (antes na ordem do control flow, ou em qualquer ponto de um loop/salto para trás que o contenha; escrita relativa `r[aL+n]` desliga a análise do shader). Os gradientes são escalados pelo LOD bias do fetch constant e da instrução e por `xe_effect.w`. Fetches com coordenada calculada continuam em LOD 0. Cobre os shaders principais da fase (simpleunlit, simplelit, fur, água, partículas: 15 PS).
- Sampler: mip filter 2 (*base map*) → `MaxLOD = 0`; `MinLOD/MaxLOD` do fetch constant; a opção "filtro anisotrópico" (1/2/4/8/16) usa o maior entre o pedido do jogo e a opção nas texturas da memória com filtro linear (não nos resolve targets).

### FXAA e SMAA

SMAA 1x oficial (`thirdparty/smaa`, MIT, commit `71c806a`): detecção de borda por luma, preset HIGH, três passes, com as tabelas AreaTex/SearchTex. `gpu/smaa.*` tem root signature, PSOs, alvos e um heap de descriptors próprios (o renderer religa os seus depois). O `SMAA.hlsl` vai embutido no exe como string (`SMAA.hlsl.inc`, gerado por `tools/embed_smaa.py`). O anti-aliasing de pós agora roda na resolução do front buffer (a interna), não mais na da janela: cópia 1:1 com o swizzle → FXAA/SMAA → apresentação escalada. Os dois atuam na imagem final, com o HUD; o FXAA amolece o texto do HUD, o SMAA quase não (`docs\capturas\antigas\graficos\aa_sem_fxaa_smaa.jpg`). Com DLSS/FSR ligado não fazem falta.

### Upscaling temporal (DLSS/DLAA e FSR 3.1)

SDKs (`thirdparty/dlss`, `thirdparty/fsr`, cada um com README da versão e a licença):

- **DLSS** 310.9.16 (repositório oficial NVIDIA/DLSS, tag `v310.9.16`): headers, `nvsdk_ngx_s.lib` (CRT estático, como o projeto) e `nvngx_dlss.dll`, copiado para o lado do exe pelo CMake. NGX via `NVSDK_NGX_D3D12_Init_with_ProjectID` (engine CUSTOM), presets padrão do DLL (transformer K em DLAA/qualidade/equilibrado, M/L nos modos de desempenho), flags `MVLowRes | AutoExposure | DepthInverted`.
- **FSR** do AMD FSR SDK 2.3.0 "Redstone" (GPUOpen FidelityFX-SDK, tag `v2.3.0`): headers da FidelityFX API e as DLLs assinadas `amd_fidelityfx_loader_dx12.dll` + `amd_fidelityfx_upscaler_dx12.dll`, carregadas em tempo de execução. A consulta de versões nesta RTX 5080 lista "3.1.5, 2.3.4" (o FSR 4 só existe em RDNA 4); o runtime pede explicitamente a 3.1.x mais nova (`ffxOverrideVersion`), com `AUTO_EXPOSURE | NON_LINEAR_COLORSPACE | DEPTH_INVERTED` e o RCAS da opção "nitidez".
- Nada de frame generation: as DLLs de FG (`nvngx_dlssg`, `amd_fidelityfx_framegeneration_dx12`) nem foram baixadas.

**Duas escalas.** `g_renderScale` é a saída (a resolução escolhida) e `g_sceneScale` = saída × razão do modo (nativo 1, qualidade 1/1,5, equilibrado 1/1,7, desempenho 1/2, ultra 1/3). A cena 3D é o que o jogo desenha nos RTs com MSAA 2x/4x (as passadas do tiling predicado); pós-processamento, HUD e menus usam RTs 1x. Então um RT criado com MSAA ganha a escala da cena, e um 1x, a de saída. Cada resolve target pode ter duas superfícies (uma por escala): o resolve escreve a superfície da escala da origem e a torna a *current*, e fetches e `VdSwap` leem a current (o `xe_tex_size_scale` de cada fetch vem da superfície). Assim os resolves da cena (cor `0x0A642000`, profundidade `0x0A9DA000`, velocidade, cópias para refração) ficam pequenos para os próprios passes da cena.

**Onde o upscaler entra.** No primeiro draw 2D (alvo 1x) que lê o resolve de cor da cena daquele quadro (color 0 de um RT de cena; na fase, a cópia `simpleunlit` da cena para o RT 1x, antes do motion blur), `RunTemporal` gera os motion vectors, chama o DLSS/FSR (cena → saída) e copia o resultado para a superfície de saída do mesmo resolve target, que vira a current. O resto do quadro (cópia, motion blur, DOF, HUD, resolve do front buffer) roda em resolução de saída, sem jitter. Não foi preciso identificar o HUD draw por draw: ele já é desenhado depois, nos alvos 1x. Menus e logos sem cena não passam pelo upscaler.

**Jitter.** Halton(2,3) com 8·(saída/cena)² fases, somado no epílogo do vertex shader (`p.xy += xe_jitter.xy · p.w`) só nos draws de cena, nunca em rectangle lists (clears) e só em vértices com W ≠ 1: os quads de tela (sombra, escurecimentos) têm W = 1 e ficam alinhados aos pixels. Em NDC do RT, `2·j/largura` (a viewport do RT é calculada em pixels guest). Com upscaling, os gradientes das texturas são multiplicados por cena/saída (o LOD bias negativo recomendado pelos dois SDKs).

**Motion vectors** (RG16F, em UV, atual → anterior):

- Objetos que se mexem: o mapa de velocidade do jogo (RT 1 da cena, a entrada do motion blur). O vs `429975a046a1a236` calcula a posição com o MVP atual (c4–c7) e o anterior (c16–c19), e o PS grava `saturate((ndc_atual − ndc_anterior)·K + 0,5)` em xy e 1 em zw, com K = c255.x = 5 (lido do próprio shader). O clear por aliasing de profundidade deixa alfa 0, então alfa > 0,5 separa "objeto escreveu" de "fundo". O resolve troca R/B, então x está no azul.
- O resto (cenário, céu): reprojeção da câmera pela profundidade. A view-projection sai das constantes dos vertex shaders da cena: `VP = MVP · Model⁻¹` (c4–c7 e c12–c15 de `vs aecab5202246c470` e `429975a046a1a236`); fica o valor em que a maioria dos até 64 draws do quadro concorda (o céu e outros diferem). Profundidade: `z_janela = −ndc + 1` (viewport ZSCALE −1/ZOFFSET 1, lido dos draws), ou seja invertida, o que vira a flag `DepthInverted`. Para o FSR, near/far/FOV saem da decomposição da VP (0,5 / 1000 / 50,5° na vila).
- `reset` quando o quadro anterior não passou pelo upscaler (menus, carregamento) ou quando a VP muda mais da metade (corte de câmera).
- Conferido com `CRASH_GPU_SHOW_MOTION=20` (a cena mostra os vetores: vermelho/verde = componentes, azul = veio do mapa do jogo): andando para a frente o campo se expande a partir do ponto de fuga, o Crash vem do mapa de velocidade (`docs\capturas\antigas\graficos\dlaa_movimento_motion_vectors.jpg`). O log tem `[gpu] temporal frame N: camera ok (...)` nos primeiros quadros e a cada 900.

**Medido** (RTX 5080, janela 1280x720): DLAA, DLSS Qualidade, FSR 3.1 nativo e FSR 3.1 Qualidade sem falhas em todos os quadros (`docs\capturas\antigas\graficos\upscalers_720p.jpg`); 2560x1440 com DLSS Desempenho (cena 1280x720) também (`docs\capturas\antigas\graficos\dlss_desempenho_1440p.jpg`). Custo de CPU da chamada: 0,1 ms (720p) e 0,17 ms (1440p); o jogo continua a 30 fps com o mesmo ritmo do modo sem upscaler. A inicialização do NGX leva ~1,2 s: só o upscaler escolhido é sondado no boot; o outro, quando a tela SETTINGS pergunta (o FSR leva 3 ms). As capturas saem no tamanho da janela, então os detalhes acima de 720p aparecem reduzidos nelas.

**Limitações.** O mapa de velocidade do jogo tem 8 bits (±1/K em NDC, quantização de ~0,25 px a 720p) e só cobre o que o jogo marca: partículas e transparências não têm vetor próprio e podem deixar um leve rastro. A primeira execução numa escala nova recompila os shaders da cena (alguns segundos na carga da fase).

### Ferramentas e diagnóstico

- Perfil `graficos` (`config/run.toml`): LOAD GAME até a vila, anda, gira a câmera, ataca e pula, com capturas pelo relógio do input. O roteiro de entrada ganhou `LS_*`/`RS_*` (analógicos a toda; `tools/run_game.py` aceita os nomes).
- O ritmo de quadros (`[gpu] present pacing`) agora mostra também submit, o `Present` mais longo e as esperas por espaço nos anéis de upload/descriptors; o resumo do renderer inclui quadros upscalados e o custo de CPU por chamada.
- `diagnostics.cpp`: o handler de crash é *vectored* e via as exceções de primeira chance de todo o processo. Acessos inválidos fora da memória guest agora seguem para o tratamento normal (SEH) do código que os gerou (driver, NGX, FidelityFX fazem leituras de sondagem dentro de `__try`), e um `SetUnhandledExceptionFilter` reporta os que ninguém trata (o código recompilado não tem frames de SEH, então os crashes dele continuam reportados). As exceções repassadas aparecem como `[diag] first-chance access violation ...` (as 8 primeiras). Motivo: duas execuções (1440p + DLSS, perfil `graficos`) caíram logo no primeiro quadro com leitura de `0xFFFFFFFFFFFFFFFF` (uma em `ntdll`); não se repetiu em mais 9 tentativas, uma delas com a mesma configuração e o mesmo perfil. Com várias instâncias do jogo rodando ao mesmo tempo (outras sessões testando) o ritmo de quadros piora e o link do exe falha; os testes de desempenho acima foram refeitos sozinhos.
- Visto durante os testes e **anterior a este passo**: uma faixa vertical mais escura no personagem congelado no gelo em x ≈ 832, a emenda das faixas 2 e 3 do tiling predicado. Provável causa: a refração lê a cópia da cena, que à esquerda da emenda já tem o gelo da faixa anterior (`docs\capturas\antigas\graficos\gelo_emenda_tiling.jpg`). Corrigido depois: passo 16, "Faixa no gelo na emenda x = 832".
- Crash ao abrir com **DLSS + FXAA/SMAA** (visto em 3840x2160 sem borda, DLSS Qualidade): acesso inválido dentro do driver (`nvwgf2umx.dll`, leitura de `0x2B88`) no 5º quadro, logo depois do primeiro quadro upscalado. Com upscaler, o front buffer muda de tamanho entre a escala da cena e a de saída, e `EnsureAaTargets` recriava os alvos do AA no meio do present com `WaitIdle()`, que submete e reinicia a lista de comandos; o present seguia sem root signature nem descriptor heaps (e com um descriptor já devolvido ao anel). Agora o front buffer e os alvos do AA são resolvidos antes de o present gravar qualquer comando. Conferido: `boot` com a configuração do crash (FXAA, SMAA e sem AA, mesmo desempenho) e `graficos` até a vila, 14 mil quadros upscalados sem falha.

## Passo 21: gravação e replay do controle no relógio do jogo (F5)

Para montar testes jogando, com analógicos e gatilhos, e que não dessincronizem quando o jogo carrega mais devagar. Código em `kernel/input_record.cpp` (gravação, replay, detecção de carregamentos; interface interna em `kernel/input_record.h`, pública em `kernel/input.h`), chamado por `kernel/xam_input_imports.cpp`; F5 em `gpu/window.cpp`, indicador em `gpu/overlay.cpp`, `SHOT` em `d3d12_renderer.cpp` (`input::ConsumeShotRequest`), leituras de arquivo em `kernel/file_imports.cpp` (`input::NoteGameRead`), observador de log em `diagnostics.cpp` (`SetLogObserver`, para o `WAIT`). Uso no README, "Gravando um perfil jogando (F5)".

### O relógio: leituras do controle (medido)

`CRASH_INPUT_TRACE=<csv>` escreve uma linha por `XamInputGetState(0)`. Numa execução do `primeira_fase_load`: um único ponto de chamada (lr `0x8228A804`) e uma única thread; ~47 leituras/s nos vídeos da abertura e **exatamente 30/s** daí em diante (150 a cada 5 s), mesmo com quadros de 62 ms: o laço do jogo é de passo fixo a 30 Hz, recupera os passos atrasados e lê o controle uma vez por passo. Então a leitura N é o passo de simulação N, e entregar o estado gravado na leitura N repete o que o jogo viu, com qualquer FPS. O tick é `g_tick` (incrementado só em `XamInputGetState` do controle 0; `XamInputGetCapabilities`/`SetState` e a tela SETTINGS leem sem avançar, `ReadController(..., poll)`). Nos arquivos, `t = tick / 30` com duas casas (volta exata com `round(t * 30)`).

Mas o tick não para nos carregamentos: com 3 instâncias ao mesmo tempo, o último carregamento depois do LOAD terminou nas leituras 1363/1404/1356 contra 1323 sozinho (1,5 a 2,7 s depois). Daí os pontos de sincronização.

### Carregamentos e SYNC

`file_imports.cpp` informa cada leitura de arquivo do jogo (`NoteGameRead(offset, bytes, default.rcf?)`). O `default.rcf` (fases, menus) é lido em rajadas de MB quando algo carrega (no `primeira_fase_load`: 3,5 MB no boot, 4,6 MB aos 5,7 s, quatro rajadas de ~2,4 MB nas intros, 3,8 + 2,2 + 1,9 + 1,8 MB depois do Y). A soma de bytes não serve de âncora: varia entre execuções equivalentes (uma das 3 leu 1,7 MB a mais no menu, e as rajadas se partem diferente). O que se repete é **qual pedaço** é lido. A gravação fecha um carregamento quando uma rajada de ≥ 1 MB fica 30 ticks sem leituras do `default.rcf` e escreve `SYNC` com o tick da última leitura, o offset dela e quantas vezes o jogo já tinha lido aquele offset. O replay, ao chegar no `SYNC`, segura a linha do tempo até `reads[offset] >= n` **e** 5 ticks sem leituras, e faz `shift = última leitura do replay - tick gravado` (pode ser negativo). Só os `SYNC` seguidos de alguma entrada vão para o arquivo. Segurar por mais de 60 s solta com `replay sync ... timed out` (o `expect` dos perfis gravados falha nisso).

`WAIT "texto"`: espera uma linha do log contendo o texto. Os `WAIT` casam em ordem (cada um com uma linha depois da do anterior, vistas mesmo antes de a linha do tempo chegar lá) e deslocam o resto pelo tempo esperado. O observador do log formata a linha fora do lock do log e ignora as linhas `[input]` (o próprio replay loga o texto esperado).

### Gravação

Sempre ligada desde a primeira leitura (é barata): por tick, o estado que o jogo recebe antes de a tela SETTINGS esconder (físico + teclado + `CRASH_INPUT_SCRIPT` + replay). Botões viram `[início, nome, duração]` ao soltar; analógicos em centésimos com a regra de 0,02 / centro / fim (|v| < 0,05 = 0); gatilhos igual. Na primeira leitura também guarda na memória `content\` e `settings.toml` da pasta de dados (29 KB aqui; limite 64 MB). F5 e F2 chegam pela janela e valem no **próximo tick** (atômicos consumidos em `Poll`), o mesmo caminho do `SHOT` do replay: assim a captura da execução gravada e a do replay saem do mesmo tick (antes, com o F5 na thread da GPU, a captura do fim saía 1 a 2 quadros diferente e a câmera girando não batia). F5 inicia: nome `gravado_<data>_<hora>`, grava o snapshot em `<CRASH_RECORD_DIR>\<nome>\userdata` e o perfil; a cada 60 ticks reescreve o `.toml` (via `.tmp` + `MoveFileEx`); F5 de novo ou `WM_CLOSE` (`input::StopRecording`) fecham com `SHOT` no fim e `END` 30 ticks depois. `CRASH_RECORD_DIR` vem do `run_game.py` (`config/recorded`); sem ele, `<exe>\..\config\recorded` quando existe `config\run.toml` ao lado, senão `<exe>\recorded`. `CRASH_RECORD_INPUT=1` começa no boot (perfil `gravar`). O perfil herda `audio` (de `CRASH_NOAUDIO`) e `CRASH_SETTINGS`.

### Replay

`CRASH_REPLAY=<arquivo>`: o `run_game.py` converte o `replay` do perfil em `replay.txt` na pasta da execução (`tick TIPO args` por linha: `BTN nome ticks`, `LS/RS x y`, `LT/RT v`, `SYNC offset_hex n`, `WAIT texto`, `SHOT`, `END`). `Step(tick)` solta os botões vencidos e aplica as entradas com `tick_gravado + shift <= tick`; um botão fica apertado pela duração a partir de quando foi aplicado (um deslocamento nunca corta um aperto). O estado do replay soma ao físico (botões OR; analógico do replay vale quando não está no centro; gatilho, o maior). `END` loga `replay finished` e chama `ExitProcess(0)`. O `dwPacketNumber` agora muda com qualquer mudança do controle (antes, só com botões), como no XInput.

O `CRASH_INPUT_SCRIPT` antigo (relógio de parede) também ganhou analógico em qualquer posição: `ms:LS=x/y[:ms]`, `RS=x/y`, `LT=v`, `RT=v`; no `run.toml`, `[s, "LS", x, y, (s)]`. E os botões `L3`/`R3`.

### Verificação (`docs\capturas\gravacao\`)

Gravação: LOAD GAME (o slot 1 entra direto na fase) e, na fase, LS (0,45; 0,9), RS (0,7; 0), LS (-0,6; 0,5), X, RT 1,0, A, LS (1; 0,35), RS (0,9; -0,2), com F5 aos 40 s, F2 aos 48,5 s e F5 aos 56 s (teclas enviadas à janela por `PostMessage`, o mesmo `WM_KEYDOWN` do teclado). Saiu `gravado_20261020_220035` (4 SYNC: boot, intros, LOAD, fase). Replay sozinho, duas vezes: SYNC de -0,07 a +0,40 s, capturas iguais à gravação a menos de 1,8/255 de diferença média (`sozinho_gravacao_replay1_replay2.png`). Três replays ao mesmo tempo: os carregamentos terminaram até 1,13 s depois cada um, linha do tempo deslocada +1,13 / +1,57 / +2,57 s no fim, nenhum timeout, as três terminaram `ok` com o Crash no mesmo lugar e diferenças de pose/câmera de 3,8 a 7,7/255 (`carga_3_instancias_replay.png`): o jogo não é totalmente determinístico (há coisas pelo relógio real), mas não dessincroniza. `gravacao_vs_replay.png` é da primeira gravação (pela cena de abertura, ainda com o F5 na thread da GPU). `menu_loaded` e `primeira_fase` (botões no relógio de parede) continuam passando.

## Mods de áudio: sons substituídos por arquivos soltos (`audio/audio_override.*`)

Parte das ferramentas de assets (README, "Extraindo e modificando assets"). Os sons do jogo são `.rsd` ("RSD6XMA ": cabeçalho de 0x800 bytes com canais, taxa e o caminho do `.wav` original; em 0x800 o formato XMA2, a tabela de busca e os pacotes XMA2 em blocos de 64 KB, com o stream `s` de cada bloco começando no pacote `s` e seguindo pelo campo *skip* do cabeçalho do pacote). Não existe encoder XMA aberto, então um mod não consegue gerar XMA novo. A solução não mexe no caminho de dados do jogo:

1. `tools/audio.py build --mod NOME` gera, para cada `mods\NOME\audio\<caminho exportado>.wav|.ogg` (`sons\music\...`, `vozes\english\...`), um `.rsd` substituto em `mods\NOME\rcf\<arquivo>\<entrada>` para cada entrada que toca aquele som (a exportação guarda uma vez só os sons idênticos, como os gritos iguais nos seis idiomas; servido pelo overlay de entradas de `.rcf`, `fs/rcf_overlay.cpp`). Ele copia o cabeçalho do original, ajusta o formato XMA2 e a tabela de busca para a nova duração e preenche os pacotes com **quadros marcadores** de 128 bits: comprimento (15 bits, 128), magia `CRWV` (`0x43525756`), id do som (32 bits, FNV-1a de `arquivo/entrada`), índice do stream (8 bits) e número do quadro (24 bits). São 63 quadros por pacote (o máximo do campo de contagem), o resto do pacote fica com bits 1 (comprimento `0x7FFF` = enchimento), e os pacotes seguem o mesmo leiaute de blocos/streams/skip do original. Para o jogo é um som XMA comum: ele lê o arquivo, monta os contextos e envia os buffers como sempre.
2. `audio/xma.cpp`, em `DecodeFrame` (a única mudança fora do arquivo novo, duas linhas): antes de mandar o quadro ao WMA Pro, `audio_override::DecodeMarkerFrame` confere a magia; se for um marcador, ele acrescenta as 512 amostras daquele quadro e o FFmpeg não é chamado. A contabilidade de 512 amostras por quadro, os loops, a troca de buffers e o anel de saída continuam idênticos, então qualquer forma de o jogo tocar o som (streaming, loop, cenas) funciona.
3. `audio_override.cpp` lê na primeira vez os `mods\<mod>\audio\overrides.txt` (id, arquivo, entrada, canais e taxa do `.rsd`) na ordem de carga dos mods (vale o primeiro), decodifica o arquivo uma vez (WAV PCM 8/16/24/32 bits ou float; Ogg Vorbis com um demuxer de páginas Ogg próprio e o decodificador Vorbis do FFmpeg), converte para os canais e a taxa do `.rsd` (mapeamento mono/estéreo/5.1; reamostragem linear) e guarda em memória. Cada contexto recebe os canais `[stream × canais, +canais)` do som. Id desconhecido ou arquivo inválido = silêncio, com uma linha no log.

Log: `[audio] mod NOME: N sound replacement(s)` e, no primeiro uso, `[audio] <arquivo>/<entrada> <- mod NOME: x.wav (2 ch, 44100 Hz, 40.00 s -> 2 ch, 48000 Hz)`. Teste: a música do menu (`sound1.rcf` `sound\rsd\6\6ef753b2af326053.rsd`, `crash_idea43_part3_wave1`, achada correlacionando o `audio.wav` do perfil `menu` com as músicas exportadas: 1,0) trocada por um tom de 440 Hz (L) / 660 Hz (R) pulsando a 2 Hz, perfil `menu` com `--set mods_dir=private/assets/test_mods/audio`: o `audio.wav` da execução tem o pico em 440/660 Hz a partir de ~12 s, com o pulso de 0,5 s em ordem (`docs\capturas\antigas\assets_audio_mod_menu.wav`). Limites: o som substituto não deve ser muito mais longo que o original (o jogo pode guardar durações em outros dados); loops de música usam os mesmos campos do original ajustados à nova duração.

## Pacotes de idioma: tradução PT-BR (`settings/language_pack.*`, `settings/language_font.*`)

Pedido: textos em português do Brasil como **idioma novo** na tela SETTINGS, sem substituir o inglês e com as vozes originais em inglês (README, "Tradução PT-BR"). Código novo: `settings/language_pack.{h,cpp}`, `settings/language_font.{h,cpp}`; pontos de contato: `fs/rcf_overlay.*` (reescritas que mudam o tamanho), `settings/settings.*` (lista de idiomas dinâmica, `Language::pack`/`help`), `settings/settings_menu.cpp` (linhas de ajuda do idioma), `main.cpp` (`LoadLanguagePacks` antes do `settings::Load`, `RegisterLanguagePack` depois do `RegisterFrontendPatches`; `--languages`), `tools/texts.py` (`lang-init`, `lang-check`), `config/run.toml` (perfis `ptbr`, `ptbr_novo`).

**Desenho.** Cada pasta `languages\<id>\` com `language.toml` vira um `Language` a mais (id, nome, linhas de ajuda), que copia do idioma base o XConfig, o país, a letra da bíblia e o `.rcf` de voz: para o jogo é inglês. Quando é o idioma do boot, `RegisterLanguagePack` lê `texts\<bíblia>.txt` (mesmo formato do `texts.py export`, com seções `[pacote ...]`) e registra uma **reescrita** (`rcf_overlay::RegisterEntryRewrite`) para cada pacote listado. Não se gera nada em disco: o pacote é só texto, e editar um `.txt` vale no próximo boot. Por que não pacotes `.p3d` prontos num mod: `cdd70a8c.p3d` também tem o logo do título e `b4c85fe7.p3d` as fontes; um pacote pronto apagaria um mod de textura desses arquivos. A reescrita trabalha sobre a entrada como o jogo a veria (original ou do mod) e só troca os blocos de idioma.

**`rcf_overlay`.** `ApplyBuiltins` agora agrupa por entrada e passa os bytes por todos os patchers registrados para ela, em ordem (patches de mesmo tamanho e reescritas). Resultado do mesmo tamanho → sobreposições de bytes como antes; tamanho diferente → um `Replacement` com os bytes em memória (`data`), depois do fim do arquivo e do último mod, com o registro do diretório reapontado. Ordem no `main.cpp`: rótulo SETTINGS (patch embutido) primeiro, o idioma depois, então as chaves 0x1E96/0xE1F3 ficam com CONFIGURAÇÕES do pacote. A primeira leitura de cada entrada substituída ou reescrita sai no log (`[fs] rcf overlay: the game reads package\b4c85fe7.p3d (idioma pt-br)`): no Xbox o jogo lê `b4c85fe7` (fontes HD + `persistent`), `7efdcd91`, `cdd70a8c` (frontend) e `7a8185b0` (ingame da primeira fase).

**Reescrita das bíblias** (`language_pack.cpp`): percorre o `.p3d` inteiro reconstruindo os chunks (cabeçalho copiado, filhos reescritos, tamanho total recalculado; bytes que não formam chunk são copiados), e cada chunk de idioma (0x1800E) da letra base dentro de uma bíblia (0x1800D) que o pacote traduz ganha um buffer novo: texto do pacote (a seção do pacote, depois as linhas comuns) ou o original, cada string distinta uma vez; hashes, ordem e módulo ficam. 12 pacotes, 2 828 textos, no primeiro acesso ao `default.rcf`.

**Cópias diferentes da mesma bíblia.** As chaves são hash do nome módulo 0x1003F, e as cópias antigas (PS2/Wii/dev em `aabeaa37`, `ad83ca09`, `b9c6f232`, ...) usam a mesma chave para textos sem relação (`frontend` 0x0091 é " ´ " numa e "JEFF SOLWAY" noutra; `shared` difere em 85 de 91). Por isso o arquivo do pacote tem as linhas comuns (cópia maior primeiro, como o `texts.py`) e seções por pacote só com as chaves que diferem. Obs.: o `texts.py build` antigo aplica uma linha editada a todas as cópias com aquela chave, o que erra nessas cópias (não corrigido aqui).

**Fontes** (`language_font.cpp`). As fontes do Xbox (`Titans_Large`/`Titans_Small` em `b4c85fe7.p3d`) têm ã Ã Ê Ô (e `" * ~ < >`) como glifos de largura zero (só avançam: "Não" saía "N o") e não têm õ/Õ (o glifo some). Fonte Pure3D: 0x22000 (versão, nome, 4 floats, nº de texturas no fim do cabeçalho) com filhos 0x19000 (texturas; imagem 0x19001 → 0x19002 = u32 tamanho + PNG 8 bits paletizado, paleta branca com alfa em `tRNS`), 0x22001 (u32 n + registros de 40 bytes ordenados pelo código: textura, u0 v0 u1 v1 com v para cima, bearings, largura, avanço, código), 0x22006 (os códigos 0xB1–0xBE dos ícones de botão, tipo 5) e 0x22005. Cada receita `"ã = a + ñ - n @ å"` compõe o glifo. Os glifos são brancos com a forma no alfa: preenchimento opaco e um contorno de ~meio alfa em volta (o jogo pinta os dois de cores diferentes). O preenchimento do acento é o opaco que o ñ tem e o n (alinhado pelo corpo) não tem, acima do corpo; pedaços menores que 1/4 do maior saem (restos de células vizinhas). O contorno do acento é cada pixel do ñ a até uma largura de contorno desse preenchimento e não mais perto do preenchimento do corpo. O acento mantém a distância até o corpo (desloca pela diferença entre os topos de n e a), é centrado na base e entra pelo maior alfa. Antes (2026-10-20/21) o acento era "as linhas acima do topo do n", e depois "só o que difere do n". O contorno do til encosta no do N, então o primeiro cortava o til ao meio e o segundo deixava um buraco no contorno embaixo à direita do til sobre o O (`docs\capturas\ptbr\comparacao_acentos.jpg`). Os acentos originais da fonte (Á À É Í Ó Ú Â Ç) estão inteiros; o "topo 0" de Á/À é um risco da célula de cima. PNG lido com WIC (`windowscodecs`, `#pragma comment`) e escrito por um encoder próprio (blocos deflate sem compressão, mesma paleta/`tRNS`, cor mais próxima). Com menos de 32 páginas a fonte ganha uma página nova (clone de uma página com os nomes marcados com `P`, contagem +1). **Limite de 32 páginas**: a `Titans_Large` já tem 32 (18 de letras + 14 de ícones); com uma 33ª os glifos novos ficaram vazios e o texto do menu tremia. Nessas fontes o glifo vai para a célula do doador (`@ å`, letras que nenhum texto em inglês ou português usa: å ò Å Ò È Ö), redesenhada na página dela, e o registro do alvo aponta para lá. Fontes sem as letras de origem (as KLEPTO de outras plataformas) ficam como estão (`cannot compose` no log).

**Tela.** O nome é `PORTUGUES (BR)` (fonte do overlay sem acentos; "PORTUGUES (BRASIL)" com `< >` passava da borda) e o `help` do `language.toml` aparece na ajuda do item IDIOMA ("TEXTOS EM PORTUGUES DO BRASIL." / "AS VOZES CONTINUAM EM INGLES.").

**Teste**: perfis `ptbr` (menu, SETTINGS com o idioma, LOAD GAME, menu do episódio, vila, pausa e OPÇÕES) e `ptbr_novo` (NEW GAME: nome, sobrescrever, salvando, título do episódio), com `CRASH_SETTINGS="game.language=pt-br"`; `menu` em inglês continua igual. Capturas em `docs\capturas\antigas\ptbr\` (`00_antes_fonte_sem_til.jpg` mostra o Õ faltando antes da composição). `tools\texts.py lang-check pt-br` confere ícones, códigos de controle, espaços nas pontas, quebras de linha, chaves desconhecidas e letras que as fontes de `b4c85fe7` não desenham (contando as receitas).

## Cores do mojo: "PS2" (`settings/mojo_colors.cpp`)

Pedido: no port todo mojo é azul; no PS2 o mojo maior é roxo/rosa (e há um dourado). Correção do usuário depois da primeira versão (que pintava o XL de dourado): na primeira fase do PS2 os mojos são só azuis ou roxos, e o XL da piscina perto do começo não pode ser amarelo; o dourado só aparece mais adiante. Segunda correção: o roxo é o XL (o que tinha ficado dourado), e o L é um **azul bem claro, que brilha mais** que o azul normal. O dourado não precisa de correção.

**Causa: os dados do Xbox 360 (não é bug do renderer).** Os mojos que o jogo desenha vêm de `package\PermanentContent.p3d` (`bca1bc0b`, `default.rcf`): as quatro malhas `mojoSShape`, `mojoMShape`, `mojoLShape` e `mojoXLShape` usam **o mesmo** shader `c_mojo_m` (spheremap, reflexo `cone04.bmp`), com **a mesma** textura `c_mojo_256.tga.0` (PNG 256x256 de 8 bits com paleta) e as mesmas quatro paletas `c_mojo_256.tga.0`..`3` (chunk 0x19006 com filho 0x19007 de 1024 bytes B,G,R,A, a ordem do `D3DCOLOR`; a entrada 0 da paleta 0 é `2B 1B 07`, o `07 1B 2B` do PLTE do PNG ao contrário). A animação `TEX_c_mojo_m` (0x121000, tipo `PALA`, "palette switching", `m_RenderUsingPaletteSwitching` no executável) alterna as paletas 0-1-2-3-2-1: um brilho azul/ciano. Não há cor por tamanho em nenhum lugar: os parâmetros de cor do shader (`DIFF`, `AMBI`, `COLB`...) são brancos, o código não tem tabela de cor de mojo, e os pacotes `Collectables_c_mojo{S,M,L,XL}.p3d` e `props\c_mojo*.p3d` têm o mesmo material azul por tamanho (paletas idênticas, só em outra ordem). O mojo falso usa nomes próprios (`FAKEMOJOc_mojo_m1`, `c_fakemojo_256.tga`) e textura verde. O renderer desenha o que os dados pedem: com a paleta do L trocada, os orbes L ficam roxos no jogo (e com `CRASH_MOJO_COLORS=S=290` o pequeno também), então o caminho paleta → textura → shader está certo.

**Correção (opcional, padrão ligado).** `[game] mojo_colors = "ps2" | "xbox360"` no `settings.toml` e o item **CORES DO MOJO** na tela SETTINGS (vale ao reiniciar). Com `ps2`, `RegisterMojoPatches` registra uma reescrita (`rcf_overlay::RegisterEntryRewrite`) de `PermanentContent.p3d` que, no primeiro acesso ao `default.rcf`:

- acha os 8 chunks de nível 1 do material compartilhado (as 4 paletas, a textura, o shader, a animação `PALA` e o controlador 0x121201 `TEX_c_mojo_mPALAANIM`) e, logo depois de cada um, insere uma cópia por tamanho recolorido, com os nomes trocados no mesmo comprimento (`c_mojo_256.tga` → `c_mojo_256.tgN`, `c_mojo_m` → `c_mojo_N`, o que também cobre `TEX_c_mojo_m`; N = 3 para L, 4 para XL);
- nas cópias, troca o matiz das 4 paletas e do PLTE do PNG (CRC refeito), mantendo saturação e brilho (HSV): **L → mesmo matiz azul, saturação ×0,6 e brilho ×1,45 (azul bem claro; o halo também fica mais forte)** e **XL → 258° (lilás, saturação ×0,6, brilho ×1,05)**; o roxo foi medido numa captura do PS2 mandada pelo usuário (o mojo da piscina perto do começo: matiz ~262°, saturação ~0,35; a primeira tentativa, 290° com saturação ×1,1, ficou rosa demais; com 265° e ×0,75 o XL medido no jogo deu 271°/0,48, então foi ajustado); S e M ficam azuis. Um matiz "mantido" (`KEEP`) recalcula o matiz de cada cor;
- faz o mesmo com o **halo** de cada mojo, o efeito `c_mojo360glow` que o drawable anexa (0x802000b, "library/sparks/"): 5 chunks (animação `BQG_c_mojo360glowShape`, esqueleto, billboard 0x17006 com o quad `c_mojo_360glowBBShape`, composto 0x123000 e cena 0x120100), copiados com `360glow` → `360gloN`. A cor do halo é um azul escuro aditivo, `3F 1C 06 FF` (B,G,R,A), no quad (4 bytes depois de `AAX\0` no 0x17005) e no canal de cor `CLR` da animação (0x121109: `[versão]["CLR\0"][u32 chaves][u16 quadro por chave][cor por chave]`, `3F1C06FF` → `3F1C02FF`); nas cópias essas cores recebem o mesmo matiz. Sem isso o halo azul continua somado à cor nova (com um matiz dourado de teste, `CRASH_MOJO_COLORS=XL=44`, o XL parecia rosado);
- renomeia do mesmo jeito a malha do tamanho (shader do primitive group e o 0x121201 dentro dela) e o drawable 0x802000a dela (`TEX_c_mojo_m` no 0x8020003 e o efeito `c_mojo360glow`). Os nomes que o jogo procura (`c_mojoL`, `c_mojoLCollectableShape`, ...) não mudam.

O pacote passa de 3 098 979 para 3 175 407 bytes (`[settings] mojo colours: PermanentContent ... 8 material + 5 halo chunks copied for L hue kept sat x0.60 val x1.45, XL hue 258 sat x0.60 val x1.05` no log). Por que copiar em vez de recolorir no lugar: as quatro malhas dividem os mesmos objetos no inventário, então mudar a paleta mudaria todos os tamanhos. A reescrita vale para um mod que substitua `PermanentContent.p3d` (ela recusa e deixa como está se não achar os 8 + 5 chunks). Teste de canais: com `CRASH_MOJO_COLORS=L=0` o L sai vermelho, não azul, então não há troca R/B no caminho paleta → textura.

Depuração: `CRASH_MOJO_COLORS="S=290,M=44"` recolore esses tamanhos com esses matizes (os outros ficam azuis), independente do `settings.toml`; `CRASH_MOJO_DUMP=<arquivo>` grava o pacote reconstruído (confere com `tools\p3d.py`: ida e volta byte a byte).

**Teste**: perfil `mojo` (NEW GAME até a vila da primeira fase, como o `primeira_fase`, numa cópia dos saves; captura parada aos 150 s do relógio do input). Usa NEW GAME porque o LOAD GAME carrega o save mais recente, que pode estar em outra fase (o da máquina do usuário já está na fase 2, onde o cenário e os mojos mudam). Logo no início da vila aparecem os três tipos: mojos S no telhado da casa da esquerda, um **L** na janela esquerda da casa da árvore e um **XL** na janela direita (identificados recolorindo um tamanho por vez com `CRASH_MOJO_COLORS`). Capturas em `docs\capturas\antigas\mojo\` (`antes_xbox360.jpg`, `depois_ps2.jpg`, `comparacao.jpg`, `janelas_L_XL_zoom.jpg`; o `LEIA-ME.txt` lista todas). Com o pacote reescrito a fase carrega normalmente pelo LOAD GAME (sem `[fatal]`/`[crash]`).

## Legendas (`subtitles/`, `tools/subtitles.py`)

Pedido: legendas opcionais nas cenas (o jogo não tem), em inglês e PT-BR. Uso e arquivos: README, "Legendas". Código novo: `subtitles/subtitles.{h,cpp}` (texto, impressões digitais, relógio), `subtitles/subtitle_draw.{h,cpp}` (GDI → bitmap → overlay), `tools/subtitles.py`. Pontos de contato: `audio/xma.cpp` (2 chamadas e o campo `HostContext::inputValid`), `audio/audio_output.*` (`SubmittedFrames()`), `gpu/overlay.*` (`DrawBitmap`, `DrawUploaded`, correção do pixel shader), `gpu/d3d12_renderer.cpp` (uma chamada depois do present, antes da tela de configurações, das capturas e do overlay), `settings/*` (3 opções), `main.cpp` (`subtitles::Init()` depois do `RegisterLanguagePack`).

**Texto.** Nada nas bíblias além dos títulos dos episódios (`frontend`/`persistent`, ex. `0x7AA5 = A NEW HOP`). As falas das cenas são um `.rsd` de 6 canais por cena (`vo_nis\6_channel\nisNN_chX`, 3 streams estéreo; diálogo no centro), as avulsas um clipe por fala (`tutorial\hint001_aku` ...). `tools/subtitles.py transcribe` passa o canal central (ou a mixagem, nos clipes) a 16 kHz para o faster-whisper (large-v3, GPU via as DLLs `nvidia-*` do pip). Numa passada só por cena o Whisper juntava falas distantes e pulava as do meio (a abertura saía com 14 trechos e buracos de 15 s); cortando em trechos de fala pelo envelope (−42 dB, silêncios > 0,45 s) saem 23, com tempos justos. 862 sons, ~5 min na RTX 5080.

**Reconhecer o som.** O jogo não diz qual som toca: o decodificador só vê pacotes XMA em memória física. Cada buffer de entrada novo (bit de válido que sobe) tem os primeiros 8 pacotes (16 KB, ou menos num buffer menor) passados por FNV-1a 64; numa thread na abertura o runtime lê do `.rcf` (`vfs::Locate`, diretório/nomes como no `tools/rcf.py`) cada `.rsd` legendado e calcula o mesmo hash para: o bloco 0 a partir do pacote `s` de cada stream `s` (o jogo dá a cada contexto um buffer começando no primeiro pacote do seu stream: `0x0F400000`, `+0x800`, `+0x1000` na abertura) e o início de cada bloco de 64 KB seguinte (os buffers seguintes de todos os contextos começam no bloco). 858 sons → 2 186 hashes, 2 ambíguos (descartados). Um bloco seguinte reconhece o som no meio e dá a posição pela tabela de busca do XMA2 (amostra inicial de cada bloco). A taxa do `.rsd` está em `0x10` (canais em `0x08`, bits em `0x0C`).

**Relógio.** Primeira tentativa: frames decodificados pelo contexto (512 amostras cada). Não serve: o streamer do jogo decodifica a cena em rajadas de ~2,3 s (posição decodificada 3,4 s à frente logo no início, depois saltos de 2,27 s a cada ~2,3 s) e clipes curtos inteiros de uma vez. O relógio usado é o de áudio do jogo (`audio::SubmittedFrames()`, 256 amostras a 48 kHz por quadro enviado à saída), desde o reconhecimento, limitado ao decodificado (não passa do que o jogo já tem) e parado se o contexto não decodifica há > 3,5 s (pausa). Atraso medido: correlação do `audio.wav` da execução com a mixagem 5.1→estéreo da `nis01` (0,96): o som começa 243 ms (11 658 amostras) depois do reconhecimento; mais ~43 ms da fila do XAudio2 → `g_offset` = 0,28 s (`CRASH_SUBTITLE_OFFSET_MS`). Conferido também com o Whisper na gravação: "Episode 1. A New Hop" e "Crash. To walk or run..." soam no instante em que os clipes são reconhecidos (eles são decodificados ao tocar, não na carga).

**Fim.** Contexto que passa a outro áudio: stream não terminado → acabou (cena pulada); som já decodificado inteiro → continua tocando da memória do jogo (o clipe do episódio é seguido logo pela cena no mesmo contexto). Três sons legendados no mesmo contexto em 0,5 s = descompactação na carga, descartados (não visto até aqui; os sons da fase descompactados na carga são efeitos). Bug corrigido: o relógio de um som decodificado inteiro ficava preso no fim do som e, com o atraso, a última fala ("A New Hop") nunca saía da tela; agora ele pode passar do fim.

**Desenho e o travamento da GPU.** `subtitle_draw.cpp` rasteriza o texto com GDI (Segoe UI semibold, `ANTIALIASED_QUALITY`, 4,3% da altura da janela × tamanho, `DT_WORDBREAK` em 82% da largura) numa DIB, monta RGBA (caixa, contorno por dilatação separável, texto) e desenha 1:1 com `overlay::DrawBitmap` (buffer de upload que cresce e reaproveita a cópia enquanto o texto não muda). Na primeira versão a GPU travava (`DXGI_ERROR_DEVICE_HUNG`, TDR) ao desenhar a legenda, e depois as texturas do jogo falhavam. Bissecção com `DrawImage`: só em **y ímpar**. O pixel shader do overlay faz `discard` fora da imagem e depois lê `pixels[p.y * w + p.x]` de um root SRV (sem limites); as *helper lanes* dos quads na borda continuam executando a leitura com `p.y = -1` e leem antes do começo do buffer. O overlay e a tela SETTINGS nunca tinham caído em coordenada ímpar. Correção no shader: `p = clamp(p, 0, rect.zw - 1)` depois do `discard`.

**Testes**: perfis `legendas` e `legendas_ptbr`; capturas em `docs\capturas\antigas\legendas\`. NEW GAME/LOAD não mudam (o caminho de NEW GAME até a vila é o mesmo do `primeira_fase`, que continua passando).

## Tela de configurações nativa (`settings/settings_menu.cpp`, `settings/menu_art.*`)

Pedido: a tela SETTINGS parecia um overlay de depuração; deveria parecer um menu do jogo, com as fontes dele (a do aviso "não desligue o Xbox 360" no corpo e a do NEW GAME nos títulos), abas por tema trocadas com LB/RB e, depois, com a mesma transição das outras telas do menu e numa tela própria, como o LOAD GAME. Uso: README, "Configurações" e "Visual". Código novo: `settings/menu_art.{h,cpp}`; reescrito: `settings/settings_menu.{h,cpp}`; pontos de contato: `settings/frontend_patches.cpp` (destino do `ExitCalibration`, patch da tela de opções), `settings/settings.{h,cpp}` (`SettingsScreenEmptied`, nomes FRANÇAIS/ESPAÑOL), `settings/language_font.{h,cpp}` (`DecodePng` público, `CleanCrop`), `gpu/overlay.{h,cpp}` (canais do `DrawBitmap`), `gpu/d3d12_renderer.cpp` (uma chamada: `settings::menu::Draw`), `kernel/xam_input_imports.cpp` (`GameButtons`), `main.cpp` (`settings::menu::Init`, `--settings-preview`), `languages/pt-br/language.toml` (nome e ajuda com acentos), `config/run.toml` (perfil `configuracoes`).

**A tela do jogo por trás.** Primeira tentativa: deixar o `ExitCalibration` ir para a tela de calibração (nó 0x23B) e esvaziá-la. Mudar a lista de páginas do `GameStart_VideoCalibration.scr` (0x18001: nome, u32 0, u32 nº de páginas, nomes) ou a flag do layer (0x18020: nome, u32 1, u32 1, u32 1, u32 0xFF) não teve efeito (o jogo mostra as páginas e os layers por conta própria); zerar o alfa das cores dos elementos funcionou, mas a tela de calibração é preta (página `FE_Black` + o nó não lista o fundo) e entra por corte. No `Frontend.bfig`, as telas com o tecido listam as texturas `FE_Background_*` no nó; o nó `OptionsScreen` (0x63, menu de opções do **PS2**: WIDESCREEN / SOUND / VIBRATION / CREDITS, alvo do `ExitOptions` do menu de PS2) existe nos dados do Xbox, lista o fundo, mostra `FE_Button_Back`/`FE_Button_Select` e o `ExitBack` dele vai ao menu principal (0x46). Registro de saída (`0xBC27359D`): `[u32 nó dono][u32][u32][f32 -1][u32][f32 -1][u32 destino]` depois do nome (preenchido a 4; um nome de 8 letras não ganha NUL). Então: o `ExitCalibration` vai para o destino do `ExitOptions` (procurado pelo nome, não fixo) e a página `GameStart_OptionsPS2.pag` de `package\cdd70a8c.p3d` fica transparente por um patch de mesmo tamanho (alfa 0 nas cores B,G,R,A: textos `00 0F 37 E1` e sombra `00 40 80 32` ×7, menu 0x18010 `EB DE 94 FF`/`A0 7D 1E FF`, logo Pro Logic 0x18022 `FF FF FF FF`; o patch confere as contagens 14/2/1 e não aplica nada se não bater). O jogo troca de tela por corte (como nas outras telas); a tela das configurações aparece 0,12 s depois, com fade de 0,22 s subindo 18 px virtuais. Fechar grava o `settings.toml`, some em 0,11 s e `GameButtons()` devolve B ao jogo por 0,2 s em `ReadController(0)` (somado depois do controle zerado): o `ExitBack` da tela de opções toca o som de voltar e volta ao menu principal (cursor em NEW GAME). O A do jogo (`ExitAccept`, que no PS2 entraria nas opções) nunca chega ao jogo com a tela aberta. Se o patch da página não aplicar (`SettingsScreenEmptied()` falso), a tela é desenhada no estilo do menu de pausa: fundo escurecido (alfa ~0,92), espirais `InGame_Pause_Background` nas bordas (somadas como luz: o jogo as desenha aditivas; o preto da textura some) e o próprio (B) VOLTAR.

**Fontes** (`menu_art.cpp`). Só existem `Titans_Large` (texturas `Titans_v3_96`, células de 89 px, 32 páginas) e `Titans_Small` (`Titans_v3_64`, 59 px, 20 páginas) no Xbox (`package\b4c85fe7.p3d`; as KLEPTO são de outras plataformas). Os itens do menu principal usam a Large e os avisos de save ("Salvando o jogo... Não desligue o console Xbox 360.") a Small. Cada glifo é uma célula de altura fixa (a linha) com bearing esquerdo e avanço; o branco tem alfa 254 no preenchimento e ~105 (0,41) no contorno, e o jogo pinta as duas faixas: o texto desenhado mapeia alfa 0,41 → cor do contorno e 1,0 → cor do preenchimento, com a cobertura pelo alfa até 0,41. Cores medidas nas capturas: item laranja `(214,120,14)` / contorno `(70,28,0)`, selecionado `(150,218,212)` / `(30,104,110)`; no menu de pausa `(232,160,0)` / `(104,48,8)` e `(144,216,232)` / `(24,104,128)`. Ícones de botão: códigos 0xB1–0xBE com textura `0x80000000 | página` (± A, ² B, ³ X, ´ Y, µ BACK, ¶ START, · RT, ¸ RB, ¹ analógico, º seta esquerda vazada, » seta direita cheia, ¼ setas em círculo, ½ LT, ¾ LB); a seta esquerda é desenhada como a direita espelhada. Letras que faltam (ã õ Ã Õ Ê Ô) são compostas com `language_font::ComposeFont` e as mesmas receitas do PT-BR; `* < > " ~ ^ _ |` não existem (por isso o "vale ao reiniciar" é o ícone ¼ e não `*`). **Vazamento entre linhas**: alguns glifos passam alguns pixels para a célula de baixo (traço sobre Á/Í). A célula de cada glifo é limpa por preenchimento a partir da tinta da linha logo acima dela (acentos nunca encostam nessa linha). O mesmo vazamento estragava a composição do PT-BR na Titans_Small: a célula do Â começa com o traço da linha de cima, que era tomado pelo circunflexo, e o Ê/Ô saíam só com um traço; `language_font.cpp` agora recorta as células com `CleanCrop` (a mudança vale também para os textos do jogo em PT-BR).

**Outras artes**: `MSG_Background` (b4c85fe7; canto superior esquerdo de uma caixa de mensagem: curva, borda e miolo preto) espelhado nos quatro cantos, com a parte reta esticada e a curva em escala uniforme; `FE_star_open` (c1e387c7, sprite de um tile) para as barras: 10 estrelas, a última recortada na fração, cheia = estrela na cor do contorno 1,5 px maior + estrela na cor do texto, vazia = estrela menor marrom translúcida (`FE_star_closed` quase não aparece); `FE_Name_Divider_Left/Right` (cdd70a8c) em volta do título. Sprites são cortados em tiles com borda de 1 px (`tools/textures.py`); para um tile só, o campo de tamanho pode vir 0 (estrelas) e então a imagem é usada inteira. As entradas são lidas do `default.rcf` (ou do arquivo solto de um mod em `mods\<mod>\rcf\default\`), não pelo `rcf_overlay` (as reescritas de PT-BR não interessam aqui). ~0,5 s numa thread iniciada logo depois do `subtitles::Init`; a tela espera até 2 s se abrir antes. Sem as fontes, cai na fonte do overlay (sem acentos).

**Desenho.** CPU, no tamanho da janela (o back buffer: nem a resolução interna nem a cena do DLSS/FSR), num espaço virtual 1280x720 escalado por `min(w/1280, h/720)` e centralizado. Glifos e imagens são reamostrados por área (redução) ou bilinear (ampliação), separável, e guardados em cache por tamanho (limpo quando a janela muda). A imagem (RGBA pré-multiplicado, exportada sem pré-multiplicar para o PSO do overlay) é refeita só quando algo muda (botão, valor, idioma, paleta, tamanho); durante o fade é copiada com o alfa escalado. Vai ao back buffer com `overlay::DrawBitmap` no canal próprio `BITMAP_SETTINGS`: antes o `DrawBitmap` tinha um único buffer de upload estático, e as legendas e a tela juntas se reenviariam a cada quadro (o buffer de 4 fatias de 3840x2160 tem ~133 MB de memória de upload; só existe depois da primeira abertura nesse tamanho). Nos quadros em que nada muda não há custo de CPU (o tempo de um redesenho não foi medido à parte; nas execuções do perfil `configuracoes` o frame time médio ficou em ~30 ms, a 30 fps).

**Abas e entrada.** ÁUDIO (4), VÍDEO (6), GRÁFICOS (8; 9 com o HD AI Remaster, veja abaixo), JOGO (idioma, cores do mojo, armazenamento) e LEGENDAS (3); LB/RB (`MenuButtons` já mapeia Q/E) dão a volta; cada aba lembra a linha; ao abrir, ÁUDIO, primeira linha. Itens informativos (RESOLUÇÃO DAS SOMBRAS, ARMAZENAMENTO) não são selecionáveis. Textos em inglês, ou em português quando o idioma escolhido (o valor atual da opção) começa com `pt`. O log usa as chaves do `settings.toml` e o valor em inglês/neutro: `[settings] audio.master -> 90%`, `[settings] video.antialiasing -> FXAA`, `[settings] tab -> video`. Opções, ajuda, o que vale na hora / ao reiniciar, `NeedsRestart` e a gravação não mudaram.

**`--settings-preview <pasta>`**: depois do `vfs::Init`/`settings::Load`, desenha cada aba em inglês e PT-BR em 1280x720, 1920x1080, 3840x2160 e 2560x1080 sobre um amarelo liso e sai (sem renderer). Usado para ajustar o layout sem abrir o jogo; as imagens são dados do jogo (ficam em `private\`).

**Teste**: perfil `configuracoes` (abre SETTINGS, LEFT ×2 = volume 90%, RB = VÍDEO, DOWN ×4, A = FXAA, RB ×4 passando por GRÁFICOS, JOGO, LEGENDAS e voltando a ÁUDIO, B, LOAD GAME; capturas pelo relógio do input com `CRASH_SCREENSHOT_TIMES`), em inglês e em PT-BR com `CRASH_SETTINGS="game.language=pt-br;video.window=borderless;video.resolution=3840x2160;graphics.upscaler=dlss;graphics.upscaler_quality=quality"` (janela 1920x1080: a tela fica na resolução de saída), e `menu_loaded`. Capturas em `docs\capturas\settings_nativo\`. Achado de passagem: `settings::Load` reescreve o `settings.toml` em toda abertura (o `if (!in)` depois de ler até o fim é sempre verdadeiro); os valores são os mesmos, mas os comentários feitos à mão se perdem e um `CRASH_SETTINGS` seria gravado no arquivo se o perfil usar `userdata_mode = "shared"`.

## Chiratta de capacete com o rosto escuro: culling de personagens espelhados

**Sintoma** (gravação `gravado_20261020_221744`, quadros 2600–2800, primeira área do episódio 2): um dos ratlabs, o Chiratta com o capacete de cientista (cúpula escura com lâmpadas, `Accessories_cbt_chiratta_scientist.p3d`), aparecia com o rosto e o corpo escuros e acinzentados. Os outros Chiratta estavam normais. Capturas em `docs\capturas\ratlab\` (`antes_depois_zoom.jpg`; `antes_*.jpg` / `depois_*.jpg` em 1920x1080 com resolução interna de 3840x2160; `depois_final_*.jpg`; `primeira_fase_antes_depois.jpg`).

**Como foi achado** (nesta ordem):
1. Trace do quadro (`CRASH_GPU_TRACE_SWAP`). Os três corpos de Chiratta são o mesmo draw: `vs 429975a046a1a236`, `ps a31efa3e1c035104` (`palsimpleshaderlitfp`), a mesma textura de 8 bits com a mesma paleta, as mesmas constantes de PS e os mesmos bools de luz. O capacete é o draw seguinte (`ps 79ab15b821efc5f7`, 516 triângulos, textura 32x32), e as lâmpadas são um draw aditivo de 18 triângulos.
2. Override do PS (`shaderoverride`) mostrando a cor da paleta sem luz: o Chiratta de capacete fica normal (rosto laranja, jaleco branco). O problema era a iluminação.
3. Override mostrando a normal interpolada: nos Chiratta normais ela sai verde/ciano, no de capacete sai rosa. Mesmo assim, as normais no vertex buffer (skinning na CPU) e a `matRot` (c0–c2 do `simpleshaderlitvp`) estavam coerentes nos três (conferido com `CRASH_GPU_TRACE_DUMP`).
4. A diferença estava no estado: o corpo do Chiratta de capacete tem `PA_SU_SC_MODE_CNTL` = `0x18002` (`cull_back`, `face` 0), os outros têm `0x18000` (sem culling). A matriz de mundo dele tem **determinante −1**: o jogo desenha esse Chiratta espelhado em Z (sempre o de capacete, em todas as execuções). O espelho inverte o sentido dos triângulos: os que estão de frente para a câmera ficam anti-horários na janela. Lendo `face` 0 como "horário é frente", o `cull_back` removia exatamente o lado de fora, e sobrava o interior do corpo, iluminado pelo lado errado. Com `CRASH_GPU_NO_CULL=1` (sem culling nenhum) ele fica certo.

**Causa: duas regras de "frente" no Xenos.** O bit `face` orienta o **culling** com 0 = anti-horário é frente (como no R6xx e no Xenia; no D3D do 360, `D3DCULL_CW` = 2 = `cull_back` com `face` 0 e `D3DCULL_CCW` = 6 = `cull_back` com `face` 1). O stencil de duas faces segue a regra do D3D9, horário = operações da frente, qualquer que seja o `face`. O quad de tela inteira do passe de sombra (horário, `face` 0) precisa das operações da frente (passo 16). Era por isso que a correção do passo 16 tinha virado o `face` para tudo: ela consertou o stencil, mas inverteu o culling. Até aqui ninguém tinha notado porque o jogo quase não usa culling. Em todos os traces (fase 1, menus, episódio 2), os únicos draws com culling são esse corpo espelhado e as lâmpadas do capacete dele, ambos com `D3DCULL_CW`.

**Correção** (`d3d12_renderer.cpp`, montagem do `PipelineKey`): `FrontCounterClockwise` = false sempre (stencil e `SV_IsFrontFace` com horário = frente), e o culling é traduzido pelo `face`: `face` 0 → `cull_back` corta os horários (`D3D12_CULL_MODE_FRONT`), `cull_front` corta os anti-horários; `face` 1 → o contrário. Para `face` 0 sem culling (todo o resto do jogo) nada muda.

**Efeito colateral esperado:** as lâmpadas do capacete também usam `D3DCULL_CW`, mas a matriz delas não é espelhada (det +1). O jogo trata o Chiratta inteiro como espelhado. Com a regra certa, o brilho aditivo das lâmpadas (`chiratta_scientist_m1`) é cortado como seria no console, e as lâmpadas ficam só com a textura do capacete. Antes elas tinham um brilho a mais.

**Conferido:** a gravação (`saída ok`, replay terminado) com o Chiratta normal em todos os quadros de 2690 a 2750; `primeira_fase` (NEW GAME até a vila) idêntica à execução anterior (sombras por stencil, sem escurecimento nem faixas); a gravação começa com LOAD GAME, então o fluxo de LOAD também passou. Os dados do asset (`chiratta.glb`, `Accessories_cbt_chiratta_scientist.glb`, renderizados fora do jogo) mostram o rosto laranja sob a cúpula, como agora.

**Ferramentas novas** (só depuração): o trace de um draw agora mostra os bools (`bool consts`) e as constantes não nulas do VS (c0–c31); `CRASH_GPU_TRACE_DUMP=1` grava o vertex buffer de cada draw traçado em `draw_<n>_vf<fc>.bin` na pasta das capturas; `CRASH_GPU_NO_CULL=1` desliga todo o culling.

## Mods grandes: `.rcf` acima de 2 GB e texturas HD (`fs/rcf_overlay.cpp`, `fs/xenon_drive_hooks.cpp`, `gpu/hd_textures.*`)

**O limite de 2 GB era do jogo, não do formato.** Com um mod de 1,45 GB o `default.rcf` virtual passava de 2,6 GB e o jogo chamava `XamShowDirtyDiscErrorUI` (~quadro 300) e travava em `sub_823EA7E8` (escrita em 0x50, no fluxo do `XamLoaderLaunchTitle` que vem depois), sem ter lido nenhuma entrada do mod. Causa: `core::XenonDrive::vf14` (`sub_82296ED0`, a leitura da camada de arquivos do jogo: `r4` handle, `r7` offset u32, `r8` buffer, `r9` tamanho, `r10` lidos) chama o `SetFilePointer` do XAPI (`sub_823E8220`) com `lpDistanceToMoveHigh = NULL`. Aí a distância é um `LONG` com sinal (`extsw`), e um resultado acima de 0x7FFFFFFF dá erro 131/87, como no Win32. A leitura falha, e a thread de erro de disco (`sub_82288328`) mostra o "disco sujo". As outras chamadas de `SetFilePointer` são do `XenonSaveDrive` (saves) e de leituras com OVERLAPPED zero. `xenon_drive_hooks.cpp` reimplementa `sub_82296ED0` para offsets ≥ 2 GB: o mesmo `SetFilePointer`, com um dword alto zerado na pilha guest, e depois o `ReadFile` (`sub_823E87C8`), `GetLastError` (`sub_82265440`) e o status do drive (`sub_82296A90`, em `this+208`) como o original. Abaixo de 2 GB o caminho original é usado. O limite passa a ser o dos offsets u32 do diretório: 4 GB.

**Arranjo compacto.** `Layout()` mantém do original o cabeçalho (0x800), o diretório, a tabela de nomes e toda entrada não substituída; o resto do arquivo (as entradas substituídas, e o padding entre elas) fica livre. Cada substituição vai, nesta ordem: para o próprio slot, se cabe no trecho livre que começa ali; para o trecho livre em que cabe melhor (best fit, as maiores primeiro); para depois do fim. O `Read` serve qualquer substituição pelo offset (busca binária), também dentro do original. Patches internos de mesmo tamanho em entradas que um mod substitui agora ficam numa cópia em memória do arquivo do mod (antes eram patches de bytes no offset do apêndice). Entradas que não cabem em 4 GB ficam de fora, e o arranjo é refeito para que o slot delas continue com o original. O log mostra onde cada entrada ficou e o tamanho final (`virtual size ...`); acima de 2 GB vem um `WARNING`.

**Conferido:** mod de estresse `private\remaster_l1\mods_teste_2gb` (`scripts\derive_teste_2gb.py`: as 170 texturas do `fonte_max` no tamanho máximo, com o `Crash_V2`): 1 006 entradas, 2,39 GB regravados → arquivo virtual de 2,98 GB (184 entradas no lugar, 260 em espaço liberado = 0,59 GB reaproveitados, 562 no fim, 463 depois de 2 GB). O `primeira_fase` chegou à vila (170 s, 5 100 quadros, sem falha), com o log `XenonDrive::Read at 0xBE239800 (past 2 GB)`.

**Texturas HD no host.** O jogo reserva no boot `MmAllocatePhysicalMemoryEx` 0x8700000 + 0x17000000 (503 de 512 MB), e as texturas regravadas nos pacotes entram nesses blocos. O pacote HD não toca no guest: o jogo carrega e amostra a textura dele, e o renderer liga outra SRV no lugar. A chave (`hd::Key`, estável: os pacotes são nomeados por ela) é um hash do nível 0 **decodificado** (texels do host, sem o padding do pitch nem o tiling) com largura, altura e formato (sem distinção de gamma). É calculada no upload, então render targets e resolves nunca entram (vêm de `g_resolveTargets` antes). O nível 0 é decodificado primeiro em memória comum (`g_hdKeyScratch`): ler de volta do upload heap (write-combined) custava 1,3 s num quadro do carregamento da fase. Na troca, `texSizeScale` do slot vira guest/HD, para que coordenadas não normalizadas, offsets de texel e `getWeights` continuem em texels do guest. Por isso, com um pacote ativo, os shaders usam a variante "escalada" (`SetRenderScaled`), que já está no cache para quem joga acima de 720p. O sampler desloca MinLOD/MaxLOD em log2(HD/guest); num sampler só de nível base (MaxLOD 0), o MaxLOD vira esse deslocamento, com filtro linear entre mips, para minificar como o guest. Os arquivos do pacote são lidos por uma thread no início (até 1 GB) e liberados depois do upload.

Fatos do dump: quase todas as texturas do jogo são `k_8_8_8_8` (formato 6) com swizzle 0x60A (BGRA na memória), **de cabeça para baixo** (primeira linha = última do PNG exportado; vêm de texturas `.bmp`); só a `WaterDistortMap` apareceu como DXT1. Por isso o `tools/hd_textures.py` calcula a chave direto da exportação (`runtime_key`, a mesma conta do C++), e confere essa previsão com todo dump: 82 de 82 iguais. As faixas 256x1 que mudam a cada quadro (centenas) não são imagens exportadas. Uma entrada de textura reenviada há menos de 60 quadros é dinâmica e não vai para o dump.

**Conferido** (`primeira_fase`, `mods_dir=private\remaster_l1\mods_hd`: as 170 texturas do `fonte_max`, 87 MB em BC7): 48 trocas na abertura e no começo da vila; as mesmas `MmAllocatePhysicalMemoryEx` do jogo sem mod; 31,0 fps e no máximo 150 ms por quadro, contra 30,7 fps e 119 ms com o mesmo pacote e `hd_textures = false` (498 texturas, 0 HD). O BC7 (modo 6, codificador numpy do próprio script) volta com erro médio de 0,2 a 0,8 por canal e 43 a 45 dB.

## HD AI Remaster: o mod da fase 1 como opção (`settings/hd_remaster.cpp`, `fs/vfs.*`, `settings/settings_menu.cpp`)

Pedido: o mod de remaster por IA da fase 1 (`private\remaster_l1`, o "MOD FINAL" `mods_final\remaster_l1_final`) como opção das configurações, **HD AI Remaster**, desligada por padrão. Uso: README, "HD AI Remaster".

**Depois: o jogo todo (2026-10-21).** `HdRemasterDir()` escolhe a primeira pasta com dados (`rcf\` ou `hd_textures\`): `private\remaster_full\mods\remaster_full` (todos os episódios, montado fora do jogo, ainda não testado nele), senão a da fase 1. `HdRemasterWholeGame()` troca a ajuda da linha para "(jogo todo)". O pipeline, as conferências e os riscos (memória do jogo por fase, orientação dos pedaços das sprites, texturas DXT) estão em `private\remaster_full\README.md`.

**Carregamento.** O mod fica onde foi montado (é derivado do jogo; não vai para `mods\` nem para o repositório). `settings::EnableHdRemaster()` roda em `main.cpp` logo depois do `settings::Load` (antes de `menu_art`, `hd_textures::Init` e da primeira leitura do `default.rcf`). Com `graphics.hd_remaster = true` e os dados presentes (`rcf\` ou `hd_textures\` na pasta), ela chama o novo `vfs::AddMod`, que põe a pasta no fim da lista de mods (depois dos de `mods\`, que ganham dela). Daí em diante é um mod como outro qualquer: o `rcf_overlay` monta o `default.rcf` virtual com as 700 entradas dele, e o `gpu::hd` indexa as 133 texturas HD. Nada mais muda de caminho, e a opção só vale no próximo boot (a lista de mods é fixa depois do início). Pasta: `CRASH_PRIVATE_DIR/remaster_l1/mods_final/remaster_l1_final`, ou `CRASH_HD_REMASTER_DIR` (testes). Desligada, a função só confere a lista: um mod da pasta `mods\` (ou do `--mods`) com `remaster` no nome, ou que seja a mesma pasta, é carregado mesmo assim, e o log avisa (`[remaster] WARNING ...`). Ligada e já na lista (`--set mods_dir=private/remaster_l1/mods_final`), não entra de novo. Sem dados: `[remaster] ... has no remaster data: playing without it`.

**Tela.** Linha `HD AI REMASTER` no fim da aba GRÁFICOS (mesmo nome em EN e PT), com o ícone de "vale ao reiniciar" e o `NeedsRestart` (desde "Aplicar as opções sem fechar o jogo", abaixo, ela fica na aba JOGO e mudá-la reinicia o jogo). Sem os dados, o valor é INDISPONÍVEL/UNAVAILABLE na cor dos itens informativos, sem setas, e o A/setas não mudam nada (o valor salvo é mantido para quando os dados voltarem); a ajuda diz que os arquivos não estão no PC. Com 9 linhas a aba passava do espaço entre as abas e a caixa de ajuda (8 × 44 px virtuais), então o passo das linhas agora é `min(44, 360 / linhas)` (40 px na aba GRÁFICOS; as outras abas não mudam). Achado de passagem: em PT-BR o aviso "REINICIE O JOGO PARA APLICAR" encostava no SELECIONAR (A) do jogo (que começa em x ~786 virtual, contra ~870 do SELECT em inglês). Agora, em português, ele fica centrado em 598, com no máximo 350 px. O `--settings-preview` grava também a aba GRÁFICOS com a linha nova selecionada (`*_3_graphics_hd_remaster.png`).

**Níveis.** Não há. As pastas `mods_{minimo,personagens,completo}[_modelos]` são as versões anteriores, só por pacotes, e o `mods_final` cobre as três. Não existe um `mods_final` sem modelos: o `models.py build --chain` gravou os modelos nos mesmos pacotes das texturas de 256 cores. Separar exigiria outro build (`textures.py build` só com `textures\`).

**Conferido** (2026-10-21, `docs\capturas\hd_remaster\`): `configuracoes` com o padrão passa (LOAD GAME incluído) sem nenhuma linha `[remaster]` e sem mods (`[fs] mods folder: ...\mods, 0 mod(s)`, `.rcf` virtual de 1,18 GB = só o patch interno). Ligando na tela (perfil `configuracoes` com outro `input`: RB ×2, UP, A, B), em EN e PT-BR: `graphics.hd_remaster -> ON` / `LIGADO` e `hd_remaster = true` no `settings.toml` da execução. `primeira_fase` (input e quadros iguais aos da execução de referência `20261021-112336_primeira_fase`, 170 s): desligado = 30,6 fps, nenhuma linha do remaster; com `CRASH_SETTINGS="graphics.hd_remaster=true"` = 30,2 fps, 133 texturas no pacote, 67 trocas, 700 entradas e arquivo virtual de 2,26 GB, sem falha, com as mesmas imagens da referência. `boot` com a opção ligada e `CRASH_HD_REMASTER_DIR` inexistente: roda sem o mod.

## Janela: proporção mantida e DPI real (`gpu/d3d12_renderer.cpp`, `gpu/window.cpp`)

**Proporção.** Antes, o present desenhava a imagem na janela inteira, então uma janela que não fosse 16:9 esticava o jogo. Agora o retângulo da imagem tem a proporção do front buffer (o 1280x720 do jogo, ou o tamanho do resolve), cabe na janela e fica centralizado. O resto do back buffer é limpo de preto: barras nas laterais ou em cima e embaixo. O `PRESENT_PS` ganhou a constante `origin` (7ª e 8ª das 8 que a root signature de blit já tinha) e calcula `uv = (posição - origin) / tamanho da imagem`. A cópia 1:1 para o anti-aliasing usa origin 0. Legendas, tela de configurações e overlay continuam desenhados na janela inteira.

**DPI.** O processo não declarava DPI awareness. Numa tela 4K com escala de 200%, a janela era de 1920x1080 "lógicos": o swap chain tinha metade da resolução e o Windows esticava 2x (imagem borrada, inclusive com `resolution = 2560x1440`). Agora `window::Create` chama `SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)` e escala o tamanho inicial pelo DPI do sistema (1280x720 a 200% vira 2560x1440 de verdade). `WM_DPICHANGED` aplica o tamanho sugerido ao trocar de monitor. O log mostra `[gpu] window: 192 dpi (200% scaling), client 2560x1440`.

**Conferido** (2026-10-21, tela 3840x2160 a 200%): `menu` com a janela redimensionada no meio da execução para 3174x2119 (mais alta que 16:9) deu imagem 3174x1785 centralizada, com barras de 167 px em cima e embaixo, sem esticar. `primeira_fase` passou (31,0 fps, sem falha). Antes, as capturas com `resolution = 2560x1440` saíam em 1920x1009 (a janela não-DPI); agora saem no tamanho real da janela.

## Aplicar as opções sem fechar o jogo (`settings/restart.cpp`, `settings/settings_menu.cpp`, `gpu/d3d12_renderer.cpp` `Reconfigure`, `tools/run_game.py`)

Pedido: as opções que valiam "ao reiniciar" não devem mais exigir fechar o jogo à mão (alternativa 1: aplicar/recarregar; alternativa 2: o jogo reinicia sozinho), e a linha HD AI REMASTER passa para a aba JOGO. Uso: README, "Aplicar sem fechar o jogo". Capturas em `docs\capturas\settings_aplicar\`.

**O que dá para aplicar ao vivo e o que não dá.**
- *Resolução interna, upscaler e modo*: só o renderer depende deles, e o renderer já guarda os tamanhos lógicos em unidades guest (passo 19, "Resolução interna"). Ao fechar a tela com um deles mudado (`settings::RendererDiffers`), `gpu::renderer::RequestReconfigure()`; no fim do `Swap` seguinte (depois do `Present`, antes do jitter do próximo quadro), `Reconfigure()`: `WaitIdle`, novas `g_renderScale`/`g_sceneScale`, `upscaler::Select` (troca DLSS/FSR/nenhum e libera a feature/contexto do que saiu), `CreateTemporalPipeline` se o jogo abriu sem upscaler, histórico temporal zerado (`reset` no próximo quadro), `g_tileStash` limpo, cada `RenderTarget` recriado na escala nova (`CreateRenderTargetResource`, extraído de `GetRenderTarget`) e cada superfície de resolve que existia recriada (a 0 na escala de saída, a 1 na da cena quando diferem; o "packed" de profundidade também). Os descritores (RTV/DSV/SRV) são reaproveitados: os alocadores são lineares, e recriar com descritores novos vazaria a cada troca. Conteúdo: limpo (a tela só abre no frontend, que redesenha e resolve tudo a cada quadro; nada apareceu preto nas capturas). Se a variante dos shaders muda (`SetRenderScaled`: 720p sem upscaler nem HD usa o HLSL original), todas as `ShaderEntry` voltam a "não traduzida" e `g_pipelines` é limpo (a chave do pipeline usa o hash do microcódigo, não a variante); o cache em disco tem as duas variantes de quem já jogou nas duas, então não compila nada (`0 compiled` nos testes). `settings::RendererApplied` atualiza os valores "em vigor" (`AtStartup()` agora quer dizer isso), e o overlay, o `frame.quality` do upscaler e o `NeedsRestart` seguem. Medido: ~20 ms por troca. Conferido: 720p → 1080p sem upscaler, 720p → 1080p + DLSS Qualidade (cena 1280x720) e 1440p + DLSS → 720p sem upscaler, cada um seguido de LOAD GAME até a fase (perfil `configuracoes_resolucao`, `ao_vivo_*.png`). Achado de passagem (antigo, não mudou): a primeira vez que a tela mostra DLSS, a sondagem do NGX trava a tela ~1,2 s na thread do renderer; um roteiro de botões precisa de folga ali.
- *Volumes de música/efeitos/vozes*: na primeira versão ficaram no reinício (o patch do `mixers.p3d` só vale no boot). Depois passaram a valer na hora pelos volumes de categoria do próprio jogo: veja "Volumes ao vivo, opções do jogo e a tela na pausa" abaixo.
- *Idioma*: `sub_821D8170(this, letra)` troca o idioma ao vivo (é o que a tela nórdica `BootUp_LanguageSelect` usa), mas a bíblia de texto do frontend já foi lida dos pacotes do boot, o `.rcf` de voz já está montado, e o pacote PT-BR (reescrita das bíblias pelo `rcf_overlay`, decidida pelo idioma de boot) e as legendas também são decididos no boot. → reinício.
- *Cores do mojo e HD AI Remaster*: patches/mods de `.rcf` aplicados na montagem do `default.rcf` virtual e pacotes carregados no boot (`PermanentContent`, lista de mods fixa). → reinício.
Não existe um "recarregar" do jogo que releia isso tudo: o fluxo de boot (`BootUp` → `LoadPersistentPostBootUp` → título) carrega os pacotes persistentes uma vez; voltar a ele com os arquivos trocados por baixo seria arriscado (ponteiros para dados já carregados). Por isso o reinício é do processo.

**Reinício** (`settings::Restart`, `restart.cpp`). Ao sair da tela (B) com uma opção de boot mudada (`settings::NeedsRestart`: idioma, mojo, HD AI Remaster com dados; os volumes de categoria também, até passarem a valer na hora), a tela troca as linhas por uma caixa do jogo (`MSG_Background`): "Algumas opções só valem depois de reiniciar: <itens> / Reiniciar o jogo agora? / (A) REINICIAR (B) DEPOIS" (ícones de botão das fontes; aberta pela pausa, `RequestOpen(false)`, uma linha avisa que o progresso não salvo será perdido). B fecha como antes (as opções ficam para a próxima abertura; o renderer ainda aplica a resolução). A grava o `settings.toml`, desenha "Reiniciando..." por 250 ms e chama `Restart()`: `CreateProcessW` com o exe (`GetModuleFileNameW`), os argumentos do processo (sem um `--restarted` anterior, requotados) + `--restarted <pid>`, handles herdados (o log continua no mesmo console/arquivo), a mesma pasta e o ambiente sem `CRASH_SETTINGS` (já gravado no arquivo), `CRASH_INPUT_SCRIPT`, `CRASH_REPLAY` e `CRASH_RECORD_INPUT` (recomeçariam do zero); `CRASH_RESTART_WINDOW=x,y,l,a` leva a posição/tamanho da janela com moldura (`window::Placement`; o `window::Create` do processo novo usa se ainda estiver numa tela). Depois `ExitProcess(0)`. O processo novo, logo no começo do `main`, espera o antigo terminar (`HandleRestartArguments`, até 10 s: janela, áudio, saves).

**Pular as intros.** Com `--restarted`, `settings::IntroSkipButtons()` (somado em `menu::GameButtons()`, o mesmo caminho do B ao fechar a tela) aperta START por 200 ms a cada 500 ms a partir de 1 s do relógio do controle, até 25 s ou até o jogador (ou um roteiro) apertar algo (`input::MenuButtons()`); o log diz `intro skip handed over to the player` / `ended`. START pula os logos e a tela de título, e no menu principal não faz nada (testado: START aos 20–24 s com o menu aberto deixa NOVO JOGO selecionado, `20261021-132205_menu`). O menu aparece em ~15 s. Tentativa descartada: redirecionar a saída `SkipToTitleScreen` do `BootUp` no `Frontend.bfig` (a condição dela é Lua compilado em `Frontendlua.blua`, e o fluxo carrega os pacotes persistentes no caminho).

**`CRASH_RESTART`**: `relaunch` (padrão), `exit` (sai com `RESTART_EXIT_CODE` = 4 em vez de abrir outro processo) e `off` (sem pergunta; a tela volta a "VALE AO REINICIAR" / "REINICIE O JOGO PARA APLICAR"). O `run_game.py` usa `exit` (com `setdefault`, um perfil pode trocar) e reabre o jogo ele mesmo quando a saída é 4: mesmos argumentos + `--restarted <pid>`, ambiente sem os overrides da primeira abertura e `CRASH_SCREENSHOT_DIR`/`CRASH_FRAME_STATS` em `reinicio_N\`; `restart_input`, `restart_env`, `restart_timeout`, `max_restarts` e `expect.restarts` no perfil. Assim o script continua acompanhando o processo certo (se o jogo se reabrisse sozinho, o script veria a saída 0 do primeiro e perderia o segundo).

**Tela.** O ícone de setas em círculo marca só as opções de reinício (música, efeitos, vozes, idioma, cores do mojo, HD AI Remaster); a ajuda delas diz "O jogo reinicia para aplicar (pergunta antes)"; o rodapé, "REINICIA O JOGO" e, com alguma mudada, "O JOGO VAI REINICIAR" (na cor de aviso). Resolução, DLSS/FSR e modo perderam o ícone. HD AI REMASTER foi para a aba JOGO (IDIOMA, CORES DO MOJO, HD AI REMASTER, ARMAZENAMENTO); no `settings.toml` continua em `[graphics]`. GRÁFICOS volta a 8 linhas. O `--settings-preview` grava também `*_4_game_hd_remaster.png` e a caixa de reinício (`*_4_game_reiniciar.png`).

**Conferido** (2026-10-21, `docs\capturas\settings_aplicar\`): `configuracoes_reinicio` (idioma PT-BR → caixa → A → saída 4 → o script reabre → intros puladas → menu em PT-BR aos 16 s → LOAD GAME no episódio 1 em PT-BR); a mesma sequência com `CRASH_RESTART=relaunch` (o próprio jogo abriu o processo novo, que esperou o antigo sair, voltou ao menu em PT-BR e parou de apertar START aos 25 s); com B na caixa (fecha, `IDIOMA apply on the next launch`, menu ainda em inglês); `configuracoes_resolucao` nos dois sentidos; `configuracoes` (padrão) e `primeira_fase` (NEW GAME até a vila) passando.

## Frontend de PC: SAIR DO JOGO, sem Xbox 360 e sem o Y (`settings/pc_frontend.*`, `settings/quit_game.*`, `settings/frontend_hooks.cpp`)

Pedido: uma opção 5 no menu para fechar o jogo, nenhuma menção ao Xbox 360 nos textos ("Não desligue o PC.") e nenhum botão Y de armazenamento nas outras telas. Uso: README, "Menu de PC". Código novo: `settings/pc_frontend.{h,cpp}` (patches de dados, registrados no fim de `RegisterFrontendPatches`), `settings/quit_game.{h,cpp}` (o fechamento); pontos de contato: `settings/frontend_hooks.cpp` (dois ganchos nativos e o marcador 4343), `gpu/d3d12_renderer.cpp` (fim do `Swap`), `audio/audio_output.*` (`audio::Shutdown`), `languages/pt-br/texts` e `GLOSSARIO.md`, `config/run.toml` (perfil `sair_do_jogo`).

**Que menu.** O menu principal do Xbox tinha 4 itens (NEW GAME, LOAD GAME, CREDITS, SETTINGS): o "5" é o SAIR DO JOGO no fim dele. O menu de pausa (VOLTAR AO JOGO, OPÇÕES, SALVAR E SAIR) não tem submenu de sair: o "SALVAR E SAIR" (`ExitQuitGame` → nó 0x439 → `ExitSaveAndExit`) salva e volta ao menu do episódio, cujo (B) EXIT GAME (`ConfirmQuitGame`) volta ao menu principal. Ficou como está.

**As condições do frontend são código nativo.** `Frontendlua.blua` tem as condições do `Frontend.bfig` em Lua (Lua 5.1 deste jogo: cabeçalho de 14 bytes; por função fonte, u32 linha, 4 bytes, line info em u16, locais, upvalues, constantes com números float, funções, e o código por último; instrução = op[0..5] C[6..14] B[15..23] A[24..31], Bx[6..23], constantes RK a partir de 250; JMP 20, EQ 21, CALL 25, RETURN 27). Uma função por nó devolve o **número da saída** a tomar (as saídas são numeradas na ordem do arquivo: ExitAccept do menu = 0x4B, ExitPS2 0x4C com 4 filhas, ExitWii 0x51 com 5, ExitXenon 0x57 com 4 (88–91), ExitBack 92, ExitXenonProfileChanged 93, ExitShowXenonDeviceSelect 94). Mas com `CRASH_LOG_ALL_IO` o jogo **nunca lê** o `.blua` (650 leituras do `default.rcf` até o menu, nenhuma no intervalo dele): as mesmas condições estão compiladas no executável, em `sub_820F9BA8(tree, node, ...)` (um `switch` por nó, `li r3,<saída>`), com `GetMenuIndex` = `sub_820F9A10(tree, página, menu)` (devolve o índice ou -1) e `IsButtonPressed` = `sub_821DBB30(sub_821FA440(sub_821FB4F0(), 6), evento, ...)` (ACCEPT 1, BACK 10, CHEER/Y 13). Uma primeira versão que alterava o `.blua` não teve efeito nenhum (o Y continuava voltando ao NEW GAME, como o usuário viu).

**SAIR DO JOGO.**
1. `package\cdd70a8c.p3d` (reescrita, cresce 194 bytes): na página `GameStart_Start_Xenon.pag`, menu `StartMenu`, um quinto filho `QuitGameItem` (0x18011 > 0x18013 > texto 0x18023 > referência 0x1800B), cópia do item do SETTINGS (`CalibrationItem`: mesma fonte, cores, caixa de 437 de largura) 46 unidades abaixo, com o texto `persistent` / `FE_Button_Exit_Game_Text` (a chave 0x57B5 da bíblia `persistent`, que já é EXIT GAME / QUITTER JEU / SPIEL VERLASSEN / SALIR DEL JUEGO / ESCI DAL GIOCO / SPEL AFSLUITEN / SAIR DO JOGO no pacote PT-BR; uma página pode citar outra bíblia, como a `GameStart_HUB.pag` já faz). Os quatro itens sobem 23 unidades (página 640x480, y para cima). A navegação do menu é genérica: o cursor anda pelos 5 e dá a volta.
2. `fighttrees\Frontend.bfig` (mesmo tamanho): a saída do menu do Wii `ExitLanguage` (a quinta filha de ExitWii, número 0x51 + 5 = 86; nunca tomada no Xbox; achada pelo nome e por ter `ExitCredits` e `ExitCalibration` como irmãs) passa a ir para o menu principal (0x46, o destino do Y) e o trecho `PlaySoundSelect()\r\n\r\n` vira `PlaySoundSelect(4343)`.
3. Gancho `sub_820F9BA8`: com A apertado o caso do menu principal chama `GetMenuIndex(Xenon, StartMenu)` para 0, 1, 2 e 3; com o item 4 nada casa e ele devolve -1. O gancho de `sub_820F9A10` marca (thread_local, só dentro de uma chamada de `sub_820F9BA8`) quando a página é `GameStart_Start_Xenon`, o menu `StartMenu` e o índice 4; se a chamada externa termina em -1, devolve 86 (`settings::QuitExit()`, só se os patches 1 e 2 aplicaram e ExitWii for 0x51).
4. A saída 86 roda `PlaySoundSelect(4343)`; o gancho do thunk Lua (`sub_822E29D8`) chama o original (o som) e `settings::RequestQuit()`: grava o `settings.toml`, para uma gravação F5 e marca o pedido. No fim do `Swap` seguinte o thread do CP (dono do device) espera a GPU (`WaitIdle`), sai da tela cheia exclusiva se estiver nela, chama `upscaler::Shutdown()` e `FinishQuit()`: `audio::Shutdown()` (fecha o `audio.wav` com o cabeçalho final, `Stop`/`FlushSourceBuffers` da voz e `StopEngine`) e `ExitProcess(0)`. Sem renderer, ou sem um present em 3 s, `FinishQuit` roda de outro thread. Log: `[pc] frontend: EXIT GAME chosen on the main menu (exit 86)`, `[quit] EXIT GAME chosen ...`, `[settings] saved ...`, `[gpu] exit: GPU idle, windowed, upscalers released`, `[quit] exiting with code 0`. O `run_game.py` já trata 0 como `ok`.

**Y de armazenamento.** As saídas que abrem o seletor (`ExitShowXenonDeviceSelect`, trecho `SaveGameManager:ShowDeviceSelector()`) são 94 (menu principal), 265 (tela de slots do frontend: `andi. r11,r11,266; addi r3,r11,-1`) e 523 (tela de slots no jogo); só o Y as devolve. O gancho de `sub_820F9BA8` troca as três por -1 (`[pc] frontend: Y (storage device) ignored (exit 265)`). As dicas: a do menu já era escondida (`frontend_patches.cpp`); as das telas de slots são o registro de visibilidade `[0x7B6567EE][GameStart_GameSlot|InGame_GameSlot][GameStart_DeviceSelector|InGame_DeviceSelector][u32 1]` dos blocos `ContentDeviceSelector`, que passa a 0. O Y do menu do episódio (CONTINUAR HISTÓRIA, saída 604) e o da tela do nome (pronto) são outras saídas e não mudam. O LOAD GAME e o NEW GAME continuam chamando o seletor uma vez (lr `0x821CF488`, o mesmo do Y) pelos trechos `ExitNoDeviceSelected`/`BeginAccess`, que não foram tocados.

**Textos.** Patch embutido de mesmo tamanho (`textos de PC (sem Xbox 360)`) nas bíblias `frontend`, `ingame` e `persistent` dos pacotes que o Xbox lê (`7efdcd91`, `cdd70a8c`, `b4c85fe7`, `c1e387c7`, `97597d1b` e os `ingame`/`shared` por idioma: `7a8185b0` E, `7a88cba0` U, `7a81fa0f` F, `7a826e6e` G, `7a87e2e2` S, `7a83572c` I, `7a811151` D). Cada troca só acontece se o texto original ainda contém a palavra esperada (`Xbox`, `console`, `storage device`, `stockage`, `Speichergerät`, `unidad de memoria`, `periferica`, `opslagapparaat`) e cabe no lugar dele (o resto vira NUL; nenhuma outra chave pode apontar para dentro da string). As 84 trocas (6 idiomas + U, nos pacotes acima) foram conferidas contra as bíblias exportadas antes de ir para o código. O PT-BR troca as mesmas chaves no próprio pacote (a reescrita do idioma vem depois e ganha). Ficaram de fora: os créditos (0x397E, 0xBA15, 0xE12E: cargos "CONSOLE ..."), o rótulo `STORAGE DEVICE` (0xC12C, só usado pela dica escondida), as mensagens de perfil (0x7364, 0xA3D8: genéricas, nunca aparecem no PC), os textos de PS2/Wii e das cópias que o Xbox não lê (`aabeaa37`, `ad83ca09`, `b9c6f232`, `1725c617`, `79290aaa`) e os idiomas que o runtime não oferece (A, H, J, N, R, W). Textos novos:

- `frontend` 0x1737 (acessando os saves (NEW GAME / LOAD GAME)):
  - EN: Accessing Crash of the Titans save game files. Please do not turn off your PC.
  - FR: Accès aux données Crash of the Titans. Ne pas éteindre le PC.
  - DE: Zugriff auf CRASH OF THE TITANS-Speicherdaten. Bitte den PC nicht ausschalten.
  - ES: Accediendo a los datos guardados de Crash of the Titans. No apagues el PC.
  - IT: Accesso ai file di gioco salvati di Crash of the Titans in corso. Non spegnere il PC.
  - NL: Opgeslagen spelgegevens van Crash of the Titans worden geopend. Zet de pc niet uit.
  - PT-BR: Acessando os arquivos salvos de Crash of the Titans. Não desligue o PC.
- `frontend` 0x3B85 (salvando):
  - EN: Saving Crash of the Titans game, please do not turn off your PC.
  - FR: Sauvegarde des données Crash of the Titans, ne pas éteindre le PC.
  - DE: CRASH OF THE TITANS-Spiel wird gespeichert, bitte den PC nicht ausschalten.
  - ES: Guardando partida de Crash of the Titans. No apagues el PC.
  - IT: Salvataggio della partita di Crash of the Titans in corso. Non spegnere il PC.
  - NL: Bezig met opslaan spel Crash of the Titans. Schakel de pc niet uit.
  - PT-BR: Salvando o jogo de Crash of the Titans. Não desligue o PC.
- `frontend` 0xAE13 (carregando o save):
  - EN: Loading Crash of the Titans save game, please do not turn off your PC.
  - FR: Chargement des données Crash of the Titans, ne pas éteindre le PC.
  - DE: CRASH OF THE TITANS-Spielstand wird geladen, bitte den PC nicht ausschalten.
  - ES: Cargando partida guardada de Crash of the Titans. No apagues el PC.
  - IT: Caricamento della partita salvata di Crash of the Titans in corso. Non spegnere il PC.
  - NL: Bezig met laden opgeslagen spel Crash of the Titans. Schakel de pc niet uit.
  - PT-BR: Carregando o jogo salvo de Crash of the Titans. Não desligue o PC.
- `frontend` 0x4820 (nenhum dispositivo escolhido):
  - EN: No save folder is available. Without it you will be unable to save your game progress.
  - FR: Aucun dossier de sauvegarde n'est disponible. Sans lui, vous ne pouvez pas sauvegarder vos progrès dans le jeu.
  - DE: Kein Speicherort verfügbar. Ohne ihn kannst du deinen Spielfortschritt nicht speichern.
  - ES: No hay ninguna carpeta de guardado disponible. Sin ella, no podrás guardar tu progreso.
  - IT: Nessuna cartella di salvataggio disponibile. Senza di essa non potrai salvare i progressi di gioco.
  - NL: Er is geen opslagmap beschikbaar. Zonder opslagmap kun je je spelvoortgang niet opslaan.
  - PT-BR: Nenhuma pasta de jogos salvos está disponível. Sem ela, não será possível salvar o seu progresso.
- `frontend` 0x4AAE (dispositivo desconectado):
  - EN: The save folder is not available.
  - FR: Le dossier de sauvegarde est indisponible.
  - DE: Speicherort nicht verfügbar.
  - ES: No se encuentra la carpeta de guardado.
  - IT: La cartella di salvataggio non è disponibile.
  - NL: Opslagmap niet beschikbaar.
  - PT-BR: A pasta de jogos salvos não está disponível.
- `frontend` 0xB5DF (dispositivo desconectado, tentar de novo):
  - EN: The save folder is not available. Please Retry or Continue Without Saving.
  - FR: Le dossier de sauvegarde est indisponible. Réessayer ou Continuer sans sauvegarder.
  - DE: Speicherort nicht verfügbar. Bitte erneut versuchen oder ohne zu speichern fortfahren.
  - ES: No se encuentra la carpeta de guardado. Vuelve a intentarlo o continúa sin guardar.
  - IT: La cartella di salvataggio non è disponibile. Riprova o continua senza salvare.
  - NL: Opslagmap niet beschikbaar. Probeer het opnieuw of ga door zonder op te slaan.
  - PT-BR: A pasta de jogos salvos não está disponível. Escolha Tentar de Novo ou Continuar Sem Salvar.
- `frontend` 0xB841 (sem perfil (botão Guia Xbox)):
  - EN: You do not have an active player profile. You will be unable to save your game progress without an active profile.
  - FR: Vous n'avez pas de profil de joueur actif. Vous ne pourrez pas sauvegarder vos progrès sans profil de joueur actif.
  - DE: Du hast kein aktives Spielerprofil. Ohne aktives Spielerprofil kannst du deinen Spielfortschritt nicht speichern.
  - ES: No tienes un perfil de jugador activo. No podrás guardar la partida hasta que tengas un perfil de jugador activo.
  - IT: Non hai un profilo giocatore attivo. Senza profilo giocatore attivo, non puoi salvare i tuoi progressi di gioco.
  - NL: U hebt geen actief profiel. U kunt het spel niet opslaan zonder actief profiel.
  - PT-BR: Você não tem um perfil de jogador ativo. Sem um perfil de jogador ativo, não será possível salvar o seu progresso.
- `ingame` 0x1737 (acessando os saves (no jogo)):
  - EN: Accessing Crash of the Titans save game files. Please do not turn off your PC.
  - FR: Accès aux données Crash of the Titans. Ne pas éteindre le PC.
  - DE: Zugriff auf CRASH OF THE TITANS-Speicherdaten. Bitte den PC nicht ausschalten.
  - ES: Accediendo a los datos guardados de Crash of the Titans. No apagues el PC.
  - IT: Accesso ai file di gioco salvati di Crash of the Titans in corso. Non spegnere il PC.
  - NL: Opgeslagen spelgegevens van Crash of the Titans worden geopend. Zet de pc niet uit.
  - PT-BR: Acessando os arquivos salvos de Crash of the Titans. Não desligue o PC.
- `ingame` 0x3266 (salvando (no jogo)):
  - EN: Saving Crash of the Titans game, please do not turn off your PC.
  - FR: Enregistrement en cours. N'éteignez pas votre PC.
  - DE: CRASH OF THE TITANS-Spiel wird gespeichert, bitte den PC nicht ausschalten.
  - ES: Guardando partida de Crash of the Titans. No apagues el PC.
  - IT: Salvataggio della partita di Crash of the Titans in corso. Non spegnere il PC.
  - NL: De content wordt opgeslagen. Zet de pc niet uit.
  - PT-BR: Salvando o jogo de Crash of the Titans. Não desligue o PC.
- `ingame` 0x4820 (dispositivo indisponível (no jogo)):
  - EN: The save folder is not available. Without it you will be unable to save your game progress.
  - FR: Le dossier de sauvegarde est indisponible. Sans lui, vous ne pouvez pas sauvegarder vos progrès dans le jeu.
  - DE: Speicherort nicht verfügbar. Ohne ihn kannst du deinen Spielfortschritt nicht speichern.
  - ES: No se encuentra la carpeta de guardado. Sin ella, no podrás guardar tu progreso.
  - IT: La cartella di salvataggio non è disponibile. Senza di essa non potrai salvare i progressi di gioco.
  - NL: Opslagmap niet beschikbaar. Zonder opslagmap kun je je spelvoortgang niet opslaan.
  - PT-BR: A pasta de jogos salvos não está disponível. Sem ela, não será possível salvar o seu progresso.
- `ingame` 0x6C7E (dispositivo indisponível, continuar sem salvar):
  - EN: Save folder not available.  Would you like to continue without saving?
  - FR: Dossier de sauvegarde indisponible. Continuer sans sauvegarder ?
  - DE: Speicherort nicht verfügbar. Möchtest du ohne zu speichern fortfahren?
  - ES: No se encuentra la carpeta de guardado. ¿Seguro que quieres continuar sin guardar?
  - IT: Cartella di salvataggio non disponibile. Continuare senza salvare?
  - NL: Opslagmap niet beschikbaar. Wil je doorgaan zonder op te slaan?
  - PT-BR: Pasta de jogos salvos indisponível.  Deseja continuar sem salvar?
- `persistent` 0x5823 (reconecte o controle do Xbox 360):
  - EN: Please reconnect the controller to continue.
  - FR: Reconnectez la manette pour continuer.
  - DE: Bitte den Controller wieder anschließen.
  - ES: Vuelve a conectar el mando para continuar.
  - IT: Riconnetti il controller per continuare.
  - NL: Verbind de controller opnieuw om verder te gaan.
  - PT-BR: Reconecte o controle para continuar.

**Teste** (exe linkado à parte, `build\CrashRecomp_menus_pc.exe`, porque outra sessão estava com o `build\CrashRecomp.exe` aberto; depois o principal também linkou): `sair_do_jogo` em inglês e PT-BR (saída `ok`, Y sem chamar o seletor, as linhas acima no log); `primeira_fase` (NEW GAME, Y na tela de slots, sobrescreve o slot 1: "Saving ... do not turn off your PC.", `XamContentCreateEx ... GameSlot 1 ... flags 0x12`) e `menu_loaded` (LOAD GAME com Y apertado três vezes na tela de slots: `ignored (exit 265)`, só a chamada normal ao seletor, carrega e chega ao menu do episódio), os dois em EN e PT-BR; `configuracoes` (o SETTINGS, agora o 4º de 5 itens, abre e fecha como antes). Capturas em `docs\capturas\menus_pc\`. Atenção nos roteiros: setas com 0,6 s entre si no menu contaram um toque a mais (o cursor deu a volta); com 1 s funcionam.

## Logo Definitive Edition (`settings/title_logo.*`)

Pedido: "Definitive Edition" escrito embaixo do logo, com cara de parte do jogo; depois, só na tela do PRESS START ("é só no press start"). Reescritas registradas por `RegisterFrontendPatches` depois das do frontend de PC e antes das do pacote de idioma (que mantém a chave nova: ele só troca textos das chaves que já existem). `CRASH_TITLE_LOGO=off` desliga; `CRASH_TITLE_LOGO="text_y=95;colour=0xFFFFFFFF"` ajusta o layout (chaves em `g_layout`).

- **Chave de texto.** As chaves das bíblias são hashes do nome: `h = (h * 64 + c) mod 0x1003F` sobre os bytes (`sub_822F2170`; o módulo é o campo da língua); cada língua guarda os hashes em ordem crescente e a busca (`sub_822F2360`) é binária. A reescrita "texto Definitive Edition" de `package\cdd70a8c.p3d` e `package\7efdcd91.p3d` põe `FE_Definitive_Edition_Text` (0xF8EF) nas 12 línguas da bíblia `frontend`, no lugar certo da ordem, com a string no fim do buffer, e corrige os tamanhos da língua, da bíblia e dos pais.
- **Página.** `GameStart_Title.pag` (camada `Layer 1`: `PressStart` 120,50 400x41 Titans_Small e o sprite `TitleLogo` 77,81 480x345). O logo sobe 22 unidades e entra `DefinitiveEditionText`, cópia do `PressStart` em 120,92 400x41, fonte Titans_Large, cor 0xFFFFB428, apontando para a chave nova. O cabeçalho de texto/sprite depois do nome: u32 versão, x, y, largura, altura (página 640x480, y para cima, canto inferior esquerdo), **um byte**, u32 alinhamento horizontal (4 = centro) e vertical, cor B G R A (offset 29), dois u32, e no texto a fonte (offset 41; no sprite, contagem e nomes de imagem). A primeira versão escreveu cor e fonte em 28/40: o carregador de texto (`sub_82327D30`) procurou uma fonte inexistente e o jogo caiu em `sub_822F11A0` lendo 0 ao carregar o projeto do frontend (derrubou execuções de outras sessões por alguns minutos; ficou desligado até a correção).
- **Menu principal (tentado e retirado).** Uma cópia do sprite do logo + o texto acima dos cinco itens, que desciam 40 unidades. O sprite com o byte depois da caixa em 0 (como nos dados do jogo) é desenhado numa escala fixa a partir do canto superior esquerdo da caixa, ignorando largura e altura; com 1 ele encolhe para a caixa. O usuário preferiu o menu como era.

**Verificação** (exe à parte `build\CrashRecomp_logo.exe`): capturas em `docs\capturas\logo\` — título em EN e PT-BR com interna 1280x720 e com 3840x2160 + DLSS Qualidade, e o menu principal (sem mudanças) em EN e PT-BR. Perfil `menu` passou com o patch ligado; os outros perfis não foram rodados a pedido do usuário (a mudança só toca a página do título e acrescenta uma chave de texto).

## Câmera livre em terceira pessoa (`camera/free_camera.*`, experimental)

Pedido: um mod opcional de câmera de terceira pessoa "padrão" (órbita com o analógico direito / mouse, distância ajustável com colisão, andar relativo à câmera, recentrar), sem quebrar as cenas e câmeras roteirizadas. Opção `[camera] free_camera` (desligada por padrão), aba CÂMERA da tela SETTINGS; uso no README, "Câmera livre (experimental)".

### Como a câmera do jogo funciona (lido no código recompilado)

- **Gerenciadores**: `sub_821FB4F0()` devolve a tabela em `0x824A7A24`; o gerenciador `i` fica em `+28 + 4*i` (`sub_821FA440`). Medido em jogo: 1 `CActorManager`, 2 `CAIManager`, 4 `CCameraManager`, 5 `CCollisionManager`, 7 `CInputManager`, 12 `CRenderManager`, 15 `CSoundManager`, 18 `CCinematicsManager`, 20 `CWorldManager`.
- **A câmera de render** é um `pure3d::VectorCamera` em `CRenderManager+356`: `+16` FOV, `+28` aspecto, `+32/+36` near/far, `+48..+76` planos do frustum, `+80` matriz mundo→câmera e `+144` câmera→mundo (linhas: direita, cima, frente, posição), `+208` byte "matrizes válidas", `+216` posição, `+228` direção, `+240` up. Os setters (`sub_822F2038` posição, `sub_822F1FE8` direção, `sub_822F2010` up) só gravam o vetor e zeram `+208`; quem lê uma matriz (`Camera::vf13/vf14`) a reconstrói antes (`VectorCamera::vf26` = `sub_822F2060`: orientação a partir de direção/up, posição na linha 3, inversa).
- **`CCameraBehaviourController::vf08`** (`sub_820E0498`, fase `r6 == 3`) é o fim da câmera de gameplay: roda os comportamentos (`CCameraVolumeBehaviour`, `CScriptCameraBehaviour`, `CCameraTransitionBehaviour`; o `camera.lua` só cria o ator `camera1` com esse controlador), passa pela colisão de câmera do jogo (`sub_820E1B70` → `sub_821C3AC0`, que testa os 4 cantos do near plane contra **planos de colisão de câmera** postos pelo designer, não contra a geometria), soma o tremor (`+184`), interpola o FOV (`+204..+232`, `sub_82223388`) e copia a matriz `*(this+168)` para a VectorCamera (up = linha 1, direção = linha 2, posição = linha 3 + tremor). Também manda a matriz para o ouvinte do som (`sub_82240190`). **Durante as cinemáticas ele não é chamado**: `CCinematicCameraAction::vf04` (`sub_821B6EC8`) escreve a VectorCamera direto (medido no `primeira_fase`: nenhuma chamada da fase 3 entre o fim do menu e o fim da cena de abertura).
- **O jogador**: `CControllerGroundBehaviour::vf08` (`sub_820E96F0`), `this+28` = o ator, `ator+44` = matriz de mundo (posição em `+48`, frente em `+32`), `**(this+52)` = o mapa de input (`+36 != 0`: controlado por um controle). Canal `c` do mapa: o float em `hist[idx*4]`, com `{hist, ?, idx}` em `input + (c+4)*12`; 0/1 = analógico esquerdo, 2/3 = direito (cima = +1), 8 = A. Os canais dos botões mudam com o esquema de controle (o R3 apareceu como 12 numa execução e 15 em outra), então o R3 vem do controle 0 (`input::MenuButtons`).
- **Andar**: `sub_821EBE18` (vetor de movimento) → `sub_821EBC70(input, out, x, y)`: frente = linha 2 da câmera→mundo da VectorCamera, achatada; `out = direita*x + frente*y`. Antes, `sub_821EBA50` guarda a frente em `input+468` e, se a câmera virou mais de ~20° (cos 0,9397) com o analógico parado no mesmo lugar, trava a direção antiga (`+516` bit `0x80`) até o analógico mexer 0,1: é o "corte de câmera" das câmeras fixas.
- **Raio no mundo**: `sub_821C5EA0(colisão+4, raio, hit, 1)` com `raio = {de xyz, para xyz, -1, -1, 0}` devolve se bateu e a distância em `hit[0]` (é o teste da sombra-blob, `sub_8222E608`, raio de 50 para baixo). Percorre os objetos de colisão do mundo (`colisão+4..+8`) com teste de AABB antes.

### O mod

Ganchos (`PPC_FUNC` sobre as funções recompiladas, chamando a original):

| Função | O quê |
|---|---|
| `sub_820E0498` (`CCameraBehaviourController::vf08`) | depois da original, na fase 3: calcula a órbita e escreve posição, direção e up na VectorCamera (zera `+208`). O controlador e os comportamentos do jogo continuam rodando como antes, só a saída é trocada |
| `sub_820E96F0` (`CControllerGroundBehaviour::vf08`) | anota o ator seguido e o mapa de input dele (o primeiro com `+36 != 0`; troca só quando o mapa do atual parou de ser usado: co-op segue o jogador 1) |
| `sub_820E63A0` (`CScriptCameraBehaviour::vf08`), `sub_821B6EC8` (`CCinematicCameraAction::vf04`) | contam câmeras roteirizadas e cinemáticas no tique: com qualquer uma, a câmera do jogo volta |
| `sub_821EBA50` | com a câmera livre ativa, apaga a trava de direção (`+516` bit `0x80`) depois da original: a câmera gira um pouco a cada tique e o andar deve segui-la |
| `sub_821EBC70` | marca o jogador como controlado (o mapa de input dele foi convertido em movimento: andando, no ar ou montado num titã) e guarda a direção de mundo do último movimento do jogador (o alvo do R3); com `CRASH_CAMERA_PROBE`, registra esse vetor contra o yaw da câmera |

- **Andar relativo à câmera**: nada além da VectorCamera. O jogo já converte o analógico com a câmera de render, então a órbita vale para o andar sozinha. Medido (`CRASH_CAMERA_PROBE`): analógico para cima → yaw do movimento = yaw da câmera (−148,7° e −148,7°; −19,4° e −19,4°), direita = +90°, para trás = +180°.
- **Órbita**: alvo = posição do ator + 1,3 (suavizado, 14/s; pulo de mais de 8 = teleporte), distância base 6 × `distance`%, pitch de −20° a 70°, 160°/s de yaw e 100°/s de pitch no analógico todo × `sensitivity`%, zona morta 0,15. Mouse: `input::pc::TakeMouseLook(true)` enquanto ativa e `input::pc::MouseLookDelta` a cada tique (0,12°/contagem × sensibilidade da câmera × `controls.mouse_sensitivity`, `controls.invert_y` por cima do `invert_y` da câmera; `kernel/pc_input.h`, da sessão de controles de PC). O sentido do yaw é tirado da linha "direita" da matriz do jogo ao ligar. R3 recentra atrás da direção em que o jogador andou por último (o vetor que o próprio jogo fez do analógico esquerdo; as linhas da matriz do ator não seguem a frente dele de forma confiável: deram 120–160° de erro), com aproximação exponencial (10/s); qualquer movimento da câmera cancela.
- **Colisão**: 5 raios do alvo para trás (o centro e ±0,25 para os lados e para cima/baixo), margem 0,35, mínimo 1,4; aproxima na hora e volta a 4/s. Se uma parede deixa a câmera a menos de 60% da distância e olhar de cima (+40°) dá mais espaço, a câmera sobe proporcionalmente (suavizado, 6/s).
- **Entrar e sair**: ao ativar, yaw/pitch/distância saem da câmera do jogo naquele momento, e há uma mistura de 0,35 s (smoothstep) entre as duas nos dois sentidos. Volta para a câmera do jogo com: opção desligada, sem VectorCamera, ator inválido (vtable mudou), mapa de input do jogador sem uso por 300 tiques, câmera roteirizada, cinemática. Nas cinemáticas o controlador nem roda, então elas aparecem como foram feitas mesmo antes da contagem. Na pausa o controlador não roda e a imagem fica onde estava. Nova fase (outro controlador) = começa de novo. O log mostra cada troca: `[camera] free camera: active (...)`, `game camera (cinematic)`, `following actor ...`, `recenter ...`.
- **Desligada**: os ganchos só leem (o do chão anota o ator; os outros contam) e a VectorCamera não é tocada, nem a trava de direção.

### Verificação (`docs\capturas\camera\`, `LEIA.txt` lá)

- **`camera_livre`** (perfil novo, `test = true`): LOAD GAME até o episódio 2 com a opção ligada. Assume com yaw/pitch da câmera do jogo; RS gira (≈160°/s), o andar segue a câmera (log do probe: o yaw do vetor de movimento é o da câmera, ±90° e 180° nos lados e para trás), pulo, R3, olhar para cima com a subida por colisão com o chão, voltar em direção à câmera, que se aproxima (2,4) quando bate atrás. `orbita_andar_colisao.png`.
- **Cutscene**: `camera_livre_abertura` (NEW GAME): a cena de abertura inteira com a câmera do jogo (o controlador não roda; `game camera (cinematic)` no fim), a câmera livre assume na vila e gira, anda, recentra. `abertura_cena_e_vila.png`.
- **Titã**: replay `gravado_20261020_221744` com `CRASH_CAMERA_FREE_AFTER=78`: liga com o Crash já montado, `riding (titan): farther away`, e o titã anda para onde a câmera olha (analógico para cima: 75° com a câmera em 71°). `tita_montado.png`. Antes da correção a câmera voltava para a do jogo depois de 10 s ("player not controlled"), porque só o comportamento de chão marcava o jogador como controlado.
- **Desligada**: o mesmo replay sem a opção dá o mesmo enquadramento que a execução de ontem, antes do mod: diferença média 0,64/255 no início e 8,2 no fim (a luta), contra 0,67 e 8,8 entre duas execuções de ontem. Nenhuma linha `free camera: active` no log. `desligada_igual_antes.png`.
- **Tela**: `--settings-preview` em inglês e PT-BR (`settings_preview\`).
- Encontrado e corrigido nos testes: `DEFAULT_PITCH` é macro do `wingdi.h`; o R3 lido pelos canais do mapa de input mudava de canal; recentrar pela linha "frente" da matriz do ator errava 120–160°; olhar para cima abaixo de −20° colava a câmera no chão; a parede invisível atrás do ponto de partida do episódio 2 deixava a câmera a 0,8 (dentro da cabeça), daí o mínimo de 1,4; "montado" por "nem chão nem ar" disparava em todo pulo (o gancho do ar não marcava).

### Limitações conhecidas

- **A fase não foi feita para uma câmera livre**: as câmeras de volume escondem o que fica fora do enquadramento planejado. Olhando para trás no começo do episódio 2 aparecem paredes invisíveis logo atrás do ponto de partida (a câmera para a 1,4 do Crash), bordas de cenário e fundos sem acabamento.
- **Paredes invisíveis** estão na mesma lista de colisão das visíveis: a câmera bate nelas também (não dá para distingui-las pelo raio; o `hit` só devolve a distância).
- **Culling e LOD**: o jogo escolhe o que desenhar pela câmera de render (a órbita), mas algumas partes são carregadas/ligadas por volumes pensados para a câmera original; podem aparecer objetos surgindo (popping) nas bordas.
- **Som**: o ouvinte continua na câmera do jogo (a matriz do controlador), não na órbita.
- **FOV**: continua o do volume do jogo (`SetCameraVolumeFOV`); a órbita não muda o FOV.
- **Perfis gravados** com a câmera do jogo desincronizam com a câmera livre (o analógico esquerdo é relativo à câmera); `CRASH_CAMERA_FREE_AFTER=<s>` liga a câmera livre só a partir de um ponto do replay.
- **Titã**: montado, o Crash não roda mais os comportamentos de chão/ar (o titã é dirigido pelo mesmo mapa de input) e o ator dele vai junto, nos ombros do titã; o ator do Crash vai junto, nos ombros do titã; a câmera continua seguindo o Crash, com o alvo um pouco mais alto (1,8) e a distância 1,6× ("montado" = o comportamento de chão do Crash parado há mais de 45 tiques, 1,5 s, com o input em uso; um pulo dura menos, e numa queda longa a câmera só se afasta um pouco, aos poucos). Perto de pedras e paredes a distância cai como a pé, e o titã ocupa a parte de baixo da imagem. Os golpes e o alvo do titã são os do jogo; a câmera não mira inimigos.
- **Co-op**: segue o primeiro jogador.
- **Câmeras roteirizadas** (corredores, perseguições) devolvem o controle; volumes "normais" não.

## Volumes ao vivo, opções do jogo e a tela na pausa (`audio/game_volumes.cpp`, `settings/game_options.cpp`, `settings/settings_menu.cpp`, `diagnostics_memdiff.cpp`)

Pedido: música/efeitos/vozes em tempo real; a tela de configurações aberta também durante o jogo, com as opções do menu original de pausa (cooperativo etc.) dentro da aba JOGO e a tela original escondida, sem duplicar; essas opções também no menu principal. Uso: README, "Durante o jogo e as opções do próprio jogo". Capturas: `docs\capturas\settings_aplicar\pausa_*`, `menu_principal_*`, `volume_musica_ao_vivo_*.wav`.

**Por que não por stream XMA.** Primeira ideia: classificar cada buffer XMA pelo `.rsd` de onde veio (o caminho original no cabeçalho: `sounds\music\` = música, `sounds\character\` = vozes, o resto efeitos) e escalar as amostras decodificadas. Não serve para os efeitos: muitos são descompactados uma vez na carga da fase (passo das legendas: "descompactação na carga") e tocados da memória do jogo, então o volume deles ficaria preso ao da carga.

**Ferramentas de memória** (`diagnostics_memdiff.cpp`, só depuração): `CRASH_MEMDIFF_TIMES="t1,t2,..."` copia a memória guest comprometida (heaps do título, imagem 0x80000000–0xA0000000 e os 512 MB físicos, ~534 MB) nesses segundos do relógio do input e grava em `memdiff_<n>.bin` (na pasta das capturas) cada palavra de 32 bits que mudou desde a anterior (`{u32 endereço, u32 antigo, u32 novo}` big-endian). Alternando uma opção ida e volta entre intervalos parados, os candidatos são as palavras que mudam nos dois sentidos e não mudam nos intervalos parados (script Python ad hoc). `CRASH_WATCH="0xADDR,..."` (até 4) + `CRASH_WATCH_TIME=<s>`: breakpoints de escrita de hardware (DR0–DR3, reaplicados em todas as threads a cada 1 s por uma thread própria; handler vetorado que reconhece `Dr6`) que logam o valor novo, a função guest que escreveu (`FindGuestFunction` do RIP) e as funções guest achadas na pilha host: `[watch] write to 0x... by sub_...; stack: ...`. Os endereços do heap se repetem entre execuções com o mesmo roteiro, o que permite achar um endereço numa execução e vigiá-lo na seguinte.

**Volumes** (`audio/game_volumes.cpp`). Música 1,0 → 0,8 no menu de opções do jogo mudava três cópias; o watchpoint pegou `sub_82241838` (`stfs f1,36(r4)`), chamado por `sub_82241968` (limita a [0, 1] e grava em `(this+4)->+36`); `sub_822419A8`/`sub_822419E8` fazem o mesmo para `+8`/`+12`, e `sub_82241840/48/50` leem por `sub_82241830` (`lfs f1,36(r4)`). `this` vem de `sub_82241820` = ponteiro global em `0x824A8FB0`. Categorias, pelo áudio gravado: `+4` música, `+8` diálogo, `+12` efeitos (com efeitos = 0 o som de seleção do menu some). O sistema de som multiplica esse valor pelos snapshots do mixer a cada atualização (0,85 × 0,8 = 0,68 apareceu num campo combinado, e o ganho vai com o quadrado), então gravar o campo muda o som na hora. Os ganchos `PPC_FUNC(sub_82241838)` e `PPC_FUNC(sub_82241830)` guardam o valor que o jogo quer (devolvido nas leituras dele) e gravam `sqrt(porcentagem)` da configuração no lugar; `audio::volumes::Tick()` (a cada leitura do controle) acha o objeto quando ele aparece (as categorias nascem nulas e são preenchidas depois do ponteiro; ~12 s no boot) e regrava se o jogo escrever direto. As estrelas do jogo deixaram de contar: a tela original está escondida, então os volumes da tela são o único controle. O patch de boot do `mixers.p3d` (`PatchMixers` em `frontend_patches.cpp`) foi removido para não aplicar duas vezes. Conferido com o `audio.wav`: música a 0% com o padrão (RMS 0 a partir do momento em que as categorias aparecem, ~12 s; o que toca antes, nos logos, não é afetado), efeitos a 0% apaga o som de seleção, e na tela aberta (menu e pausa) segurar ← leva a música a 0% / 10% e → de volta, sem reiniciar.

**Opções do jogo** (`settings/game_options.cpp`). O memdiff alternando cada opção na tela original: COOPERATIVO muda o byte global `0x8245FA99` (1 = LEAPFROG/pula-sela, o padrão; 0 = PIGGYBACK/cavalinho), escrito por `sub_82152C60` (`stb r3`); SALVAMENTO AUTOMÁTICO, o bit 0x10 do byte `+100` de um objeto do heap, escrito por `sub_821D0750(this, bool)` (que, ao desligar, põe `-1` no índice do slot em `+99`); INVERTER EIXO não apareceu como palavra estável (cópias de UI). O formato veio de `CFrontendManager::CSaveGameHandler::vf02` (`sub_821D9AA8`, a leitura das opções do save): três floats (os volumes, pelos setters acima) e uma palavra de flags: bit 0 → bit 0x02 do byte `+16652` do `CFrontendManager` (inverter eixo; a câmera, `sub_8221B298`, testa esse bit e faz `fneg` no analógico a cada quadro), bit 1 → `sub_82152C60` (cooperativo), bit 2 → `sub_821D0750` no gerente 13 (salvamento automático). Os gerentes: `sub_821FB4F0` devolve o objeto estático `0x824A7A24`, e `sub_821FA440(obj, n)` lê a palavra em `obj + (n + 7) * 4`: FrontendManager = 6 (`0x824A7A58`), SaveGameManager = 13 (`0x824A7A74`). `settings::GameOptionsTick()` (a cada leitura do controle) compara e regrava: cooperativo e inverter eixo sempre como no `settings.toml` (vale no menu principal e por cima do que o save carregado traz: o log mostra a reaplicação depois do LOAD GAME); salvamento automático desligado limpa o bit (sem mexer no índice do slot), ligado só devolve um bit que o runtime limpou e só com slot válido, porque o próprio frontend liga/desliga o salvamento conforme o slot (`SetAutoSaveGame(true)` em `ExitOverWrite`/`ExitAvailableSlot`, `false` em `ExitContinueWithoutSaving`/`ExitInGame`). Confirmação de que o jogo usa: com o salvamento automático desligado pela tela, o jogo passa à pausa "sem salvar" (SAIR DA FASE).

**A tela na pausa.** `frontend_hooks.cpp` (o gancho de condições `sub_820F9BA8`, da sessão do frontend) agora avisa a tela do nó atual (`NoteFrontendNode`, o `r4` da chamada externa: o id de nó, o mesmo número dos destinos de saída). O nó da tela de opções da pausa sai do `Frontend.bfig` (patch só de leitura registrado em `menu::Init`: o dono da primeira saída depois do nome `InGameOptions`, 1154); as três variantes da pausa (`PauseScreen` 1081, `NoSavePauseScreen` 1109, dois jogadores) têm `ExitOptions` → 1154. Quando o nó vira 1154, `RequestOpen(false)`: a tela abre sobre a tela de opções original, com o fundo escuro quase opaco (alfa 252) que a esconde, e o jogo não recebe nada; B fecha e aperta B para o jogo (o `ExitBack` dela volta à pausa), como no menu principal. Se o nó mudar sozinho, a tela fecha sem apertar nada. Uma versão intermediária tinha a tela original como primeira aba (a do jogo funcionando por baixo, com só as abas desenhadas por cima e LB/RB filtrados), mas duplicava os volumes e foi trocada a pedido.

**Testes**: `configuracoes_pausa` (pausa > OPÇÕES, música ao vivo, as três opções na aba JOGO, resolução 1600x900 ao fechar, reabrir pela pausa "sem salvar", voltar a jogar) e `configuracoes_jogo` (as opções mudadas no menu principal, LOAD GAME até a fase, reaplicadas por cima do save); `configuracoes`, `configuracoes_reinicio`, `configuracoes_resolucao` e `primeira_fase` continuam passando.

## Controles de PC: teclado, mouse, DualSense e ícones (`kernel/pc_input.*`, `kernel/hid_pads.*`, `kernel/stick_rotation_hook.cpp`, `settings/controls.*`, `settings/prompt_icons.*`, `settings/menu_mouse.*`)

Uso e teclas no README, "Controles de PC". Aqui o que foi achado no jogo e como o runtime faz.

### Os controles do jogo

- **Mapa de entradas**: `script\mixins\inputmap_methods.lua` (em `default.rcf`). É bytecode **Lua 5.0** com cabeçalho de 5.1 (`1B 4C 75 61 51`, little-endian, número float de 4 bytes; instrução = op 6 bits | C 9 | B 9 | A 8, RK constante quando ≥ 250, opcodes do 5.0: `CALL` = 25, `SELF` = 11). Os `.blua` das árvores de luta usam o mesmo formato. Desmontado, `InputMapMethods.SetupController` faz `CInputMap:BeginEvent(evento) / AddInputDown(EControllerValue_*) / EndEvent` por mapa (`GENERAL`, `GENERAL_AIR`, `GENERAL_GROUND`). Os `EControllerValue` têm nomes de PS2; no Xbox: `X` = A, `CIRCLE` = B, `SQUARE` = X, `TRIANGLE` = Y, `R1` = RT (o texto "PRESS AND HOLD · TO BLOCK" usa o ícone 0xB7 = RT), `R2` = RB, `L2` = LB. Eventos: JUMP = A; ACTION / COUNTER = B; DETACH = Y; ATTACK_LIGHT = X (no ar também SPIN, o giro planando); ATTACK_HEAVY = Y (+ segurado); BLOCK e GROUND_SLIDE = RT segurado; FINISHMOVE = RB; CHEER = LB + Y, IDLE = LB + B, ATTACK_COMBO1/2 = LB + A / X; LIGHT_LIGHT_HEAVY etc. com janelas de 0,5/0,75 s. A tabela física do driver (`0x824733B4`, "Xenon Gamepad": 20 botões com as máscaras XInput, `0x824733D0`) não muda nada disso.
- **Golpes de girar o analógico** (textos do jogo): OLD SKOOL `¹¼,³,³,³` (analógico + girar + ataque leve repetido), SPIN DISMOUNT (girar + ação), GYRO JACKHAMMER e golpes de titã (girar + ataque pesado).
- **Detector de rotação** (`sub_821EC3E0`, chamado por `sub_821EC4A8` para cada controle: `device+492` analógico esquerdo, `+496` direito): `sub_821EB4D0` classifica o analógico em 8 octantes (`x²+y² > 0,36`; índice = 4·(x≥0) + 2·(y≥0) + (|x|>|y|)); o octante visto recebe 1,0 (`sub_821EA518`); todos decaem 1,333/s (`sub_821EB438`); com no máximo um octante apagado (`sub_821EA530`) o byte `+40` vira 1 por 0,2 s e os octantes zeram. Analógico no centro zera tudo. As amostras vêm do histórico do dispositivo (uma por leitura do controle).
- **Sacudir** (`ControlWaggleCount`, `sub_821EB558`): conta trocas de lado do analógico com `x²+y² ≥ 0,4436` em janelas de 0,2 s; usado nos "escape" (≥ 6 trocas).

### Teclado e mouse (`kernel/pc_input.cpp`)

- A janela (`gpu/window.cpp`) manda WM_KEY*/WM_SYSKEY* (Shift/Ctrl/Alt esquerdo e direito separados), botões e roda do mouse, WM_MOUSEMOVE e WM_INPUT (movimento cru do mouse, `RegisterRawInputDevices`). As teclas viram um estado "segurada" + "apertada desde a última leitura do jogo" (um toque mais curto que um passo de simulação ainda conta). Perder o foco solta tudo. `WM_SYSKEYUP` de Alt/F10 e `SC_KEYMENU` não chegam ao `DefWindowProc` (abriam o menu da janela e paravam o laço de mensagens). `WM_MOUSEACTIVATE` devolve `MA_ACTIVATEANDEAT`.
- **Contexto**: `frontend_hooks.cpp` chama `input::pc::NoteFrontendNode` em toda avaliação das condições do Frontend (`sub_820F9BA8`, uma vez por quadro com uma tela do frontend ativa). Um patch só de leitura do `fighttrees\frontend.bfig` (`FindGameplayNodes`) acha os nós de gameplay: o destino de `ExitGameRunning` (638 nesta cópia) e os donos de `ExitPlayerSkips`/`ExitSequencerDone`/`ExitArenaCinematic`/`ExitWhenDone1` que vão para ele (11 cinemáticas); a pausa é o destino de `ExitPause` (1081). Sem avaliação há 250 ms (vídeos da abertura, carregamentos) vale a gameplay.
- **Gameplay**: as ligações de `[controls]` viram botões, gatilhos e analógicos (WASD normalizado na diagonal; andar devagar = `walk_speed`); o mouse capturado vira analógico direito (~15 contagens por leitura = deflexão total a 100%). A rotação avança 40° por leitura do jogo (`XamInputGetState`), sempre com raio 1, partindo da direção do WASD; o ataque giratório aperta X por 2 leituras a cada 4, a partir da 9ª. O teclado e o mouse somam com o controle (botões OR, analógico do teclado vence quando mexido).
- **Testes**: `CRASH_INPUT_SCRIPT` aceita `KEY_<nome>` (passa pelas mesmas ligações; também é capturado pela aba CONTROLES), `MOUSE=x/y`, `CLICK=x/y`, `RCLICK=x/y` (imagem 1280x720; com eles o mouse real é ignorado sobre a janela, que recebe `WM_MOUSEMOVE` mesmo sem foco) e `DEVICE_XBOX|PLAYSTATION|KEYBOARD`. O gravador (F5) continua gravando o controle que o jogo leu, então o teclado e o giro sintetizado entram nos perfis gravados como analógicos.
- **Verificado** (`teclado`): o log tem `[input] keyboard: walking (left stick at 40%)`, `keyboard: rotating the left stick (spin attack)` e, logo depois, `game: left stick rotation detected on controller 0` (o hook `stick_rotation_hook.cpp` lê o byte `+40` do detector); nas capturas o Crash faz o giro (anel laranja do SpinAttack).

### Controles PlayStation (`kernel/hid_pads.cpp`)

- Thread de busca (SetupAPI + `hid.dll`, a cada 2 s e em `WM_DEVICECHANGE`), só caminhos com `vid_054c` (USB) ou `vid&0002054c` (Bluetooth); modelos 0x05C4/0x09CC/0x0BA0 (DS4), 0x0CE6 (DualSense), 0x0DF2 (Edge), usage 1/5. Cada controle tem uma thread com `ReadFile` sobreposto.
- Relatórios: DS4 USB e o "simples" 0x01 que os dois mandam por Bluetooth: `[1..4]` analógicos, `[5]` hat + □✕○△, `[6]` L1 R1 L2 R2 Share Options L3 R3, `[7]` PS/touchpad, `[8..9]` L2/R2; DS4 BT 0x11: 2 bytes depois; DualSense USB 0x01 (64 bytes): `[1..4]` analógicos, `[5..6]` L2/R2, `[8]` hat + face, `[9]` ombros/Create/Options/L3/R3, `[10]` PS/touchpad/mute; DualSense BT 0x31: 1 byte depois. USB × Bluetooth pelo tamanho do relatório de entrada (64 = USB).
- Vibração: DualSense USB 0x02 (`[1]` = 0x03, `[3]` direito, `[4]` esquerdo), BT 0x31 (seq/tag 0x10, CRC-32 de 0xA2 + relatório nos 4 últimos bytes); DS4 USB 0x05, BT 0x11 (0xC0, CRC). Não vai para um PlayStation juntado a um Xbox virtual (o DSX/DS4Windows repassa a do virtual). Cada controle tem um id que nunca se repete (`Pads()`, `ReadPad`, `SetPadVibration`); quem vai para qual jogador é `pad_slots.cpp`.
- `CreateFile` com "acesso negado" num DualSense = DSX/DS4Windows/HidHide: o controle entra em `HiddenPads()` e o movimento XInput passa a contar como PlayStation para os ícones.
- **Verificado** com um DualSense real por USB (`--hid-monitor`): ~500 relatórios/s, os 14 botões, gatilhos e analógicos no curso todo; `--hid-selftest` cobre os 5 formatos e o CRC (`CBF43926`).

### Co-op local (`kernel/pad_slots.*`, 2026-10-22)

- **Como o jogo faz**: lê os quatro usuários a cada tick (`sub_8228A7C8`, um objeto por controle, conectado em `+40`, `XamInputGetCapabilities` e depois `XamInputGetState`; o número do pacote não importa). Um segundo usuário conectado faz o HUD mostrar JOIN GAME (B); B junta o jogador 2 na mochila do jogador 1, `EInputMapEvent_DETACH` (Y, `inputmap_methods.lua`) solta; START nesse controle = P2 PAUSED. O jogador 1 é o controle que apertou START na tela título: `sub_821DB680` confere os quatro e chama `sub_821FC498(managers, índice)`, que guarda o índice em `managers+348` (e o perfil em `+328/+332`, a partir de `XamUserGetSigninState`; o usuário 1 não precisa de perfil). Isso só acontece na tela título (4 chamadas, todas antes do menu principal); a pausa do P2 não chama.
- **O defeito antigo**: o runtime somava o n-ésimo PlayStation (HID) no usuário n **e** o XInput n no mesmo usuário, e o teclado no usuário 0. Xbox + DualSense, ou DSX/DS4Windows (Xbox virtual no XInput 0) + um DualSense visível = tudo no jogador 1. Além disso, só o controle 0 tinha o número do pacote mudando.
- **Agora**: `pad_slots.cpp` mantém uma tabela de aparelhos (slot XInput, id HID estável de `hid::Pads()`, ou os dois juntos) com o jogador de cada um (fixo até desconectar; novos pegam o primeiro livre; com 2+ aparelhos `controls.player1/2` mandam). Jogador p vai para o usuário `[P1, depois os outros em ordem][p]`, com P1 = `sub_821FC498` (hook no mesmo arquivo). Em AUTO, o hook troca o jogador 0 com o aparelho que apertou START (nada muda de usuário, só o teclado segue); com um P1 escolhido, o aparelho escolhido vai para aquele usuário. Atualiza no máximo a cada 3 ms; slots XInput vazios são testados 1x por segundo.
- **Espelho DS4Windows/DSX**: um PlayStation novo com XInput conectado fica "desconhecido" (não joga) até os apertos dizerem: aperto do HID e aperto do XInput com o mesmo número de botões em −40..+150 ms = igual; 2 iguais no mesmo slot = espelho (junta), 1 sem par e nenhum igual (ou 2 sem par) = próprio. Um XInput que aparece depois de um PlayStation "próprio" reabre a conferência só com esse slot (DS4Windows aberto com o jogo rodando): se for espelho, o virtual entra no aparelho do PlayStation e ele mantém o jogador.
- `xam_input_imports.cpp`: o bloco do teclado/script/gravação/filtros roda no usuário do P1 (`slots::PlayerOneUser()`), os outros usuários recebem só o controle físico e as entradas `P2_`..`P4_` do script; a tela SETTINGS e o console também escondem os outros controles do jogo; pacote por usuário.
- Verificado: `--pads-selftest` (17 casos), `coop_p2` (virtual: JOIN GAME, mochila, Y, anda, P2 PAUSED), `coop_jogadores` com dois DualSense USB reais (troca pela tela no log), `coop_manual` jogado de verdade com o segundo DualSense no P2 (`docs\capturas\coop\`). `GetCurrentPlayer(1)` do Lua devolve o jogador 1 mesmo com o 2 solto (não serve para achar o P2).

### Ícones (`settings/prompt_icons.cpp`, `gpu/d3d12_renderer.cpp`)

- Os ícones 0xB1-0xBE das fontes são texturas **k_8 paletizadas** (índices de cabeça para baixo, `host R8_UNORM`, swizzle 0xA00) lidas pelos shaders `pal*fp` (`palette.fxh`: `tex2D(palette, tex2D(diffuse, uv))`, paleta 256x1 RGBA8 no sampler s8, bilinear feito à mão sobre 4 amostras). Por isso o dump de texturas (só RGBA8/BC) não as via, e as chaves "previstas" das exportações RGBA não batiam. A chave certa é `hd::Key` dos índices como R8: os índices da paleta do PNG exportado (palettizado), invertidos na vertical. 33 texturas (Titans_v3_32/48/64/96, KLEPTO 20/24/36; o B de 64 px é também o "aperte B" do HUD) estão na tabela de `prompt_icons.cpp` com a área opaca de cada uma.
- No upload, texturas R8 até 128x128 têm a chave calculada; se for um ícone, a entrada guarda qual. Ao ligar a textura, se o estilo (`Style()`: aparelho, menus × gameplay e as teclas ligadas) mudou, o renderer desenha o ícone novo (`DrawPalettized`: formas com distância com sinal + a fonte Titans do jogo, quantizado em até 256 cores) e liga os índices novos; a paleta nova substitui a textura 256x1 do mesmo draw (`g_drawIconPalette`). As versões ficam em cache por (chave, estilo). `menu_art::DrawIcon` (tela SETTINGS) usa o mesmo desenho.
- `CrashRecomp.exe --prompt-preview <pasta>` gera a folha (o jogo, PlayStation, teclado na gameplay e nos menus) em 32/64/128 px.

### Mouse nos menus (`settings/menu_mouse.cpp`, `pc_input.cpp`)

- `pure3d::frontend::Menu`: itens em `+112..+116` (vetor de ponteiros), índice selecionado `int8` em `+152`; `TextMenuItem+140` = o texto, com a posição em `+92 / +96` (unidades de página / 640, y para cima, canto inferior esquerdo). `GetMenuIndex` (`sub_820F9A10`) = página por nome (`sub_820F99D8`, projetos 5 e 1) → `sub_822F1CD0(page, nome)` → `+152`.
- `Menu::vf05` (`sub_822F2BA0`) roda também para menus escondidos e não roda para outros: o menu ativo vem de **`CMenuAction::vf05`** (`sub_820B2E98`, a ação de menu da árvore do frontend, por quadro; `this+24` = o `Menu`; lê os eventos de cima/baixo e chama `sub_822F2B50/2B78/2F58/3068`). Largura e altura de cada item vêm dos chunks de texto `0x18023` (`18, x, y, w, h`) de todas as páginas (`0x18002`) dos pacotes do frontend (patches só de leitura); itens movidos por patch casam pelo mesmo x.
- Cursor → página: imagem 16:9 centrada na janela (o present encaixa o front buffer 1280x720 com barras pretas), `input::pc::CursorOnImage` dá o ponto na imagem 1280x720 e a página 640x480 é esticada nela (y para cima). Linhas (saves) aceitam a área acima do rótulo (a figura).

#### Seleção direta (revisão "como num jogo de PC", 2026-10-22)

Antes o hover apertava o D-pad pelo jogo até o índice chegar ao item, então o destaque e o som passavam por todos os itens do meio. Agora o gancho de `CMenuAction::vf05` (depois do original, que já leu os pads e o teclado) age sobre o menu ativo:

- **As funções do `Menu`** (`sub_822F2xxx`, achadas em `CMenuAction::vf05`): `sub_822F2E40(menu, i)` = **SetSelection**: chama o "desmarcar" do item antigo (vf68) e o "marcar" do novo (vf64, com o bit `+153 & 0x40`), move o sprite do cursor (`sub_822F2D08`, `menu+128`, pela diferença das posições), grava `+152 = i` e zera o tempo `+148`. `sub_822F2F58` / `sub_822F3068` = anterior / próximo (devolvem 1 se andou): com `+153 & 0x80` dão a volta e pulam os itens sem `item+137 & 0x80` (o bit "selecionável"); sem ele param nas pontas. `sub_822F2B50` / `2B78` = esquerda / direita no item (vf76 / vf80, para valores). `sub_822F2F50` é só um thunk do SetSelection.
- **O som**: a ação toca, depois de anterior / próximo, `sub_821DB260` / `sub_821DB280` (`sub_821DB1D8` com os nomes em `0x82006DA4` + `0x82006DC8` / `0x82006DBC`) no tocador `sub_821FA440(sub_821FB4F0(), 6)`. Menus em fileira (`action+8` byte `+56` ≠ 0, os saves) usam os eventos 23/33 (esquerda/direita) para anterior/próximo; colunas, 39/16 (cima/baixo).
- **Hover**: só quando o cursor se mexeu (`CursorMoves` mudou) e o item embaixo é outro e selecionável: `SetSelection` + o som (próximo se o índice sobe, anterior se desce), **uma vez**. Um menu novo não conta o lugar parado do cursor como hover. **Clique** num item: SetSelection (se preciso) e `PressMenuAccept` (A por 2 leituras do jogo, em `MouseMenuButtons`); fora dos itens, nada. **Roda**: anterior / próximo por dente (até 8 por quadro), com o som de cada passo. Botão direito = B; numa tela sem menu, clique = A (`TakeMenuClick`, cliques de até 400 ms).
- Enquanto a tela SETTINGS está aberta (ou soltando os botões, `BlocksGameInput`) o gancho descarta cliques e roda: o menu de opções do PS2 por trás dela também roda e ganharia o hover.
- Outras ações de menu do jogo (`sub_820BE0C0`, `sub_821D5858`) também chamam anterior/próximo com sons próprios; as telas testadas não passam por elas.
- `CRASH_MOUSE_TRACE=1`: `[menu] update` (itens, caixas, `off` = não selecionável) e `[menu] 0x... selection a -> b` em toda troca de índice, venha de onde vier; `[input] mouse: hover / click / wheel` saem sempre.

#### Tela SETTINGS com o mouse (`settings_menu.cpp`)

- `Rasterize` grava as zonas do que desenhou (`g_zones`, unidades virtuais 1280x720): cada aba, LB / RB, cada linha (de `PANEL_L` a `PANEL_R`, a altura do passo), as setas de cada linha com valor (onde ficam quando a linha está selecionada), as estrelas, (B) VOLTAR / SELECIONAR (A) (as dicas do jogo em x 232-375 / 873-1040, PT 232-410 / 786-1040; no modo escurecido, o VOLTAR desenhado por nós) e REINICIAR / DEPOIS da pergunta. Setas e estrelas entram antes da linha (`ZoneAt` pega a primeira).
- `Mouse()` (no `Update`, thread do render): cursor da área cliente (0..1) → pixels do desenho → virtual. Passar numa linha a seleciona; clique: aba = abre; LB/RB, setas, linha (A), VOLTAR, SELECIONAR, REINICIAR / DEPOIS = os botões (entram em `pressed` e seguem a lógica de sempre: captura de tecla, pergunta de reinício); estrelas = valor direto, em estrelas inteiras (`ceil(t·10)/10 · máximo`, no passo da opção) e arrastar enquanto o botão segue apertado. Roda = `Move` por dente (`TakeSettingsWheel`). Durante a captura de tecla da aba CONTROLES o clique é a tecla (MOUSE1), não a interface. Respeita `controls.mouse_menus`.
- Abas longas (CONTROLES) agora rolam só o necessário para a seleção ficar visível (`g_first`), em vez de centrar nela: as linhas não fogem do cursor ao passar por cima.

**Verificado** (`mouse_menus`, `build\mouse_menus\`): `hover menu 0xA0386410 item 3 -> selected (was 0)` e uma linha só `selection 0 -> 3` (NEW GAME → SETTINGS); roda `3 -> 4`, `4 -> 3`; KEY_UP `3 -> 2` e D-pad `2 -> 3` com o mouse parado; clique abre SETTINGS; `tab -> audio (mouse)`, `audio.effects -> 50% (mouse)`, arrasto `audio.voice` 10 → 30 → 80%, clique em LEGENDAS = ON, roda, RB = VÍDEO, seta esquerda = `video.fps_limit` 30 → ILIMITADO, `tab -> controls (mouse)`, roda −9 rolando a lista, clique em VOLTAR fecha; LOAD GAME (`0 -> 1`, clique), slot, CONTINUE, menu do episódio, pausa (hover OPTIONS, clique RESUME). Janela de teste em 2560x1440 (escala de DPI): o mapeamento acerta.

## Abas da tela SETTINGS: Definitive Edition (`settings/settings_menu.cpp`)

Pedido: CORES DO MOJO, HD AI REMASTER e a câmera experimental juntos numa aba **DEFINITIVE EDITION** (o mesmo nome nos dois idiomas), a aba LEGENDAS dentro de ÁUDIO e a ordem JOGO, ÁUDIO, VÍDEO, GRÁFICOS, CONTROLES, DEFINITIVE EDITION. Só a disposição mudou: opções, ajudas, o que vale na hora / ao reiniciar, `NeedsRestart`, as chaves do `settings.toml` e os nomes no log (`[settings] game.mojo_colors -> ...`) são os mesmos. Uso: README, "Configurações". Capturas: `docs\capturas\settings_abas\`.

- `TABS` (ids no log `[settings] tab -> ...`): `game` (IDIOMA, COOPERATIVO, INVERTER EIXO, SALVAMENTO AUTOMÁTICO, ARMAZENAMENTO), `audio` (os 4 volumes, o cabeçalho LEGENDAS e as 3 opções de legenda), `video` e `graphics` (sem mudança), `controls` (sem mudança), `definitive` (CORES DO MOJO, HD AI REMASTER, o cabeçalho CÂMERA EM TERCEIRA PESSOA e as 4 opções da câmera livre). A aba CÂMERA e a LEGENDAS sumiram. A tela abre em JOGO (a primeira aba); `GAME_TAB = 0`, `DEFINITIVE_TAB = 5`.
- **Sub-grupos**: itens `Item::HeaderSubtitles` / `HeaderCamera`, não selecionáveis (`Selectable`), desenhados como o nome centrado na cor do título, menor (80% da linha), entre linhas que somem para fora. `Move` (cima/baixo) pula eles como pulava ARMAZENAMENTO e RESOLUÇÃO DAS SOMBRAS, e `Reset` põe cada aba na primeira linha selecionável. ÁUDIO fica com 8 linhas e DEFINITIVE EDITION com 7, abaixo do limite de 9 (`MAX_ROWS`).
- **Barra de abas**: o espaço das abas agora desconta LB/RB (ícone + 22 de folga + 24 da borda de cada lado, ~1010 virtuais em vez de 1060). Se os nomes, com o espaço mínimo de 16 entre eles, não couberem, as letras das abas diminuem (centradas na mesma linha) em vez de LB/RB saírem da tela. A primeira versão, com o limite antigo de 1060, deixava LB e RB encostados nas bordas da tela com "DEFINITIVE EDITION". A tela é desenhada numa área virtual 1280x720 centrada, então 1920x1080, 3840x2160 e 2560x1080 ficam iguais (conferido no `--settings-preview`).
- `--settings-preview` grava cada aba (`*_1_game` ... `*_6_definitive`) com a segunda linha selecionada e mais `*_2_audio_legendas`, `*_6_definitive_camera`, `*_6_definitive_reiniciar` (caixa de reinício) e `*_1_game_pausa` (aberta pela pausa, COOPERATIVO selecionado).
- **Perfis** (`config/run.toml`): `configuracoes` (RB = ÁUDIO para o volume, RB = VÍDEO para o FXAA, RB até DEFINITIVE EDITION e de volta a JOGO e ÁUDIO; confere `tab -> controls` e `tab -> definitive`; 58 s), `configuracoes_reinicio` (sem RB: IDIOMA é a primeira linha da aba inicial), `configuracoes_resolucao` (RB ×2 = VÍDEO), `configuracoes_jogo` (sem RB; DOWN ×1 = COOPERATIVO), `configuracoes_pausa` (as três opções da aba JOGO primeiro, RB = ÁUDIO para a música, RB = VÍDEO para a resolução) e `controles_tela` (E ×4 = CONTROLES). Os perfis da câmera livre e do mojo usam `CRASH_SETTINGS` e não navegam na tela; os dois gravados de `config/recorded` não entram em SETTINGS.

## Launcher (`launcher/`, alvo CMake `CrashLauncher`)

`CrashLauncher.exe` edita o `settings.toml` antes de abrir o jogo, com a aparência do frontend do jogo (README, "Launcher"). É Dear ImGui 1.91.8 (`thirdparty/imgui`, MIT; só o núcleo e os backends Win32 e Direct3D 11) linkado com partes do runtime, sem cópia de código:

- `settings/settings.cpp` e `controls.cpp`: `Values`, `Load`/`Save`, nomes e limites; `language_pack.cpp` para a lista de idiomas; `hd_remaster.cpp` para saber se o remaster existe; `paths.cpp` com `Init(argc, argv, false)` (sem a janela de pasta e sem erro quando não há jogo) e `PickGameFolder`; `fs/vfs.cpp` só para o `menu_art` achar o `default.rcf` e os mods.
- `settings/menu_art.cpp`: as fontes Titans e a arte. Acrescentei a ele o fundo do menu principal (`FE_Background_Weave`, `_Fold`, `_Foliage_Top`, `_Foliage_Bottom`, `_Light_Map`) e o `FE_Title_Logo` de `package\cdd70a8c.p3d`, e a montagem de sprites em vários tiles (`StitchSprite`, a mesma regra do `sprite_layout` do `tools/textures.py`: tiles linha a linha com borda de 1 px); o logo é um sprite de 4 tiles. O runtime carrega essas imagens também (alguns ms na thread do `menu_art`), sem usá-las.
- `launcher/launcher_stubs.cpp`: `Log` vai para `launcher.log`; `prompts::Style/Draw` (ícones de PlayStation/teclado, que dependem da entrada do jogo) e `rcf_overlay::RegisterEntryRewrite` (reescrita das bíblias pelo pacote de idioma) não fazem nada.

**Fontes**: cada letra das Titans é desenhada pelo `menu_art::DrawText` (preenchimento branco, contorno preto) e vira um glifo de uma fonte do ImGui (`AddCustomRectFontGlyph`), em duas versões: uma com o glifo inteiro (o contorno) e outra só com a parte clara (o preenchimento). O texto é desenhado duas vezes, primeiro na cor do contorno e depois na do preenchimento, então cada estilo tem as duas cores do jogo (laranja/marrom, ciano/azul-petróleo). Os ícones de botão (A B X Y LB RB START...) entram coloridos na área de uso privado (`U+E0B1`...). O que as Titans não têm cai na Segoe UI Bold. Atlas 2048x2048, 592 imagens de glifo, desenhadas maiores (60/44 px) e reduzidas conforme a janela.

**Fundo**: composto na CPU no tamanho da janela: degradê amarelo, trama (`Weave`, repetida), dobras (`Fold`), mancha de luz (`Light_Map`, escurece as bordas) e folhagens nos quatro cantos num laranja mais escuro. A moldura é o `MSG_Background` espelhado nos quatro cantos com tamanho fixo (as partes retas esticadas); as estrelas e os separadores são os da tela SETTINGS.

**Arquivo `windows.h`**: o `menu_art.cpp` é compilado com o `windows.h`, onde `DrawText` é a macro `DrawTextA`, então o símbolo exportado é `menu_art::DrawTextA`; o launcher inclui o `windows.h` antes do `menu_art.h` pelo mesmo motivo.

**Verificação** (`docs\capturas\launcher\`): as sete abas pelo `--screenshot`, do `build\` e do `dist\` (com os caminhos resolvidos pelo exe); o laço normal abre, fica parado e fecha pelo X com saída 0 (106 MB); o jogo, depois da mudança no `menu_art`, continua com a arte da tela SETTINGS completa (`11/11 images`) e passa no `boot --dist`.

## Desempenho de CPU: command processor, buffer de comandos do D3D e estado do D3D12 (`gpu/d3d12_renderer.cpp`, `gpu/command_processor.cpp`, `gpu/ring_buffer_hook.cpp`, `gpu/gpu.cpp`, `tools/perf/`)

Relato: numa RTX 3080 + Ryzen 7 3700X em 1080p o jogo ficava em ~50 fps, e o DLSS não mudava nada. Medido nesta máquina (Ryzen 7 5700X, Zen 3, 8 núcleos; RTX 5080), na vila da primeira fase (a cena mais pesada: ~1.600 draws, 1,35 milhão de índices e ~100 texturas por quadro), 1080p, perfil `desempenho_vila`. Capturas, JSONs e gráficos em `build\desempenho\` (`LEIA.md` lá).

### O gargalo

**CPU, numa thread só: a do command processor** (a thread host que lê o PM4 e grava tudo em D3D12). A GPU espera ~0 ms por quadro em todas as medições, e baixar a resolução ou ligar DLSS não muda o FPS (nativo 65,3, DLSS Qualidade 64,1, DLSS Desempenho 63,8 no exe original). A thread do jogo **não** é o limite: o amostrador mostrou que ela passa ~70% do tempo dentro de `sub_8226D968`, o passo de espera do D3D do Xbox (um laço de `db16cyc` + leitura do fence), chamado por `sub_8226C958` (espera até o fence que a GPU escreve em `*(device+10768)` passar de um valor). Ou seja, o jogo gira esperando o command processor.

Por que espera: o D3D do título monta os comandos num buffer secundário dividido em segmentos e, antes de reaproveitar um segmento, espera a GPU ter passado do fence dele. O Crash passa `D3DRING_BUFFER_PARAMETERS` (em `D3DPRESENT_PARAMETERS + 72`, lidos por `sub_8226D300`) com 6 MB em 32 segmentos de 192 KB, e as constantes dos shaders moram nesse mesmo buffer (`LOAD_ALU_CONSTANT` aponta para lá): na vila são ~6 MB por quadro, então o jogo ficava sem espaço no meio do quadro. Além disso, o D3D só deixa **1 quadro em voo**: o `Swap` (`sub_82274080`) guarda o fence do quadro em `device+14788`, e a primeira troca de segmento do quadro seguinte (`0x8226CE10`, em `sub_8226CCC8`) espera por ele. Isso não foi mexido: o jogo reaproveita buffers dinâmicos de vértices contando com essa garantia.

Na thread do command processor (exe original, ~10,9 ms de trabalho por quadro): **hash do conteúdo das texturas** (cada textura usada é hasheada uma vez por quadro: ~25 MB/quadro, a ~7,5 GB/s) ~19%, **montagem dos índices** ~16%, **chamadas D3D12** ~22% (~17 por draw, inclusive `SetGraphicsRootSignature` em todo draw), parsing PM4 ~12%, e ~14% parada esperando o jogo (a simulação do quadro seguinte, sem comandos novos).

### O que mudou (nenhuma mudança visual ou de jogabilidade)

1. **Índices numa passada** (`BuildIndices`): lê, troca o endian, aplica reset/offset/clamp e mede o intervalo no mesmo laço (sem ramificação quando não há índice de reset, então vetoriza); o vetor vai para a topologia por `swap`, sem cópia. 5,0 → 0,8 ms/quadro.
2. **Registradores em bloco** (`WriteRegisters`): uploads de constantes (`SET_CONSTANT`, `LOAD_ALU_CONSTANT`, type-0) viram `memcpy`, já que o arquivo de registradores fora da primeira página é big-endian como o stream; a primeira página (trap) e os registradores de scratch continuam um a um.
3. **Hash de conteúdo das texturas** (`HashContent`): a acumulação do XXH3 em SSE2 (multiplicação 32×32→64 por 16 bytes) no lugar do `HashBytes` de 64 bits só para esse uso (as chaves de cache continuam iguais): 7,5 → 12–19 GB/s. O layout dos mips só é recalculado na conferência do quadro, não em cada um dos ~20 lookups. "textures" 4,8 → 1,5 ms.
4. **Cópia de vértices por endian** (um laço por modo, vetoriza) e **fences consultados só quando o anel de upload/descritores enche** (antes: `GetCompletedValue` em cada alocação, ~10 mil por quadro).
5. **Buffer de comandos do D3D de 32 MB** (`ring_buffer_hook.cpp`, gancho em `sub_8226D300`): 170 segmentos do mesmo tamanho (o D3D só guarda o tamanho do segmento, `device+14760`). O jogo deixa de esperar espaço; sobra só a trava de 1 quadro. `CRASH_D3D_RING_MB=<n>` (0 = os 6 MB do jogo). 32 MB rodaram sem problema; o jogo sonda a memória física (aloca 368 MB e libera) antes de criar o device.
6. **Cache do estado do command list** (`g_drawState`): entre draws seguidos só é gravado o que mudou (root signature, pipeline, render targets, viewport, scissor, topologia, blend factor, stencil ref, tabela de samplers, constantes de efeitos); um index buffer só para o anel de upload inteiro, com o índice inicial no draw. `InvalidateDrawState()` em `Submit`, `Resolve`, `RunTemporal` (DLSS/FSR) e `Swap` (SMAA, overlay, present), os únicos outros lugares que gravam estado no `g_list`. Custo por 1.000 draws: 11,3 → 6,5 ms.
7. **Espera do D3D cedendo a CPU** (gancho em `sub_8226D968`): quando o passo diz "continua esperando", `_mm_pause` (~16) e, a cada 32 passos, `SwitchToThread`. O giro a toda velocidade disputava cache/memória (e a linha de cache do fence) com o command processor: com FPS sem limite o command processor levava ~25% mais por quadro do que com limite 120 (onde o jogo dorme na espera do vblank) e o "sem limite" dava menos FPS que o limite 120. `CRASH_D3D_SPIN=busy` volta ao giro original.
8. **Limitador de quadros nos limites 60 e 120** (`gpu::PaceFrame`, chamado no `VdSwap`): antes o limite era só o vblank emulado a 2× o limite (o jogo troca a cada 2 vblanks), então um quadro que passava um pouco de 1/60 s esperava o próximo par: 33,3 ms. Medido no exe original com limite 60: 1.007 quadros de 16,7 ms, 35 de 25 ms e **455 de 33,3 ms** em 1.500 (56,6 fps de média num PC que faz 65 sem limite). Num 3700X (~20–25% mais lento por núcleo, ~50 fps sem limite) isso dá a média de ~50 fps relatada. Agora, em 60 e 120, o vblank fica a 1000 Hz (como sem limite) e o `VdSwap` dorme até o horário do quadro (sleep + espera curta, com ressincronização depois de um atraso maior que um quadro): um quadro atrasado custa só o que levou. O limite 30 (original) continua igual ao console. `CRASH_FRAME_LIMITER=vblank` volta ao jeito antigo.
9. **Tearing permitido** com V-Sync desligado (`DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING` / `DXGI_PRESENT_ALLOW_TEARING`, exceto em tela cheia exclusiva): sem isso, a janela sem foco ficava presa à taxa do monitor (144 Hz aqui) pelo DWM.

### Números (5700X + RTX 5080, vila, 1080p; primeiros 20 s jogáveis, mesmas posições)

| | original | final |
|---|---|---|
| sem limite, nativo | 65,1 fps (p99 17,0 ms) | **124,8 fps** (p99 8,9 ms) |
| sem limite, DLSS Qualidade | 64,1 (`v0_dlss_q`) | 98,4 |
| limite 60 | 56,6 (p99 21,7 ms) | 59,5 (p99 16,8 ms; build intermediário, antes dos itens 7 e 8) |
| limite 120 | — | 118,6 (antes do item 7) |
| trabalho do command processor | 10,9 ms/quadro | 4,2 ms/quadro |

Etapas intermediárias (sem limite): renderer (1–4) + buffer de 32 MB + cache de estado = 96,6 fps; + espera cedendo a CPU = 124,8. Com afinidade em 4 núcleos sem SMT: 101,7 fps (build intermediário; não depende do número de núcleos: são duas threads ocupadas). **Estimativa para o 3700X**: Zen 2 rende ~20–25% menos por núcleo neste tipo de carga (latência de memória, L3 dividido em dois CCX), então ~50–52 fps sem limite no original (bate com o relato) e ~95–100 fps no final; com limite 60, 60 estáveis.

Com DLSS, todas as etapas do command processor ficam ~25–50% mais lentas por igual no build final (o DLSS em si custa 0,15 ms de CPU): não investigado. Não foi mexido no XenonRecomp nem nas flags (o código recompilado da thread do jogo não é o gargalo).

### Ferramentas (`tools/perf/`)

- `sampler.cpp` (compilado em `build\perf\sampler.exe`): amostrador sem admin/ETW. Suspende cada thread que usou CPU desde o último tique, lê a pilha com `StackWalk64` (o `.pdata` do exe cobre também as funções recompiladas) e retoma; pesa cada amostra pelos ciclos da thread (`QueryThreadCycleTime`). Na prática ~60 tiques/s (a pilha é cara), com pouco efeito no FPS.
- `profile.py`: dá nome às amostras com o mapa do linker (`build\CrashRecomp.map`, `/MAP` no `CMakeLists.txt`) e `private\symbols\functions.csv`; perfil por thread (self e inclusivo), threads nomeadas pelo `run.log`.
- `bench.py`: roda `desempenho_vila` com opções (`--settings`, `--fps`, `--env`, `--exe`, `--affinity`, `--sample`) e grava `build\desempenho\<nome>.json`; `walk_window.py`: FPS do trecho jogado; `report.py`: gráficos SVG.
- Diagnóstico: `CRASH_D3D_WAIT_STATS=1` (tempo nas esperas de fence do D3D por endereço de chamada, a cada 10 s) e a linha `[gpu] CP work/frame` (draws, índices, texturas e MB hasheados, uploads, MB de PM4) a cada 300 quadros.

### Benchmark sem relógio: piloto automático dos menus e destravar o FPS na hora certa

O caminho até a vila por botões no relógio de parede se perdia com FPS sem limite, e o replay no relógio do jogo também, porque a cena de abertura lê o controle uma vez por quadro (o relógio do replay corre junto com o FPS). E a cena não pula com nenhum botão (START, A, B, X, Y, BACK testados).

- `CRASH_MENU_AUTOPILOT=newgame` (`settings/menu_autopilot.cpp`): aperta o botão de cada tela assim que ela aparece, pelo nó do `Frontend.bfig` que o jogo avalia a cada quadro (`sub_820F9BA8`, `frontend_hooks.cpp`): 44 logos, 46/59 título (START repetido), 70 menu (A), 96 nome (A, Y), 105 dificuldade (A), 154 slots (A), 165 sobrescrever (DOWN, A), 320 salvo (A). Passos com `+` repetem o botão a cada 1,5 s até a tela mudar (um A durante a animação de entrada do menu é ignorado); um passo cuja tela não apareceu é pulado. O mouse de verdade é ignorado (passar o cursor sobre um item o seleciona: na primeira tentativa o A caiu em CREDITS). Chega à cena de abertura em ~34 s do relógio do controle, sem depender da velocidade da máquina.
- O replay do perfil espera (`WAIT`) a linha `frontend node 756` (começo do carregamento; o 638 pisca antes dele) e depois `frontend node 638` (a jogabilidade: o `pc_input` já sabia que é o nó "game running"), com `CRASH_FRONTEND_TRACE=1`.
- `CRASH_FPS_LIMIT_AT=<s>:<fps>` (`gpu.cpp`): troca o limite quando o relógio do replay passa de `<s>` (não durante um `SYNC`/`WAIT`).
- A caminhada deriva um pouco com o FPS (o jogo move o Crash pelo tempo real), então no fim um build lento olha outra parte da vila: as comparações usam os primeiros 20 s jogáveis, onde as capturas coincidem (diferença média 3/255).

### Segunda rodada (2026-10-22): boot direto, janela fora de foco, texturas vigiadas, opções do recompilador

Medido com o perfil `desempenho_vila_direto` (`boot = "level:L1_E1"` + `skip_cinematics`, HD AI Remaster desligado, sem andar: os 20 s logo depois de o jogador ganhar o controle, `gameplay_ready` + 1 s, o momento em que o FPS mais cai), sem limite, 1080p nativo, 5700X + RTX 5080. Cada execução leva ~45 s.

| passo | FPS | o que mudou |
|---|---|---|
| início da rodada (itens 1–9) | ~94 (amostrado) | |
| SRV único de vértices + perfil embutido 1 quadro em 4 | 109,6 | `CreateShaderResourceView` por draw era ~7% do command processor; ~11 `QueryPerformanceCounter` por draw |
| sem power throttling + prioridades | 123,5 | fora de foco |
| cache do microcódigo + contadores simples no PM4 | 123,3 | |
| **texturas vigiadas por proteção de página** | **168,7** | hash de 24,5 → 0,7 MB por quadro |
| opções do XenonRecomp (LR, MSR, CTR, XER, reservation, CR locais) | **176,2** | código gerado menor (exe 17,9 → 14,4 MB) |

10. **Janela fora de foco** (`main.cpp`, `cpu/guest_thread.cpp`, `gpu/command_processor.cpp`): o usuário viu ~100 fps fora de foco contra ~125 em foco. O Windows 11 aplica *power throttling* (EcoQoS) a processos em segundo plano e ignora o `timeBeginPeriod(1)` deles. Agora o processo sai disso (`SetProcessInformation(ProcessPowerThrottling)` com `EXECUTION_SPEED` e `IGNORE_TIMER_RESOLUTION` desligados), e a thread principal do jogo e a do command processor ficam com `THREAD_PRIORITY_ABOVE_NORMAL`. O log de pacing diz `window focused / not focused`, e o `bench.py` registra quantos trechos estavam em foco. Medido fora de foco: 109,6 → 123,5.
11. **Um SRV só para os vértices** (`VERTEX_DATA_DESCRIPTOR`, o último slot do heap visível): um SRV cru sobre o anel de upload inteiro (256 MB), criado uma vez; o `vbBases` de cada busca de vértice passa a ser o deslocamento no anel. O intervalo enviado cobre todos os vértices que o draw lê (os índices do draw, ou o buffer inteiro quando o índice não vem do vértice), então o resultado é o mesmo.
12. **Microcódigo de shader por endereço** (`PM4_IM_LOAD`): o jogo carrega VS e PS a cada draw (~1,6 cargas por draw) dos mesmos endereços; enquanto os bytes lá são os da última vez (`memcmp`), o shader já achado é usado de novo, sem trocar o endian e sem o FNV-1a (`HashUcode`, que continua sendo a chave dos shaders, do cache em disco e dos patches gráficos).
13. **Contadores do PM4 sem `lock`**: `g_type3Counts`, `g_skippedPredicated` e afins eram incrementos atômicos por pacote (milhões por segundo com o tiling predicado); só o command processor escreve neles, então viraram load + store relaxados. Era metade do tempo próprio do `ExecuteBuffer`.
14. **Texturas vigiadas** (`gpu/texture_watch.*`): uma textura com o mesmo conteúdo em 3 conferências seguidas tem as páginas das duas visões da memória física (0xA0000000 e 0xC0000000) protegidas como só leitura e é hasheada de novo para pegar uma escrita entre o hash e a proteção. Enquanto nenhuma página dela foi escrita (`watch::Clean`), a conferência do quadro pula o hash. Uma escrita (do jogo ou do runtime) dispara uma exceção: o handler (`AddVectoredExceptionHandler`, antes dos outros) desprotege o bloco de 64 KB em volta nas duas visões e marca as páginas sujas; a textura volta a ser hasheada no próximo uso. Escritas do kernel não disparam exceção (falham), mas todas as que caem na memória do jogo já passam por buffer + `memcpy` (`HostTransfer` em `file_imports.cpp` e o caminho do `.rcf`). Uma textura sujada 4 vezes sem mudar de conteúdo (página dividida com algo que o jogo escreve) deixa de ser vigiada. A proteção e o handler mudam proteção e estado juntos sob um `SRWLOCK`. Na vila: ~79 de ~100 texturas por quadro puladas, 0,7 MB hasheados, 0 a 0,3 exceções por quadro (também em LOAD GAME e na pausa). `CRASH_TEXTURE_WATCH=0` desliga. O log `CP work/frame` mostra `unchanged by write watch`, `protected` e `write faults`.
15. **Opções do XenonRecomp** (`config/Crash.toml`; código regenerado; o anterior está em `backups\generated_ppc_2026-10-22_sem_otimizacoes`): `skip_lr`, `skip_msr`, `ctr_as_local`, `xer_as_local`, `reserved_as_local` e `cr_as_local` ligadas. O runtime só lia o LR em logs e o CTR no relatório de chamada indireta inválida: agora usa `GuestLR(ctx)`/`GuestCTR(ctx)` (`cpu/ppc_registers.h`), que dão 0. **Não dá para ligar** `non_argument_as_local` nem `non_volatile_as_local`: os blocos `__finally` do SEH são funções separadas que recebem o quadro do pai em r12 e usam os registradores não voláteis dele (`sub_82268B14`: `addi r31,r12,-320` e `lwz r3,1408(r21)`); com eles locais, o jogo quebra logo na inicialização do CRT.

**Onde está o gargalo agora**: na vila, a thread do command processor fica ociosa a maior parte do tempo, e quem limita é a **thread principal do jogo** (código recompilado): ~20% dela ainda é a espera do D3D, e o resto é o próprio jogo, com destaque para um laço de transformação de vértices na CPU (`sub_82353478`, ~18%: matriz × vértice com `fmadds`, o skinning dos personagens) e o `memcpy` do jogo (`sub_823CEC90`, ~6%; trocá-lo por um `memcpy` nativo não mudou o FPS medido, então ficou como estava). Os ganhos daqui para a frente vêm de acelerar o código recompilado (veja a lista abaixo).

**O que falta, por ordem de ganho esperado**:
1. Código recompilado da thread do jogo: `non_volatile_as_local`/`non_argument_as_local` com um ajuste no XenonRecomp para os funclets do SEH (passar r12 e salvar/restaurar os não voláteis do pai no `PPCContext` só em volta das chamadas a funclets), e a mesma investigação para o laço de skinning (`sub_82353478`): é FP escalar com conversão `float`↔`double` a cada operação.
2. LTO/ThinLTO e PGO no `CrashPPC` + runtime (não testados; o link fica bem mais lento).
3. Com DLSS, as etapas do command processor ficaram ~25–50% mais lentas por igual na primeira rodada: não investigado.
4. A armadilha de MMIO do `CP_RB_WPTR` (duas exceções e dois `VirtualProtect` por kick) é pouca coisa com segmentos de 192 KB; segmentos menores pioraram (64 KB: 95 fps, 32 KB: 93, contra 110 no mesmo build). `CRASH_D3D_SEGMENT_KB` existe para testar.

## Boot direto, intros puladas e eventos para testes (`settings/direct_boot.*`, `settings/frontend_hooks.cpp`, `kernel/input_record.cpp`, `kernel/xam_input_imports.cpp`, `tools/run_game.py`, `tools/frontend_tree.py`, `tools/convert_recording.py`)

Pedido: testes que não dependam de apertar botões em horários fixos (que se perdiam quando o carregamento demorava, com outras instâncias abertas ou quando o menu mudou), entrando direto na fase ou no save pelo código do próprio jogo, e um modo que pula todas as intros. Uso: README, "Boot direto e eventos".

### A árvore do frontend

O frontend inteiro é uma fight tree (`fighttrees\Frontend.bfig`): telas (nós) e saídas numeradas juntas, na ordem do arquivo, a partir de 1 (a própria árvore). Registros: `[u32 tipo][u32 tamanho do nome][nome com NULs até 4][u32 pai][u32 filhos]...`, tipos `0xE056B923` árvore, `0x83A93633` nó, `0xE8CB721B` condição, `0xBC27359D` saída (destino em +0x18 depois do pai); depois textos com tamanho na frente (os snippets Lua que a saída roda e os nomes de páginas e texturas). `tools/frontend_tree.py` imprime tudo (1220 registros; `--node 575`, `--find StartLevel`). O `sub_820F9BA8(árvore, nó, ...)` (as condições, nativas) devolve o número da saída: o mesmo número.

O caminho que interessa:

| Nó | Tela | Saída usada |
|---|---|---|
| 3 | BootUp | (condições `SkipToLevel` / `SkipToTitleScreen` dos desenvolvedores, nunca verdadeiras no disco) |
| 44 | LicenseScreen (texto legal) | 45 `Exit` quando o tempo no nó passa de 5,0 s (`tree+172 → +28 → f132 > 5.0f`) |
| 46 | LoadPersistentPostBootUp (`GotoGameState(LOAD_PERSISTENT_POST_BOOTUP)`) | quando termina: `InGameState(5) ? 51 SkipToLevel → 630 InGame : 52 ExitTitleScreen → 59` (a árvore aceita a 51 devolvida por outro nó, ex.: o 70) |
| 59 | TitleScreen | 65 `ExitAccept`: `IsButtonPressed(FrontendManager, 29 START ou 1 ACCEPT, consumir = 1, controle -1)` → 70 |
| 70 | MainMenuScreen | 88–91 NEW GAME / LOAD GAME / CREDITS / SETTINGS (Xbox), 86 SAIR DO JOGO |
| 111 → 123 → 125 → 148 | AccessMemoryCard, seletor de dispositivo, leitura | o jogo decide sozinho (~3 s) |
| 154 | GameSlotScreen | 180 + 20 × (slot − 1): `ExitLoad > ExitSlotNotEmpty`, snippet `SaveGameManager:LoadGame(slot − 1)` → 305 |
| 305 → 308 | LoadGameScreen → LoadComplete | 309 `ExitToHUB` (`SetHUBEpisodeMenuIndexToLastLevel`) → 575 |
| 575 | HUBScreen (menu do episódio) | 604 `ExitContinueStory` (Y, `IsButtonPressed(.., 13, ...)`; snippet `StartNextLevel`), 600 `ExitToLevel` (`StartLevel(GetSelectedItemUnlockableIndex([[Level_Timeline]]))`) |
| 630 | InGame | (aparece só depois do carregamento; o frontend não roda enquanto a fase carrega) → 633 LevelIntroCinematic ou 638 GameRunning |
| 638 | GameRunning | 648 cenas (655 e filhos 656–1051), 651 pausa (1081), 652 game over (1192) |

### As funções do jogo

- O objeto `Frontend` expõe ao Lua (registro em `sub_82103A88`, método nativo = o ponteiro passado ao construtor `sub_820F8xxx` antes de cada nome): `InGameState` `sub_820F5CE0`, `GotoGameState` `sub_820F5D20`, **`StartLevel(índice)` `sub_820F5D58`**, `StartLevelByName(nome)` `sub_820F5D98` (procura o nome e chama `StartLevel`), `StartNextLevel` `sub_820F5DE8`, `GetSelectedItemUnlockableIndex` `sub_820F62E8`, `GetMenuIndex` `sub_820F9A10` e mais 25.
- O objeto dos managers (`sub_821FB4F0` devolve `0x824A7A24`, o mesmo de `game_options.cpp`): estado atual +12, próximo estado +20 (`GotoGameState` = `stw r4,20(r3)`, `sub_821F9EC0`), índice da fase +24 (`sub_821F9EC8`, que antes prepara a troca: `sub_8224CC08`/`sub_8224CF00`), lista de fases em +156 (ponteiros) com a quantidade em +316, nome da fase em fase+32. `StartLevel(i)` = índice i e estado 5 (fase). `sub_821F9FA0(managers, nome)` acha o índice pelo nome (`strcmp`).
- 21 fases: 0 L1_E1, 1 L1_E2, 2 L1_E3, 3 L1_E4, 4 L2_E1, 5 L2_E2, 6 L2_E4, 7 L3_E1, 8 L3_E1A, 9 L3_E1B, 10 L3_E2, 11 L3_E3, 12 L3_E4, 13 L4_E1, 14 L4_E2, 15 L4_E3, 16 L4_E4, 17 L5_E1, 18 L5_E2, 19 L5_E3, 20 SoundTest (o runtime loga a lista: `[test] levels (21): ...`). O índice + 1 é o número do episódio que o jogo mostra (L1_E2 = "EPISODE 2").
- Os vídeos de logo: `CMoviePlayerBehaviour::PlayMovie(índice)` (`sub_8214ABD0`) toca `d:\movies\<nome>` da tabela em `0x8245F224` (0 dolby, 1 radical, 2 sierra, 3 shadow, 4 attract, por idioma em `0x8245F238..44`, todos a mesma) e o callback de fim (`sub_8214B210`, estado em +12) toca o seguinte; depois do 3 acaba. O da Sierra não existe nos discos: o player falha ao abrir e o callback segue na hora.
- `TestLevels.lua` (`script\`) é um script de teste dos desenvolvedores que também navega o frontend por botões (`TapControllerButton`, `MoveDpadDown`, `WaitForFrontEnd`, `LoadLevel` com setas + START + X): não há um seletor de fases escondido para chamar.

### O que o runtime faz

- `CRASH_SKIP_INTROS=1` / `--skip-intros`: `PlayMovie` pede o vídeo da Sierra no lugar dos logos 0, 1 e 3 (o attract continua); no nó 44 toma a saída 45 sem os 5 s; no título aperta START de verdade (`boot::Buttons()`, pelo `GameButtons` da tela de configurações): **forçar a saída 65 não serve**, porque o controle que aperta START vira o jogador, e sem ele o LOAD GAME cai em 114 NoProfile. Um reinício pela tela SETTINGS (`--restarted`) usa o mesmo caminho (o antigo `IntroSkipButtons`, START a cada 0,5 s até o menu, saiu).
- **O jogador 1 precisa de tempo nos menus.** Começar a fase saindo de um menu 3 quadros depois de ele aparecer (saída forçada) deixa a fase sem jogador 1: a HUD só tem a barra da esquerda e "JOIN GAME", o primeiro START entra como jogador 2 (duas barras, marcador "1 2") e a pausa (`IsButtonPressed(.., 29, 0, -1)`, saída 651) nunca abre. Medido: forçar a 604 no HUB com 3 avaliações quebra, com 60 funciona; o Y de verdade (~1 s depois de o HUB aparecer) funciona; o `level:` saindo do nó 46 (sem nenhum menu) quebra, saindo do menu principal depois de 60 ticks funciona. Não achei onde o jogo guarda isso (`FrontendManager+16588` e `+16608`, os controles do `IsButtonPressed`, ficam −1 nos dois casos; o diff de memória do START que vira jogador 2 é ruidoso demais).
- `CRASH_BOOT=level:<nome>`: título (START de verdade) → menu principal → depois de 60 ticks (`MENU_SETTLE_TICKS`) chama `StartLevel(índice)` (`sub_820F5D58` com um `PPCContext` copiado) e devolve 51 `SkipToLevel` (do nó 46: a árvore pula para o destino da saída devolvida, de qualquer nó). Sem save: dificuldade e progresso padrão, sem salvamento automático (nenhum slot escolhido).
- `CRASH_BOOT=load:<slot>[:continue|hub|<fase>]`: as saídas 89, 180 + 20 × (slot − 1) e 309; no HUB aperta Y de verdade (ou para, com `hub`). Com uma fase, troca o índice que o `StartNextLevel` do Y passa ao `sub_821F9EC8`. Os snippets das saídas rodam como num clique: `LoadGame`, `EndAccess`, `SetHUBEpisodeMenuIndexToLastLevel`, `StartNextLevel`.
- Uma saída só é forçada quando o jogo não escolheu nenhuma (−1), o nó está na tela há 3 avaliações e só uma vez por boot; só na avaliação de fora (`t_conditionDepth == 1`, o hook chama `sub_820F9BA8` recursivamente para os sub-nós). Se a árvore não for a conhecida (`settings::QuitExit() != 86`, achado pelo nome em `pc_frontend.cpp`), o boot desliga.
- Eventos (`boot::Event`): `[test] event <nome> (frame F, input clock S s, tick T)[: detalhe]` em `title` (59), `main_menu` (70), `save_slots` (154/421), `load_complete` (308), `hub` (575), `level_start` (`sub_821F9EC8`, qualquer jeito de começar uma fase, com o nome), `level_ready` (saiu do 630), `cinematic` (633 ou 655–1051), `gameplay_ready` (638 por 45 ticks seguidos: ele aparece por ~0,5 s antes da cena de abertura do L1_E3), `pause` (1081), `game_over` (1192), `boot_done`, `boot_failed` (151/313 ou fase desconhecida).
- `CRASH_INPUT_SCRIPT` aceita `evento[#n]+ms:BOTÃO` e `SHOT`; `CRASH_SCREENSHOT_TIMES` aceita `evento[#n]+s`; `boot::Resolve` devolve o relógio do input do evento (−1 antes dele).
- Replay: `tick EVENT nome` segura a linha do tempo até a linha do evento e então faz `shift = tick do evento − tick gravado` (alinha nos dois sentidos, como o `SYNC`; o `WAIT` só atrasa). A gravação guarda `main_menu`, `hub`, `level_ready` e `gameplay_ready` como `[t, "EVENT", ...]` (só os seguidos de alguma entrada, como os `SYNC`) e escreve `boot`/`skip_intros` da execução gravada.

### Medido (`build\testes_auto\`)

Rodada 1 (`build\testes_auto\rodada1\`, cada perfil sozinho): os 17 perfis migrados passam (o `configuracoes_pausa` falhou na primeira versão, com a saída 604 forçada; passou depois do Y de verdade no HUB). Rodada 2 (`build\testes_auto\rodada2\`, duas filas ao mesmo tempo, sempre outra instância aberta): `menu`, `menu_loaded`, `load_pelos_menus` e `primeiro_slot_load` passam com os eventos nos mesmos tempos (HUB 14,4–14,6 s, `gameplay_ready` 18,8–19,0 s no `primeiro_slot_load`); a rodada foi interrompida aí a pedido (as pastas com 1–8 s são das execuções mortas na interrupção).

| Marco | Antes (botões no relógio) | Agora |
|---|---|---|
| Menu principal | ~14 s (START a cada 0,5 s), scripts esperando até 15–17 s | 6,0 s (4,8 s sem o START real no título) |
| Fase do slot 1 jogável (`primeiro_slot_load`) | ~34 s (Y aos 30,5 s) | 19 s |
| Fase sem save (`level:`) | — | 10 s (+ a cena da fase) |
| Soma dos timeouts dos perfis que já existiam | 1159 s | 815 s (−30%); `primeiro_slot_load` 150 → 40 s, `teclado` 95 → 65 s, `configuracoes_jogo` 82 → 50 s |

Pular a cena de abertura do L1_E1 (nó 756 `R1_NIS01`, ~100 s) forçando a saída 758 `ExitWhenDone`: a árvore vai para 638, mas a tela fica em LOADING para sempre (60 s testados): a vila só é liberada quando a cena termina por dentro. Não serve; resolvido pelo fim da NIS no próprio sequenciador (abaixo).

### Pular as cenas (`settings/skip_cinematics.cpp`)

`CRASH_SKIP_CINEMATICS=1` / `--skip-cinematics` / `skip_cinematics = true` no `run.toml`. Uma cena (NIS) é a ação de uma `MasterCinematicTrack` da fight tree (a do L1_E1 está no próprio `Frontend.bfig`, nó 756, com as trilhas de atores, câmera `cam_NIS01` e som): um `CMasterCinematicAction`, com uma máquina de estados em `+412` que só muda por `SetState` (`sub_821B92E8(ação, estado)`, sem nome no RTTI) e roda no update (`vf05`, `sub_821BA100`; jump table de 8 casos nos dois):

| Estado | O que faz |
|---|---|
| 0 | prepara e escolhe 1 ou 2 |
| 1 | fade de entrada (`preCinematicFade`) |
| 2 | começa: câmera e sons carregados (espera a lista `+136` esvaziar) → 3 |
| 3 | tocando (`sub_821B9FF0`): a ação da câmera (`+340`) e a sequência de som (`+336`) andam; o tempo, em quadros a 30/s, fica em `[ação+12]+124` e **segue a posição do stream de som** enquanto ele toca (`sub_821B9E50`), então acelerar o relógio do jogo não adiantaria. Vai para 4 quando câmera e som acabam, quando não há trilhas, ou quando o bit `0x80` de `managers+352` liga (é o `HasXenonProfileChanged`/`sub_821D9190`, o perfil trocado, não um "pular") |
| 4 | fim: para o som (`sub_82300D38`), escolhe 5 ou 6 |
| 5 | fade de saída (`postCinematicFade`) → 6 |
| 6 | trilhas de depois (`+236`), jogador mostrado de volta → 7 |
| 7 | concluída: `ação+418 |= 0x80` e `[managers 12]+452 |= 0x80`, o que a condição `ExitWhenDone` do nó da cena espera |

O jogo não tem como pular (não há parâmetro "skippable" na trilha; os nomes dos parâmetros estão em `0x8201AACC..`: `pre/postCinematicFade`, `player1ShowLocatorName`, `disabledPlayerControl`...). O runtime chama `SetState(4)` na primeira atualização em estado 3 (`CRASH_SKIP_CINEMATICS_AFTER=n` espera n atualizações): o mesmo caminho do fim natural, então o som para, o fade e as trilhas de depois rodam, a cena fica concluída e o frontend sai do nó sozinho. O log ganha `[cinematic] 0x... state a -> b (frame F, tick T)` em toda troca (com ou sem a opção) e os eventos `cinematic_skipped` e `cinematic_done` (este também no fim natural).

Medido (`build\runs\20261022-0120*` a `-0124*`, uma instância por vez):

| | Sem a opção | Com a opção |
|---|---|---|
| L1_E1 (`level:L1_E1`): cena | 3 → 4 no quadro 3103 (103 s), estados 4 → 5 → 6 → 7 | 3 → 4 no quadro 0, os mesmos estados |
| L1_E1: `gameplay_ready` | 117,7 s | **13,9 s** (perfil `vila_sem_cena`, a pausa abre como P1) |
| L1_E3 (`fase_direta`): `gameplay_ready` | 18–22 s (`build\testes_auto\rodada1`) | 12,4 s (4 → 6, sem fade de saída) |

A vila depois do pulo fica igual à do fim natural (Crash no mesmo lugar, Crunch no bloco de gelo, mesma HUD e dica); só o título "EPISODE 1 / A NEW HOP" aparece por alguns segundos já com o controle, porque no fim natural ele passa durante a cena.

#### O botão do jogador (`settings/skip_cinematics.*`, 2026-10-22)

Pedido: pular qualquer cena (começo ou meio do jogo) por um botão, com um aviso pequeno no canto. Uso: guia, "Pular cenas e vídeos".

- **Quando há uma cena**: o update (`vf05`) roda em estado 3 **e** o frontend está num nó de cena (633 ou 655–1051, os mesmos do evento `cinematic`). A segunda condição é necessária: o L5_E1 roda `CMasterCinematicAction`s durante o jogo (frontend em 638, uma depois da outra, 157–257 quadros cada, até o fim da fase); sem ela o aviso ficava na tela jogando e o B segurado "pulava" essas sequências. O `CRASH_SKIP_CINEMATICS` dos testes segue a mesma regra.
- **Vídeos**: `PlayMovie` (`sub_8214ABD0`, hook do `direct_boot.cpp`, que chama `cutscenes::NoteMovie`) começa um, o callback `CMoviePlayerBehaviour::CMovieCallback::vf00` (`sub_8214B210`) roda quando ele acaba e começa o próximo; por segurança, um vídeo também acaba quando o nó do frontend muda. O pulo aperta START por 4 leituras (o pulo do próprio jogo). Sierra (2, não existe no disco) e attract (4, o que passa no título parado; continua saindo com qualquer botão) não têm aviso. Cada vídeo e cada cena tem o seu aviso (um contador por vídeo; a ação por cena), senão o logo da Radical, que começa logo depois do da Dolby, herdava o aviso já apagado.
- **Entrada** (`cutscenes::FilterGameInput`, chamado em `ReadController` logo depois do `save_screens`): segurar B, START ou A (o teclado chega como esses botões: Esc = START em jogo e B nos menus, Espaço = A), Enter (`GetAsyncKeyState`, só com a janela em primeiro plano) ou o botão esquerdo com o cursor no aviso por 0,8 s. Durante a cena/vídeo esses três botões não chegam ao jogo; depois do pulo continuam escondidos até serem soltos. Primeira versão errada: a checagem "solto?" olhava os botões já escondidos e liberava na mesma leitura; com Esc (START em 638) a pausa abria ao soltar (`20261022-142238_pular_cena_teclado`, achado com `CRASH_SKIP_TRACE=1`).
- **Desenho** (`cutscenes::Draw`, no presenter logo depois das legendas, canal `BITMAP_SKIP_PROMPT` do overlay): placa escura arredondada no canto de baixo à direita (dentro da faixa preta das cenas), ícone `menu_art::DrawIcon(ICON_B)` (que segue o aparelho: B do jogo, ○ desenhado, ou no teclado uma tecla ESC desenhada aqui) com um anel que enche, e o texto na Titans_Large. Some depois de 4 s sem entrada (fade de 0,5 s) e volta com qualquer botão, tecla ou movimento do mouse.
- **Legendas**: `subtitles::EndCutscenes()` apaga na hora as trilhas das cenas (chaves sem `/`); antes elas ficavam até o stream parar de decodificar (3,5 s).
- **Opção**: `[game] skip_cutscenes` (padrão ligado), SETTINGS > JOGO > PULAR CENAS.

Medido (`build\runs\20261022-14*`, `build\pular_cena\` com as capturas):

| Perfil | Cena | Pulo | Jogável |
|---|---|---|---|
| `pular_cena` (Xbox, EN) | L1_E1 NIS01 (nó 756), aviso aos 10,6 s, apagado aos 5 s, B segurado | 18,4 s (quadro 234 de 3103) | 20,5 s, pausa ok |
| `pular_cena_teclado` (PT-BR) | L1_E1, Esc segurado | 13,7 s | 15,8 s, Esc pausa depois |
| `pular_cena_ps` (EN) | L1_E3 (abertura), ○ | 13,2 s | 14,8 s |
| `pular_cena_fim` | L5_E1 (Doominator, nó 1017) | 12,6 s | 14,7 s; nenhum aviso depois, B segurado na fase não pula nada |
| `pular_videos` | Dolby, Radical, teatro de sombras | 11,0 / 13,8 / 16,6 s | título aos 17,3 s (24,5 s sem pular a Radical) |
| `pular_cena` com `skip_cutscenes=false` | L1_E1 | nenhum aviso, a cena segue | — |

O L4_E4 (episódio de chefe) não tem cena no começo da fase (a NIS11 vem depois), e as cenas de chefe/final (`L1_E4_R8`, `L3_E4_R4`, `L5_E3_R4`) só aparecem jogando até elas: não testadas, mas usam a mesma ação.

## Teclado do nome e apagar saves (`settings/save_screens.*`, `kernel/xam_content.h`, `kernel/pc_input.*`)

Pedido: trocar a roda de letras do NEW GAME por um teclado QWERTY no estilo do jogo (controle, teclado físico e mouse) e poder apagar saves na tela do LOAD GAME, com confirmação. Uso: README, "Nome e saves no PC". Código novo: `settings/save_screens.{h,cpp}`, `kernel/xam_content.h` (+ duas funções no fim de `xam_content_imports.cpp`); pontos de contato: `settings/frontend_hooks.cpp` (nó e saída), `kernel/xam_input_imports.cpp` (`FilterGameInput` em `ReadController`), `kernel/pc_input.*` (modo de digitação), `settings/prompt_icons.cpp` (ícones de A e Y ao digitar), `settings/menu_mouse.cpp` (sem hover com a pergunta aberta), `gpu/overlay.h` (canal `BITMAP_SAVE_SCREENS`), `gpu/d3d12_renderer.cpp` (uma chamada de `Draw`), `config/run.toml` (5 perfis).

**A tela do nome no código do jogo.** `CNameEntryScreenAction` (nó 96 `NameEntryScreen`): vf04 `sub_820B4A88` (entra), vf05 `sub_820B45A0` (todo quadro), vf06 `sub_820B43C0` (sai). `this+24` = o layer da página `GameStart_NameEntry.pag`, `+28` = o texto do nome, `+32`/`+36` = as setas dos lados da roda (piscam), `+44` = a letra da roda (0–25 = A–Z, 26–35 = 0–9). O nome é global: `char[32]` em `0x82497328`, o tamanho em `0x82497348`, **no máximo 5**. vf05 pergunta ao input pelos eventos 23/33 (gira), 1 (A: som `FE_SFX_NameScreen`/`letter_select` e `sub_820B44C0`, que acrescenta a letra de `+44` com `"%c"`/`"%i"` e atualiza o texto com `sub_820B42E8`), 18 (X: `erase` e `sub_820B4568`) e 13 (Y: com nome, `done` e o bit `0x80` do byte 16653 do tocador de sons do frontend, `sub_821FA440(sub_821FB4F0(), 6)`; a condição do nó 96 em `sub_820F9BA8` toma a saída 97 `ExitDone` quando vê esse bit; sem nome, `FE_SFX`/`backpackNONONO`). A roda são os elementos `Char_a`…`Char_z`, `Char_0`…`Char_9`, postos num círculo a cada quadro, com os sprites `Divider_TopLeft/_TopRight/_BottomLeft/_BottomRight` em volta do centro. Só A–Z e 0–9: a fonte Titans tem acentos, mas o nome não passa por outro caminho, então o teclado não tem espaço, minúsculas, Shift nem acentos.

**Por que desenhar por cima e não refazer a página Scrooby.** O teclado precisa de 38 teclas com destaque, hover e clique; uma página Scrooby nova exigiria criar ~80 elementos (texto + sprite por tecla) num `.p3d` reescrito e ainda assim a lógica seria a do `CNameEntryScreenAction` (uma roda: só "anterior/próximo" e "acrescentar a letra atual"). Desenhando no presenter, como a tela SETTINGS (`menu_art`: as fontes Titans, as cores medidas do frontend, os ícones por aparelho), a página do jogo continua a mesma (pano, título, nome digitado e as dicas X APAGAR / B VOLTAR / A SELECIONAR / Y PRONTO dele), só a roda some, e cada tecla chama o código da roda. Mesma escolha para a pergunta de apagar: a pergunta de sobrescrever do jogo é um nó da árvore (165) com página própria; uma pergunta nova na árvore exigiria nó e saídas novas no `Frontend.bfig`.

**O que o runtime faz na tela do nome.** O vf05 roda normalmente, com o controle escondido dele (só o B passa: é o `ExitBack` 98 da árvore); depois dele, as 36 letras, as duas setas e os quatro divisores ganham alfa 0 (`sub_822EEA18`, o alfa do `Drawable`), procurados uma vez por tela: as letras são textos do layer (`sub_822F1DA8(layer, "Char_a")`, que recebe o nome), os divisores são sprites da página (`this+20`; `sub_822F1DA0(página, hash)` recebe o **hash** do nome, `sub_822E4A90(nome)`, como o vf04 acha as setas; passando o nome, nada é achado). Cada tecla: `+44` = a letra, o som `letter_select`, `sub_820B44C0` (o caminho do A da roda); APAGAR = `erase` + `sub_820B4568`; PRONTO = o caminho do Y. A leitura do controle é a que o jogo faria (`FilterGameInput`, chamado em `ReadController` antes de zerar: botões + analógico como D-pad, bordas acumuladas nas leituras do jogo), com repetição ao segurar (380 ms, depois a cada 100 ms). Teclado físico: `input::pc::SetTextEntry(true)` enquanto o teclado está na tela; aí as teclas A–Z / 0–9 / numérico entram numa fila de texto (`TakeTypedText`, também as `KEY_*` dos testes) e não viram mais W/A/S/D = D-pad, Q/E = LB/RB, P = START, e Enter vira PRONTO em vez de A; setas, Espaço (A), Esc (B), Backspace/Delete (X) e Tab (Y) continuam. Os ícones de teclado das dicas do jogo acompanham (`prompt_icons.cpp`: A = ESPAÇO e Y = ENTER enquanto se digita). O clique numa tela sem menu (que o `pc_input` transforma em A) não vale com o teclado aberto: o próprio teclado usa o clique. Ao sair (vf06, outro nó ou 400 ms sem vf05), o jogo continua sem ver o controle até todos os botões soltarem, para o Enter/A do PRONTO não escolher a dificuldade.

**Apagar.** `CGameSlotScreenAction::vf05` (`sub_820AEE90`), só no nó 154 (o `GameSlotScreen` do menu: LOAD GAME e NEW GAME; o 421, de dentro do jogo, não): `this+24` = o menu de slots, `+152` = o item (slot − 1); slot N = pacote `CrashOTT GameSlot N`. O X do slot (Delete/Backspace no teclado, clique no prompt) só abre a pergunta se o pacote existe (`xam::content::SavedGameExists`, que também lê o nome de exibição do `.header`: "Slot: 2  Name: AA  Last Save: 10.19.26  Time: 00:00"). Com a pergunta aberta o jogo não vê o controle e o `menu_mouse` não mexe na seleção. SIM chama a função de apagar do próprio jogo, `core::XenonSaveDrive::vf19` (`sub_82296710`, r3 = o drive, r4 = o nome do arquivo): ela enumera os pacotes e chama `XamContentDelete` com o `XCONTENT_DATA` daquele (o runtime apaga a pasta, o `.header` e a miniatura). O jogo nunca a chama de um menu (no Xbox os saves eram apagados no painel). O drive é o `this` de `sub_822959D8` (a enumeração que a tela de slots roda todo quadro); se não houver, ou o pacote continuar lá, `xam::content::DeleteSavedGame` apaga direto. Achado: **apagar o pacote não atualiza a lista** (a tela lê os slots ao entrar: nós 111 `AccessMemoryCard` → 148 `ReadingCard` → 154), mesmo com a enumeração de todo quadro vendo 4 pacotes. Por isso, no quadro seguinte a condição do nó 154 toma a saída 314 `ExitToGameSlotScreen` (a volta do `LoadFailed`, sem trecho de Lua; a árvore aceita tomá-la do 154): o jogo lê os slots de novo, volta ao 154 (`save_slots#2`) e o runtime põe a seleção no mesmo slot (`Menu::SetSelection`, `sub_822F2E40`). Sem nenhum save no LOAD GAME, o `ReadingCard` vai sozinho ao `NoValidSaveFiles` (mensagem e menu principal). Só vale com a numeração conhecida do `Frontend.bfig` (`QuitExit() == 86`, como o boot direto).

**Sons** (strings do executável, conferidas na primeira chamada; sem elas tudo fica como no jogo): `FE_SFX_NameScreen` `0x820075BC` com `letter_select` `0x8200759C`, `letter_scroll` `0x820075AC` (andar no teclado), `erase` `0x82007594` (apagar letra e apagar save), `done` `0x8200758C`; `FE_SFX` `0x82006DA4` com `backpackNONONO` `0x8200757C`, `back` `0x82006DAC`, `select` `0x82006DB4`; mover na pergunta = `sub_821DB260`/`sub_821DB280` (os do menu); volume = o float em `0x82000DC8`, o mesmo da roda. Tocados com `sub_821DB1D8(tocador, banco, som, f1)`.

**Desenho.** CPU no tamanho da janela, como a tela SETTINGS, no canal `BITMAP_SAVE_SCREENS` do `overlay::DrawBitmap`, só quando algo muda (tecla, tamanho do nome, idioma, ícones, janela); 160 ms de fade ao aparecer. Teclas de 50x46 a cada 56x52 no espaço 1280x720 da imagem do jogo, primeira linha em y 296 (entre o nome e as dicas), placas arredondadas marrons translúcidas; a destacada em creme com contorno verde-água e a letra no ciano do item selecionado do jogo, maior. O prompt X APAGAR fica entre as dicas do jogo (y 620, mesma altura de letra); a pergunta usa o `MSG_Background` nos quatro cantos e as cores do menu de pausa, com a altura ajustada às linhas. Logs: `[saves] name: 'C' -> "C"`, `name: DONE -> "CRASH"`, `delete slot 2 (...): asking`, `cancelled (B)` / `(NO)`, `XenonSaveDrive::vf19(...) -> deleted`, `slot screen: reading the slots again (exit 314)`, `selection back on slot 2`. Eventos de teste novos: `name_entry`, `save_deleted`.

**Teste** (exe linkado à parte, `build\CrashRecomp_nome_saves.exe`, porque o `build\CrashRecomp.exe` estava aberto por outra sessão): os 5 perfis em inglês e em PT-BR (`--set 'env={ CRASH_SETTINGS = "game.language=pt-br" }'`); os três de nome encadeados com `userdata_from` (cada um grava um slot: 3 AQ1Z, 4 CRASH, 5 MOUSE) e um LOAD GAME no fim mostra os três nomes; `apagar_save` termina carregando o slot 1. Capturas e a lista de saves de cada etapa em `build\nome_saves\`.

## Iluminação (remaster): SSAO, luz indireta, sombras de contato e sombras suaves (`gpu/lighting.*`, `gpu/graphics_options.cpp`, `gpu/d3d12_renderer.cpp`, `gpu/overlay.*`, `gpu/window.*`, `settings/*`)

Pedido: um remaster da iluminação (sombras dinâmicas, melhor iluminação, SSAO, SSGI), opcional e sem mudar a cara do jogo. Tudo é pós-processamento da cena 3D; os dados e a jogabilidade não mudam. Uso e números no README, "Iluminação (remaster)"; capturas em `build\iluminacao\` (`execucoes.txt` diz a pasta de cada execução) e a seleção em `docs\capturas\iluminacao\`.

### Onde entra

`RunTemporal` (passo 20) virou o "passo da cena": antes do primeiro desenho 2D que lê o resolve de cor da cena (o HUD, o motion blur e o DOF vêm depois), agora roda também **sem upscaler** sempre que alguma opção de `[lighting]` está ligada (`ScenePassActive()`: `NoteSceneResolve`, `NoteSceneCamera` e `UpscaleBeforeReading` deixaram de depender só de `g_temporal.active`). Ordem: motion vectors (o mesmo `MOTION_PS`) → `lighting::Apply` na superfície da escala da cena (escreve pela RTV dela) → upscaler, se houver. Sem upscaler o caminho termina depois da iluminação (atualiza `lastEvaluated` e a VP anterior, que os motion vectors do quadro seguinte usam). A iluminação só aceita câmera em perspectiva: nos primeiros quadros (logos, carregamento) a VP "votada" é ortográfica (P32 = 0) e o passe não roda (nem cria alvos).

### `gpu/lighting.*`

Root signature, PSOs, alvos e heap de descriptors próprios (como `smaa.*`): 32 constantes em `b0`, tabela de 8 SRVs, samplers estáticos. Um único HLSL com um ponto de entrada por passe (`-D PREP/GATHER/TEMPORAL/BLUR/COMPOSITE`), compilado pelo `CompileHlsl` do renderer (cache em disco):

1. **prep** (resolução do AO): profundidade da janela → z de vista (`ViewZ`: céu/clear e além de 90% do far = 0) e normal pela profundidade dos vizinhos do lado mais plano. Frestas de 1–2 pixels mais fundas que os dois lados (as emendas do terreno da vila: viravam riscos pretos no AO e verdes na luz indireta) recebem a média dos lados (`HealedZ`).
2. **gather**: bitmask de visibilidade (Therrien et al. 2023, "Screen Space Indirect Lighting with Visibility Bitmask"): por fatia, ângulo do horizonte da frente e de trás (espessura 0,5) de cada amostra → setores de 32 bits; AO = setores livres; luz indireta = cor (amostrada na cena) × setores escondidos pela primeira vez × cossenos. Amostras abaixo de 0,07 + 0,004·z da superfície não contam (degraus, decalques). Sombra de contato: 16 passos ao longo de +y do mundo (em vista) até 1,2 unidade, na profundidade cheia.
3. **temporal**: reprojeção pelos motion vectors, rejeição por profundidade (4%), mistura 0,12; o ruído (interleaved gradient noise) gira a cada quadro.
4. **blur**: bilateral 9 taps horizontal + vertical (profundidade e normal).
5. **composite** (resolução da cena): upsampling bilateral de meia resolução, `cor × max(AO × sombra, 0,5) + cor × luz indireta × intensidade`; pixels muito claros perdem até 60% do AO.

Câmera: a vista sai da VP por Gram-Schmidt (linha w = eixo z, linhas x e y sem a parte z), `P = VP · V⁻¹`; na vila P00 1,19, P11 2,12, P22 1,0005, P23 −0,5 (near 0,5), P32 1, P33 0, termos fora da forma padrão < 1e-8. Resolução: BAIXA = meia; ALTA = cheia até 1440 linhas de cena, meia acima (em 4K a cheia custava 3,8 ms). Tempo de GPU por timestamps (4 slots, lidos 4 quadros depois) no `LogStats`: `[lighting] N frames, X ms of GPU per frame`.

**F8** (`window::ConsumeLightingToggles`, `lighting::ToggleBypass`): desliga/religa tudo, inclusive as sombras suaves, sem gravar; `overlay::Notice` mostra o aviso 2,5 s em qualquer modo do overlay (também desligado). `CRASH_LIGHTING_SHOW=ao|gi|shadow|split`, `CRASH_LIGHTING_TUNE=...`, `CRASH_LIGHTING_NO_TEMPORAL=1`.

### Sombras suaves (`graphics_options.cpp`, `ps fdf408078ea760bb`)

O patch da suavidade agora também insere `if (xe_jitter.z > 0.5) { xe_soft_shadow(); return; }` no início do `xe_main`. A forma do shader original: 9 fetches do stencil (z depois do swizzle do fetch > 0) em uv + c2..c10, cor = 1 − Σ(sombreados) (1 − r1)·c30..c38. A versão suave usa a mesma fórmula com a fração sombreada de um disco girado (por pixel, fixo no tempo) de 16 amostras 1,35× mais largo, cada uma "sombreada se algum dos 4 texels do Gather for" (como o fetch filtrado do original; com PCF bilinear a sombra do Crash ficava mais fraca). `xe_jitter.z` vem de `g_effects.jitter[2]` (o translator não mudou, então os outros shaders não recompilam).

### O que se descobriu sobre as luzes e as sombras do jogo

- Os shaders iluminados (`simpleshaderlitfp` 79ab15b821efc5f7, `palsimpleshaderlitfp` b5fcdc5e1a123bc2, `furshaderlitfp` 6f7b17ecf6aca653, `palfurshaderlitfp` a31efa3e1c035104) têm `LightDirection0..3` em c9/c11/c13/c15 (mundo, a normal vem de `matRot`) e cores em c10..c16, mas os bools `LightEnabled0..3` (PS b0–b3 = bits 0–3 da dword 4 dos bools; FogEnabled b69 = bit 5 da dword 6) vêm **zerados** na maioria dos desenhos da vila: quase tudo é ambiente + emissivo + luzes de proximidade.
- As sombras de stencil são malhas **extrudadas na CPU**: os volumes são desenhados com o vs genérico `a74e7d48f78e133a` (MVP + modelo, sem extrusão), `depthctl 0x1C7187B3`, logo antes do quad `fdf408078ea760bb`. A direção da luz das sombras não está em nenhuma constante. Uma sombra de contato na direção de `LightDirection1` (a única ligada em alguns desenhos, (0,752; −0,499; 0,430)) caía do lado oposto ao da sombra do jogo; por isso o raio é vertical.
- Shadow maps de verdade: veja o README (opção c): sem luz confiável e dobrando o trabalho do thread do command processor.

### Medido (`desempenho_vila_direto`, `tools\perf\bench.py`, sozinho na máquina)

1080p: 175 fps desligado, 168 ALTA + tudo (0,75 ms GPU), 178 BAIXA + tudo (0,17 ms); 4K: 148–153 desligado, 119 ALTA (1,09 ms), 122 BAIXA (0,83 ms); 4K DLSS desempenho: 150 → 131 (0,85 ms); sombras suaves sozinhas em 4K: 148 → 141. CP thread 2,88 × 2,83 ms/quadro a 1080p (custo de CPU nulo). Estabilidade: termo de AO parado, diferença média 0,05/255 entre quadros seguidos.

## Console de desenvolvedor e troca de skin (`dev/dev_console.*`, `dev/dev_skins.cpp`, `dev/dev_cheats.cpp`, `gpu/window.cpp`, 2026-10-22)

Pedido: mandar comandos com o jogo aberto (unlock all, mojo, trocar de skin...). Uso: guia, "Console de desenvolvedor"; catálogo e endereços: `docs/dev_features.md`, "Skins".

- **Três threads**: a janela digita (`OnKeyDown`/`OnChar`/`OnWheel`; o `WM_CHAR` vem do `TranslateMessage` que já existia no laço), o jogo roda os comandos (`RunPending`, chamado do mesmo gancho do `CScriptManager::vf10` que roda os blocos Lua, onde dá para chamar o Lua e as funções do jogo) e o renderizador desenha (`Draw`, depois da tela de SETTINGS). Tudo compartilhado fica sob um mutex.
- **Desenho**: GDI (Consolas, `ANTIALIASED_QUALITY`) num DIB de 32 bits, texto branco sobre preto; a cobertura vira alfa e a cor sai da linha (uma cor por faixa de linha), em RGBA reto, e vai para o `overlay::DrawBitmap` num canal próprio (`BITMAP_CONSOLE`), só reenviado quando muda (o cursor pisca a cada 530 ms). Entra nas capturas (F12, `SHOT`).
- **Tecla**: pela posição (scan code `0x29`, a tecla antes do 1 em qualquer layout) ou F10 (`WM_SYSKEYDOWN`). O `WM_CHAR` dessa tecla é engolido; em layouts onde ela é tecla morta (`` ` `` no US-Internacional) o estado de tecla morta é limpo com um `ToUnicode` de espaço, senão o acento ia parar na frente da próxima letra. Aberto: teclas que descem não chegam ao `pc_input` (as que sobem chegam, nada fica preso), o cursor aparece, o mouse-câmera para, e o `ReadController` zera o controle 0 (`BlocksGameInput`, mais 250 ms depois de fechar). F1-F9, F11 e F12 continuam valendo.
- **Testes**: `CRASH_DEV_CONSOLE_SCRIPT` posta `WM_KEYDOWN`/`WM_CHAR` na própria janela (`window::Handle()`), o mesmo caminho de uma tecla de verdade. Primeira versão postava o `WM_CHAR` da tecla de abrir junto com o `WM_KEYDOWN`, e o `TranslateMessage` gerava outro: um `` ` `` sobrava na linha.
- **Saída**: o `fputs` do stdout do Lua (o `devprint`) e as mensagens de erro do `RunLua` (agora devolvidas em `error`) vão para o console; o `_TRACEBACK` já escreve `erro: ...`, então o console não repete a mesma mensagem.
- **god**: a flag do `CRASH_DEV_INVINCIBLE` virou atômica (`dev::SetInvincible`). **bighead**: o que o verificador de cheats faz (`sub_821DB680`): `SettingsDatabase` +94 e, para cada jogador, o comportamento 38 (render) → drawable: escala 2,2 nos +32/+36/+40 e o índice da junta `Head` em +44 (0xFFFF = nenhuma). **level**: `Frontend:StartLevelByName` (`sub_820F5D98`, que não usa o `self`) e, dentro de uma fase, `UberManager:ForceReload` (sem ele a troca não acontece em jogo).
- **Skin**: a skin atual é o hash do template do jogador em `0x824A8270`; o VESTIR (`sub_821D9270`) troca, carrega o novo e solta o antigo. No frontend o console chama a mesma função; numa fase a skin fica pendente e entra no gancho do `CStreamManager::vf05` (antes de carregar os templates; o `vf04` já soltou o antigo). `skin X now` = pendente + `ForceReload` (que passa por `vf04`/`vf05`).
- **Crash_Hulk**: (1) `sub_82215730` montava `Characters:Crash_Crash_Hulk`; o gancho devolve o hash de `Characters:Crash_Hulk` (e o do Crash normal para 1 e 18-20). (2) O Crash **atravessava o chão** (o usuário viu na janela de teste; `pos` dava y = -33 e o dano de queda caía a cada segundo): o objeto `crash` do pacote dela (`344a15f9`) não tem `crashEllipsoidShape`/`Interactor_VolumeShape`. Tentativas: trocar no template Lua (`TemplateActorFunctions["Characters:Crash_Hulk"]`, elipsoide `crash_hulk`) não muda nada, o jogador nasce do template nativo do pacote, cujo nome de elipsoide já é `crash`; procurar no inventário global (`sub_82252F88(4)`) não acha o objeto e o `vf12` lê um ponteiro nulo. O certo: o `r5` do `CPhysicsBehaviour::vf12` (`sub_82147568`) e do `CIntersectionBehaviour::vf12` (`sub_8211EA88`) é a entrada do `CStreamManager` do template (lista em `*0x824A90CC` +4..+8, hash em +20, dependências a partir de +48); para a Hulk passa a ser a entrada do `Characters:Crash` normal. Esse template só fica carregado como o do jogador 2 numa fase (o VESTIR da Hulk solta o do Crash: entrar na fase pelo menu derrubava o jogo no `vf12`), então o runtime segura uma referência própria a ele (`sub_8224D3C8`) da primeira vez que a Hulk é escolhida. (3) A tela de skins (`CSkinScreenAction`, `sub_820BBB68`) tem um switch próprio ESkin → template de pré-visualização; 22 caía em "nenhum", que soltava a pré-visualização e lia a solta (crash ao chegar nela com esquerda/direita); o gancho faz para 22 o que o switch faz com as outras, com os textos 22 do próprio jogo ("CRASH HULK"; as listas têm 25). (4) Desbloqueio sem vaga na lista de 20: `IsSkinUnlocked` (`sub_82215900`) e esquerda/direita (`sub_822159F8`) com `[dev] hulk_skin`. (5) Cenas: sem `_nis`, o ator da cena usa o Crash normal (`sub_821B6388`).
- Perfis: `console_dev`, `console_hulk`, `console_skin_menu` (todos `test = true`). Capturas: `build\console\`.

## Próximos passos sugeridos

1. Feitos: memória, critical sections e TLS (passo 5); threads e sincronização (6); GPU, primeira etapa (7); sistema de arquivos e mods (8); loop de frames estável (9); renderização D3D12, notificações, sign-in, entrada e relógio de áudio (10); fibers e contextos XMA (11); decodificador XMA e saída de som (12); `RtlNtStatusToDosError` e vídeos que faltam (13); música do menu / XMP (14); seletor de dispositivo, saves e relógio (15); funções engolidas, primeira fase, sombras/faixas do 3D, engasgos e motion blur (16); nomes de classes e métodos virtuais pelo RTTI (17); overlay de desempenho, marcas e gravação de botões (18); tela de configurações no menu principal (19); opções gráficas de PC: motion blur, mipmaps/anisotrópico, SMAA, DLSS/DLAA e FSR 3.1 (20); vozes das cenas, XMA2 de vários streams (20); gravação e replay do controle no relógio do jogo, com analógicos e SYNC nos carregamentos (21).
2. Feito: o boot sai do estado 2 (fibers, passo 11). O `switch` das jump tables no XenonRecomp já usa `.u32` (README). O decodificador XMA destravou o estado 3 (passo 12). O vídeo da Sierra que falta é pulado (`RtlNtStatusToDosError`, passo 13), e o jogo chega ao **menu principal**. A música do menu toca (XMP, passo 14), e o retângulo amarelo do menu sumiu junto. NEW GAME, nome, dificuldade e slot funcionam, e o save é gravado em `userdata\content` (passo 15). As funções engolidas pelo config foram corrigidas de uma vez com `tools/find_swallowed.py` (passo 16), e o jogo **entra na primeira fase**: cena de abertura e gameplay, sem falha. O motion blur exagerado (mapas de velocidade sem clear) foi corrigido com o aliasing de EDRAM dos clears do D3D (passo 16).
3. **Renderização, depois do boot** (copyright e tela de carregamento já saem corretos): mipmaps, cubemaps/3D, aliasing de EDRAM, resolves escritos na memória guest, MSAA (ver a lista do passo 10).
4. **XAM**: feito o básico de saves (passo 15); faltam `XamShowSigninUI`, `XamShowDirtyDiscErrorUI` e marketplace, se aparecerem. **Áudio**: interrupções do XMA, subframes e Bink (passo 12).
5. **Mods dentro dos `.rcf`**: os quatro `.rcf` já são lidos pelo VFS; falta o formato para substituir entradas individuais.
6. TLS estático (`XEX_HEADER_TLS_INFO`), `XexCheckExecutablePrivilege` e `KeSetBasePriorityThread` real. (Carregar direto do `default.xex`: feito em 2026-10-21, "Caminhos".)

## v1.0 (2026-10-22): limpeza, instalador e publicação

- **Repositório**: git com só o nosso código (`.gitignore` recusa `private/`, `generated/`, `dist/`, `build/`, `tools/XenonRecomp/`, os binários de `thirdparty/`, `docs/capturas/`, `tests/shadercache/`, `languages/*/subtitles.txt` e qualquer `*.xex`/`*.rcf`/`*.bik`/`*.rsd`/`*.p3d`/`image.bin`/`game/`). Licença MIT + `THIRD_PARTY_NOTICES.md`. O README antigo virou `docs/GUIA_DO_PROJETO.md`; o `README.md` novo é público (EN + PT-BR).
- **Sem caminhos da máquina**: `tools/buildenv.py` acha o dump (`config/local.toml`, fora do git, senão o `[paths] game` do `dist/userdata/settings.toml`, senão `dist/game`), o `vcvars64.bat` (vswhere) e clang-cl/CMake/Ninja (PATH e pastas comuns). `run.toml` ficou com `game = ""`; `run_game.py`, `assets.py` e `texts.py` usam o buildenv.
- **XenonRecomp**: as correções locais estão em `patches/XenonRecomp.patch` (commit `ddd128bc` da hedge-dev). Clonar + `git apply --ignore-whitespace` gera um `generated/ppc` **idêntico byte a byte** ao de antes.
- **Instalador** (`tools/setup.py`): pré-requisitos, confere o `default.xex` (SHA-256 `e76824b2...`), clona e corrige o XenonRecomp, baixa FFmpeg 8.1 LGPL (BtbN autobuild-2026-09-30), DLSS 310.9.16 e FSR 2.3.0 das fontes oficiais com SHA-256, recompila (pulado se nada mudou: `generated/ppc/.setup_stamp`), compila, instala os extras da release (`--subtitles`, `--remaster`, conferidos pelo `SHA256SUMS.txt`), monta o `dist` (`--copy-game` copia o dump) e exporta os assets (`--extract-assets`). Linhas `@@step/@@progress/@@download/@@prereq/@@error/@@finished` para a interface. Testado de uma cópia limpa: tudo passa e o perfil `boot` passa.
- **Instalador gráfico** (`CrashSetup.exe`, alvo CMake `CrashSetup` = `launcher.cpp` + `installer.inc` com `CRASH_INSTALLER`): a cara do launcher (fundo, logo, fontes Titans, moldura e estrelas lidos do `default.rcf` escolhido; reabre com `--game` depois da escolha), pré-requisitos com instalação pelo `winget` (console visível) e PATH relido do registro, opções (legendas, HD AI Remaster, assets para modding, copiar o jogo), progresso por passo com as estrelas, fim com JOGAR / ABRIR PASTA, erro com TENTAR DE NOVO / ABRIR LOG (`setup.log`). Um job object para cmake/ninja/clang no CANCELAR. `--autostart`, `--option <x>` e `--screenshot` para testes.
- **Release** (`tools/make_release.py <tag>`): `CrashRecomp-<tag>.zip` (git archive + `CrashSetup.exe`), `-legendas.zip`, `-HD-AI-Remaster.partN.zip` (≤ 1,9 GB de entrada por parte, limite de 2 GB do GitHub) e `SHA256SUMS.txt`.
- **HD AI Remaster do jogo todo testado no jogo** pela primeira vez: L1_E1, L3_E2 e L5_E3 por boot direto com `graphics.hd_remaster=true`, ~30 fps, sem crash, 1 720 texturas HD + pacotes de modelos. Continua experimental.
- **Limpeza** (com o OK do usuário, 43 GB): runs e capturas de teste, `private/assets` (export), scripts decompilados, intermediários do remaster (`fonte`, `hd_src`, `modelos`), exes antigos, caches de shader e de IA (huggingface, pip, uv). Saves e `settings.toml` do jogador copiados para `Downloads\CrashRecomp_backups`; `settings.toml` voltou ao padrão. O dump foi movido para `dist\game` a pedido do usuário.

## v1.01 (2026-10-22): pular cenas e console de desenvolvedor

- O que entrou: pular cenas e vídeos de abertura segurando B/Esc, com aviso no canto (commit 54a73bd, seção "Pular as cenas") e o console de desenvolvedor com troca de skin e a Crash_Hulk (67767a1, seção "Console de desenvolvedor e troca de skin").
- A release publica só `CrashRecomp-v1.01.zip` (código + `CrashSetup.exe`) e o seu `SHA256SUMS.txt` (`tools/make_release.py v1.01 --only source`). As legendas e o HD AI Remaster são os mesmos da v1.0 e continuam lá: o `tools/setup.py` baixa os dados da release em `RELEASE_TAG` (`v1.0`), que só muda quando esses assets forem refeitos.

## v1.02 (2026-10-22): co-op local e o instalador instala os pacotes Python da exportação de assets

- **Co-op local**: um segundo controle joga como jogador 2 (B entra, Y desce da mochila); `kernel/pad_slots.*`, JOGADOR 1 / JOGADOR 2 na aba CONTROLES e no launcher. Detalhes em "Co-op local" (seção Controles de PC).
- Problema relatado na v1.01: ASSETS PARA MODDING falhava com `ModuleNotFoundError: No module named 'numpy'` (textures.py/models.py importam numpy e Pillow, e o instalador nunca os conferia nem instalava). O resto da instalação passava.
- `tools/requirements.txt` (numpy>=1.23, Pillow>=9.1). O `setup.py` lista os pacotes na checagem (`@@pypkg`, nunca bloqueia), instala com `"<o mesmo python>" -m pip install -r tools/requirements.txt` no começo (com `--extract-assets`; tenta `ensurepip` se faltar o pip) e de novo antes da etapa 9. Uma falha só derruba a etapa 9, com o comando para rodar à mão (`@@hint`); o jogo fica pronto (`@@game-ready`). `--assets-only` refaz só a exportação; `--check-packages` só confere os pacotes.
- `CrashSetup.exe`: os pacotes que faltam aparecem em PRÉ-REQUISITOS (clicar instala pelo pip num console); se só os assets falharem, o título vira O JOGO ESTÁ PRONTO, aparecem JOGAR e o comando do pip, e TENTAR DE NOVO roda só `--assets-only`. Usa o Python de um venv ativado (`VIRTUAL_ENV`).
- Testado num clone limpo com um venv sem numpy/Pillow (detalhes no commit). A release publica só `CrashRecomp-v1.02.zip` + `SHA256SUMS.txt`; dados continuam na v1.0.

## Toolchain portátil (2026-10-23): nada para instalar

Pedido da comunidade depois da v1.02: o instalador exigia Visual Studio Build Tools + LLVM (2 a 4 GB instalados no sistema). Agora o zip da release traz tudo o que compila o jogo, e nada é instalado nem baixado para compilar. A decisão de distribuir só código + instalador (nenhum exe com código recompilado do jogo) continua.

- **`toolchain\`** (fora do git; no zip; num clone, `python tools\toolchain.py fetch`): llvm-mingw 20261006 (UCRT, x86-64: clang 23, lld, libc++, headers e bibliotecas de importação do mingw-w64), CMake 4.4.4, Ninja 1.13.2 e o Python 3.13.17 embutido oficial com numpy 2.5.4 e Pillow 13.0.0 desempacotados em `Lib\site-packages`. `tools/toolchain.py` fixa URL + SHA-256 de cada arquivo e corta o que não serve (os outros alvos do llvm-mingw, lldb, clangd, os runtimes de sanitizer, o manual do CMake): 737 → 386 MB no llvm-mingw, ~480 MB no total, **zip da release de 148 MB**.
- O `python313._pth` do Python embutido lista `..\..\tools`: com `._pth` o Python não põe a pasta do script no `sys.path`, e os scripts de `tools\` importam os vizinhos (`buildenv`, `rcf`...).
- **`tools/buildenv.py`**: o toolchain padrão é o de `toolchain\` (`cmd_prelude` só põe as pastas dele na frente do `PATH`); `[build] toolchain = "msvc"` em `config/local.toml`, `CRASH_TOOLCHAIN=msvc` ou `setup.py --toolchain msvc` voltam ao Visual Studio + clang-cl. Cada pasta de build guarda o toolchain que a configurou em `.crashrecomp_toolchain` e `configure_build` / `build_lines` a reconfiguram do zero (apaga `CMakeCache.txt` e `CMakeFiles\`) quando ele muda; `run_game.py`, `assets.py` (assettool) e `setup.py` usam isso. O `build\` desta máquina passou para o llvm-mingw assim.
- **CMake** (`if (MINGW)`): `-mavx` no lugar de `/arch:AVX`, `-O2` (o que o `/O2` do clang-cl faz; o padrão do CMake seria `-O3`), `-static` (libc++, libunwind e winpthreads dentro do exe: ele só importa DLLs do Windows e a UCRT), `_WIN32_WINNT=0x0A00` (o mingw-w64 assume um Windows antigo e esconde `SetProcessInformation`, `GetDpiForSystem`, `AdjustWindowRectExForDpi`...), `xaudio2_9` no lugar de `xaudio2`, `hid`/`setupapi`/`windowscodecs` explícitos, e `-gcodeview -Wl,--pdb= -Wl,--Xlink=/map:...` para o PDB e o mapa no formato do lld-link que o `tools/perf` lê (os nomes agora são Itanium, `_Z12sub_82353478R10PPCContextPh`; o `profile.py` desmangla os dois).
- **DLSS sem Streamline**: a `nvsdk_ngx_s.lib` da NVIDIA (MSVC, /MT) é COFF comum e o lld em modo mingw a linka sem mudança, com `/nodefaultlib:LIBCMT,OLDNAMES,libcpmt` e `runtime/gpu/ngx_msvc_compat.cpp`, que fornece os 12 símbolos do runtime do Visual C++ que faltavam: `_Init_thread_header/_footer/_epoch` (statics locais thread-safe, o mesmo protocolo do CRT), `__security_cookie` / `__security_check_cookie`, `__GSHandlerCheck` e `__GSHandlerCheck_EH` (repassa ao `__CxxFrameHandler3`), `_fltused`, `operator new/delete` com os nomes do MSVC e `std::_Xbad_alloc/_Xlength_error` (encerram com mensagem). `__CxxFrameHandler3` e `_CxxThrowException` já vêm da ucrtbase (`api-ms-win-crt-private`). O FSR (DLLs carregadas na hora) e o FFmpeg (DLLs) não precisaram de nada; o SMAA é código.
- **XenonRecomp**: compila com o llvm-mingw com `-include cstdlib` (o `fmt` dele usa `malloc` sem o header, e a libc++ não o inclui de tabela) e `-static`, e gera um `generated/ppc` **idêntico** ao do clang-cl. O zip traz o código dele já corrigido (os arquivos que o git dele e dos submódulos rastreiam, sem `.git`) e `tools\XenonRecomp\.crashrecomp_source` = `<commit> <sha256 do patch>`; o `setup.py` aceita isso no lugar do clone, então o **Git** só é pedido num clone do repositório.
- **Instalador**: o `CrashSetup.exe` roda o `setup.py` com `toolchain\python\python.exe`; PRÉ-REQUISITOS mostra INCLUÍDOS ("Nada é instalado neste PC"); a linha nova FERRAMENTAS (INCLUÍDAS / VISUAL STUDIO) é o modo avançado, que passa `--toolchain msvc` e mantém a instalação pelo `winget`. Num clone sem `toolchain\`, clicar em PRÉ-REQUISITOS roda `tools\toolchain.py fetch` num console. TENTAR DE NOVO continua igual (`--assets-only` quando só os assets falharam).
- **Medidas** (Ryzen 7 5700X, RTX 5080): build limpo 46 s (llvm-mingw) contra 50 s (clang-cl); `desempenho_vila_direto` sem limite, duas rodadas alternadas: clang-cl 180,3 / 176,9 fps, llvm-mingw 199,9 / 189,8 fps (~8% mais rápido); DLSS (`configuracoes_resolucao`) e FSR 3.1.5 sem nenhum quadro com falha; `fase_direta` ok.
- **Teste limpo**: zip extraído em `Downloads\CrashRecomp-v1.03-test` e `CrashSetup.exe --autostart --option assets` aberto com `PATH` só com as pastas do Windows e um perfil de usuário novo e vazio (`USERPROFILE`, `APPDATA`, `LOCALAPPDATA`, `TEMP`); sem Windows Sandbox nesta máquina (e sem admin). Pré-requisitos ok sem Git, XenonRecomp do zip, jogo pronto em ~1,5 min (recompilação + build), legendas baixadas da v1.0; a exportação de assets (o `crash_assettool` compilado pelo llvm-mingw, numpy e Pillow do Python embutido) terminou em ~9 min com vídeos, sons, 1 553 texturas e 790 `.glb`. O `dist\CrashRecomp.exe` dessa instalação entrou na vila (`vila_sem_cena`) com DLSS e com FSR. Espaço: ~1,4 GB sem os assets (toolchain 0,5, build 0,37, dist 0,19, SDKs 0,18, XenonRecomp 0,08, código gerado 0,06).
- Detalhes: os arquivos baixados pelo `toolchain.py` são conferidos contra SHA-256 anotados no dia (as releases do GitHub não publicam checksums desses arquivos). O `rtti_scan.py` continua precisando do `undname.exe` do Visual Studio ou de um `llvm-undname` (o llvm-mingw não traz).

## Vulkan (2026-10-23): o único renderizador, rumo a Linux/Steam Deck (`gpu/rhi*.{h,cpp}`, `gpu/xenos_renderer.cpp`, `gpu/vulkan_loader.*`, `gpu/upscaler.*`, `tools/setup.py`)

Pedido: trocar o D3D12 por Vulkan para ter mais compatibilidade com Linux (Proton, principalmente Steam Deck) e Windows. Mac fica de fora. Primeiro os dois conviveram (`video.renderer`, `--renderer`); com a paridade, inclusive DLSS e FSR, o usuário pediu "não vamos mais trabalhar na D3D12" e o backend Direct3D 12 saiu do build no mesmo dia.

**Arquitetura.** O antigo `d3d12_renderer.cpp` virou `xenos_renderer.cpp` (PM4, EDRAM, resolves, tile stash, decodificação e cache de texturas, passes temporais, apresentação), que fala só com `rhi::Device` (`gpu/rhi.h`). Dois modelos de binding: *draws* (shaders do Xenos: b0-b3, t0-t31, s0-s31, anel de upload em t0 space1) e *passes* (b0, t0-t7 ou structured buffer, samplers fixos s0/s1). O backend é `rhi_vulkan.cpp` (Vulkan 1.3: dynamic rendering, synchronization2, timeline semaphore por submissão; shaders HLSL -> SPIR-V pelo DXC, `dxcompiler.dll` carregado em tempo de execução; `vulkan-1.dll` também). Overlay, SMAA, iluminação, upscalers e as telas (legendas, settings, saves, console) usam só o RHI. O `rhi.h` manteve a forma de quando havia dois backends: uma build nativa (WSI do Linux) ou Metal só precisam de outro `Device`.

**Sem Vulkan.** Sem GPU/driver com Vulkan 1.3 + push descriptors, ou sem um `dxcompiler.dll` que gere SPIR-V (o do Windows SDK não gera), o log diz o motivo, uma caixa de mensagem avisa o jogador e o jogo fecha. O instalador baixa o `dxcompiler.dll` assinado da release oficial do DirectXShaderCompiler (v1.9.2609); numa máquina de desenvolvimento serve o do Vulkan SDK (o CMake copia de `thirdparty/dxc/bin` ou de `%VULKAN_SDK%\Bin`). `video.renderer` de um `settings.toml` antigo é ignorado. `run_game.py --set gpu_debug=true` liga validation + sync validation.

**Upscalers.** DLSS pela API Vulkan do NGX (o mesmo `nvsdk_ngx_s.lib` 310.9.16 já tinha `NVSDK_NGX_VULKAN_*`; faltavam `nvsdk_ngx_vk.h`, `nvsdk_ngx_defs_vk.h` e `nvsdk_ngx_helpers_vk.h`). FSR 3.1 pela `amd_fidelityfx_vk.dll` assinada do FidelityFX SDK 1.1.4 (o SDK 2.x só tem DLLs DX12), com o FSR 3.1.4 (no D3D12 era o 3.1.5). O device liga as extensões que eles pedem (`rhi::DeviceInfo`): a lista do NGX e as que a DLL da AMD procura pelo nome da extensão (`vkGetBufferMemoryRequirements2KHR`, `vkSetDebugUtilsObjectNameEXT`...: sem a extensão, o `vkGetDeviceProcAddr` devolve nulo e o primeiro dispatch do FSR chamava o endereço 0). Liga também, quando existem, os recursos dos compute shaders deles (fp16/int8/int16, armazenamento de 8/16 bits, controle de tamanho de subgrupo, storage images sem formato, buffer device address). Recursos descritos à mão para o FSR: o `ffxApiGetImageResourceDescriptionVK` trata imagens de formato mutável como sRGB.

**Paridade com o D3D12 (RTX 5080, Windows, comparada antes de remover o D3D12; capturas em `build\vulkan\` do worktree):**

| Área | Vulkan | Como foi conferido |
|---|---|---|
| Boot, intros Bink, menus, frontend | igual | `menu`, `boot` |
| Tela SETTINGS (todas as abas), fonte do jogo, ícones paletizados, FXAA, hub | igual | `configuracoes` |
| L1_E1: cores, sombras de stencil, aliasing de EDRAM, resolves, tiling, MSAA, profundidade | igual | `vila_sem_cena` |
| Motion blur, DOF, suavidade de sombra, anisotrópico 16x, mipmaps | igual | `vila_sem_cena` |
| SMAA, resolução interna 1.5x | igual | `vila_sem_cena` + `CRASH_SETTINGS` |
| Iluminação (SSAO/GI/contato/sombras suaves) | igual | `iluminacao_tela` |
| Overlay F1, legendas, cursor | igual (overlay e telas conferidos; legendas e cursor pelo mesmo caminho de passes) | `configuracoes` |
| Caminho D32S8 + `VK_EXT_depth_bias_control` (o do RADV/Deck) | igual | `vila_sem_cena` + `CRASH_VULKAN_D32=1` |
| V-Sync, limite 30/60/120 | ok (FIFO / immediate ou mailbox) | `vila_sem_cena` + `CRASH_SETTINGS` |
| Tela cheia | sem modo exclusivo: tela cheia = janela sem bordas | idem |
| DLSS 310.9.16 (quality, 1080p) | igual | `vila_sem_cena` + `graphics.upscaler=dlss` |
| FSR 3.1 (quality, 1080p) | igual (3.1.4 vs 3.1.5); o primeiro dispatch leva ~0,9 s (pipelines) | `vila_sem_cena` + `graphics.upscaler=fsr` |
| Desempenho 1080p sem limite | 170 fps vs 169 (D3D12), CP 3,2 vs 3,1 ms/quadro | `bench.py --profile desempenho_vila_direto` |
| Validation layers | limpo (só o aviso de PS que escreve SV_Target0 sem attachment) | `vila_sem_cena` + `gpu_debug` |
| Pacotes HD de texturas | mesmo caminho de upload (não conferido com um pacote) | — |

**Armadilhas encontradas.**
- Render targets e depth empacotados no alinhamento de 1 KB do driver dentro da mesma página de 64 KB se corrompiam na NVIDIA (a vila mostrava padrões de depth): attachments ganham alinhamento de 64 KB.
- Push descriptors com samplers imutáveis amostravam zero de image views criadas depois dos primeiros quadros: o segundo front buffer dos menus (0x0A6F0000) aparecia preto a cada dois quadros. O resolve e a apresentação estavam certos (cópia de leitura com conteúdo), `WaitIdle` antes de cada passe não mudava nada, uma view nova por uso deixava todos os quadros pretos. Os passes agora usam um descriptor set normal por passe (pool por command buffer); os draws continuam com push descriptors (set 0, sem samplers imutáveis).
- As validation layers do SDK 1.4.363 travam em `vkCmdPushDescriptorSetKHR` quando o set de push não é o set 0 (as texturas dos draws estão no set 0 por isso).
- SPIR-V do DXC usa `OpDemoteToHelperInvocation` para `discard`: `shaderDemoteToHelperInvocation` é obrigatório.
- Leitura de stencil: SPIR-V lê `.x` de uma view de stencil (`XE_STENCIL` no HLSL; o DXBC do D3D12 lia `.y`).
- No llvm-mingw, o `__uuidof` das interfaces do DXC precisa dos GUIDs declarados (`__CRT_UUID_DECL`): o `dxcapi.h` só tem `__declspec(uuid)`, que só os compiladores de ABI MSVC leem.
- Primeira vez numa resolução nova: o DXC compila os shaders escalados (~80 ms cada, travada de ~2 s na vila a 1080p); depois vêm do cache `userdata\shadercache\*.spv`.

**Plano de teste no Steam Deck (Proton, RADV, 1280x800):**
1. Copiar a `dist` (com o `dxcompiler.dll`) para o Deck; adicionar o `CrashRecomp.exe` como jogo não-Steam, Proton Experimental.
2. `settings.toml`: `resolution = "1280x720"`, `fps_limit = 30`; janela sem bordas (16:10 fica com faixas pretas em cima e embaixo, o jogo é 16:9).
3. Rodar com `PROTON_LOG=1` (a saída do jogo vai para o `steam-*.log`) e conferir: `[vulkan] AMD Custom GPU 0405 (RADV VANGOGH) ... depth D32S8 (no sampled D24S8) ... depth bias control`. Sem `depth bias control` (Mesa < 23.3), o bias das sombras fica em unidades de D32: anotar.
4. Menu, SETTINGS, NEW GAME, vila (sombras de stencil, motion blur), uma fase com tiling (ice band), a iluminação ligada e o FSR; F1 para o FPS.
5. `CRASH_GPU_DEBUG=1` (precisa das validation layers no prefixo) num segundo teste.
6. Medir 720p/800p a 30 e 60 fps com o overlay; o gargalo esperado é a CPU do command processor (4 núcleos Zen 2).

**Próximos passos.**
1. Testar no Steam Deck e numa GPU AMD/Intel no Windows (o caminho D32S8 só foi forçado na NVIDIA).
2. Menos instâncias de rendering e barreiras por quadro (~300 e ~650 na vila): agrupar barreiras e manter a instância aberta entre draws com os mesmos alvos.
3. Build nativa para Linux (tarefa separada): SDL3 para janela/entrada/áudio, threads/memória/fibers POSIX (hoje `kernel/` usa Win32: `VirtualAlloc` com endereços fixos, fibers, eventos), `VK_KHR_wayland/xlib_surface` (headers já vendorizados), caminhos e `LoadLibrary` -> `dlopen` (`libvulkan.so.1`, `libdxcompiler.so`), ffmpeg do sistema. O DLSS do Linux precisa do `libnvsdk_ngx.a` do SDK; o FSR VK só tem DLL de Windows.

## GPU nativa, fase 0 (2026-10-24, branch `native-gpu`): onde o tempo vai e o plano (`docs/internals/native-gpu-plan.md`, `tools/d3d_map.py`)

Pedido: deixar o jogo "mais nativo, sem traduções em tempo real e com mais performance". Antes de mexer, medido (`bench.py native_base_sampled --profile desempenho_vila_direto --sample 300 --sample-start 20 --sample-seconds 15`, worktree de `a906172`, Vulkan, 1080p sem limite, sozinho na máquina): **162 fps**, quadro de 5,83 ms no renderer.

- A thread do jogo passa **62% do tempo** na espera do fence do quadro anterior (`sub_8226D968` < `sub_8226C958` chamada direto de `sub_8226CCC8`: o fence do `Swap` em `device+14788`); 28% é o jogo/engine, 5,3% o D3D montando PM4 e 4,5% o `pure3d::xn*`. O command processor trabalha 3,5 ms por quadro e fica **44% parado** esperando comandos. 2,3 + 3,5 ≈ 5,8 ms: as duas threads trabalham **em série**.
- A causa é o **tiling predicado**: entre o `BeginTiling` (0x8227A5F8 -> 0x82281D00, liga `device+13088`) e o `EndTiling` (0x8227AB80 -> 0x82281E70), o kick do D3D (0x8226CE40) não manda o segmento para o anel primário: põe um pacote de indirect buffer numa lista em `device+13224`, que o `EndTiling` reexecuta uma vez por tile. A cena só chega ao command processor no fim, e é interpretada três vezes.
- A camada que desenha é o backend Xenon do Pure3D (classes `pure3d::xnContext`, `xnDisplay`, `xnTexture`, `xnPrimBuffer`, `xnShader` e 15 shaders de efeito, 0x8238B000-0x823AC000). Ela faz quase todas as chamadas ao D3D: 98 pontos de entrada (0x82268C00-0x822845A0 e as 4 funções de draw em 0x823EA9A8-0x823EBBD8). Fora dela só chamam o D3D o `movie::PlayerBink`, o `CFrontendManager` (2 funções no fim do intervalo, talvez nem sejam D3D) e uns ajudantes de textura em 0x823EBBD8-0x823EC880. `tools/d3d_map.py` gera a lista com os chamadores e os slots das vtables xn.
- `xnContext::vf04` (0x8238CD00) abre o tiling, `vf05` (0x8238E068) fecha e faz o resolve (0x8227F9E8), 0x8238CE98 é o fechamento no meio do quadro (o resolve da cena antes da refração), `vf101`/`vf102` um segundo par; `xnDisplay::vf15` chama o `Swap`; `xnPrimBuffer::vf15` desenha (0x823EB398 / 0x823EB780).
- Shaders e texturas **não** custam por quadro hoje (cache por hash; 0,7 MB hasheados por quadro com a vigia de escrita): pré-compilar tira as travadas da primeira vez, não aumenta a média.

Plano em `docs/internals/native-gpu-plan.md`: M1 shaders pré-compilados; **M2 cena numa passada só, sem tiling, ainda no caminho PM4** (o maior ganho esperado: as threads passam a trabalhar juntas; teto estimado ~1,6x mais o PM4 da cena interpretado uma vez só); M3 draws diretos (ganchos nas funções de draw do D3D, sem PM4); M4 recursos nativos com `--gpu-path pm4|native`; M5-M8 efeitos, DLSS/FSR/iluminação/HD, limpeza, desempenho. Com quadro de tarefas para colaboradores.
## GPU nativa, M2 (2026-10-24, branch `native-gpu`): a cena numa passada só, sem tiling predicado (`gpu/tiling_hooks.cpp`, `gpu/tiling.h`)

**Resultado**: `desempenho_vila_direto`, mesmo build, janela fora de foco nos dois: com tiling 179 fps (CP 3,3 ms/quadro) -> **passada única 429 fps** (p99 3,3 ms, 1% low 214 fps, CP 1,39 ms/quadro, 1.585 -> 574 draws e 1,38 M -> 0,46 M índices por quadro). A espera do fence na thread do jogo caiu de 62% para 5%. Ligado por padrão; `CRASH_SINGLE_PASS=0` volta ao tiling do console.

**O que o rastreio mostrou** (`CRASH_TRACE_TILING=<n>[@<quadro>]`, um quadro da vila): um bracket por quadro. `BeginTiling(flags 0, 3 tiles 0-448/416-864/832-1280, sem clear)`, `Clear(0x1F)` e `Clear(0x2, 0x808080)` (velocidade), a cena, 10 resolves lógicos emitidos 3 vezes cada (um por tile: o xn faz `SetPredication(2 << tile)` + `Resolve(retângulo do tile, ponto = origem do tile)`), dois clears de stencil nos 3 retângulos, `EndTiling(0x300 = resolve + limpa cor e profundidade, para um dos dois front buffers)`. `xnContext::vf101/vf102` (0x8227B038/0x8227B380) são chamadas por draw, não tiling.

**Os RTs da cena eram do tamanho do tile**: `xnDisplay::vf04` cria cor 0, depth/stencil (formato 0x1A220197) e cor 1 (velocidade) por `0x8226A868` com largura `align32(1280/3 + 63)` = 480; o renderer desenhava cada tile num RT host de 480 px e resolvia o pedaço. Na passada única eles nascem com 1280x720, mas três superfícies 1280x720 2x (1.440 tiles cada) não cabem no alocador de EDRAM do D3D (2.048 tiles; `0x8226BAD0`, conferido em `0x8226A868`): a 2ª e a 3ª criações voltavam nulas e o `Resolve` caía lendo `device+12308` (RenderTarget[1]) -> endereço 0x18. Agora recebem bases explícitas por `D3DSURFACE_PARAMETERS` (Base +0, HierarchicalZBase +4, ColorExpBias +8; com parâmetros o alocador é pulado): 1440, 2048 e 2880. Os RTs host são identificados por (base, largura host, formato), sem o MSAA, então as bases também precisam ser diferentes das dos alvos 1x do pós/HUD (cor em 0, depth em 720).

**Os caminhos "sem tiling" do xn são código morto**: com `xnContext+1220 = 0` o xn faz um resolve só da superfície inteira, mas lê slots de textura de depth/sombra que nunca são criados (e mistura índices 295/297). Por isso o `+1220` continua ligado: o `BeginTiling` é pulado (o jogo não pede clear nele), um `EndTiling` (vf05 e o resolve do meio do quadro 0x8238CE98) vira um `Resolve` da superfície inteira com as flags, clears e parâmetros dele, e dos 3 resolves por tile do xn o primeiro vira o da superfície inteira e os outros dois são descartados. O `TileStashBeforeResolve` fica desligado nesse modo (não há emendas). A faixa no gelo em x = 832 não pode acontecer: a refração lê a cópia da cena inteira antes do gelo.

**Conferido** (pares com e sem passada única, `compare_runs.py`, mesmas capturas): `menu`, `configuracoes`, `iluminacao_tela`, `fase_direta` (L1_E3), `vila_sem_cena`, vila com DLSS e com FSR; diferenças dentro do ruído de timing (duas execuções com tiling diferem igual: 37% de pixels na 3ª captura da vila, partículas e câmera). `vila_sem_cena` com `gpu_debug`: só o aviso conhecido de PS sem attachment. Capturas em `build
ative_gpu\m2\`.

**Agora o gargalo é a thread do jogo** (amostrado): 63% jogo/engine, 16% D3D montando PM4, 15% Pure3D xn; o command processor fica ocioso a maior parte do tempo. O próximo marco (M3, draws diretos) ataca os 31% de D3D + xn.

## GPU nativa, M3a (2026-10-24, branch `native-gpu`): o flush de estado e o kick do D3D em C++ (`gpu/d3d_native.cpp`)

**Onde estava o custo**: amostrado depois do M2, as funções de draw do D3D eram ~15% da thread do jogo: ~5% a espera residual do fence e o resto o flush recompilado dos grupos de estado sujos (`sub_8226E308` 3,4%, `sub_8226E6A8` 2,8%) e o kick (`sub_8226C3F0` 3,3%, por causa da armadilha MMIO no `CP_RB_WPTR`). Os setters que o xn chama são baratos.

**Modelo de estado do D3D** (lido em `sub_823EB780`, o draw indexado): cinco máscaras de 64 bits em `device+0..+32`; cada bit é um grupo de registradores-sombra guardados no device na ordem dos registradores do Xenos (constantes VS em `+1792` = registrador 0x4000, PS em `+5888` = 0x4400, contexto em `+10240` = 0x2000...). No draw, cada sequência de grupos sujos vira um pacote type-0 no segmento atual (cursor = último dword escrito em `device+48`, fim em `device+52`). `sub_8226E308`: um registrador por bit; `sub_8226E6A8`: 16 dwords (4 float4) por bit, com NOPs type-2 para alinhar os dados a 16 bytes (cópias VMX). `sub_8226DCD0` continua a sequência no próximo segmento. O kick (`sub_8226C3F0`) escreve pacotes INDIRECT_BUFFER (0xC0013F00, endereço, tamanho) no anel primário (`device+14744`, máscara `+14748`, índice `+10824`) depois de esperar espaço (`sub_8226BD58`) e grava o índice no `CP_RB_WPTR` (0x7FC80714, que só ele escreve).

**O que mudou**: as três em C++ (cópias em bloco dos registradores big-endian, sem trocar o endian; o caso que cruza o segmento ainda chama o `sub_8226DCD0` do D3D; o kick entrega o `CP_RB_WPTR` direto ao command processor, sem exceção; os caminhos de PIX/captura ficam com o original). A saída é idêntica byte a byte: `CRASH_NATIVE_D3D_VERIFY=1` roda as duas e compara (`vila_sem_cena`: 2,1 milhões de flushes de registradores e 0,5 milhão de constantes, 0 diferenças). Ligado por padrão; `CRASH_NATIVE_D3D=0` volta às recompiladas.

**Armadilha encontrada**: o gancho do `Resolve` do M2 tinha sido declarado `extern "C" PPC_FUNC(sub_8227F9E8)`. O código gerado chama `sub_X` com linkage C++ (alias fraco do `__imp__sub_X`, que é C), então o `extern "C"` criou um símbolo à parte que o jogo nunca chamava: a deduplicação dos resolves por tile não estava ativa no M2 (os números e capturas do M2 valem sem ela). Corrigido; com ela, 429 -> 441 fps.

**Medido** (`desempenho_vila_direto`, sozinho): 441 fps sem o D3D nativo -> **464 fps** com ele; CP 1,27 ms/quadro. Conferido contra o caminho com tiling: `menu`, `configuracoes`, `iluminacao_tela`, `fase_direta`, `vila_sem_cena` (também com `CRASH_SINGLE_PASS=0` e com DLSS); diferenças dentro do ruído.

**O que limita agora** (amostrado): thread do jogo 62% jogo/engine, 13% xn, 5,6% D3D e **19% esperando o fence do quadro anterior**. O command processor fica ~50% parado, mas durante a fase de render do jogo processa mais devagar do que o jogo emite (~1,25 ms de trabalho por quadro: `DrawImpl` 31% inclusivo, draw/rendering/present do Vulkan ~17%), e a trava de 1 quadro em voo do D3D (necessária) faz o jogo esperar. Segmentos menores do D3D (64/32/16 KB, baratos agora sem a armadilha) não ajudaram (450–457 fps). Trocar o transporte do estado dos draws por um registro próprio (M3b, sem PM4) só tiraria o parse do PM4 no CP (~5% dele): fica para depois do custo do renderer (M8) e dos recursos nativos (M4).

## GPU nativa, M8a (2026-10-24, branch `native-gpu`): o teto de 500 fps do vblank, instâncias de rendering e a thread de interrupções (`gpu/gpu.cpp`, `gpu/rhi_vulkan.cpp`, `gpu/command_processor.cpp`)

**O teto de 500 fps**: o swap espera 2 vblanks por quadro, e sem limite (e nos limites 60/120) o vblank emulado rodava a 1000 Hz. Com a cena numa passada só e o D3D nativo a vila já fazia ~465 fps: o command processor esperava, a cada quadro, até um vblank na sequência de swap do D3D. A sequência: `SCRATCH_REG1` (escrito de volta em 0x0870B004, `SCRATCH_ADDR` = 0x0870B000, `SCRATCH_UMSK` = 0x20033) = 1, o callback de swap (`sub_82273A40`, timebase e estatística do quadro) pela interrupção e um `WAIT_REG_MEM` até a CPU zerar o scratch, o que o tratador de vblank do jogo faz quando os 2 vblanks passam. O jogo esperava o CP. `CRASH_CP_WAIT_STATS=1` mostra as esperas que bloqueiam, por endereço, e os pacotes antes delas. `FAST_VBLANK_HZ` = 2000 (gpu.cpp): 464 -> 485–510 fps, p99 3,7 -> 2,7 ms, 1% low 210 -> 265 fps; 4000 Hz não deu mais nada. O modo 30 (vblank de 60 Hz, igual ao console) não mudou; limite 60 conferido: 59,8 fps, p99 16,75 ms.

**Instâncias de rendering em superconjunto** (`PipelineInsideRendering`, `CRASH_VULKAN_SUBSET_RENDERING=0` desliga): um draw cujos alvos são um subconjunto dos da instância aberta (a mesma imagem em cada slot que ele escreve, a mesma depth ou nenhuma) é gravado dentro dela com uma variante do pipeline (`DrawPipelineKey::keepFormats`: os formatos da instância, sem escrita nos slots a mais, sem teste/escrita de depth e stencil quando o draw não tem depth). Na vila, de ~71 para ~54 instâncias por quadro (a linha `rendering instances begun because ...` dá os motivos que sobram: depth 25, número de cores 11, alvos 11, nenhuma aberta 7); CP 1,27 -> 1,20 ms/quadro; validation limpa.

**Thread de interrupções sem giro**: `condition_variable::wait_until` arredonda para milissegundos inteiros no Windows; com o vblank abaixo de 1 ms ela voltava na hora e a thread girava (~72% de um núcleo, já era assim a 1000 Hz). Agora espera num waitable timer de alta resolução + um evento (sem eles, Windows anterior ao 10 1803, volta à condition variable): ~5% de um núcleo, mesmo FPS, 2000 vblanks/s medidos. Faz diferença em máquinas de 4 núcleos (Steam Deck).

**Onde o quadro está agora** (amostrado, vila, ~500 fps): a thread do jogo fica ocupada ~98% do quadro (73% código do jogo/engine, 15% xn, 9,5% D3D) e espera o command processor só ~2%; o CP trabalha ~1,2 ms em ~2 ms de quadro. Mais trabalho no renderer (uma depth/velocidade "grudada" na instância, o custo do `DrawImpl`/`GetTextureView`) não sobe mais o FPS nesta máquina: diminui o uso de CPU e ajuda CPUs mais fracas. Os próximos ganhos de FPS estão no código recompilado do jogo (`sub_82353478`, o skinning na CPU; o memcpy do jogo) e no xn, fora do caminho da GPU. Conferido: `menu`, `configuracoes`, `iluminacao_tela`, `fase_direta`, `vila_sem_cena` (também com tiling e com DLSS) e `vila_sem_cena` com `gpu_debug`.

## GPU nativa, M1 (2026-10-24, branch `native-gpu`): shaders compilados antes do uso (`gpu/shader_warmup.*`, `--precompile-shaders`, `tools/setup.py`)

**O que os arquivos do jogo têm**: os 24 pixel shaders estão em `<jogo>\shaders\*.out` (mais um nos dados do executável). Os vertex shaders que chegam à GPU **não estão em arquivo nenhum**: o D3D do Xbox amarra cada vertex shader a uma declaração de vértices em tempo de execução e reescreve as instruções de fetch. Na vila, 13 dos 14 VS só existem assim (3 pequenos são dados do exe). Achado com `CRASH_DUMP_SHADERS=1`, que agora grava também o microcódigo (`.ucode`, big-endian) ao lado do HLSL.

**Variantes**: duas por shader, 720p e escalada (resolução interna diferente de 1x, cena escalada pelo DLSS/FSR ou texturas HD). `TranslateShader(..., renderScaled)` recebe a variante em vez de ler o estado global. O cache de SPIR-V é indexado pelo HLSL final, então os dois convivem.

**O que mudou**: o runtime grava o microcódigo de cada shader que traduz em `userdata\shadercache\ucode\<vs|ps>_<hash>.ucode` (local; é derivado do jogo, nunca vai para release). A cada início, uma thread de prioridade baixa traduz e compila as duas variantes de todo shader conhecido (os pixel shaders dos `.out` + os gravados) que faltam no cache. `CrashRecomp.exe --precompile-shaders` faz o mesmo em todos os núcleos, e o `tools/setup.py` roda isso depois de montar a `dist\` (24 × 2 em 0,85 s com 16 threads). `CRASH_NO_SHADER_WARMUP=1` desliga o aquecimento. O DXC passou a ter um compilador por thread, e o cache é gravado com nome temporário + rename.

**Medido** (userdata temporária): cache vazio = 46 compilações na primeira ida à vila; depois do `--precompile-shaders`, 32 (14 VS amarrados + 18 shaders de pass do renderer, uma vez por instalação); trocando para 1080p depois de uma execução, o aquecimento compilou as variantes escaladas de 39 shaders em 329 ms no boot e o renderer compilou **0** durante o jogo (antes eram ~2 s de compilações na primeira vez numa resolução nova). O HLSL gerado pelo aquecimento é o mesmo do renderer (o cache bateu nos 39).

**A variante escalada custa?** Medido a pedido: vila 720p 532/496 fps contra 1080p 531/503 fps, GPU esperando ~0. Não custa nada mensurável nesta máquina: a variante só acrescenta umas multiplicações por leitura de textura, e o limite é a CPU. No Xbox o jogo era 720p nativo; aqui o PC desenha os mesmos draws numa imagem maior.

**O que fica de fora**: um VS amarrado inédito ainda compila no primeiro draw (~80 ms, uma vez por instalação), e os shaders de pass do renderer compilam na primeira execução.


## Só a versão nativa no `main` (2026-10-24)

Decisão do usuário: "vamos juntar o novo launcher com a versão native e eliminar a versão antiga, vamos trabalhar somente na nativa a partir de agora". A branch `native-gpu` recebeu o `main` (launcher simples, `e4d0701`; o único conflito foi este arquivo) e o `main` avançou até ela; a branch e o worktree `CrashRecomp-native` foram apagados depois.

**O que saiu**: o caminho do tiling do console (o `BeginTiling` original, o `EndTiling` original, `gpu/tiling.h`, `CRASH_SINGLE_PASS`), o tile stash do renderer (`TileStashBeforeResolve`, `CRASH_GPU_NO_TILE_STASH`), e as chaves `CRASH_NATIVE_D3D`, `CRASH_VULKAN_SUBSET_RENDERING` e `CRASH_NO_SHADER_WARMUP`. Ficaram as ferramentas de checagem (`CRASH_NATIVE_D3D_VERIFY`, `CRASH_TRACE_TILING`, `CRASH_CP_WAIT_STATS`, `CRASH_DUMP_SHADERS`).

**Conferido antes de remover**: seis fases a mais (L2_E1, L2_E4, L3_E2, L4_E1, L4_E3, L5_E2) rodaram numa passada só sem crash nem defeito visual; o `EndTiling` do meio do quadro (`0x8238CE98`, chamado por `CRenderManager::vf10`) não apareceu em nenhuma delas nem na vila: continua passando pelo mesmo substituto (um `Resolve` da superfície inteira), mas não foi exercitado. No build do `main`: `menu`, `vila_sem_cena`, `configuracoes`; a `dist` refeita com o `make_dist.py` e os shaders pré-compilados nela; a `userdata` do jogador ficou idêntica (hash dos arquivos antes e depois); o launcher abre na vista simples (`--screenshot`).

## Launcher simples + opções compartilhadas com a tela SETTINGS (2026-10-24) (`settings/options.*`, `launcher/launcher.cpp`, `settings/settings_menu.cpp`, `gpu/hd_textures.cpp`)

Pedido: o launcher abria com todas as opções; queria só idioma, resolução, limite de FPS, HD AI Remaster e uma ILUMINAÇÃO única (Original / Remaster), com um AVANÇADO no canto para o resto; sem a linha de texturas HD; a aba DEFINITIVE EDITION do launcher igual à do jogo (faltavam as cinco linhas de iluminação e o título delas); e sem a linha RESOLUÇÃO DAS SOMBRAS (só informativa) na tela do jogo. Capturas em `build\launcher_menus\` (`LEIA.txt`).

**Uma definição só.** As duas telas tinham cópias próprias do enum de itens, das abas, dos nomes, da ajuda, dos valores e do LEFT/RIGHT, e tinham divergido (DEFINITIVE EDITION sem iluminação, JOGO do launcher sem PULAR CENAS, textos de ajuda diferentes). Agora `runtime/settings/options.{h,cpp}` tem `Item`, `Tabs()`, `Key`, `Label`, `Value`, `Help`, `Percent`, `Change` e `RestartOnly`, e as duas telas só acrescentam o que é delas: `options::Context` (o launcher diz "Vale ao abrir o jogo." onde o jogo diz "Vale na hora." / "O jogo reinicia..."; DLSS/FSR disponíveis; legendas presentes), JOGADOR 1/2 (o jogo lista os controles conectados), a captura de teclas, a aba INSTALAÇÃO e o título TECLADO E MOUSE do launcher. O launcher tira ARMAZENAMENTO (aviso do Y do menu principal). Os textos que divergiam ficaram com a versão mais específica (DLSS sem placa/DLL, remaster e legendas indisponíveis apontando a pasta `data\`). Armadilha: com `Item` em `settings::options`, um wrapper `Key(Item)` em `settings::menu` fica ambíguo por ADL com `options::Key`; a tela chama os de `options` direto.

**Vista simples.** Padrão do launcher: uma página OPÇÕES com IDIOMA, RESOLUÇÃO, LIMITE DE FPS, HD AI REMASTER e ILUMINAÇÃO. `options::LightingPreset`: REMASTER = SSAO alta, luz indireta ligada a 100%, sombras de contato e suaves; ORIGINAL = tudo desligado (a intensidade da luz indireta só conta com ela ligada e o ORIGINAL não mexe nela); qualquer outra mistura é PERSONALIZADO (RIGHT vai a REMASTER, LEFT a ORIGINAL). AVANÇADO / SIMPLES fica no canto inferior direito (clique, Y no controle, Y ou F2 no teclado). A vista é `[launcher] view = "simple" | "advanced"` no `settings.toml` (o jogo ignora); `SetAdvanced` grava na hora a partir dos valores salvos, então as outras mudanças continuam "não salvas". Sem a pasta do jogo, ou ao pedir JOGAR sem ela, o launcher vai ao AVANÇADO > INSTALAÇÃO. `--view simple|advanced` e `--tab N` (implica avançado) servem às capturas e não gravam.

**Navegação do launcher.** Dirigindo o launcher por teclas sintéticas (`keybd_event`) para testar, o DOWN não andava: o ImGui ficava com `NavId` 0 (a janela abriu atrás de outra e o pedido de foco do `SetKeyboardFocusHere` se perdeu), e o v1.03 faz igual. O launcher agora move a seleção sozinho (UP/DOWN, D-pad, analógico, dando a volta como o jogo) e ativa a linha com A / Enter / Espaço, sem a navegação do ImGui, como o instalador já fazia; o mouse e a roda não mudaram.

**Texturas HD.** A linha PACOTES DE TEXTURAS HD controlava `[graphics] hd_textures`, o carregador genérico de `mods\<mod>\hd_textures` (`gpu/hd_textures.cpp`). A chave fica (padrão ligada, sem linha em nenhuma tela; os pacotes dos mods carregam sozinhos). Como sem a linha um `false` antigo não teria volta pela interface, o pacote do próprio HD AI Remaster agora vale sempre que `graphics.hd_remaster` está ligado, mesmo com `hd_textures = false` (conferido: `vila_sem_cena` com `hd_remaster=true;hd_textures=false`, 1720 texturas indexadas, 42 trocas).

**Tela do jogo.** GRÁFICOS perdeu RESOLUÇÃO DAS SOMBRAS (não existe: sombras de stencil, veja "Por que não há resolução de sombra" no guia) e os textos dela. Era a última linha e não era selecionável, então nenhum perfil mudou de contagem; o passo das linhas (44) também não muda com 7 linhas.

**Conferido.** Perfis `configuracoes`, `configuracoes_resolucao`, `iluminacao_tela`, `controles_tela` (passaram); `--settings-preview` em EN e PT-BR (`build\launcher_menus\jogo\`); capturas do launcher simples / avançado (todas as abas) / PERSONALIZADO em EN e PT-BR; o launcher dirigido por teclas (DOWN x4, RIGHT ou LEFT, F5) com ILUMINAÇÃO = REMASTER e depois ORIGINAL gravou `[lighting]` certo e abriu o `CrashRecomp.exe`; o `vila_sem_cena` com essa userdata mostrou `[settings] lighting: ssao high, gi on (100%), contact shadows on, soft shadows on` e o passe `[lighting]` (~0,35 ms de GPU), e com ORIGINAL tudo desligado e sem passe.

## Tela preta piscando na Intel UHD (issue #3, 2026-10-24) (`gpu/rhi_vulkan.cpp`, `gpu/rhi_ring.h`, `gpu/rhi_common.cpp`, `settings/settings.*`, `diagnostics.cpp`)

Relato: notebook i5-13420H com Intel UHD integrada (8 GB compartilhados), sem mods; 1080p/60/V-Sync e 1600x900/30 sem V-Sync piscam a tela inteira de preto e, às vezes, só a cena 3D (o HUD por cima continua): nas capturas da v1.03, o terço esquerdo ou os dois terços esquerdos, nas emendas dos tiles do tiling predicado (x = 416/832 de 1280). Continua na v1.04 (passada única, sem tiles). Aqui só há a RTX 5080; nada reproduziu o preto. Capturas em `build\intel_flicker\`.

**Validação de verdade.** O `CRASH_GPU_DEBUG` só ligava a validation layer básica: a validação de sincronização, que a documentação dizia ligada, nunca foi pedida. Agora vai por `VK_EXT_layer_settings` (`validate_sync`), com `=nosync` para tirar e `=selftest` (uma chamada inválida e um WRITE_AFTER_WRITE de propósito) para provar que as duas chegam ao log (chegam). O messenger também procura o `VK_EXT_debug_utils` nas extensões da própria layer. Resultado, tudo com 0 mensagens: `menu`; `vila_sem_cena` a 720p, 1080p/60/V-Sync sem motion blur e 1600x900/30 sem V-Sync; `fase_direta` (L1_E3) a 1080p; o caminho que a Intel deve tomar (`CRASH_VULKAN_D32=1` + `CRASH_VULKAN_NO_EXT=depth_clip,depth_bias_control,maintenance5`); o modo seguro inteiro. Barreiras, layouts, acquire/present e reuso de imagens estão corretos pela especificação nesses caminhos.

**O que a validação não vê: a CPU à frente de uma GPU lenta.** Na RTX a GPU nunca atrasa (espera 0 ms por quadro); no iGPU ela é o gargalo. Para emular: `CRASH_VULKAN_GPU_LOAD=<k>` (um passe 64x64 com k mil iterações dependentes por pixel em cada quadro; 600 = ~10 ms, 1800 = ~16 ms de espera da CPU por quadro) e `CRASH_VULKAN_RING_MB=<n>` (anel de upload menor). Um primeiro jeito, atrasar cada submissão com um timeline semaphore sinalizado pelo host, é inválido: o present espera uma submissão cuja espera ainda não foi sinalizada e o driver da NVIDIA trava.

**Bug achado: o anel de upload liberava dados de um draw ainda não gravado.** O renderer manda índices, vértices, texturas e constantes de um draw antes de gravá-lo; se no meio vem uma submissão (anel cheio sem nada pendente -> `WaitIdle`; pool de sampler sets ou de descriptor sets dos passes cheio), o `OnSubmit` marcava o anel no head, a submissão terminava, o trecho era liberado e reaproveitado enquanto o draw, gravado no command buffer seguinte, ainda ia lê-lo. Com `CRASH_VULKAN_RING_MB=8`: geometria lixo e **device lost** em ~10 s. Agora `DrawImpl` e `DrawPass` seguram os uploads (`Device::HoldUploads`, `Ring::Hold/Release`, aninhável) e a submissão no meio marca o ponto do hold; com 8 MB foram 2.418 submissões no meio do quadro, sem erro e imagem correta. As constantes de efeito do `Draw` vão para antes da instância de rendering (um `WaitIdle` ali gravava o draw fora dela). Device lost numa espera agora é fatal com mensagem (antes, o laço do anel repetia `vkWaitSemaphores failed` e encheu 1 GB de log). Na vila a 1080p o anel chega a 47 MB, mesmo com a GPU lenta: com os 256 MB normais esse bug não deve disparar em jogo; só em GPUs com `maxStorageBufferRange` pequeno (o anel é limitado por ele) ou picos de upload grandes. Por isso não está provado que seja a causa da issue.

**Outras correções:** `VK_SUBOPTIMAL_KHR` repetido refazia a swap chain todo quadro (espera da GPU + imagens novas); agora só duas vezes seguidas, depois mantém e loga. O uso da swap chain fica restrito ao que a superfície suporta. A transição da imagem adquirida espera o semáforo de acquire também quando o estado anterior é `Present` (só acontecia com o modo seguro).

**Modo seguro e diagnóstico (para o reporter).** `[video] gpu_safe_mode = "on"` ou lista (`rhi::SafeMode`; `CRASH_GPU_SAFE` sobrepõe): `barriers` (toda barreira ALL_COMMANDS + MEMORY_READ/WRITE), `instances` (sem draws em instância de rendering maior), `serial` (a CPU espera a GPU a cada quadro), `memory` (uma alocação por imagem), `depth` (D32S8), `present` (sem immediate; mantém o conteúdo da imagem da swap chain), `cache` (ignora o `vulkan_pipelines.bin`). Na RTX, tudo ligado: 60 fps a 1080p, validação limpa. `[video] gpu_diagnostics = true` (`CRASH_GPU_DIAG`): extensões, recursos, tipos de memória, recursos por formato e a superfície no log, que também vai para `userdata\crashrecomp_log.txt` (o jogador não tem console); uma validation layer copiada para `vulkan_layers\` ao lado do exe é carregada (`VK_ADD_LAYER_PATH`), para rodar a validação na máquina do reporter. Sempre no log: driver (versão no formato do fabricante), heaps, limites, formatos sem um uso pedido, e a cada estatística swap chains / suboptimal / quadros não mostrados / pico do anel / submissões no meio do quadro / orçamento de memória.

**Conferido:** `vila_sem_cena` e `configuracoes` normais; os de validação acima; GPU lenta (1800) com anel de 256 e de 64 MB (sem terço preto nas capturas); modo seguro inteiro com validação; diagnóstico pelo `settings.toml` com a layer em `vulkan_layers\`.

**Pendente:** teste na máquina do reporter (build com modo seguro e diagnóstico). Sem Intel nem lavapipe/SwiftShader aqui (baixar o Mesa precisa de OK).
