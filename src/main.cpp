#include "config.h"
#include "system.h"
#include "window.h"
#include "native_theme.h"

#include <exception>

#include <windows.h>

extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) DWORD AmdPowerXpressRequestHighPerformance = 1;
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int)
{
    SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_ABOVE_NORMAL);
    SetThreadDescription(GetCurrentThread(),L"SIMP Window / Input");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    initializeNativeTheme();
    if (!initializeSystemServices())
    {
        MessageBoxW(nullptr, L"Failed to initialize Windows imaging services.", L"SIMP", MB_OK | MB_ICONERROR);
        shutdownNativeTheme();
        return 1;
    }

    int exitCode = 0;

    try
    {
        load_config(getConfigPath());
        showSplashScreen(instance, nullptr, getPreferredMonitorBounds());
        initWindow(instance);

        while (processEvents())
        {
            if (shouldRender())
                renderFrame();
            else
                WaitMessage();
        }

        save_config(getConfigPath());
    }
    catch (const std::exception& error)
    {
        MessageBoxA(nullptr, error.what(), "SIMP Error", MB_OK | MB_ICONERROR);
        exitCode = 1;

        try
        {
            save_config(getConfigPath());
        }
        catch (...)
        {
        }
    }

    cleanupWindow();
    shutdownSystemServices();
    shutdownNativeTheme();
    return exitCode;
}
