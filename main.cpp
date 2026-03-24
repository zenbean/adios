#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <psapi.h>
#include <iostream> 
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <shellapi.h> // system tray
#include "resource.h"

#define WM_TRAYICON (WM_USER+1)
#define ID_TRAY_EXIT 1001

NOTIFYICONDATA notifyIconData = {};

// global flag controls infinite loop
std::atomic<bool> g_IsRunning = true;

void SetSpotifyMute(bool muteSpotify);

struct TargetWindow {
    HWND hwnd; // window handle
    std::wstring title; // wide string type
};

bool IsPlayingAd(const std::wstring& title) {
    // blacklist names
    if (title.find(L"Spotify Premium") != std::wstring::npos ||
        title.find(L"Advertisement") != std::wstring::npos ||
        title.find(L"Spotify") != std::wstring::npos ||
        title.find(L"ad-free") != std::wstring::npos) {
        return true;
    }

    // sometimes ads are just dashes
    if (!title.empty() && title.find_first_not_of(L"-") == std::wstring::npos) {
        return true;
    }

    if (title.find(L" - ") == std::wstring::npos) {
        return true;
    }

    return false;
}

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != NULL) return TRUE;

    // get dynamic PID of window
    DWORD processID;
    GetWindowThreadProcessId(hwnd, &processID);

    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processID);
    if (hProcess != NULL) {
        wchar_t processName[MAX_PATH] = L"<unknown>";
        HMODULE hMod;
        DWORD cbNeeded;

        // .exe name
        if (EnumProcessModules(hProcess, &hMod, sizeof(hMod), &cbNeeded)) {
            GetModuleBaseNameW(hProcess, hMod, processName, sizeof(processName) / sizeof(wchar_t));
        }
        CloseHandle(hProcess);

        std::wstring exeName(processName);

        if (exeName == L"Spotify.exe") {

            int length = GetWindowTextLengthW(hwnd);
            if (length > 0) {
                std::wstring title(length, L'\0');
                GetWindowTextW(hwnd, &title[0], length + 1);

                // ignore spotify invisible helper windows
                if (title != L"Default IME" && title != L"MSCTFIME UI") {
                    auto* foundWindows = reinterpret_cast<std::vector<TargetWindow>*>(lParam);
                    foundWindows->push_back({ hwnd, title });
                    return FALSE;
                }
            }
        }
    }
    return TRUE;
}

void SetSpotifyMute(bool muteSpotify) {
    IMMDeviceEnumerator* pEnumerator = NULL;
    IMMDevice* pDevice = NULL;
    IAudioSessionManager2* pSessionManager = NULL;
    IAudioSessionEnumerator* pSessionEnumerator = NULL;

    // Audio Device Enumerator
    CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
    if (!pEnumerator) return;

    // Default Audio Endpoint
    pEnumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &pDevice);
    if (pDevice) {

        // Get session manager for audio output
        pDevice->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, NULL, (void**)&pSessionManager);
        if (pSessionManager) {

            // list everything playing
            pSessionManager->GetSessionEnumerator(&pSessionEnumerator);
            if (pSessionEnumerator) {
                int count = 0;
                pSessionEnumerator->GetCount(&count);

                for (int i = 0; i < count; i++) {
                    IAudioSessionControl* pSessionControl = NULL;
                    IAudioSessionControl2* pSessionControl2 = NULL;
                    pSessionEnumerator->GetSession(i, &pSessionControl);
                    if (pSessionControl) {
                        pSessionControl->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&pSessionControl2);
                        if (pSessionControl2) {
                            DWORD pid = 0;
                            pSessionControl2->GetProcessId(&pid);

                            // look up the .exe name from the PID
                            HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
                            if (hProcess) {
                                wchar_t processName[MAX_PATH] = L"";
                                HMODULE hMod;
                                DWORD cbNeeded;
                                if (EnumProcessModules(hProcess, &hMod, sizeof(hMod), &cbNeeded)) {
                                    GetModuleBaseNameW(hProcess, hMod, processName, sizeof(processName) / sizeof(wchar_t));

                                    if (std::wstring(processName) == L"Spotify.exe") {

                                        // mute/unmute :)
                                        ISimpleAudioVolume* pVolumeControl = NULL;
                                        pSessionControl->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&pVolumeControl);
                                        if (pVolumeControl) {
                                            pVolumeControl->SetMute(muteSpotify, NULL);
                                            pVolumeControl->Release(); // remeber to release COM object
                                        }
                                    }
                                }
                                CloseHandle(hProcess);
                            }
                            pSessionControl2->Release();
                        }
                        pSessionControl->Release();
                    }
                }
                pSessionEnumerator->Release();
            }
            pSessionManager->Release();
        }
        pDevice->Release();
    }
    pEnumerator->Release();
}

void SpotifyMonitorThread() {
    // Initialize COM for background thread
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    while (g_IsRunning) {
        std::vector<TargetWindow> found;
        EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&found));

        if (!found.empty()) {
            std::wstring currentTitle = found[0].title;
            std::wstring msg = L"[TITLE] " + currentTitle + L"\n";
            OutputDebugStringW(msg.c_str());
            if (IsPlayingAd(currentTitle)) {
                OutputDebugStringW(L"[ACTION] Ad detected! Muting...\n");
                SetSpotifyMute(true);
            }
            else {
                OutputDebugStringW(L"[ACTION] Song detected. Unmuting...\n");
                SetSpotifyMute(false);
            }
            Sleep(500);
        }
        else {
            Sleep(5000);
        }
    }
    CoUninitialize();
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_TRAYICON:
        // right click?
        if (lParam == WM_RBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt); // get mouse coords
            HMENU hMenu = CreatePopupMenu();
            InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING, ID_TRAY_EXIT, L"Exit Spotify Muter");
            SetForegroundWindow(hwnd);
            TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(hMenu);
        }
        break;
    case WM_COMMAND:
        // lick exit?
        if (LOWORD(wParam) == ID_TRAY_EXIT) {
            g_IsRunning = false; // Kill the background audio thread
            PostQuitMessage(0);  // Kill the main window loop
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine, _In_ int nCmdShow)
{
    // background polling thread
    std::thread monitorThread(SpotifyMonitorThread);

    const wchar_t CLASS_NAME[] = L"SpotifyMuterHiddenClass";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(
        0, CLASS_NAME, L"Spotify Muter Controller",
        WS_OVERLAPPEDWINDOW,
        0, 0, 0, 0,
        HWND_MESSAGE,
        NULL, hInstance, NULL
    );

    notifyIconData.cbSize = sizeof(NOTIFYICONDATAW);
    notifyIconData.hWnd = hwnd;
    notifyIconData.uID = 1;
    notifyIconData.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    notifyIconData.uCallbackMessage = WM_TRAYICON;
    notifyIconData.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON1));
    wcscpy_s(notifyIconData.szTip, L"Spotify Ad Muter (Running)");

    Shell_NotifyIconW(NIM_ADD, &notifyIconData);

    // for tray icon
    MSG msg = { };

    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    Shell_NotifyIconW(NIM_DELETE, &notifyIconData);

    g_IsRunning = false;

    // wait for the background thread to finish 
    if (monitorThread.joinable()) {
        monitorThread.join();
    }

    return 0;
}