#if defined(_WIN32)
#include <windows.h>
#include <array>

// Minimal askpass helper for OpenSSH on Windows.
// Reads password from SSH_PROXY_PASSWORD env and writes to stdout.
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    WCHAR buffer[4096];
    DWORD len = GetEnvironmentVariableW(L"SSH_PROXY_PASSWORD", buffer,
                                        static_cast<DWORD>(std::size(buffer)));
    if (len == 0 || len >= std::size(buffer)) {
        return 1;
    }

    DWORD written = 0;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!out) {
        return 1;
    }
    if (!WriteFile(out, buffer, len * sizeof(WCHAR), &written, nullptr)) {
        return 1;
    }
    return 0;
}
#endif
