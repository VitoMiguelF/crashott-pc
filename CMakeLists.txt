cmake_minimum_required(VERSION 3.20)
project(CrashRecomp CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "The recompiled code relies on Clang extensions; configure with clang (llvm-mingw) or clang-cl.")
endif()

# Two toolchains build the same code (tools/buildenv.py picks one):
# - llvm-mingw (the default; toolchain\llvm-mingw in the release zip, nothing to install): clang with
#   the mingw-w64 headers and import libraries and the UCRT. Everything links statically (libc++,
#   libunwind, winpthreads), so the exe needs no DLL of the toolchain.
# - clang-cl with Visual Studio / the Windows SDK (MSVC ABI; [build] toolchain = "msvc" in
#   config/local.toml).
# Elsewhere (Linux / Steam Deck, work in progress: docs/internals/linux_port.md) plain clang with
# SDL3 for the window, input and audio.
if (MINGW)
    set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")   # what clang-cl's /O2 means
    set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")
    add_link_options(-static)
    add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00)  # mingw-w64 defaults to older Windows APIs
    set(CRASH_ARCH_FLAGS -mavx)
    set(CRASH_XAUDIO2_LIB xaudio2_9)
elseif (WIN32)
    set(CRASH_ARCH_FLAGS /arch:AVX)
    set(CRASH_XAUDIO2_LIB xaudio2)
elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    # ARM64 (Android, docs/internals/android_port.md): SIMDe maps the recompiled code's vector ops
    # to NEON, which every ARM64 CPU has. char is unsigned there by default: keep x86's signed char.
    set(CRASH_ARCH_FLAGS -fsigned-char)
else()
    # x86-64 (the Steam Deck's Zen 2 has AVX2): the recompiled code's vector ops are SSE/AVX
    # through SIMDe.
    set(CRASH_ARCH_FLAGS -mavx)
endif()

# Only the installer and the launcher (CrashSetup, CrashLauncher): no recompiled code, so a machine
# without the game builds them (tools/setup.py --installer-only: the CI that makes the Linux AppImage).
option(CRASH_INSTALLER_ONLY "Build only CrashSetup and CrashLauncher (no generated/ppc needed)" OFF)

