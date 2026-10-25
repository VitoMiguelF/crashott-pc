#include "overlay.h"
#include "upscaler.h"
#include "overlay_font.h"
#include "window.h"
#include "../diagnostics.h"
#include "../kernel/input.h"
#include "../settings/settings.h"
#include "../platform/platform.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>

namespace gpu::overlay
{
    namespace
    {
        // Layout in overlay pixels (1x; the pixel shader scales it up by an integer factor).
        constexpr int PAD = 4;
        constexpr int CELL_W = overlay_font::WIDTH + 1;
        constexpr int LINE_H = overlay_font::HEIGHT + 3;
        constexpr int COLUMNS = 40;           // the widest line (the image is as wide as its text)
        constexpr int MIN_GRAPH_COLUMNS = 34; // the graph is at least this wide
        constexpr int TEXT_LINES = 10;        // the most lines (full mode in a profile run, upscaler, replay, recording)
        constexpr int GRAPH_H = 40;
        constexpr int IMAGE_W = PAD * 2 + COLUMNS * CELL_W;
        constexpr int IMAGE_H = PAD * 2 + TEXT_LINES * LINE_H + 2 + GRAPH_H;
        constexpr int GRAPH_W = IMAGE_W - PAD * 2; // frame-time history kept (one column per frame)
        constexpr double GRAPH_MAX_MS = 66.7;
        constexpr int STATS_WINDOW = 240; // frames for average / max / 1% low
        constexpr uint32_t UPLOAD_SLICES = 4; // > frames in flight (Swap keeps at most 2 queued)
        constexpr uint64_t UPLOAD_SLICE_SIZE = (uint64_t(IMAGE_W) * IMAGE_H * 4 + 255) & ~uint64_t(255);
        constexpr double TOAST_SECONDS = 2.5;
        constexpr int MAX_IMAGE_SIDE = 512;  // DrawImage
        constexpr uint64_t IMAGE_SLICE_SIZE = uint64_t(MAX_IMAGE_SIDE) * MAX_IMAGE_SIDE * 4;

        // RGBA8 in memory order (R in the low byte).
        constexpr uint32_t Rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return r | (g << 8) | (b << 16) | (a << 24); }
        constexpr uint32_t BACKGROUND = Rgba(0, 0, 0, 170);
        constexpr uint32_t TEXT = Rgba(240, 240, 240, 255);
        constexpr uint32_t TOAST = Rgba(110, 220, 255, 255);
        constexpr uint32_t PROFILE = Rgba(255, 200, 90, 255);
        constexpr uint32_t RECORDING = Rgba(255, 70, 70, 255);
        constexpr uint32_t GRAPH_BACKGROUND = Rgba(40, 40, 40, 200);
        constexpr uint32_t GUIDE = Rgba(140, 140, 140, 255);
        constexpr uint32_t BAR_GOOD = Rgba(90, 220, 90, 255);
        constexpr uint32_t BAR_SLOW = Rgba(240, 200, 60, 255);
        constexpr uint32_t BAR_HITCH = Rgba(240, 70, 60, 255);

