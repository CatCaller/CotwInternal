#include <Windows.h>

#include "application/application.hpp"

extern "C" BOOL WINAPI DllMain(const HMODULE module, const DWORD reason, LPVOID reserved)
{
    (void)reserved;

    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        const HANDLE thread = CreateThread(
            nullptr,
            0,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(&cotw::application::run),
            module,
            0,
            nullptr);

        if (thread != nullptr)
        {
            CloseHandle(thread);
        }
    }

    return TRUE;
}
