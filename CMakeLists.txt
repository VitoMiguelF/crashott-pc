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
if (MINGW)
    set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")   # what clang-cl's /O2 means
    set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")
    add_link_options(-static)
    add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00)  # mingw-w64 defaults to older Windows APIs
    set(CRASH_ARCH_FLAGS -mavx)
    set(CRASH_XAUDIO2_LIB xaudio2_9)
else()
    set(CRASH_ARCH_FLAGS /arch:AVX)
    set(CRASH_XAUDIO2_LIB xaudio2)
endif()

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
add_executable(CrashRecomp ${CRASH_RUNTIME_SOURCES})
target_link_libraries(CrashRecomp PRIVATE CrashPPC XexImage ${CRASH_XAUDIO2_LIB} winmm version ole32
    hid setupapi windowscodecs)
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
else()
    target_link_options(CrashRecomp PRIVATE /MAP /DEBUG /OPT:REF /OPT:ICF)  # /DEBUG alone turns REF/ICF off
endif()
target_compile_definitions(CrashRecomp PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN)

# FFmpeg (LGPL, shared DLLs loaded at run time) for the XMA decoder: headers and DLLs of an LGPL
# build in thirdparty/ffmpeg (include/, bin/). Without it the runtime builds and XMA decodes to silence.
set(CRASH_FFMPEG_DIR ${CMAKE_SOURCE_DIR}/thirdparty/ffmpeg)
if (EXISTS ${CRASH_FFMPEG_DIR}/include/libavcodec/avcodec.h)
    target_include_directories(CrashRecomp PRIVATE ${CRASH_FFMPEG_DIR}/include)
    target_compile_definitions(CrashRecomp PRIVATE CRASH_HAVE_FFMPEG=1)
    file(GLOB CRASH_FFMPEG_DLLS ${CRASH_FFMPEG_DIR}/bin/*.dll)
    add_custom_command(TARGET CrashRecomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CRASH_FFMPEG_DLLS} $<TARGET_FILE_DIR:CrashRecomp>)
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
set(CRASH_DLSS_DIR ${CMAKE_SOURCE_DIR}/thirdparty/dlss)
if (EXISTS ${CRASH_DLSS_DIR}/include/nvsdk_ngx.h AND EXISTS ${CRASH_DLSS_DIR}/lib/nvsdk_ngx_s.lib)
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
if (EXISTS ${CRASH_DLSS_DIR}/bin/nvngx_dlss.dll)
    add_custom_command(TARGET CrashRecomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CRASH_DLSS_DIR}/bin/nvngx_dlss.dll $<TARGET_FILE_DIR:CrashRecomp>)
endif()
set(CRASH_FSR_DIR ${CMAKE_SOURCE_DIR}/thirdparty/fsr)
if (EXISTS ${CRASH_FSR_DIR}/include/ffx_api/vk/ffx_api_vk.h AND EXISTS ${CRASH_FSR_DIR}/bin/amd_fidelityfx_vk.dll)
    target_include_directories(CrashRecomp PRIVATE ${CRASH_FSR_DIR}/include)
    target_compile_definitions(CrashRecomp PRIVATE CRASH_HAVE_FSR=1)
    add_custom_command(TARGET CrashRecomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CRASH_FSR_DIR}/bin/amd_fidelityfx_vk.dll $<TARGET_FILE_DIR:CrashRecomp>)
else()
    message(STATUS "thirdparty/fsr not found: FSR disabled")
endif()

# Vulkan renderer (runtime/gpu/rhi_vulkan.cpp, the only one): the Khronos headers
# (thirdparty/vulkan, in git) with vulkan-1.dll loaded at run time (no import library), and DXC to
# compile HLSL to SPIR-V (thirdparty/dxc: dxcapi.h in git; dxcompiler.dll with SPIR-V support,
# loaded at run time and copied next to CrashRecomp.exe from thirdparty/dxc/bin or the Vulkan SDK).
target_include_directories(CrashRecomp PRIVATE ${CMAKE_SOURCE_DIR}/thirdparty/vulkan/include ${CMAKE_SOURCE_DIR}/thirdparty/dxc/include)
set(CRASH_DXC_DLL "")
if (EXISTS ${CMAKE_SOURCE_DIR}/thirdparty/dxc/bin/dxcompiler.dll)
    set(CRASH_DXC_DLL ${CMAKE_SOURCE_DIR}/thirdparty/dxc/bin/dxcompiler.dll)
elseif (DEFINED ENV{VULKAN_SDK} AND EXISTS "$ENV{VULKAN_SDK}/Bin/dxcompiler.dll")
    file(TO_CMAKE_PATH "$ENV{VULKAN_SDK}/Bin/dxcompiler.dll" CRASH_DXC_DLL)
endif()
if (CRASH_DXC_DLL)
    add_custom_command(TARGET CrashRecomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CRASH_DXC_DLL} $<TARGET_FILE_DIR:CrashRecomp>)
else()
    message(STATUS "dxcompiler.dll not found (thirdparty/dxc/bin or the Vulkan SDK): the Vulkan renderer can only use cached shaders")
endif()

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
target_link_libraries(CrashLauncher PRIVATE d3d11 dxgi d3dcompiler windowscodecs ole32 shell32 dwmapi)

# Installer (launcher/installer.inc): the launcher's look in front of tools/setup.py. The same
# sources with CRASH_INSTALLER; the release bundle ships it prebuilt as CrashSetup.exe (our code
# only: it reads the game's art from the dump the user picks, like the launcher).
get_target_property(CRASH_LAUNCHER_SOURCES CrashLauncher SOURCES)
add_executable(CrashSetup WIN32 ${CRASH_LAUNCHER_SOURCES})
target_include_directories(CrashSetup PRIVATE ${CRASH_IMGUI_DIR} ${CMAKE_SOURCE_DIR}/thirdparty/vulkan/include)
target_compile_definitions(CrashSetup PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS CRASH_INSTALLER)
target_compile_options(CrashSetup PRIVATE -Wno-deprecated-declarations -Wno-unused-function)
target_link_libraries(CrashSetup PRIVATE d3d11 dxgi d3dcompiler windowscodecs ole32 shell32 dwmapi advapi32 uuid bcrypt)

# The player's folder (dist\): CrashRecomp.exe + DLLs, languages\, data\ (optional, derived from the
# user's game), and mods\ / userdata\ / game\ kept as they are. `cmake --build build --target dist`
# or `python tools\make_dist.py` (see its --help).
find_package(Python3 COMPONENTS Interpreter)
if (Python3_FOUND)
    add_custom_target(dist
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/make_dist.py --exe $<TARGET_FILE:CrashRecomp>
        DEPENDS CrashRecomp CrashLauncher
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        USES_TERMINAL)
endif()