if (ANDROID)
    # Android (tools/android_build.py, docs/internals/android_port.md): the game is libmain.so,
    # loaded by SDL3's Java glue, with libSDL3.so built here from the SDL source (CRASH_SDL3_SOURCE).
    # Static libraries go into a shared one: position-independent code.
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    set(CRASH_SDL3_SOURCE "" CACHE PATH "SDL3 source tree (Android builds libSDL3.so from it)")
    if (NOT EXISTS ${CRASH_SDL3_SOURCE}/CMakeLists.txt)
        message(FATAL_ERROR "Android: set CRASH_SDL3_SOURCE to the SDL 3 source tree")
    endif()
    set(SDL_SHARED ON CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    add_subdirectory(${CRASH_SDL3_SOURCE} sdl3 EXCLUDE_FROM_ALL)
    find_package(Threads REQUIRED)
    set(CRASH_FREETYPE_INCLUDE "")  # no FreeType on Android yet: no console / subtitle text
elseif (NOT WIN32)
    # SDL3: thirdparty/sdl3 (setup.py: the Steam Runtime 3's headers and library, what a Steam Deck
    # builds with, having no SDL headers), else the system's.
    set(CRASH_SDL3_DIR ${CMAKE_SOURCE_DIR}/thirdparty/sdl3)
    if (EXISTS ${CRASH_SDL3_DIR}/include/SDL3/SDL.h AND EXISTS ${CRASH_SDL3_DIR}/lib/libSDL3.so.0)
        add_library(SDL3::SDL3 SHARED IMPORTED)
        set_target_properties(SDL3::SDL3 PROPERTIES IMPORTED_LOCATION ${CRASH_SDL3_DIR}/lib/libSDL3.so.0
            IMPORTED_SONAME libSDL3.so.0 INTERFACE_INCLUDE_DIRECTORIES ${CRASH_SDL3_DIR}/include)
    else()
        find_package(SDL3 REQUIRED CONFIG)
    endif()
    find_package(Threads REQUIRED)
    # Text of the developer console and the subtitles (platform/text_canvas_posix.cpp), and the
    # launcher's: FreeType's headers only (libfreetype / libfontconfig are loaded at run time):
    # thirdparty/freetype (setup.py, what a Steam Deck builds with), else the system's.
    set(CRASH_FREETYPE_INCLUDE "")
    if (EXISTS ${CMAKE_SOURCE_DIR}/thirdparty/freetype/include/ft2build.h)
        set(CRASH_FREETYPE_INCLUDE ${CMAKE_SOURCE_DIR}/thirdparty/freetype/include)
    else()
        find_package(PkgConfig)
        if (PkgConfig_FOUND)
            pkg_check_modules(CRASH_SYSTEM_FREETYPE freetype2)
            set(CRASH_FREETYPE_INCLUDE ${CRASH_SYSTEM_FREETYPE_INCLUDE_DIRS})
        endif()
    endif()
endif()

# The game: CrashRecomp and everything it links, down to the launcher's section.
if (NOT CRASH_INSTALLER_ONLY)

set(XENON_RECOMP_DIR ${CMAKE_SOURCE_DIR}/tools/XenonRecomp)

# Output of XenonRecomp (config/Crash.toml -> generated/ppc/). Regenerate before configuring.
set(CRASH_PPC_DIR ${CMAKE_SOURCE_DIR}/generated/ppc)
file(GLOB CRASH_PPC_SOURCES CONFIGURE_DEPENDS ${CRASH_PPC_DIR}/*.cpp)
if (NOT CRASH_PPC_SOURCES)
    message(FATAL_ERROR "generated/ppc/ is empty: run XenonRecomp config/Crash.toml first.")
endif()

# Recompiled game code. Built as a static library so it compiles without the
# kernel/XAM import implementations, which the runtime will provide later.
add_library(CrashPPC STATIC ${CRASH_PPC_SOURCES})
target_include_directories(CrashPPC PUBLIC
    ${CRASH_PPC_DIR}
    ${XENON_RECOMP_DIR}/thirdparty/simde)
set(CRASH_COMPILE_OPTIONS
    ${CRASH_ARCH_FLAGS}
    -fno-strict-aliasing
    -Wno-unused-label
    -Wno-unused-variable
    -Wno-unused-but-set-variable)
target_compile_options(CrashPPC PRIVATE ${CRASH_COMPILE_OPTIONS})

# XEX decryption at startup (runtime/xex_image/): XenonRecomp's XenonUtils (AES, basic and LZX
# compression) turns <game>\default.xex into the image the recompiled code expects, so no
# pre-made image.bin is needed. Its own library: XenonUtils' headers clash with the runtime's.
if (NOT CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER ${CMAKE_CXX_COMPILER})
endif()
enable_language(C)
set(XENON_THIRDPARTY_DIR ${XENON_RECOMP_DIR}/thirdparty)
add_library(XexImage STATIC
    ${CMAKE_SOURCE_DIR}/runtime/xex_image/xex_image.cpp
    ${XENON_RECOMP_DIR}/XenonUtils/xex.cpp
    ${XENON_RECOMP_DIR}/XenonUtils/image.cpp
    ${XENON_RECOMP_DIR}/XenonUtils/xex_patcher.cpp
    ${XENON_RECOMP_DIR}/XenonUtils/memory_mapped_file.cpp
    ${XENON_THIRDPARTY_DIR}/libmspack/libmspack/mspack/lzxd.c
    ${XENON_THIRDPARTY_DIR}/tiny-AES-c/aes.c)
target_include_directories(XexImage PRIVATE
    ${XENON_RECOMP_DIR}/XenonUtils
    ${XENON_THIRDPARTY_DIR}/simde
    ${XENON_THIRDPARTY_DIR}/libmspack/libmspack/mspack
    ${XENON_THIRDPARTY_DIR}/tiny-AES-c
    ${XENON_THIRDPARTY_DIR}/TinySHA1)
target_compile_definitions(XexImage PRIVATE NOMINMAX)
target_compile_options(XexImage PRIVATE -w)

# Runtime (runtime/): guest memory, XEX loader, kernel/XAM import stubs and the entry point.
# Nothing is baked in: the game folder, mods\, userdata\, languages\ and data\ are found next to
# the exe at run time, or given with --game / --mods / --userdata / --languages / --data
# (runtime/paths.h). The image is decrypted from <game>\default.xex at startup.
file(GLOB_RECURSE CRASH_RUNTIME_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/runtime/*.cpp)
list(FILTER CRASH_RUNTIME_SOURCES EXCLUDE REGEX "/runtime/xex_image/|/runtime/gpu/dlss_module/")
# OS backends (runtime/platform/ and the window/input/audio ones): *_win32.cpp on Windows,
# *_posix.cpp and *_sdl.cpp (SDL3) elsewhere. See docs/internals/linux_port.md.
if (WIN32)
    list(FILTER CRASH_RUNTIME_SOURCES EXCLUDE REGEX "_(posix|sdl)\\.cpp$")
else()
    list(FILTER CRASH_RUNTIME_SOURCES EXCLUDE REGEX "_win32\\.cpp$")
endif()
# *_android.cpp: Android only (the SDL_main entry).
if (NOT ANDROID)
    list(FILTER CRASH_RUNTIME_SOURCES EXCLUDE REGEX "_android\\.cpp$")
endif()
if (ANDROID)
    add_library(CrashRecomp SHARED ${CRASH_RUNTIME_SOURCES})
    set_target_properties(CrashRecomp PROPERTIES OUTPUT_NAME main)  # libmain.so, what SDLActivity loads
else()
    add_executable(CrashRecomp ${CRASH_RUNTIME_SOURCES})
endif()
if (WIN32)
    target_link_libraries(CrashRecomp PRIVATE CrashPPC XexImage ${CRASH_XAUDIO2_LIB} winmm version ole32
        hid setupapi windowscodecs)
elseif (ANDROID)
    target_link_libraries(CrashRecomp PRIVATE CrashPPC XexImage SDL3::SDL3-shared Threads::Threads log android ${CMAKE_DL_LIBS})
    target_include_directories(CrashRecomp PRIVATE ${CMAKE_SOURCE_DIR}/thirdparty/stb)
    # Only SDL_main is exported; every symbol must resolve at link time (a .so would defer it to
    # load time, where a missing one only shows on the phone); 16 KB pages (android_port.md).
    set_target_properties(CrashRecomp CrashPPC XexImage PROPERTIES CXX_VISIBILITY_PRESET hidden C_VISIBILITY_PRESET hidden)
    target_link_options(CrashRecomp PRIVATE -Wl,--no-undefined -Wl,--exclude-libs,ALL -Wl,-z,max-page-size=16384)
    # libadrenotools (BSD-2, CRASH_ADRENOTOOLS_SOURCE, optional): loads Mesa's Turnip in place of an
    # Adreno's own driver when that one is older than Vulkan 1.3 (android_main_android.cpp). Its
    # hook libraries (libhook_impl.so, libmain_hook.so) go into the APK next to libmain.so.
    set(CRASH_ADRENOTOOLS_SOURCE "" CACHE PATH "libadrenotools source tree (Turnip on old Adreno drivers)")
    if (EXISTS ${CRASH_ADRENOTOOLS_SOURCE}/CMakeLists.txt)
        add_subdirectory(${CRASH_ADRENOTOOLS_SOURCE} adrenotools EXCLUDE_FROM_ALL)
        target_link_libraries(CrashRecomp PRIVATE adrenotools)
        target_compile_definitions(CrashRecomp PRIVATE CRASH_ADRENOTOOLS=1)
        target_link_options(hook_impl PRIVATE -Wl,-z,max-page-size=16384)
        target_link_options(main_hook PRIVATE -Wl,-z,max-page-size=16384)
        add_dependencies(CrashRecomp hook_impl main_hook)
    endif()
else()
    # tools/linux_sdl3.py's build (every backend by dlopen, glibc 2.29) goes next to the exe; the
    # Steam Runtime's (setup.py's .debs) doesn't: it needs libraries a bare SteamOS may lack.
    if (EXISTS ${CRASH_SDL3_DIR}/.portable AND EXISTS ${CRASH_SDL3_DIR}/lib/libSDL3.so.0)
        add_custom_command(TARGET CrashRecomp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CRASH_SDL3_DIR}/lib/libSDL3.so.0 $<TARGET_FILE_DIR:CrashRecomp>)
    endif()
    target_link_libraries(CrashRecomp PRIVATE CrashPPC XexImage SDL3::SDL3 Threads::Threads ${CMAKE_DL_LIBS})
    # The .so files next to the exe (FFmpeg's, DXC, an SDL3 shipped with the game): $ORIGIN. DT_RPATH
    # where the linker honours --disable-new-dtags; zig's writes RUNPATH, which comes after the
    # LD_LIBRARY_PATH Steam sets and doesn't reach dlopen'ed libraries' dependencies (audio/ffmpeg.cpp
    # loads libswresample itself for that).
    # Only that: CMake's own build rpath (thirdparty/sdl3/lib) would put the Steam Runtime's libSDL3,
    # linked against libraries a bare SteamOS may lack, before Steam's and the system's.
    target_link_options(CrashRecomp PRIVATE "-Wl,-rpath,\$ORIGIN" -Wl,--disable-new-dtags)
    set_target_properties(CrashRecomp PROPERTIES SKIP_BUILD_RPATH ON)
    target_include_directories(CrashRecomp PRIVATE ${CMAKE_SOURCE_DIR}/thirdparty/stb)  # PNG (platform/image_posix.cpp)
    # FreeType (found above): without its headers those overlays are not drawn.
    if (CRASH_FREETYPE_INCLUDE)
        target_include_directories(CrashRecomp PRIVATE ${CRASH_FREETYPE_INCLUDE})
        target_compile_definitions(CrashRecomp PRIVATE CRASH_HAVE_FREETYPE=1)
    else()
        message(STATUS "FreeType headers not found: no developer console or subtitles text")
    endif()
endif()
target_compile_options(CrashRecomp PRIVATE ${CRASH_COMPILE_OPTIONS})
# Symbol map (build\CrashRecomp.map) and line tables in a PDB (build\CrashRecomp.pdb): name the
# sampled addresses of tools/perf/profile.py down to the source line (-gline-tables-only doesn't
# change the generated code; the recompiled CrashPPC library has none).
target_compile_options(CrashRecomp PRIVATE -gline-tables-only)
if (MINGW)
    # CodeView line tables + a PDB like clang-cl's, and lld-link's MSVC-style map (/map, not --Map).
    target_compile_options(CrashRecomp PRIVATE -gcodeview)
    target_link_options(CrashRecomp PRIVATE -Wl,--pdb= -Wl,--Xlink=/map:$<TARGET_FILE_DIR:CrashRecomp>/CrashRecomp.map
        -Wl,--gc-sections -Wl,--icf=all)
elseif (WIN32)
    target_link_options(CrashRecomp PRIVATE /MAP /DEBUG /OPT:REF /OPT:ICF)  # /DEBUG alone turns REF/ICF off
else()
    target_link_options(CrashRecomp PRIVATE -Wl,--gc-sections)
    # The map file where the linker takes it (zig's driver refuses -Map: the Steam Deck build has none).
    include(CheckLinkerFlag)
    check_linker_flag(CXX "-Wl,-Map=${CMAKE_BINARY_DIR}/check.map" CRASH_LINKER_MAP)
    if (CRASH_LINKER_MAP)
        target_link_options(CrashRecomp PRIVATE -Wl,-Map=$<TARGET_FILE_DIR:CrashRecomp>/CrashRecomp.map)
    endif()
endif()
target_compile_definitions(CrashRecomp PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN)

# FFmpeg (LGPL, shared DLLs loaded at run time) for the XMA decoder: headers and DLLs of an LGPL
# build in thirdparty/ffmpeg (include/, bin/). Without it the runtime builds and XMA decodes to silence.
set(CRASH_FFMPEG_DIR ${CMAKE_SOURCE_DIR}/thirdparty/ffmpeg)
set(CRASH_FFMPEG_INCLUDE ${CRASH_FFMPEG_DIR}/include)
if (NOT WIN32 AND NOT EXISTS ${CRASH_FFMPEG_DIR}/lib)
    # No bundled .so files: the system's libavcodec is loaded, so build against its headers (the
    # structures are used directly and the major version must match; thirdparty/ffmpeg's are FFmpeg 8).
    find_path(CRASH_SYSTEM_FFMPEG_INCLUDE libavcodec/avcodec.h)
    if (CRASH_SYSTEM_FFMPEG_INCLUDE)
        set(CRASH_FFMPEG_INCLUDE ${CRASH_SYSTEM_FFMPEG_INCLUDE})
    endif()
endif()
if (EXISTS ${CRASH_FFMPEG_INCLUDE}/libavcodec/avcodec.h)
    target_include_directories(CrashRecomp PRIVATE ${CRASH_FFMPEG_INCLUDE})
    target_compile_definitions(CrashRecomp PRIVATE CRASH_HAVE_FFMPEG=1)
    if (WIN32)
        file(GLOB CRASH_FFMPEG_DLLS ${CRASH_FFMPEG_DIR}/bin/*.dll)
    else()
        file(GLOB CRASH_FFMPEG_DLLS ${CRASH_FFMPEG_DIR}/lib/*.so.*)  # else the system's libavcodec.so.62
    endif()
    if (CRASH_FFMPEG_DLLS)
        add_custom_command(TARGET CrashRecomp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CRASH_FFMPEG_DLLS} $<TARGET_FILE_DIR:CrashRecomp>)
    endif()
else()
    message(STATUS "thirdparty/ffmpeg not found: XMA audio will decode to silence")
endif()

# Temporal upscalers (runtime/gpu/upscaler.cpp), both optional and loaded at run time:
# - NVIDIA DLSS: CrashDlss.dll (runtime/gpu/dlss_module, our code + the static nvsdk_ngx_s.lib of the
#   NGX SDK) and NVIDIA's nvngx_dlss.dll, both next to CrashRecomp.exe. With the SDK in
#   thirdparty/dlss (include/, lib/) the DLL is built here; the installer instead carries it prebuilt
#   in thirdparty/dlss/bin, so building the game needs no SDK file. On a PC without an NVIDIA RTX GPU
#   the installer leaves both DLLs out.
# - AMD FSR 3.1 (FidelityFX API, Vulkan backend, thirdparty/fsr: include/ffx_api headers and the
#   signed bin/amd_fidelityfx_vk.dll, loaded at run time and copied next to CrashRecomp.exe).
# On Linux both come prebuilt (made once on the release machine: tools/linux_dlss.py builds
# libCrashDlss.so against NGX's Linux library and fetches NVIDIA's libnvidia-ngx-dlss.so.*) and are
# copied next to the exe when thirdparty/dlss/bin has them.
set(CRASH_DLSS_DIR ${CMAKE_SOURCE_DIR}/thirdparty/dlss)
if (NOT WIN32)
    file(GLOB CRASH_DLSS_LINUX ${CRASH_DLSS_DIR}/bin/libCrashDlss.so ${CRASH_DLSS_DIR}/bin/libnvidia-ngx-dlss.so.*)
    list(LENGTH CRASH_DLSS_LINUX CRASH_DLSS_LINUX_COUNT)
    if (CRASH_DLSS_LINUX_COUNT EQUAL 2)
        add_custom_command(TARGET CrashRecomp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CRASH_DLSS_LINUX} $<TARGET_FILE_DIR:CrashRecomp>)
    else()
        message(STATUS "thirdparty/dlss/bin has no libCrashDlss.so + libnvidia-ngx-dlss.so.* (tools/linux_dlss.py): DLSS disabled")
    endif()
elseif (EXISTS ${CRASH_DLSS_DIR}/include/nvsdk_ngx.h AND EXISTS ${CRASH_DLSS_DIR}/lib/nvsdk_ngx_s.lib)
    add_library(CrashDlss SHARED
        ${CMAKE_SOURCE_DIR}/runtime/gpu/dlss_module/crash_dlss.cpp
        ${CMAKE_SOURCE_DIR}/runtime/gpu/dlss_module/ngx_msvc_compat.cpp)
    set_target_properties(CrashDlss PROPERTIES PREFIX "" RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    target_include_directories(CrashDlss PRIVATE ${CRASH_DLSS_DIR}/include ${CMAKE_SOURCE_DIR}/thirdparty/vulkan/include)
    target_compile_definitions(CrashDlss PRIVATE CRASH_HAVE_DLSS=1 NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(CrashDlss PRIVATE ${CRASH_DLSS_DIR}/lib/nvsdk_ngx_s.lib)
    if (MINGW)
        target_link_options(CrashDlss PRIVATE -Wl,--Xlink=/nodefaultlib:LIBCMT -Wl,--Xlink=/nodefaultlib:OLDNAMES
            -Wl,--Xlink=/nodefaultlib:libcpmt)
    endif()
    add_dependencies(CrashRecomp CrashDlss)
elseif (EXISTS ${CRASH_DLSS_DIR}/bin/CrashDlss.dll)
    add_custom_command(TARGET CrashRecomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CRASH_DLSS_DIR}/bin/CrashDlss.dll $<TARGET_FILE_DIR:CrashRecomp>)
else()
    message(STATUS "thirdparty/dlss not found (nor a prebuilt bin/CrashDlss.dll): DLSS disabled")
endif()
if (WIN32 AND EXISTS ${CRASH_DLSS_DIR}/bin/nvngx_dlss.dll)
    add_custom_command(TARGET CrashRecomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CRASH_DLSS_DIR}/bin/nvngx_dlss.dll $<TARGET_FILE_DIR:CrashRecomp>)
endif()
set(CRASH_FSR_DIR ${CMAKE_SOURCE_DIR}/thirdparty/fsr)
if (WIN32 AND EXISTS ${CRASH_FSR_DIR}/include/ffx_api/vk/ffx_api_vk.h AND EXISTS ${CRASH_FSR_DIR}/bin/amd_fidelityfx_vk.dll)
    target_include_directories(CrashRecomp PRIVATE ${CRASH_FSR_DIR}/include)
    target_compile_definitions(CrashRecomp PRIVATE CRASH_HAVE_FSR=1)
    add_custom_command(TARGET CrashRecomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CRASH_FSR_DIR}/bin/amd_fidelityfx_vk.dll $<TARGET_FILE_DIR:CrashRecomp>)
elseif (WIN32)
    message(STATUS "thirdparty/fsr not found: FSR disabled")
endif()

# Vulkan renderer (runtime/gpu/rhi_vulkan.cpp, the only one): the Khronos headers
# (thirdparty/vulkan, in git) with vulkan-1.dll loaded at run time (no import library), and DXC to
# compile HLSL to SPIR-V (thirdparty/dxc: dxcapi.h in git; dxcompiler.dll with SPIR-V support,
# loaded at run time and copied next to CrashRecomp.exe from thirdparty/dxc/bin or the Vulkan SDK).
target_include_directories(CrashRecomp PRIVATE ${CMAKE_SOURCE_DIR}/thirdparty/vulkan/include ${CMAKE_SOURCE_DIR}/thirdparty/dxc/include)
set(CRASH_DXC_DLL "")
if (WIN32)
    set(CRASH_DXC_NAME dxcompiler.dll)
    set(CRASH_DXC_SDK_DIR Bin)
else()
    set(CRASH_DXC_NAME libdxcompiler.so)  # the Linux release's lib/libdxcompiler.so
    set(CRASH_DXC_SDK_DIR lib)
endif()
if (EXISTS ${CMAKE_SOURCE_DIR}/thirdparty/dxc/bin/${CRASH_DXC_NAME})
    set(CRASH_DXC_DLL ${CMAKE_SOURCE_DIR}/thirdparty/dxc/bin/${CRASH_DXC_NAME})
elseif (DEFINED ENV{VULKAN_SDK} AND EXISTS "$ENV{VULKAN_SDK}/${CRASH_DXC_SDK_DIR}/${CRASH_DXC_NAME}")
    file(TO_CMAKE_PATH "$ENV{VULKAN_SDK}/${CRASH_DXC_SDK_DIR}/${CRASH_DXC_NAME}" CRASH_DXC_DLL)
endif()
if (CRASH_DXC_DLL)
    add_custom_command(TARGET CrashRecomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CRASH_DXC_DLL} $<TARGET_FILE_DIR:CrashRecomp>)
else()
    message(STATUS "${CRASH_DXC_NAME} not found (thirdparty/dxc/bin or the Vulkan SDK): the Vulkan renderer can only use cached shaders")
endif()

endif()  # NOT CRASH_INSTALLER_ONLY: the game

# The launcher and the installer: Win32 + Direct3D 11 on Windows, SDL3 + SDL_Renderer elsewhere (the
# same launcher/ sources; docs/internals/linux_port.md).
if (WIN32)
    # Launcher (launcher/): edits settings.toml with the game's own look before starting the game.
    # Dear ImGui (thirdparty/imgui, MIT) on Direct3D 11, plus the runtime's own settings, paths, VFS
    # and the code that reads the fonts and the frontend art from the user's default.rcf.
    set(CRASH_IMGUI_DIR ${CMAKE_SOURCE_DIR}/thirdparty/imgui)
    enable_language(RC)  # launcher.rc: the icon (launcher/crashrecomp.ico, tools/make_icon.py)
    add_executable(CrashLauncher WIN32
        ${CMAKE_SOURCE_DIR}/launcher/launcher.cpp
        ${CMAKE_SOURCE_DIR}/launcher/launcher.rc
        ${CMAKE_SOURCE_DIR}/launcher/launcher_stubs.cpp
        ${CRASH_IMGUI_DIR}/imgui.cpp
        ${CRASH_IMGUI_DIR}/imgui_draw.cpp
        ${CRASH_IMGUI_DIR}/imgui_tables.cpp
        ${CRASH_IMGUI_DIR}/imgui_widgets.cpp
        ${CRASH_IMGUI_DIR}/backends/imgui_impl_win32.cpp
        ${CRASH_IMGUI_DIR}/backends/imgui_impl_dx11.cpp
        ${CMAKE_SOURCE_DIR}/runtime/paths.cpp
        ${CMAKE_SOURCE_DIR}/runtime/platform/platform_win32.cpp
        ${CMAKE_SOURCE_DIR}/runtime/platform/file_win32.cpp
        ${CMAKE_SOURCE_DIR}/runtime/platform/image_win32.cpp
        ${CMAKE_SOURCE_DIR}/runtime/fs/vfs.cpp
        ${CMAKE_SOURCE_DIR}/runtime/gpu/png_writer.cpp
        ${CMAKE_SOURCE_DIR}/runtime/settings/settings.cpp
        ${CMAKE_SOURCE_DIR}/runtime/settings/controls.cpp
        ${CMAKE_SOURCE_DIR}/runtime/settings/menu_art.cpp
        ${CMAKE_SOURCE_DIR}/runtime/settings/language_font.cpp
        ${CMAKE_SOURCE_DIR}/runtime/settings/language_pack.cpp
        ${CMAKE_SOURCE_DIR}/runtime/settings/options.cpp
        ${CMAKE_SOURCE_DIR}/runtime/settings/hd_remaster.cpp)
    target_include_directories(CrashLauncher PRIVATE ${CRASH_IMGUI_DIR} ${CMAKE_SOURCE_DIR}/thirdparty/vulkan/include)
    target_compile_definitions(CrashLauncher PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    target_compile_options(CrashLauncher PRIVATE -Wno-deprecated-declarations)
    target_link_libraries(CrashLauncher PRIVATE d3d11 dxgi d3dcompiler windowscodecs ole32 shell32 dwmapi uuid bcrypt winhttp winmm version)

    # Installer (launcher/installer.inc): the launcher's look in front of tools/setup.py. The same
    # sources with CRASH_INSTALLER; the release bundle ships it prebuilt as CrashSetup.exe (our code
    # only: it reads the game's art from the dump the user picks, like the launcher).
    get_target_property(CRASH_LAUNCHER_SOURCES CrashLauncher SOURCES)
    add_executable(CrashSetup WIN32 ${CRASH_LAUNCHER_SOURCES})
    target_include_directories(CrashSetup PRIVATE ${CRASH_IMGUI_DIR} ${CMAKE_SOURCE_DIR}/thirdparty/vulkan/include)
    target_compile_definitions(CrashSetup PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS CRASH_INSTALLER)
    target_compile_options(CrashSetup PRIVATE -Wno-deprecated-declarations -Wno-unused-function)
    target_link_libraries(CrashSetup PRIVATE d3d11 dxgi d3dcompiler windowscodecs ole32 shell32 dwmapi advapi32 uuid bcrypt winmm version)
    set(CRASH_DIST_LAUNCHER CrashLauncher)
elseif (ANDROID)
    # No launcher or installer on Android: the PC builds the APK (tools/android_build.py).
else()
    # Linux: the same screens with SDL3 (window, input, dialogs) and Dear ImGui's SDL3 + SDL_Renderer
    # backends; the system font through fontconfig (platform/text_canvas_posix.cpp, dlopen'ed).
    set(CRASH_IMGUI_DIR ${CMAKE_SOURCE_DIR}/thirdparty/imgui)
    set(CRASH_LAUNCHER_LINUX_SOURCES
        ${CMAKE_SOURCE_DIR}/launcher/launcher.cpp
        ${CMAKE_SOURCE_DIR}/launcher/launcher_stubs.cpp
        ${CRASH_IMGUI_DIR}/imgui.cpp
        ${CRASH_IMGUI_DIR}/imgui_draw.cpp
        ${CRASH_IMGUI_DIR}/imgui_tables.cpp
        ${CRASH_IMGUI_DIR}/imgui_widgets.cpp
        ${CRASH_IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
        ${CRASH_IMGUI_DIR}/backends/imgui_impl_sdlrenderer3.cpp
        ${CMAKE_SOURCE_DIR}/runtime/paths.cpp
        ${CMAKE_SOURCE_DIR}/runtime/platform/platform_posix.cpp
        ${CMAKE_SOURCE_DIR}/runtime/platform/platform_sdl.cpp
        ${CMAKE_SOURCE_DIR}/runtime/platform/file_posix.cpp
        ${CMAKE_SOURCE_DIR}/runtime/platform/image_posix.cpp
        ${CMAKE_SOURCE_DIR}/runtime/platform/text_canvas_posix.cpp
        ${CMAKE_SOURCE_DIR}/runtime/fs/vfs.cpp
        ${CMAKE_SOURCE_DIR}/runtime/gpu/png_writer.cpp
        ${CMAKE_SOURCE_DIR}/runtime/settings/settings.cpp
        ${CMAKE_SOURCE_DIR}/runtime/settings/controls.cpp
        ${CMAKE_SOURCE_DIR}/runtime/settings/menu_art.cpp
        ${CMAKE_SOURCE_DIR}/runtime/settings/language_font.cpp
        ${CMAKE_SOURCE_DIR}/runtime/settings/language_pack.cpp
        ${CMAKE_SOURCE_DIR}/runtime/settings/options.cpp
        ${CMAKE_SOURCE_DIR}/runtime/settings/hd_remaster.cpp)
    foreach(CRASH_TARGET CrashLauncher CrashSetup)
        add_executable(${CRASH_TARGET} ${CRASH_LAUNCHER_LINUX_SOURCES})
        target_include_directories(${CRASH_TARGET} PRIVATE ${CRASH_IMGUI_DIR} ${CMAKE_SOURCE_DIR}/thirdparty/vulkan/include
            ${CMAKE_SOURCE_DIR}/thirdparty/stb ${CRASH_FREETYPE_INCLUDE})
        if (CRASH_FREETYPE_INCLUDE)
            target_compile_definitions(${CRASH_TARGET} PRIVATE CRASH_HAVE_FREETYPE=1)
        endif()
        # std::filesystem::u8path is deprecated in C++20, as on Windows (the same sources use it there).
        target_compile_options(${CRASH_TARGET} PRIVATE -Wno-deprecated-declarations)
        target_link_libraries(${CRASH_TARGET} PRIVATE SDL3::SDL3 Threads::Threads ${CMAKE_DL_LIBS})
        # libSDL3.so.0 next to the exe (the installed launcher), or in ../lib (the installer in its AppImage).
        target_link_options(${CRASH_TARGET} PRIVATE "-Wl,-rpath,\$ORIGIN:\$ORIGIN/../lib" -Wl,--disable-new-dtags -Wl,--gc-sections)
        set_target_properties(${CRASH_TARGET} PROPERTIES SKIP_BUILD_RPATH ON)
    endforeach()
    target_compile_definitions(CrashSetup PRIVATE CRASH_INSTALLER)
    set(CRASH_DIST_LAUNCHER CrashLauncher)
endif()

# The player's folder (dist\): CrashRecomp.exe + DLLs, languages\, data\ (optional, derived from the
# user's game), and mods\ / userdata\ / game\ kept as they are. `cmake --build build --target dist`
# or `python tools\make_dist.py` (see its --help).
find_package(Python3 COMPONENTS Interpreter)
if (Python3_FOUND AND NOT CRASH_INSTALLER_ONLY AND NOT ANDROID)
    add_custom_target(dist
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/make_dist.py --exe $<TARGET_FILE:CrashRecomp>
        DEPENDS CrashRecomp ${CRASH_DIST_LAUNCHER}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        USES_TERMINAL)
endif()
