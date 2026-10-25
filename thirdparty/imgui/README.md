# Dear ImGui

Dear ImGui 1.91.8 (MIT, `LICENSE.txt`), from https://github.com/ocornut/imgui/archive/refs/tags/v1.91.8.zip
(downloaded 2026-10-21). Only the core and the Win32 + DirectX 11 backends were kept; the SDL3 and
SDL_Renderer 3 backends (`backends/imgui_impl_sdl3.*`, `imgui_impl_sdlrenderer3.*`) of the same tag were added
2026-10-25 for the Linux launcher and installer (raw.githubusercontent.com/ocornut/imgui/v1.91.8/backends/).

Used by the launcher (`launcher/`, CMake target `CrashLauncher`), compiled with the rest of the project.
