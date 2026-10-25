// PlayStation pads over HID (hid_pads.h).
//
// Report layouts (the same ones Linux's hid-playstation and SDL use):
// - DualShock 4 over USB, and the "simple" report 0x01 both pads send over Bluetooth until a
//   feature report switches them to the full one: [1..4] LX LY RX RY, [5] hat (low nibble) +
//   Square/Cross/Circle/Triangle (0x10/0x20/0x40/0x80), [6] L1 R1 L2 R2 Share Options L3 R3,
//   [7] PS (0x01) / touchpad click (0x02), [8] L2, [9] R2.
// - DualShock 4 over Bluetooth, report 0x11: the same, 2 bytes later.
// - DualSense over USB, report 0x01 (64 bytes): [1..4] sticks, [5] L2, [6] R2, [8] hat + face,
//   [9] shoulders / Create / Options / L3 / R3, [10] PS / touchpad / mute.
// - DualSense over Bluetooth, report 0x31: the same, 1 byte later.
// Sticks are 0..255 with 128 in the middle and Y pointing down.

#include "hid_pads.h"
#include "pc_input.h"
#include "../diagnostics.h"
#include "../platform/platform.h"

#include <hidsdi.h>
#include <setupapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")

namespace input::hid
{
    namespace
    {
        constexpr USHORT SONY = 0x054C;
        enum class Kind : uint8_t { DualShock4, DualSense };
        struct Model
        {
            USHORT product;
            Kind kind;
            const char* name;
        };
        constexpr Model MODELS[] = {
            { 0x05C4, Kind::DualShock4, "DualShock 4" },
            { 0x09CC, Kind::DualShock4, "DualShock 4" },
            { 0x0BA0, Kind::DualShock4, "DualShock 4 (adaptador sem fio)" },
            { 0x0CE6, Kind::DualSense, "DualSense" },
            { 0x0DF2, Kind::DualSense, "DualSense Edge" },
        };

        struct Pad
        {
            std::wstring path;
            uint32_t id = 0;  // Pads(): never reused, so a pad that leaves and comes back is a new one
            HANDLE handle = INVALID_HANDLE_VALUE;
            Kind kind = Kind::DualSense;
            bool bluetooth = false;
            std::string name;
            USHORT inputLength = 0, outputLength = 0;
            std::atomic<bool> alive{ true };
            std::mutex mutex;
            XINPUT_GAMEPAD state{};
            ULONGLONG lastActive = 0;
            std::atomic<uint32_t> motors{ 0 };  // left << 16 | right, wanted
            uint32_t sentMotors = 0;
            uint8_t outputSequence = 0;
            std::thread reader;
        };

        std::mutex g_padsMutex;
        std::vector<std::shared_ptr<Pad>> g_pads;  // connection order
        std::vector<std::wstring> g_hidden;        // Sony pads we may not open (HiddenPads)
        std::atomic<bool> g_started{ false };
        HANDLE g_rescanEvent = nullptr;
        std::atomic<ULONGLONG> g_lastActive{ 0 };
        std::atomic<uint32_t> g_nextId{ 1 };

