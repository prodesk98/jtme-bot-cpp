#include <windows.h>
#include <shlobj.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <string>
#include <fstream>


struct WindowInfo {
    HWND hwnd;
    DWORD pid;
    std::string title;
};

std::vector<WindowInfo> matchingWindows;

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    char title[256];
    GetWindowTextA(hwnd, title, sizeof(title));

    if (IsWindowVisible(hwnd) && strlen(title) > 0) {
        std::string titleStr = title;
        if (titleStr.find("JTME") != std::string::npos) {
            DWORD pid;
            GetWindowThreadProcessId(hwnd, &pid);
            matchingWindows.push_back({ hwnd, pid, titleStr });
        }
    }

    return TRUE;
}

DWORD FindProcessIdByWindowTitle() {
    matchingWindows.clear();
    EnumWindows(EnumWindowsProc, 0);

    if (!matchingWindows.empty()) {
        const auto& win = matchingWindows.back();
        std::cout << "[*] Window: " << win.title << " (PID: " << win.pid << ")\n";
        return win.pid;
    }

    std::cerr << ".";
    return 0;
}

bool InjectDLL(DWORD processId, const char* dllPath) {
    HANDLE hProc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, processId);
    if (!hProc || hProc == INVALID_HANDLE_VALUE) return false;

    size_t len = strlen(dllPath) + 1;
    LPVOID remoteStr = VirtualAllocEx(hProc, nullptr, len, MEM_COMMIT, PAGE_READWRITE);
    if (!remoteStr) return false;

    if (!WriteProcessMemory(hProc, remoteStr, dllPath, len, nullptr)) return false;

    FARPROC loadLib = GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
    if (!loadLib) return false;

    HANDLE thread = CreateRemoteThread(hProc, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLib),
        remoteStr, 0, nullptr);
    if (!thread) return false;

    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    CloseHandle(hProc);
    return true;
}

std::string getCachePath() {
    char path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, path))) {
        return std::string(path) + R"(\JTME\cache)";
    }
    std::cerr << "Erro ao obter LOCAL_APPDATA\n";
    return "";
}

std::string getBasePath() {
    char buffer[MAX_PATH];
    GetModuleFileNameA(nullptr, buffer, MAX_PATH);
    const std::string fullPath(buffer);

    const size_t lastSlash = fullPath.find_last_of("\\/");
    return fullPath.substr(0, lastSlash);
}

bool runJavaws(const std::string& commandLine) {
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;

    // Cria cópia mutável da string (CreateProcess exige LPSTR)
    char cmdLine[1024];
    strncpy(cmdLine, commandLine.c_str(), sizeof(cmdLine) - 1);
    cmdLine[sizeof(cmdLine) - 1] = '\0';

    BOOL success = CreateProcessA(
        nullptr,
        cmdLine,
        nullptr,
        nullptr,
        FALSE,
        0,
        nullptr,
        nullptr,
        &si, &pi
    );

    if (!success) {
        std::cerr << "[!] CreateProcess failed: " << GetLastError() << "\n";
        return false;
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

std::string getAppDataPath() {
    char path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, path))) {
        return std::string(path);
    }
    return "";
}

void startJavaws() {
    std::string appDataPath = getAppDataPath();
    std::string jnlpHref = R"(https://otjtme.github.io/jtme-web/JTME_Web_241.jnlp)";
    std::string basePath = getBasePath();
    std::string javaws = basePath + R"(\jre\bin\javaws.exe)";
    std::string deployPath = basePath + R"(\config)";
    std::string cachePath = basePath + "\\" + appDataPath + R"(\LocalLow\Sun\Java\Deployment\cache\6.0\46\2e77fb2e-1b884d21)";

    if (!std::ifstream(javaws).good()) {
        std::cerr << "[!] javaws.exe not found at: " << javaws << "\n";
        return;
    }

    std::string cmd = "\"" + javaws + "\" -localfile -J-Djnlp.application.href=" + jnlpHref + " \"" + cachePath + "\"";
    std::cout << "[*] Running javaws: " << cmd << "\n";

    if (!runJavaws(cmd)) {
        std::cerr << "[!] Failed to start javaws.\n";
    }
}

int main() {
    const char* dllToInject = R"(C:\Users\prode\CLionProjects\DLL_Example2\cmake-build-debug\libDLL_Example2.dll)";

    std::string cachePath = getCachePath();

    std::cout << "[*] Starting JTME...\n";

    startJavaws();

    std::cout << "[*] Awaiting jp2launcher.exe...\n";

    DWORD pid = 0;
    for (int i = 0; i < 30; ++i) {
        pid = FindProcessIdByWindowTitle();
        if (pid) break;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    if (!pid) {
        std::cerr << "[!] Process jp2launcher.exe not found after waiting.\n";
        return 1;
    }

    std::cout << "[✓] Process jp2launcher.exe found with PID: " << pid << "\n";
    std::cout << "[*] Injecting DLL into process " << pid << "...\n";

    if (InjectDLL(pid, dllToInject)) {
        std::cout << "[ok] DLL injected successfully!\n";
    } else {
        std::cerr << "[!] Failed to inject DLL into process " << pid << ".\n";
        return 1;
    }

    return 0;
}