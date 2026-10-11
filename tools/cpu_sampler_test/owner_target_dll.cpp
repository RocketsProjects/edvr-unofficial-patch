// A second test module for the owner rule (src\common\cpu_sampler.h, chooseTargets). It exports VR_GetGenericInterface, as
// openvr_api.dll does, so a thread that starts in it is the owner thread. owner_test_thread spins until the flag it is given
// is set. Build: cl /LD.
#include <windows.h>

#pragma optimize("", off)

extern "C" __declspec(dllexport) void* VR_GetGenericInterface(const char*, int*) { return nullptr; }

extern "C" __declspec(dllexport) __declspec(noinline) DWORD WINAPI owner_test_thread(LPVOID param) {
    volatile LONG* stop = static_cast<volatile LONG*>(param);
    volatile unsigned long long n = 0;
    while (!*stop) n = n + 1;
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