        SHORT StickAxis(uint8_t value, bool flip)
        {
            float v = (int(value) - 128) / 127.0f;
            if (flip)
                v = -v;
            return SHORT(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
        }

        uint32_t Crc32(const uint8_t* data, size_t size, uint32_t crc = 0xFFFFFFFF)
        {
            for (size_t i = 0; i < size; i++)
            {
                crc ^= data[i];
                for (int b = 0; b < 8; b++)
                    crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
            }
            return crc;
        }

        // Bluetooth output reports end with a CRC-32 of 0xA2 (the HID "output" header) + the report.
        void SealBluetooth(std::vector<uint8_t>& report)
        {
            uint8_t header = 0xA2;
            uint32_t crc = Crc32(&header, 1);
            crc = ~Crc32(report.data(), report.size() - 4, crc);
            memcpy(report.data() + report.size() - 4, &crc, 4);
        }

        bool Parse(Pad& pad, const uint8_t* r, DWORD size, XINPUT_GAMEPAD& out)
        {
            int o;           // offset of LX
            bool dualSenseFull = false;
            if (pad.kind == Kind::DualSense && r[0] == 0x01 && !pad.bluetooth && size >= 11)
                o = 1, dualSenseFull = true;
            else if (pad.kind == Kind::DualSense && r[0] == 0x31 && size >= 12)
                o = 2, dualSenseFull = true;
            else if (pad.kind == Kind::DualShock4 && r[0] == 0x11 && size >= 12)
                o = 3;
            else if (r[0] == 0x01 && size >= 10)
                o = 1;
            else
                return false;
            uint8_t face, shoulders, system, l2, r2;
            if (dualSenseFull)
                l2 = r[o + 4], r2 = r[o + 5], face = r[o + 7], shoulders = r[o + 8], system = r[o + 9];
            else
                face = r[o + 4], shoulders = r[o + 5], system = r[o + 6], l2 = r[o + 7], r2 = r[o + 8];
            out = {};
            out.sThumbLX = StickAxis(r[o], false);
            out.sThumbLY = StickAxis(r[o + 1], true);
            out.sThumbRX = StickAxis(r[o + 2], false);
            out.sThumbRY = StickAxis(r[o + 3], true);
            out.bLeftTrigger = l2;
            out.bRightTrigger = r2;
            static constexpr WORD HAT[8] = {
                XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT,
                XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_DOWN,
                XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_LEFT,
                XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_LEFT,
            };
            WORD b = (face & 0x0F) < 8 ? HAT[face & 0x0F] : 0;
            if (face & 0x10) b |= XINPUT_GAMEPAD_X;   // Square
            if (face & 0x20) b |= XINPUT_GAMEPAD_A;   // Cross
            if (face & 0x40) b |= XINPUT_GAMEPAD_B;   // Circle
            if (face & 0x80) b |= XINPUT_GAMEPAD_Y;   // Triangle
            if (shoulders & 0x01) b |= XINPUT_GAMEPAD_LEFT_SHOULDER;
            if (shoulders & 0x02) b |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
            if (shoulders & 0x10) b |= XINPUT_GAMEPAD_BACK;    // Share / Create
            if (shoulders & 0x20) b |= XINPUT_GAMEPAD_START;   // Options
            if (shoulders & 0x40) b |= XINPUT_GAMEPAD_LEFT_THUMB;
            if (shoulders & 0x80) b |= XINPUT_GAMEPAD_RIGHT_THUMB;
            if (system & 0x02) b |= XINPUT_GAMEPAD_BACK;       // touchpad click
            out.wButtons = b;
            return true;
        }

        bool Active(const XINPUT_GAMEPAD& a, const XINPUT_GAMEPAD& b)
        {
            auto moved = [](SHORT x, SHORT y) { return std::abs(int(x) - int(y)) > 8000 && (std::abs(int(x)) > 12000 || std::abs(int(y)) > 12000); };
            return a.wButtons != b.wButtons || std::abs(int(a.bLeftTrigger) - int(b.bLeftTrigger)) > 60 ||
                   std::abs(int(a.bRightTrigger) - int(b.bRightTrigger)) > 60 || moved(a.sThumbLX, b.sThumbLX) ||
                   moved(a.sThumbLY, b.sThumbLY) || moved(a.sThumbRX, b.sThumbRX) || moved(a.sThumbRY, b.sThumbRY);
        }

        void SendMotors(Pad& pad, uint32_t motors, OVERLAPPED& overlapped)
        {
            uint8_t left = uint8_t(motors >> 24), right = uint8_t(motors >> 8);
            std::vector<uint8_t> report(std::max<USHORT>(pad.outputLength, 32), 0);
            if (pad.kind == Kind::DualSense && !pad.bluetooth)
            {
                report[0] = 0x02, report[1] = 0x03, report[3] = right, report[4] = left;
            }
            else if (pad.kind == Kind::DualSense)
            {
                report.assign(78, 0);
                report[0] = 0x31, report[1] = uint8_t(pad.outputSequence++ << 4), report[2] = 0x10;
                report[3] = 0x03, report[5] = right, report[6] = left;
                SealBluetooth(report);
            }
            else if (!pad.bluetooth)
            {
                report[0] = 0x05, report[1] = 0x01, report[4] = right, report[5] = left;
            }
            else
            {
                report.assign(78, 0);
                report[0] = 0x11, report[1] = 0xC0, report[3] = 0x01, report[6] = right, report[7] = left;
                SealBluetooth(report);
            }
            if (pad.outputLength && report.size() > pad.outputLength && !pad.bluetooth)
                report.resize(pad.outputLength);
            DWORD written = 0;
            ResetEvent(overlapped.hEvent);
            if (!WriteFile(pad.handle, report.data(), DWORD(report.size()), &written, &overlapped) && GetLastError() == ERROR_IO_PENDING)
            {
                if (WaitForSingleObject(overlapped.hEvent, 100) != WAIT_OBJECT_0)
                    CancelIo(pad.handle);
            }
        }

        void ReadLoop(std::shared_ptr<Pad> pad)
        {
            std::vector<uint8_t> buffer(std::max<USHORT>(pad->inputLength, 128));
            OVERLAPPED read{}, write{};
            read.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            write.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            bool pending = false;
            int failures = 0;
            while (pad->alive)
            {
                if (!pending)
                {
                    ResetEvent(read.hEvent);
                    DWORD got = 0;
                    if (ReadFile(pad->handle, buffer.data(), DWORD(buffer.size()), &got, &read))
                        SetEvent(read.hEvent);
                    else if (GetLastError() != ERROR_IO_PENDING)
                        break;  // unplugged
                    pending = true;
                }
                DWORD wait = WaitForSingleObject(read.hEvent, 50);
                if (wait == WAIT_OBJECT_0)
                {
                    pending = false;
                    DWORD got = 0;
                    if (!GetOverlappedResult(pad->handle, &read, &got, FALSE))
                    {
                        if (++failures > 5)
                            break;
                        continue;
                    }
                    failures = 0;
                    // CRASH_HID_TRACE=1: the first reports, then one every 5 s (diagnostics).
                    if (static const bool trace = getenv("CRASH_HID_TRACE") != nullptr; trace)
                    {
                        static std::atomic<int> s_reports{ 0 };
                        static std::atomic<ULONGLONG> s_lastTrace{ 0 };
                        int n = ++s_reports;
                        if (n <= 3 || plat::TickCountMs() - s_lastTrace.load() > 5000)
                        {
                            s_lastTrace = plat::TickCountMs();
                            std::string bytes;
                            for (DWORD i = 0; i < std::min<DWORD>(got, 16); i++)
                            {
                                char hex[4];
                                snprintf(hex, sizeof(hex), "%02X ", buffer[i]);
                                bytes += hex;
                            }
                            Log("[input] %s: report #%d, %u bytes: %s", pad->name.c_str(), n, got, bytes.c_str());
                        }
                    }
                    XINPUT_GAMEPAD state;
                    if (got && Parse(*pad, buffer.data(), got, state))
                    {
                        std::lock_guard lock(pad->mutex);
                        if (Active(state, pad->state))
                        {
                            pad->lastActive = plat::TickCountMs();
                            g_lastActive = pad->lastActive;
                            input::pc::NoteDevice(input::pc::Device::PlayStation);
                        }
                        pad->state = state;
                    }
                }
                uint32_t motors = pad->motors.load();
                if (motors != pad->sentMotors)
                {
                    pad->sentMotors = motors;
                    SendMotors(*pad, motors, write);
                }
            }
            if (pending)
                CancelIo(pad->handle);
            if (pad->sentMotors)
                SendMotors(*pad, 0, write);
            CloseHandle(read.hEvent);
            CloseHandle(write.hEvent);
            pad->alive = false;
            Log("[input] %s disconnected", pad->name.c_str());
            if (GetLastError() == ERROR_DEVICE_NOT_CONNECTED)
                Rescan();
        }

        void Open(const std::wstring& path)
        {
            HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            bool writable = handle != INVALID_HANDLE_VALUE;
            if (!writable)  // another program holds it for writing: reading still works
                handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                     FILE_FLAG_OVERLAPPED, nullptr);
            static const bool trace = getenv("CRASH_HID_TRACE") != nullptr;
            if (handle == INVALID_HANDLE_VALUE)
            {
                DWORD error = GetLastError();
                if (trace)
                    Log("[input] hid: could not open %ls (error %lu)", path.c_str(), error);
                // Hidden by HidHide / taken by DSX or DS4Windows, which then present it as a
                // virtual Xbox pad: still a PlayStation pad for the icons (HiddenPads).
                HANDLE probe = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
                HIDD_ATTRIBUTES attributes{ sizeof(attributes) };
                bool sony = probe != INVALID_HANDLE_VALUE && HidD_GetAttributes(probe, &attributes) && attributes.VendorID == SONY;
                if (probe != INVALID_HANDLE_VALUE)
                    CloseHandle(probe);
                if (sony || (error == ERROR_ACCESS_DENIED && path.find(L"0ce6") != std::wstring::npos))
                {
                    std::lock_guard lock(g_padsMutex);
                    if (std::find(g_hidden.begin(), g_hidden.end(), path) == g_hidden.end())
                    {
                        g_hidden.push_back(path);
                        Log("[input] a PlayStation pad is in use by another program (DSX / DS4Windows / HidHide); its virtual Xbox pad gets PlayStation icons");
                    }
                }
                return;
            }
            HIDD_ATTRIBUTES attributes{ sizeof(attributes) };
            const Model* model = nullptr;
            if (HidD_GetAttributes(handle, &attributes) && attributes.VendorID == SONY)
            {
                for (const Model& m : MODELS)
                {
                    if (m.product == attributes.ProductID)
                        model = &m;
                }
            }
            PHIDP_PREPARSED_DATA preparsed = nullptr;
            HIDP_CAPS caps{};
            bool gamepad = model && HidD_GetPreparsedData(handle, &preparsed) && HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS &&
                           caps.UsagePage == 1 && caps.Usage == 5;
            if (preparsed)
                HidD_FreePreparsedData(preparsed);
            if (!gamepad)
            {
                if (trace)
                    Log("[input] hid: %ls: vendor %04X product %04X usage %04X/%04X, not a known PlayStation pad", path.c_str(), attributes.VendorID,
                        attributes.ProductID, caps.UsagePage, caps.Usage);
                CloseHandle(handle);
                return;
            }
            auto pad = std::make_shared<Pad>();
            pad->path = path;
            pad->id = g_nextId++;
            pad->handle = handle;
            pad->kind = model->kind;
            pad->inputLength = caps.InputReportByteLength;
            pad->outputLength = writable ? caps.OutputReportByteLength : 0;
            // Over USB both pads have 64-byte input reports; over Bluetooth the descriptor is longer.
            pad->bluetooth = caps.InputReportByteLength != 64;
            pad->name = std::string(model->name) + (pad->bluetooth ? " (Bluetooth)" : " (USB)");
            Log("[input] %s connected (input report %u bytes%s)", pad->name.c_str(), caps.InputReportByteLength,
                writable ? "" : ", read-only: another program holds it");
            pad->reader = std::thread(ReadLoop, pad);
            std::lock_guard lock(g_padsMutex);
            g_pads.push_back(pad);
        }

