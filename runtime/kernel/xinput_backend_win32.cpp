#include "xinput_backend.h"

#include <windows.h>
#include <xinput.h>

namespace input::xinput
{
    namespace
    {
        using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
        using XInputSetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);

        struct XInputApi
        {
            XInputGetStateFn getState{};
            XInputSetStateFn setState{};

            XInputApi()
            {
                HMODULE module = LoadLibraryW(L"xinput1_4.dll");
                if (!module)
                    module = LoadLibraryW(L"xinput9_1_0.dll");
                if (module)
                {
                    getState = reinterpret_cast<XInputGetStateFn>(GetProcAddress(module, "XInputGetState"));
                    setState = reinterpret_cast<XInputSetStateFn>(GetProcAddress(module, "XInputSetState"));
                }
            }
        };

        XInputApi& Api()
        {
            static XInputApi api;
            return api;
        }
    }

    bool GetState(uint32_t slot, XINPUT_STATE& state)
    {
        return Api().getState && Api().getState(slot, &state) == ERROR_SUCCESS;
    }

    void DevicesChanged() {}

    void SetState(uint32_t slot, uint16_t left, uint16_t right)
    {
        XINPUT_VIBRATION vibration{ left, right };
        if (Api().setState)
            Api().setState(slot, &vibration);
    }
}
