// The GPU and what it can run, for the launcher (which upscalers its options offer) and the
// installer (whether the DLSS files are installed at all). Included inside launcher.cpp's anonymous
// namespace.
//
//   - DetectGpu: the GPU the game will pick (Vulkan, as rhi_vulkan.cpp chooses it: discrete, then
//     integrated, CRASH_VULKAN_DEVICE), its vendor and whether its driver has the extensions FSR 3.1
//     calls. CRASH_GPU_VENDOR (runtime/settings/gpu_vendor.h) pretends another vendor.
//   - StartDlssProbe / DlssProbeResult: NGX's own check that DLSS runs here (an NVIDIA RTX GPU and a
//     driver with DLSS), done by CrashDlss.dll's CrashDlss_Probe in a child process (this exe with
//     --probe-dlss), so a driver that crashes in NGX can't take the window with it.

struct GpuInfo
{
    uint32_t vendor = 0;       // PCI vendor id (0 = no Vulkan GPU found)
    std::string name;
    bool vulkan = false;       // a Vulkan 1.3 GPU
    bool fsrExtensions = false;  // the device extensions amd_fidelityfx_vk.dll calls
    bool forced = false;       // vendor from CRASH_GPU_VENDOR
};

GpuInfo DetectGpu()
{
    GpuInfo gpu;
    HMODULE vulkan = LoadLibraryW(L"vulkan-1.dll");
    auto gipa = vulkan ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(vulkan, "vkGetInstanceProcAddr")) : nullptr;
    auto createInstance = gipa ? reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance")) : nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "CrashRecomp GPU check";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo create{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    create.pApplicationInfo = &app;
    if (createInstance && createInstance(&create, nullptr, &instance) == VK_SUCCESS)
    {
        auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(instance, "vkEnumeratePhysicalDevices"));
        auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(instance, "vkGetPhysicalDeviceProperties"));
        auto extensions = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(gipa(instance, "vkEnumerateDeviceExtensionProperties"));
        auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(gipa(instance, "vkDestroyInstance"));
        uint32_t count = 0;
        enumerate(instance, &count, nullptr);
        std::vector<VkPhysicalDevice> devices(count);
        enumerate(instance, &count, devices.data());
        int best = -1;
        const char* wanted = getenv("CRASH_VULKAN_DEVICE");
        for (VkPhysicalDevice device : devices)
        {
            VkPhysicalDeviceProperties p{};
            properties(device, &p);
            if (p.apiVersion < VK_API_VERSION_1_3)
                continue;
            int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
            if (wanted && strstr(p.deviceName, wanted))
                score += 10;
            if (score <= best)
                continue;
            best = score;
            gpu.vendor = p.vendorID;
            gpu.name = p.deviceName;
            gpu.vulkan = true;
            uint32_t n = 0;
            extensions(device, nullptr, &n, nullptr);
            std::vector<VkExtensionProperties> have(n);
            extensions(device, nullptr, &n, have.data());
            auto has = [&](const char* name) {
                return std::any_of(have.begin(), have.end(), [&](const VkExtensionProperties& e) { return strcmp(e.extensionName, name) == 0; });
            };
            // The list of runtime/gpu/upscaler.cpp (FSR_DEVICE_EXTENSIONS).
            gpu.fsrExtensions = has(VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME) && has(VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME) &&
                                has(VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME) && has(VK_KHR_SHADER_SUBGROUP_EXTENDED_TYPES_EXTENSION_NAME);
        }
        destroy(instance, nullptr);
    }
    if (vulkan)
        FreeLibrary(vulkan);
    if (uint32_t forced = settings::ForcedGpuVendor())
    {
        gpu.vendor = forced;
        gpu.forced = true;
    }
    return gpu;
}

// --probe-dlss <folder> <result file>: the child process. Writes "1|0\n<gpu>\n<reason>\n" and exits
// with 0 when DLSS runs here.
int RunDlssProbe(const fs::path& folder, const fs::path& result)
{
    int available = 0;
    char gpu[256] = "", reason[512] = "CrashDlss.dll could not be loaded";
    SetDllDirectoryW(folder.c_str());  // nvngx_dlss.dll and what CrashDlss.dll loads, from there
    HMODULE module = LoadLibraryExW((folder / L"CrashDlss.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (module)
    {
        auto probe = reinterpret_cast<CrashDlss_ProbeFn>(GetProcAddress(module, "CrashDlss_Probe"));
        if (probe)
        {
            wchar_t temp[MAX_PATH];
            GetTempPathW(MAX_PATH, temp);
            fs::path data = fs::path(temp) / L"CrashRecomp_ngx";
            available = probe(folder.c_str(), data.c_str(), gpu, sizeof(gpu), reason, sizeof(reason));
            std::error_code error;
            fs::remove_all(data, error);  // NGX's logs of this check
        }
        else
            snprintf(reason, sizeof(reason), "CrashDlss.dll has no CrashDlss_Probe");
    }
    if (FILE* f = _wfopen(result.c_str(), L"wb"))
    {
        fprintf(f, "%d\n%s\n%s\n", available, gpu, reason);
        fclose(f);
    }
    return available ? 0 : 1;
}

struct DlssProbe
{
    std::atomic<int> state{ 0 };  // 0 not started, 1 running, 2 done
    bool available = false;
    std::string gpu, reason;
    std::thread thread;
};
DlssProbe g_dlssProbe;

// Runs the probe for the CrashDlss.dll + nvngx_dlss.dll in `folder` on a thread (state 2 when done).
void StartDlssProbe(const fs::path& folder)
{
    if (g_dlssProbe.state != 0)
        return;
    g_dlssProbe.state = 1;
    g_dlssProbe.thread = std::thread([folder] {
        wchar_t temp[MAX_PATH], self[MAX_PATH];
        GetTempPathW(MAX_PATH, temp);
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        fs::path result = fs::path(temp) / (L"CrashRecomp_dlss_probe_" + std::to_wstring(GetCurrentProcessId()) + L".txt");
        std::wstring command = L"\"" + std::wstring(self) + L"\" --probe-dlss \"" + folder.wstring() + L"\" \"" + result.wstring() + L"\"";
        STARTUPINFOW si{ sizeof(si) };
        PROCESS_INFORMATION pi{};
        bool available = false;
        std::string gpu, reason = "the check did not run";
        if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, folder.c_str(), &si, &pi))
        {
            if (WaitForSingleObject(pi.hProcess, 20000) != WAIT_OBJECT_0)
            {
                TerminateProcess(pi.hProcess, 1);
                reason = "the check did not finish (20 s)";
            }
            else
            {
                DWORD code = 1;
                GetExitCodeProcess(pi.hProcess, &code);
                reason = "the check crashed (code " + std::to_string(code) + ")";
                if (FILE* f = _wfopen(result.c_str(), L"rb"))
                {
                    char line[3][512] = {};
                    for (auto& l : line)
                        if (fgets(l, sizeof(l), f))
                            l[strcspn(l, "\r\n")] = 0;
                    fclose(f);
                    available = code == 0 && line[0][0] == '1';
                    gpu = line[1], reason = line[2];
                }
            }
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        std::error_code error;
        fs::remove(result, error);
        g_dlssProbe.available = available;
        g_dlssProbe.gpu = gpu;
        g_dlssProbe.reason = reason;
        Log("[gpu] DLSS check (%s): %s - %s", Narrow(folder.wstring()).c_str(), available ? "available" : "not available", reason.c_str());
        g_dlssProbe.state = 2;
    });
}

void JoinDlssProbe()
{
    if (g_dlssProbe.thread.joinable())
        g_dlssProbe.thread.join();
}
