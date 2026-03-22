#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <psapi.h>
#include <iostream> 
#include <vector>
#include <string>

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
                    if (IsPlayingAd(title)) {
                        OutputDebugStringW((L"[TRIGGER MUTE] Ad detected: " + title + L"\n").c_str());
                        SetSpotifyMute(true);
                    }
                    else {
                        OutputDebugStringW((L"[UNMUTE] Song playing: " + title + L"\n").c_str());
                        SetSpotifyMute(false);
                    }

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

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine, _In_ int nCmdShow)
{
    // turn on COM - for WASAPI
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	// 1. Create the System Tray Icon here...

	// 2. Start the Infinite Loop
    while (true) {
        // EMERGENCY EXIT: Hold the Escape key to close the program
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            break;
        }
		// Find Spotify
		std::vector<TargetWindow> found;
		EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&found));

		if (!found.empty()) {
			// For now, let's just show a popup with the first one we found
			std::wstring msg = L"Found Spotify! Current Title: " + found[0].title;
			MessageBoxW(NULL, msg.c_str(), L"Detector", MB_OK);
		}
		else {
			MessageBoxW(NULL, L"Spotify not found. Is it open?", L"Detector", MB_OK);
		}
		// Check if it's playing an ad
		// Mute/Unmute

		Sleep(500); // Wait half a second so we don't cook the CPU
	}
    // turn off COM
    CoUninitialize();
	return 0;
}