        const char* OVERLAY_VS = R"(
float4 main(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
)";
        const char* OVERLAY_PS = R"(
StructuredBuffer<uint> pixels : register(t0);
cbuffer Overlay : register(b0)
{
    int4 rect;  // x, y on the screen; width, height of the image
    int scale;
};
float4 main(float4 position : SV_Position) : SV_Target
{
    int2 p = (int2(position.xy) - rect.xy) / scale;
    if (any(p < 0) || any(p >= rect.zw))
        discard;
    // Helper lanes of quads on the image's edge still load: keep them inside the image (a root
    // SRV has no bounds, and a read past the buffer hangs the GPU).
    p = clamp(p, int2(0, 0), rect.zw - 1);
    uint v = pixels[p.y * rect.z + p.x];
    return float4(v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, v >> 24) / 255.0;
}
)";

        // Drawing resources.
        bool g_ready = false;
        rhi::Device* g_device = nullptr;
        rhi::Pipeline* g_pipeline = nullptr;
        rhi::Buffer* g_upload = nullptr;
        rhi::Texture* g_target = nullptr;  // BeginPresent
        uint32_t g_slice = 0;
        std::vector<uint32_t> g_image(size_t(IMAGE_W) * IMAGE_H);
        int g_imageW = IMAGE_W, g_imageH = IMAGE_H;  // the current mode's size
        rhi::Buffer* g_imageUpload = nullptr;  // DrawImage, created on first use
        uint32_t g_imageSlice = 0;

        // Settings.
        settings::OverlayMode g_mode = settings::OverlayMode::Off;
        std::string g_profile;  // CRASH_PROFILE (tools/run_game.py), shown in full mode
        bool g_inScreenshots = false;
        char g_corner[3] = "tl";
        int g_scaleOverride = 0;
        std::filesystem::path g_marksPath;
        FILE* g_frameStats = nullptr;

        // Counters.
        int64_t g_frequency{};
        int64_t g_lastFrame{};
        uint64_t g_frame = 0;
        double g_lastMs = 0.0;
        std::deque<double> g_history;           // frame times (ms), newest at the back
        std::deque<int64_t> g_swapTimes;  // swaps in the last second, for FPS
        double g_fps = 0.0;
        int64_t g_fpsUpdated{};
        double g_averageMs = 0.0, g_maxMs = 0.0, g_lowMs = 0.0;
        uint32_t g_marks = 0;
        std::string g_toast;
        int64_t g_toastTime{};
        std::string g_notice;  // Notice: shown in every mode
        int64_t g_noticeTime{};

        bool EnvFlag(const char* name)
        {
            const char* value = getenv(name);
            return value && *value && strcmp(value, "0") != 0;
        }

        double Seconds(int64_t from, int64_t to)
        {
            return double(to - from) / double(g_frequency);
        }

        // Input clock in seconds with one decimal, or "--" before the first controller read.
        std::string ClockText()
        {
            int64_t ms = input::ClockMs();
            if (ms < 0)
                return "--";
            char text[32];
            snprintf(text, sizeof(text), "%.1f", double(ms) / 1000.0);
            return text;
        }

        void Toast(std::string text)
        {
            g_toast = std::move(text);
            g_toastTime = int64_t(plat::PerfCounter());
        }

        void UpdateWindowStats()
        {
            if (g_history.empty())
                return;
            size_t count = std::min<size_t>(g_history.size(), STATS_WINDOW);
            std::vector<double> window(g_history.end() - count, g_history.end());
            double sum = 0.0;
            for (double ms : window)
                sum += ms;
            g_averageMs = sum / double(count);
            g_maxMs = *std::max_element(window.begin(), window.end());
            // 1% low: the frame time 99% of the frames stay under.
            size_t index = std::min(count - 1, size_t(std::ceil(double(count) * 0.99)) - 1);
            std::nth_element(window.begin(), window.begin() + index, window.end());
            g_lowMs = window[index];
        }

        // Rasterization ---------------------------------------------------------------------
        // The image is g_imageW x g_imageH (it depends on the mode), stored with that stride.

        void Fill(int x0, int y0, int x1, int y1, uint32_t color)
        {
            x0 = std::max(x0, 0), y0 = std::max(y0, 0);
            x1 = std::min(x1, g_imageW), y1 = std::min(y1, g_imageH);
            for (int y = y0; y < y1; y++)
                std::fill(g_image.begin() + y * g_imageW + x0, g_image.begin() + y * g_imageW + std::max(x0, x1), color);
        }

        void Text(int column, int line, const std::string& text, uint32_t color)
        {
            int x = PAD + column * CELL_W, y = PAD + line * LINE_H + 1;
            for (char ch : text)
            {
                if (const char* rows = overlay_font::Find(ch))
                {
                    for (int row = 0; row < overlay_font::HEIGHT; row++)
                    {
                        for (int col = 0; col < overlay_font::WIDTH; col++)
                        {
                            int px = x + col, py = y + row;
                            if (rows[row * overlay_font::WIDTH + col] == '#' && px < g_imageW && py < g_imageH)
                                g_image[py * g_imageW + px] = color;
                        }
                    }
                }
                x += CELL_W;
            }
        }

        void Graph(int top)
        {
            // Frame-time graph: one column per frame, newest on the right; guides at 16.7 and 33.3 ms.
            int width = g_imageW - PAD * 2, bottom = top + GRAPH_H;
            Fill(PAD, top, PAD + width, bottom, GRAPH_BACKGROUND);
            size_t count = std::min<size_t>(g_history.size(), size_t(width));
            for (size_t i = 0; i < count; i++)
            {
                double ms = g_history[g_history.size() - count + i];
                int height = std::clamp(int(std::lround(ms / GRAPH_MAX_MS * GRAPH_H)), 1, GRAPH_H);
                uint32_t color = ms <= 34.5 ? BAR_GOOD : ms <= 50.0 ? BAR_SLOW : BAR_HITCH;
                int x = PAD + width - int(count) + int(i);
                Fill(x, bottom - height, x + 1, bottom, color);
            }
            for (double guide : { 1000.0 / 60.0, 1000.0 / 30.0 })
            {
                int y = bottom - int(std::lround(guide / GRAPH_MAX_MS * GRAPH_H));
                for (int x = PAD; x < PAD + width; x += 2)
                    g_image[y * g_imageW + x] = GUIDE;
            }
        }

        // "RENDER 2560X1440 (2X) -> 1920X1080": the internal resolution and the window's.
        std::string ResolutionText(uint32_t width, uint32_t height)
        {
            const settings::Resolution& render = settings::ResolutionOf(settings::AtStartup().aspect, settings::AtStartup().renderHeight);
            char text[64];
            float scale = float(render.height) / 720.0f;
            snprintf(text, sizeof(text), "RENDER %s (%gX) -> %ux%u", render.name, double(scale), width, height);
            return text;
        }

        void Rasterize(uint32_t outputWidth, uint32_t outputHeight)
        {
            char line[128];
            std::vector<std::pair<std::string, uint32_t>> lines;
            bool graph = false;
            int columns = 0;
            // "(dot) REC 01:23" in every mode while recording (F5), even with the overlay off.
            input::RecordStatus record = input::Status();
            std::string recordLine;
            if (record.recording)
            {
                int seconds = int(record.recordSeconds);
                snprintf(line, sizeof(line), "\x01 REC %02d:%02d  F5 PARA", seconds / 60, seconds % 60);
                recordLine = line;
            }
            int64_t noticeNow = int64_t(plat::PerfCounter());
            bool notice = !g_notice.empty() && Seconds(g_noticeTime, noticeNow) < TOAST_SECONDS;
            if (g_mode == settings::OverlayMode::Off)
            {
                if (record.recording || !notice)
                    lines.push_back({ recordLine, RECORDING });
            }
            else if (g_mode == settings::OverlayMode::Fps)
            {
                snprintf(line, sizeof(line), "FPS %5.1f", g_fps);
                lines.push_back({ line, TEXT });
                if (record.recording)
                    lines.push_back({ recordLine, RECORDING });
            }
            else if (g_mode == settings::OverlayMode::FrameTime)
            {
                snprintf(line, sizeof(line), "FPS %5.1f  FRAME %5.1f MS", g_fps, g_lastMs);
                lines.push_back({ line, TEXT });
                snprintf(line, sizeof(line), "MED %5.1f MAX %5.1f 1%%LOW %5.1f", g_averageMs, g_maxMs, g_lowMs);
                lines.push_back({ line, TEXT });
                if (record.recording)
                    lines.push_back({ recordLine, RECORDING });
                graph = true;
            }
            else
            {
                std::string clock = ClockText();
                snprintf(line, sizeof(line), "QUADRO %-7llu INPUT %s%s", (unsigned long long)g_frame, clock.c_str(), clock == "--" ? "" : " S");
                lines.push_back({ line, TEXT });
                snprintf(line, sizeof(line), "FPS %5.1f       FRAME %5.1f MS", g_fps, g_lastMs);
                lines.push_back({ line, TEXT });
                snprintf(line, sizeof(line), "MED %5.1f MAX %5.1f 1%%LOW %5.1f", g_averageMs, g_maxMs, g_lowMs);
                lines.push_back({ line, TEXT });
                lines.push_back({ ResolutionText(outputWidth, outputHeight), TEXT });
                if (upscaler::Active() != settings::Upscaler::Off)
                {
                    // "DLSS 310.9.16 QUALITY: CENA 1707X960": the upscaler and the 3D scene's size.
                    float scene = settings::SceneScale();
                    snprintf(line, sizeof(line), "%s %s: CENA %ux%u", upscaler::ActiveName().c_str(),
                             settings::UpscalerQualityName(settings::AtStartup().upscalerQuality), unsigned(ceilf(1280.0f * scene)), unsigned(ceilf(720.0f * scene)));
                    lines.push_back({ line, TEXT });
                }
                // Runs started by tools/run_game.py with a profile.
                if (!g_profile.empty())
                {
                    lines.push_back({ "PERFIL " + g_profile, PROFILE });
                    input::Press press;
                    if (input::LastPress(press))
                    {
                        snprintf(line, sizeof(line), "BOTAO %s @ %.2f S Q%llu %s", press.button, double(press.clockMs) / 1000.0,
                                 (unsigned long long)press.frame, press.scripted ? "SCRIPT" : "JOGADOR");
                        lines.push_back({ line, PROFILE });
                    }
                    else
                    {
                        lines.push_back({ "BOTAO --", PROFILE });
                    }
                }
                // Replay (CRASH_REPLAY): its timeline, and how far SYNC/WAIT moved it.
                if (record.replaying)
                {
                    if (record.waiting)
                        snprintf(line, sizeof(line), "REPLAY %.1f S  ESPERANDO %s", record.replaySeconds, record.waiting);
                    else
                        snprintf(line, sizeof(line), "REPLAY %.1f S  SYNC %+.1f S", record.replaySeconds, record.shiftSeconds);
                    lines.push_back({ line, PROFILE });
                }
                if (record.recording)
                    lines.push_back({ recordLine, RECORDING });
                int64_t now = int64_t(plat::PerfCounter());
                if (!g_toast.empty() && Seconds(g_toastTime, now) < TOAST_SECONDS)
                    lines.push_back({ g_toast, TOAST });
                else
                    lines.push_back({ "F1 MODO F2 MARCA F5 GRAVA F12 PRINT", GUIDE });
                graph = true;
            }
            if (notice)
                lines.push_back({ g_notice, TOAST });
            for (const auto& [text, color] : lines)
                columns = std::max(columns, int(text.size()));
            if (graph)
                columns = std::max(columns, MIN_GRAPH_COLUMNS);
            columns = std::min(columns, COLUMNS);
            g_imageW = PAD * 2 + columns * CELL_W;
            g_imageH = PAD * 2 + int(lines.size()) * LINE_H + (graph ? 2 + GRAPH_H : 0);
            std::fill(g_image.begin(), g_image.begin() + size_t(g_imageW) * g_imageH, BACKGROUND);
            for (size_t i = 0; i < lines.size(); i++)
                Text(0, int(i), lines[i].first, lines[i].second);
            if (graph)
                Graph(PAD + int(lines.size()) * LINE_H + 2);
        }

        bool CreateResources(rhi::Device* device, DXGI_FORMAT format)
        {
            rhi::PassPipelineDesc desc;
            desc.name = "overlay";
            desc.vs = OVERLAY_VS;
            desc.ps = OVERLAY_PS;
            desc.targets[0] = format;
            desc.blend = rhi::Blend::Alpha;
            desc.bufferInput = true;
            desc.constantBytes = 32;
            g_pipeline = device->CreatePassPipeline(desc);
            if (!g_pipeline)
                return false;
            g_upload = device->CreateBuffer(UPLOAD_SLICE_SIZE * UPLOAD_SLICES);
            return g_upload != nullptr;
        }

        // Draws `imageWidth` x `imageHeight` RGBA8 pixels in `buffer` at `offset` (the image, DrawImage, DrawBitmap).
        void DrawUploaded(uint32_t width, uint32_t height, rhi::Buffer* buffer, uint64_t offset, int imageWidth, int imageHeight, int x, int y,
                          int scale)
        {
            if (!g_target)
                return;
            int32_t constants[8] = { x, y, imageWidth, imageHeight, std::max(scale, 1) };
            rhi::PassDraw pass;
            pass.pipeline = g_pipeline;
            pass.targets[0] = g_target;
            pass.viewport = { 0.0f, 0.0f, float(width), float(height) };
            pass.scissor = { std::max(x, 0), std::max(y, 0), std::min(x + imageWidth * constants[4], int32_t(width)),
                             std::min(y + imageHeight * constants[4], int32_t(height)) };
            if (pass.scissor.left >= pass.scissor.right || pass.scissor.top >= pass.scissor.bottom)
                return;
            pass.buffer = buffer;
            pass.bufferOffset = offset;
            pass.bufferBytes = uint64_t(imageWidth) * imageHeight * 4;
            pass.constants = constants;
            pass.constantBytes = sizeof(constants);
            g_device->DrawPass(pass);
        }
    }

    bool Init(rhi::Device* device, DXGI_FORMAT format, const std::filesystem::path& outputDir)
    {
        g_frequency = int64_t(plat::PerfFrequency());
        g_device = device;
        // CRASH_OVERLAY (run.toml `overlay`) wins over settings.toml for that run: 1/true = full,
        // or a mode name (off, fps, frametime, full).
        g_mode = settings::Get().overlay;
        if (const char* mode = getenv("CRASH_OVERLAY"); mode && *mode && !settings::ParseOverlayMode(mode, g_mode))
            Log("[overlay] CRASH_OVERLAY=%s is not a mode (off, fps, frametime, full)", mode);
        if (const char* profile = getenv("CRASH_PROFILE"))
        {
            for (const char* c = profile; *c; c++)
                g_profile += char(toupper(uint8_t(*c)));
        }
        g_inScreenshots = EnvFlag("CRASH_SCREENSHOT_OVERLAY");
        if (const char* corner = getenv("CRASH_OVERLAY_CORNER"); corner && strlen(corner) == 2)
        {
            g_corner[0] = char(tolower(corner[0]));
            g_corner[1] = char(tolower(corner[1]));
        }
        if (const char* scale = getenv("CRASH_OVERLAY_SCALE"))
            g_scaleOverride = std::clamp(atoi(scale), 0, 8);
        g_marksPath = outputDir / "marks.txt";
        if (const char* path = getenv("CRASH_FRAME_STATS"))
        {
            g_frameStats = fopen(path, "w");
            if (g_frameStats)
                fputs("frame,ms,input_s\n", g_frameStats);
            else
                Log("[overlay] could not open %s", path);
        }

        g_ready = CreateResources(device, format);
        if (!g_ready)
            Log("[overlay] could not create the overlay pipeline; F1 does nothing");
        Log("[overlay] mode %s (F1 next mode, F2 mark, F12 screenshot)%s", settings::OverlayModeName(g_mode),
            g_inScreenshots ? "; included in screenshots" : "");
        return g_ready;
    }

    void BeginFrame(uint64_t frame)
    {
        g_frame = frame;
        input::SetFrame(frame);
        int64_t now = int64_t(plat::PerfCounter());
        if (g_lastFrame)
        {
            g_lastMs = Seconds(g_lastFrame, now) * 1000.0;
            g_history.push_back(g_lastMs);
            if (g_history.size() > size_t(std::max(GRAPH_W, STATS_WINDOW)))
                g_history.pop_front();
            if (g_frameStats)
            {
                int64_t clock = input::ClockMs();
                if (clock >= 0)
                    fprintf(g_frameStats, "%llu,%.3f,%.3f\n", (unsigned long long)frame, g_lastMs, double(clock) / 1000.0);
                else
                    fprintf(g_frameStats, "%llu,%.3f,\n", (unsigned long long)frame, g_lastMs);
                if (frame % 30 == 0)
                    fflush(g_frameStats);
            }
        }
        g_lastFrame = now;

        // FPS: swaps over the last second, refreshed twice a second so the number is readable.
        g_swapTimes.push_back(now);
        while (g_swapTimes.size() > 1 && Seconds(g_swapTimes.front(), now) > 1.0)
            g_swapTimes.pop_front();
        if (!g_fpsUpdated || Seconds(g_fpsUpdated, now) >= 0.5)
        {
            double span = Seconds(g_swapTimes.front(), now);
            g_fps = g_swapTimes.size() > 1 && span > 0.0 ? double(g_swapTimes.size() - 1) / span : 0.0;
            g_fpsUpdated = now;
            UpdateWindowStats();
        }

        if (uint32_t presses = window::ConsumeOverlayToggles())
        {
            // F1: off -> FPS -> FPS + frame time -> full -> off; kept in settings.toml.
            SetMode(settings::OverlayMode((int(g_mode) + presses) % 4));
            settings::Values values = settings::Get();
            values.overlay = g_mode;
            settings::Set(values);
            settings::Save();
            Log("[overlay] mode %s (F1)", settings::OverlayModeName(g_mode));
        }
        for (uint32_t marks = window::ConsumeMarkRequests(); marks; marks--)
        {
            g_marks++;
            std::string clock = ClockText();
            Log("[mark] frame %llu, input t=%s s", (unsigned long long)frame, clock.c_str());
            if (FILE* file = _wfopen(g_marksPath.c_str(), L"a"))
            {
                fprintf(file, "frame %llu, input t=%s s\n", (unsigned long long)frame, clock.c_str());
                fclose(file);
            }
            input::RecordMark(frame);
            char text[64];
            snprintf(text, sizeof(text), "MARCA %u: QUADRO %llu", g_marks, (unsigned long long)frame);
            Toast(text);
        }
        // F5: start/stop recording (kernel/input_record.cpp logs and writes the profile).
        for (uint32_t toggles = window::ConsumeRecordToggles(); toggles; toggles--)
            Toast(input::ToggleRecording());
    }

    bool Visible()
    {
        // Off still shows the recording indicator.
        int64_t now = int64_t(plat::PerfCounter());
        bool notice = !g_notice.empty() && Seconds(g_noticeTime, now) < TOAST_SECONDS;
        return g_ready && (g_mode != settings::OverlayMode::Off || input::Recording() || notice);
    }

    void SetMode(settings::OverlayMode mode)
    {
        g_mode = mode;
    }

    settings::OverlayMode Mode()
    {
        return g_mode;
    }

    bool InScreenshots()
    {
        return g_inScreenshots;
    }

    void NoteScreenshot(uint64_t frame)
    {
        char text[64];
        snprintf(text, sizeof(text), "PRINT: QUADRO %llu", (unsigned long long)frame);
        Toast(text);
    }

    void Notice(const std::string& text)
    {
        g_notice = text;
        g_noticeTime = int64_t(plat::PerfCounter());
    }

    void BeginPresent(rhi::Texture* target)
    {
        g_target = target;
    }

    void Draw(uint32_t width, uint32_t height)
    {
        if (!Visible() || !width || !height)
            return;
        Rasterize(width, height);
        uint64_t offset = UPLOAD_SLICE_SIZE * g_slice;
        g_slice = (g_slice + 1) % UPLOAD_SLICES;
        memcpy(g_upload->cpu + offset, g_image.data(), size_t(g_imageW) * g_imageH * 4);

        // 2x at 720p and 1080p, 3x at 1440p...
        int scale = g_scaleOverride ? g_scaleOverride : std::max(1, int(height + 240) / 480);
        int w = g_imageW * scale, h = g_imageH * scale, margin = 8;
        int x = g_corner[1] == 'r' ? int(width) - w - margin : margin;
        int y = g_corner[0] == 'b' ? int(height) - h - margin : margin;
        x = std::max(x, 0), y = std::max(y, 0);
        DrawUploaded(width, height, g_upload, offset, g_imageW, g_imageH, x, y, scale);
    }

    void DrawImage(uint32_t width, uint32_t height, const uint32_t* pixels, int imageWidth, int imageHeight, int x, int y, int scale)
    {
        if (!g_ready || !width || !height || imageWidth <= 0 || imageHeight <= 0 || imageWidth > MAX_IMAGE_SIDE || imageHeight > MAX_IMAGE_SIDE)
            return;
        if (!g_imageUpload)
        {
            g_imageUpload = g_device->CreateBuffer(IMAGE_SLICE_SIZE * UPLOAD_SLICES);
            if (!g_imageUpload)
            {
                Log("[overlay] could not create the image upload buffer");
                return;
            }
        }
        uint64_t offset = IMAGE_SLICE_SIZE * g_imageSlice;
        g_imageSlice = (g_imageSlice + 1) % UPLOAD_SLICES;
        memcpy(g_imageUpload->cpu + offset, pixels, size_t(imageWidth) * imageHeight * 4);
        DrawUploaded(width, height, g_imageUpload, offset, imageWidth, imageHeight, x, y, scale);
    }

    void DrawBitmap(uint32_t width, uint32_t height, const uint32_t* pixels, int imageWidth, int imageHeight, int x, int y, uint64_t version,
                    BitmapChannel channel)
    {
        // Uploaded only when the contents change, each time into the next slice (a slice is rewritten
        // UPLOAD_SLICES changes later, after the frames that read it). A bigger image gets a new buffer;
        // the old one is released once the frames that read it are done.
        struct Channel
        {
            rhi::Buffer* buffer = nullptr;
            uint64_t sliceSize = 0, uploadedVersion = ~0ull;
            uint32_t slice = 0;
        };
        static Channel channels[BITMAP_CHANNELS];
        if (!g_ready || !width || !height || imageWidth <= 0 || imageHeight <= 0 || channel < 0 || channel >= BITMAP_CHANNELS)
            return;
        auto& [buffer, sliceSize, uploadedVersion, slice] = channels[channel];
        uint64_t need = (uint64_t(imageWidth) * imageHeight * 4 + 0xFFFF) & ~uint64_t(0xFFFF);
        if (need > sliceSize)
        {
            if (buffer)
                g_device->DestroyBuffer(buffer);
            buffer = g_device->CreateBuffer(need * UPLOAD_SLICES);
            if (!buffer)
            {
                Log("[overlay] could not create the bitmap upload buffer (%llu bytes)", (unsigned long long)(need * UPLOAD_SLICES));
                sliceSize = 0;
                return;
            }
            sliceSize = need;
            uploadedVersion = ~0ull;
        }
        if (version != uploadedVersion)
        {
            slice = (slice + 1) % UPLOAD_SLICES;
            memcpy(buffer->cpu + sliceSize * slice, pixels, size_t(imageWidth) * imageHeight * 4);
            uploadedVersion = version;
        }
        DrawUploaded(width, height, buffer, sliceSize * slice, imageWidth, imageHeight, x, y, 1);
    }
}