        void Scan()
        {
            // Drop the pads whose reader stopped (unplugged).
            {
                std::lock_guard lock(g_padsMutex);
                for (auto it = g_pads.begin(); it != g_pads.end();)
                {
                    if (!(*it)->alive)
                    {
                        if ((*it)->reader.joinable())
                            (*it)->reader.join();
                        CloseHandle((*it)->handle);
                        it = g_pads.erase(it);
                    }
                    else
                        ++it;
                }
            }
            GUID hidGuid;
            HidD_GetHidGuid(&hidGuid);
            HDEVINFO devices = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
            if (devices == INVALID_HANDLE_VALUE)
                return;
            SP_DEVICE_INTERFACE_DATA interfaceData{ sizeof(interfaceData) };
            for (DWORD i = 0; SetupDiEnumDeviceInterfaces(devices, nullptr, &hidGuid, i, &interfaceData); i++)
            {
                DWORD size = 0;
                SetupDiGetDeviceInterfaceDetailW(devices, &interfaceData, nullptr, 0, &size, nullptr);
                if (!size)
                    continue;
                std::vector<uint8_t> storage(size);
                auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.data());
                detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
                if (!SetupDiGetDeviceInterfaceDetailW(devices, &interfaceData, detail, size, nullptr, nullptr))
                    continue;
                std::wstring path = detail->DevicePath;
                // Sony's vendor id is in the path ("vid_054c" over USB, "vid&0002054c" over
                // Bluetooth): skip everything else without opening it.
                std::wstring lower = path;
                for (wchar_t& c : lower)
                    c = wchar_t(towlower(c));
                if (lower.find(L"vid_054c") == std::wstring::npos && lower.find(L"vid&0002054c") == std::wstring::npos)
                    continue;
                bool known;
                {
                    std::lock_guard lock(g_padsMutex);
                    known = std::any_of(g_pads.begin(), g_pads.end(), [&](const auto& pad) { return pad->path == path; }) ||
                            std::find(g_hidden.begin(), g_hidden.end(), path) != g_hidden.end();
                }
                if (!known)
                    Open(path);
            }
            SetupDiDestroyDeviceInfoList(devices);
        }

        std::shared_ptr<Pad> PadById(uint32_t id)
        {
            std::lock_guard lock(g_padsMutex);
            for (const auto& pad : g_pads)
            {
                if (pad->alive && pad->id == id)
                    return pad;
            }
            return nullptr;
        }
    }

    void Start()
    {
        if (g_started.exchange(true))
            return;
        if (getenv("CRASH_NO_HID"))
            return;
        g_rescanEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        std::thread([] {
            SetThreadDescription(GetCurrentThread(), L"hid pads");
            for (;;)
            {
                Scan();
                WaitForSingleObject(g_rescanEvent, 2000);
            }
        }).detach();
    }

    void Rescan()
    {
        if (g_rescanEvent)
            SetEvent(g_rescanEvent);
    }

    std::vector<PadInfo> Pads()
    {
        Start();
        std::lock_guard lock(g_padsMutex);
        std::vector<PadInfo> pads;
        for (const auto& pad : g_pads)
        {
            if (pad->alive)
                pads.push_back({ pad->id, pad->name });
        }
        return pads;
    }

    bool ReadPad(uint32_t id, XINPUT_GAMEPAD& pad)
    {
        std::shared_ptr<Pad> ps = PadById(id);
        if (!ps)
            return false;
        std::lock_guard lock(ps->mutex);
        pad = ps->state;
        return true;
    }

    void SetPadVibration(uint32_t id, uint16_t left, uint16_t right)
    {
        std::shared_ptr<Pad> ps = PadById(id);
        if (ps && ps->outputLength)
            ps->motors = uint32_t(left) << 16 | right;
    }

    bool RecentlyActive()
    {
        ULONGLONG last = g_lastActive.load();
        return last && plat::TickCountMs() - last < 1000;
    }

    int Count(const char** firstName)
    {
        std::lock_guard lock(g_padsMutex);
        int count = 0;
        for (const auto& pad : g_pads)
        {
            if (!pad->alive)
                continue;
            if (!count && firstName)
                *firstName = pad->name.c_str();
            count++;
        }
        return count;
    }

    int SelfTest()
    {
        // One report of each layout: Cross + Circle + D-pad right, L1 + Options, the touchpad,
        // left stick full right / up, right stick left, L2 = 0x40, R2 = 0xFF.
        struct Case
        {
            const char* name;
            Kind kind;
            bool bluetooth;
            std::vector<uint8_t> report;
        };
        auto report = [](size_t size, uint8_t id, size_t lx, std::initializer_list<std::pair<size_t, uint8_t>> bytes) {
            std::vector<uint8_t> r(size, 0);
            r[0] = id;
            r[lx] = 0xFF, r[lx + 1] = 0x00, r[lx + 2] = 0x00, r[lx + 3] = 0x80;
            for (auto [at, value] : bytes)
                r[at] = value;
            return r;
        };
        const Case cases[] = {
            { "DualSense USB 0x01", Kind::DualSense, false, report(64, 0x01, 1, { { 5, 0x40 }, { 6, 0xFF }, { 8, 0x62 }, { 9, 0x21 }, { 10, 0x02 } }) },
            { "DualSense BT 0x31", Kind::DualSense, true, report(78, 0x31, 2, { { 6, 0x40 }, { 7, 0xFF }, { 9, 0x62 }, { 10, 0x21 }, { 11, 0x02 } }) },
            { "DualSense BT simple 0x01", Kind::DualSense, true, report(10, 0x01, 1, { { 5, 0x62 }, { 6, 0x21 }, { 7, 0x02 }, { 8, 0x40 }, { 9, 0xFF } }) },
            { "DualShock 4 USB 0x01", Kind::DualShock4, false, report(64, 0x01, 1, { { 5, 0x62 }, { 6, 0x21 }, { 7, 0x02 }, { 8, 0x40 }, { 9, 0xFF } }) },
            { "DualShock 4 BT 0x11", Kind::DualShock4, true, report(78, 0x11, 3, { { 7, 0x62 }, { 8, 0x21 }, { 9, 0x02 }, { 10, 0x40 }, { 11, 0xFF } }) },
        };
        const WORD expected = XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_DPAD_RIGHT | XINPUT_GAMEPAD_LEFT_SHOULDER |
                              XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK;
        int failures = 0;
        for (const Case& c : cases)
        {
            Pad pad;
            pad.kind = c.kind;
            pad.bluetooth = c.bluetooth;
            XINPUT_GAMEPAD out{};
            bool parsed = Parse(pad, c.report.data(), DWORD(c.report.size()), out);
            bool ok = parsed && out.wButtons == expected && out.sThumbLX == 32767 && out.sThumbLY == 32767 && out.sThumbRX < -32000 &&
                      std::abs(out.sThumbRY) < 300 && out.bLeftTrigger == 0x40 && out.bRightTrigger == 0xFF;
            failures += !ok;
            printf("%-26s %s  buttons %04X (want %04X) LS %d,%d RS %d,%d LT %u RT %u\n", c.name, ok ? "ok    " : "FALHOU", out.wButtons, expected,
                   out.sThumbLX, out.sThumbLY, out.sThumbRX, out.sThumbRY, out.bLeftTrigger, out.bRightTrigger);
        }
        uint8_t header = 0xA2, data[] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
        uint32_t crc = ~Crc32(data, sizeof(data));
        printf("CRC-32 check value %08X (want CBF43926) %s; seeded with 0xA2: %08X\n", crc, crc == 0xCBF43926 ? "ok" : "FALHOU",
               ~Crc32(data, sizeof(data), Crc32(&header, 1)));
        return failures || crc != 0xCBF43926;
    }

    int Monitor(int seconds)
    {
        Start();
        plat::SleepMs(600);
        {
            std::lock_guard lock(g_padsMutex);
            printf("%zu controle(s) PlayStation:\n", g_pads.size());
            for (const auto& pad : g_pads)
                printf("  %s (relatorio de entrada %u bytes, saida %u)\n", pad->name.c_str(), pad->inputLength, pad->outputLength);
        }
        std::vector<std::pair<uint32_t, XINPUT_GAMEPAD>> last;
        ULONGLONG end = plat::TickCountMs() + ULONGLONG(seconds) * 1000;
        while (plat::TickCountMs() < end)
        {
            for (const PadInfo& info : Pads())
            {
                XINPUT_GAMEPAD pad{};
                if (!ReadPad(info.id, pad))
                    continue;
                // Cross held: both motors at half (rumble check).
                SetPadVibration(info.id, (pad.wButtons & XINPUT_GAMEPAD_A) ? 0x8000 : 0, (pad.wButtons & XINPUT_GAMEPAD_A) ? 0x8000 : 0);
                auto it = std::find_if(last.begin(), last.end(), [&](const auto& entry) { return entry.first == info.id; });
                if (it == last.end())
                    it = last.insert(last.end(), { info.id, XINPUT_GAMEPAD{} });
                if (!Active(pad, it->second))
                    continue;
                it->second = pad;
                printf("[%u] botoes %04X  LS %6d %6d  RS %6d %6d  L2 %3u R2 %3u\n", info.id, pad.wButtons, pad.sThumbLX, pad.sThumbLY, pad.sThumbRX,
                       pad.sThumbRY, pad.bLeftTrigger, pad.bRightTrigger);
                fflush(stdout);
            }
            plat::SleepMs(20);
        }
        return 0;
    }

    bool HiddenPads()
    {
        std::lock_guard lock(g_padsMutex);
        return !g_hidden.empty();
    }
}
