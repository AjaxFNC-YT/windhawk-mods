// ==WindhawkMod==
// @id              hide-from-screencapture
// @name            Capture Toggle
// @description     Toggle screen-capture exclusion for Windows 11 taskbar apps, with protected-app blocking and an optional hidden-window border.
// @version         1.0.0
// @author          AjaxFNC
// @github          https://github.com/AjaxFNC-YT
// @include         *
// @exclude         valorant-win64-shipping.exe
// @exclude         fortniteclient-win64-shipping.exe
// @exclude         easyanticheat.exe
// @exclude         easyanticheat_eos.exe
// @exclude         beservice.exe
// @exclude         beservice_x64.exe
// @exclude         vgc.exe
// @exclude         vgtray.exe
// @exclude         riotclientservices.exe
// @exclude         start_protected_game.exe
// @compilerOptions -lole32 -loleaut32 -luuid -lshell32 -ldwmapi -lshlwapi
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Capture Toggle
Hide applications from your screen capture (e.g. OBS, Zoom, Teams, Discord) by middle-clicking the app on your taskbar or by using a hover hotkey. This mod is designed for Windows 11 and may not work or be buggy on Windows 10.

## Safety
Using capture-hiding on games can cause instability, crashes, or even
anti-cheat trouble such as an in-game ban. Windhawk decides which processes the
mod is injected into before this mod's code runs.

The metadata excludes several known anti-cheat executables from injection, and
Windhawk globally excludes many common game and anti-cheat installation paths.
These exclusions are best-effort: renamed executables, other anti-cheat
components, and games in non-standard locations may still be injected.

The protected-app setting below is a separate guard which runs only after the
mod has already been injected. It can block the capture-hide action, but it
cannot prevent or undo injection and is not an anti-cheat safety guarantee.

You can disable the protection list in the mod settings, but targeting games can
still cause crashes, anti-cheat problems, or in-game bans.

## Trigger options
You can keep the original middle-click behavior, switch to a hover hotkey, or
allow both from the mod settings.

## Compatibility
This mod is designed for Windows 11. Windows 10 support is untested and is
most likely broken.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- ShowHiddenBorder: true
  $name: Show a colored border around hidden windows
  $description: Applies a colored border to windows that this mod currently hides from capture.

- HiddenBorderColor:
  - Red: 136
  - Green: 136
  - Blue: 136
  $name: Hidden window border color
  $description: RGB color used for the border around windows hidden from capture.

- DisableProtectionList: false
  $name: Disable protected-app blocking
  $description: Allows the mod to target apps that are normally blocked. This may cause crashes, anti-cheat issues, or in-game bans.

- TriggerMode: MiddleClick
  $name: Trigger mode
  $options:
  - MiddleClick: Middle click
  - HoverHotkey: Hover hotkey
  - Both: Middle click and hover hotkey

- TriggerModifier: None
  $name: Modifier for middle click
  $description: Optional modifier required for the middle-click trigger.
  $options:
  - None: None
  - Ctrl: Ctrl
  - Shift: Shift
  - Alt: Alt
  - Win: Win

- HoverHotkey: F8
  $name: Hover hotkey
  $description: Press this key while hovering a taskbar app button when hover hotkey mode is enabled.
  $options:
  - F8: F8
  - F9: F9
  - F10: F10
  - F11: F11
  - F12: F12
  - Pause: Pause
  - ScrollLock: Scroll Lock
*/
// ==/WindhawkModSettings==

#define _WIN32_WINNT 0x0A00
#define WINRT_LEAN_AND_MEAN

#include <windows.h>
#include <appmodel.h>
#include <dwmapi.h>
#include <shlwapi.h>
#include <uiautomation.h>
#include <wrl.h>

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <mutex>
#include <unordered_map>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {

extern "C" IMAGE_DOS_HEADER __ImageBase;

constexpr wchar_t kToggleMessageName[] =
    L"Windhawk.HideFromScreenCapture.ToggleMessage.v1";
constexpr wchar_t kReceiverWindowClassName[] =
    L"Windhawk.HideFromScreenCapture.Receiver.v1";
constexpr UINT kSwallowWindowMs = 500;
constexpr UINT kToggleSendTimeoutMs = 700;
constexpr UINT kExplorerHookRetryTimerId = 1;
constexpr UINT kExplorerHookRetryIntervalMs = 10000;
constexpr UINT kExplorerToggleMessage = WM_APP + 1;
constexpr UINT kStopReceiverMessage = WM_APP + 2;
constexpr COLORREF kDwmDefaultColor = 0xFFFFFFFF;

constexpr DWORD kWdaExcludeFromCapture = 0x00000011;
constexpr DWORD kWdaMonitor = 0x00000001;

#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif

enum class ToggleResult : ULONG_PTR {
    Failed = 0,
    Hidden = 1,
    Unhidden = 2,
    HiddenCompatibility = 3,
};

struct UiaButtonDescriptor {
    std::wstring name;
    std::wstring automationId;
    std::wstring className;
    std::wstring frameworkId;
    std::wstring helpText;
    std::wstring itemStatus;
};

enum class TriggerMode {
    MiddleClick = 0,
    HoverHotkey = 1,
    Both = 2,
};

enum class TriggerModifier {
    None = 0,
    Ctrl = 1,
    Shift = 2,
    Alt = 3,
    Win = 4,
};

enum class HoverHotkey {
    F8 = 0,
    F9 = 1,
    F10 = 2,
    F11 = 3,
    F12 = 4,
    Pause = 5,
    ScrollLock = 6,
};

struct ModSettings {
    bool showHiddenBorder = true;
    COLORREF hiddenBorderColor = RGB(136, 136, 136);
    bool disableProtectionList = false;
    TriggerMode triggerMode = TriggerMode::MiddleClick;
    TriggerModifier triggerModifier = TriggerModifier::None;
    HoverHotkey hoverHotkey = HoverHotkey::F8;
};

UINT g_toggleMessage = 0;

std::atomic<bool> g_processIsExplorer{false};
std::atomic<bool> g_processIsProtected{false};
std::atomic<bool> g_unloading{false};
std::mutex g_windowAffinityMutex;
std::unordered_map<HWND, DWORD> g_managedWindowOriginalAffinity;
std::unordered_map<HWND, COLORREF> g_managedWindowOriginalBorderColor;
ModSettings g_settings;

HANDLE g_receiverThread = nullptr;
DWORD g_receiverThreadId = 0;
HANDLE g_receiverReadyEvent = nullptr;
std::atomic<HWND> g_receiverWindow{nullptr};

HANDLE g_explorerHookThread = nullptr;
DWORD g_explorerHookThreadId = 0;
HANDLE g_explorerWorkerThread = nullptr;
DWORD g_explorerWorkerThreadId = 0;
HANDLE g_explorerWorkerReadyEvent = nullptr;
HHOOK g_explorerMouseHook = nullptr;
HHOOK g_explorerKeyboardHook = nullptr;
ComPtr<IUIAutomation> g_uia;

std::atomic<DWORD> g_swallowStartTick{0};
std::atomic<bool> g_hotkeyDown{false};
std::atomic<bool> g_hotkeySwallowed{false};

template <size_t N>
bool SettingEquals(PCWSTR value, const wchar_t (&literal)[N]) {
    return value && wcscmp(value, literal) == 0;
}

void LoadSettings() {
    g_settings.showHiddenBorder = Wh_GetIntSetting(L"ShowHiddenBorder") != 0;
    const BYTE borderRed = static_cast<BYTE>(std::clamp(
        Wh_GetIntSetting(L"HiddenBorderColor.Red"), 0, 255));
    const BYTE borderGreen = static_cast<BYTE>(std::clamp(
        Wh_GetIntSetting(L"HiddenBorderColor.Green"), 0, 255));
    const BYTE borderBlue = static_cast<BYTE>(std::clamp(
        Wh_GetIntSetting(L"HiddenBorderColor.Blue"), 0, 255));
    g_settings.hiddenBorderColor = RGB(borderRed, borderGreen, borderBlue);
    g_settings.disableProtectionList =
        Wh_GetIntSetting(L"DisableProtectionList") != 0;

    PCWSTR triggerModeSetting = Wh_GetStringSetting(L"TriggerMode");
    if (SettingEquals(triggerModeSetting, L"HoverHotkey")) {
        g_settings.triggerMode = TriggerMode::HoverHotkey;
    } else if (SettingEquals(triggerModeSetting, L"Both")) {
        g_settings.triggerMode = TriggerMode::Both;
    } else {
        g_settings.triggerMode = TriggerMode::MiddleClick;
    }
    Wh_FreeStringSetting(triggerModeSetting);

    PCWSTR triggerModifierSetting = Wh_GetStringSetting(L"TriggerModifier");
    if (SettingEquals(triggerModifierSetting, L"Ctrl")) {
        g_settings.triggerModifier = TriggerModifier::Ctrl;
    } else if (SettingEquals(triggerModifierSetting, L"Shift")) {
        g_settings.triggerModifier = TriggerModifier::Shift;
    } else if (SettingEquals(triggerModifierSetting, L"Alt")) {
        g_settings.triggerModifier = TriggerModifier::Alt;
    } else if (SettingEquals(triggerModifierSetting, L"Win")) {
        g_settings.triggerModifier = TriggerModifier::Win;
    } else {
        g_settings.triggerModifier = TriggerModifier::None;
    }
    Wh_FreeStringSetting(triggerModifierSetting);

    PCWSTR hoverHotkeySetting = Wh_GetStringSetting(L"HoverHotkey");
    if (SettingEquals(hoverHotkeySetting, L"F9")) {
        g_settings.hoverHotkey = HoverHotkey::F9;
    } else if (SettingEquals(hoverHotkeySetting, L"F10")) {
        g_settings.hoverHotkey = HoverHotkey::F10;
    } else if (SettingEquals(hoverHotkeySetting, L"F11")) {
        g_settings.hoverHotkey = HoverHotkey::F11;
    } else if (SettingEquals(hoverHotkeySetting, L"F12")) {
        g_settings.hoverHotkey = HoverHotkey::F12;
    } else if (SettingEquals(hoverHotkeySetting, L"Pause")) {
        g_settings.hoverHotkey = HoverHotkey::Pause;
    } else if (SettingEquals(hoverHotkeySetting, L"ScrollLock")) {
        g_settings.hoverHotkey = HoverHotkey::ScrollLock;
    } else {
        g_settings.hoverHotkey = HoverHotkey::F8;
    }
    Wh_FreeStringSetting(hoverHotkeySetting);

    Wh_Log(L"LoadSettings: showHiddenBorder=%d hiddenBorderColor=0x%08X disableProtectionList=%d triggerMode=%d triggerModifier=%d hoverHotkey=%d",
        g_settings.showHiddenBorder ? 1 : 0,
        g_settings.hiddenBorderColor,
        g_settings.disableProtectionList ? 1 : 0,
        static_cast<int>(g_settings.triggerMode),
        static_cast<int>(g_settings.triggerModifier),
        static_cast<int>(g_settings.hoverHotkey));
}

bool TriggerModeHasMouse() {
    return g_settings.triggerMode == TriggerMode::MiddleClick ||
           g_settings.triggerMode == TriggerMode::Both;
}

bool TriggerModeHasHotkey() {
    return g_settings.triggerMode == TriggerMode::HoverHotkey ||
           g_settings.triggerMode == TriggerMode::Both;
}

int GetConfiguredHotkeyVk() {
    switch (g_settings.hoverHotkey) {
        case HoverHotkey::F9:
            return VK_F9;
        case HoverHotkey::F10:
            return VK_F10;
        case HoverHotkey::F11:
            return VK_F11;
        case HoverHotkey::F12:
            return VK_F12;
        case HoverHotkey::Pause:
            return VK_PAUSE;
        case HoverHotkey::ScrollLock:
            return VK_SCROLL;
        case HoverHotkey::F8:
        default:
            return VK_F8;
    }
}

bool IsModifierSatisfied() {
    switch (g_settings.triggerModifier) {
        case TriggerModifier::Ctrl:
            return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        case TriggerModifier::Shift:
            return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        case TriggerModifier::Alt:
            return (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        case TriggerModifier::Win:
            return ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) &
                    0x8000) != 0;
        case TriggerModifier::None:
        default:
            return true;
    }
}

UINT EnsureToggleMessageRegistered() {
    if (g_toggleMessage != 0) {
        return g_toggleMessage;
    }

    const UINT msg = RegisterWindowMessageW(kToggleMessageName);
    if (msg == 0) {
        Wh_Log(L"EnsureToggleMessageRegistered: RegisterWindowMessageW failed gle=%lu",
            GetLastError());
        return 0;
    }

    g_toggleMessage = msg;
    Wh_Log(L"EnsureToggleMessageRegistered: registered msg=0x%X pid=%lu", msg,
        GetCurrentProcessId());
    return msg;
}

std::wstring ToLower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
    return value;
}

std::wstring GetWindowTextString(HWND hwnd) {
    const int length = GetWindowTextLengthW(hwnd);
    if (length <= 0) {
        return L"";
    }

    std::wstring text(length + 1, L'\0');
    const int copied = GetWindowTextW(hwnd, &text[0], length + 1);
    if (copied <= 0) {
        return L"";
    }

    text.resize(copied);
    return text;
}

std::wstring GetModuleBaseNameForPid(DWORD pid) {
    std::wstring result;

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        return result;
    }

    wchar_t path[MAX_PATH];
    DWORD size = ARRAYSIZE(path);
    if (QueryFullProcessImageNameW(process, 0, path, &size)) {
        PCWSTR fileName = PathFindFileNameW(path);
        if (fileName && *fileName) {
            result.assign(fileName);
            const size_t dot = result.rfind(L'.');
            if (dot != std::wstring::npos) {
                result.resize(dot);
            }
        }
    }

    CloseHandle(process);
    return result;
}

std::wstring GetProcessImagePathForPid(DWORD pid) {
    std::wstring result;

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        return result;
    }

    wchar_t path[MAX_PATH * 4];
    DWORD size = ARRAYSIZE(path);
    if (QueryFullProcessImageNameW(process, 0, path, &size)) {
        result.assign(path, size);
    }

    CloseHandle(process);
    return result;
}

bool PathStartsWithInsensitive(const std::wstring& path,
                               const std::wstring& prefix) {
    if (path.size() < prefix.size()) {
        return false;
    }

    return _wcsnicmp(path.c_str(), prefix.c_str(), prefix.size()) == 0;
}

bool PathContainsInsensitive(const std::wstring& path,
                             const std::wstring& snippet) {
    return ToLower(path).find(ToLower(snippet)) != std::wstring::npos;
}

bool IsProtectedProcessPath(const std::wstring& imagePath) {
    if (g_settings.disableProtectionList) {
        return false;
    }

    if (imagePath.empty()) {
        return false;
    }

    const std::wstring lowerPath = ToLower(imagePath);
    const wchar_t* fileNamePtr = PathFindFileNameW(imagePath.c_str());
    const std::wstring fileName = fileNamePtr ? ToLower(fileNamePtr) : L"";

    if (fileName == L"valorant-win64-shipping.exe" ||
        fileName == L"fortniteclient-win64-shipping.exe") {
        return true;
    }

    if (PathStartsWithInsensitive(lowerPath, L"c:\\riot games\\")) {
        return true;
    }

    return PathContainsInsensitive(lowerPath, L"\\riot vanguard\\") ||
           PathContainsInsensitive(lowerPath, L"\\easyanticheat\\") ||
           PathContainsInsensitive(lowerPath, L"\\easyanticheat_eos\\") ||
           PathContainsInsensitive(lowerPath, L"\\battleye\\");
}

bool IsProtectedWindowTarget(HWND hwnd, std::wstring* imagePathOut = nullptr) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) {
        return false;
    }

    const std::wstring imagePath = GetProcessImagePathForPid(pid);
    if (imagePathOut) {
        *imagePathOut = imagePath;
    }

    return IsProtectedProcessPath(imagePath);
}

bool IsWindowCloaked(HWND hwnd) {
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked,
                                           sizeof(cloaked))) &&
           cloaked != 0;
}

bool IsTaskbarWindowClass(const wchar_t* className) {
    return className &&
           (_wcsicmp(className, L"Shell_TrayWnd") == 0 ||
            _wcsicmp(className, L"Shell_SecondaryTrayWnd") == 0);
}

bool IsTaskbarHostWindow(HWND hwnd) {
    wchar_t className[128];
    if (!GetClassNameW(hwnd, className, ARRAYSIZE(className))) {
        return false;
    }
    return IsTaskbarWindowClass(className);
}

bool IsPointOnTaskbar(POINT pt, HWND* taskbarRoot) {
    HWND hwnd = WindowFromPoint(pt);
    while (hwnd) {
        if (IsTaskbarHostWindow(hwnd)) {
            if (taskbarRoot) {
                *taskbarRoot = hwnd;
            }
            return true;
        }
        hwnd = GetParent(hwnd);
    }

    return false;
}

bool IsCandidateTopLevelWindow(HWND hwnd) {
    if (!IsWindow(hwnd)) {
        return false;
    }
    if (GetAncestor(hwnd, GA_ROOT) != hwnd) {
        return false;
    }

    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if (style & WS_CHILD) {
        return false;
    }

    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW) {
        return false;
    }

    if (!IsWindowVisible(hwnd) || IsWindowCloaked(hwnd)) {
        return false;
    }

    wchar_t className[128];
    if (GetClassNameW(hwnd, className, ARRAYSIZE(className)) &&
        IsTaskbarWindowClass(className)) {
        return false;
    }

    return true;
}

bool IsDefinitelyNonPrimaryWindowClass(const wchar_t* className) {
    if (!className || !*className) {
        return false;
    }

    return _wcsicmp(className, L"IME") == 0 ||
           _wcsicmp(className, L"MSCTFIME UI") == 0 ||
           _wcsicmp(className, L"Default IME") == 0 ||
           _wcsicmp(className, L"WinEventWindow") == 0 ||
           _wcsicmp(className, L"OLEChannelWnd") == 0 ||
           _wcsicmp(className, L"ApplicationManager_DesktopShellWindow") == 0;
}

bool IsLikelyUserFacingPrimaryWindow(HWND hwnd) {
    if (!IsCandidateTopLevelWindow(hwnd)) {
        return false;
    }

    if (GetWindow(hwnd, GW_OWNER)) {
        return false;
    }

    RECT rc{};
    if (!GetWindowRect(hwnd, &rc)) {
        return false;
    }

    if ((rc.right - rc.left) < 80 || (rc.bottom - rc.top) < 80) {
        return false;
    }

    wchar_t className[128]{};
    if (GetClassNameW(hwnd, className, ARRAYSIZE(className)) &&
        IsDefinitelyNonPrimaryWindowClass(className)) {
        return false;
    }

    return true;
}

std::wstring GetElementString(
    IUIAutomationElement* element,
    HRESULT(STDMETHODCALLTYPE IUIAutomationElement::*getter)(BSTR*)) {
    if (!element) {
        return L"";
    }

    BSTR value = nullptr;
    std::wstring result;
    if (SUCCEEDED((element->*getter)(&value)) && value) {
        result.assign(value, SysStringLen(value));
        SysFreeString(value);
    }
    return result;
}

CONTROLTYPEID GetElementControlType(IUIAutomationElement* element) {
    CONTROLTYPEID controlType = 0;
    if (element) {
        element->get_CurrentControlType(&controlType);
    }
    return controlType;
}

bool LooksLikeTaskbarButtonElement(const UiaButtonDescriptor& descriptor,
                                   CONTROLTYPEID controlType) {
    if (descriptor.name.empty()) {
        return false;
    }

    const std::wstring lowerClass = ToLower(descriptor.className);
    const std::wstring lowerFramework = ToLower(descriptor.frameworkId);
    const std::wstring lowerAutomationId = ToLower(descriptor.automationId);

    if (lowerClass.find(L"taskbar") != std::wstring::npos ||
        lowerAutomationId.find(L"taskbar") != std::wstring::npos) {
        return true;
    }

    if (lowerFramework == L"xaml" &&
        (controlType == UIA_ButtonControlTypeId ||
         controlType == UIA_ListItemControlTypeId ||
         controlType == UIA_TabItemControlTypeId)) {
        return true;
    }

    return false;
}

HWND TryGetDirectWindowFromElement(IUIAutomationElement* element,
                                   HWND taskbarRoot) {
    if (!element) {
        return nullptr;
    }

    UIA_HWND nativeHwnd = 0;
    if (FAILED(element->get_CurrentNativeWindowHandle(&nativeHwnd)) ||
        !nativeHwnd) {
        return nullptr;
    }

    HWND hwnd = reinterpret_cast<HWND>(nativeHwnd);
    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (!root || root == taskbarRoot || IsTaskbarHostWindow(root)) {
        return nullptr;
    }

    return IsCandidateTopLevelWindow(root) ? root : nullptr;
}

HWND TryGetWindowFromAutomationId(const std::wstring& automationId,
                                  HWND taskbarRoot) {
    constexpr wchar_t prefix[] = L"Window:";
    if (_wcsnicmp(automationId.c_str(), prefix, ARRAYSIZE(prefix) - 1) != 0) {
        return nullptr;
    }

    const wchar_t* value = automationId.c_str() + ARRAYSIZE(prefix) - 1;
    while (iswspace(*value)) {
        ++value;
    }
    wchar_t* end = nullptr;
    const unsigned long long rawHandle = wcstoull(value, &end, 0);
    while (end && iswspace(*end)) {
        ++end;
    }
    if (!rawHandle || end == value || (end && *end)) {
        return nullptr;
    }

    HWND hwnd = reinterpret_cast<HWND>(
        static_cast<ULONG_PTR>(rawHandle));
    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (!root || root == taskbarRoot || IsTaskbarHostWindow(root)) {
        return nullptr;
    }

    return IsCandidateTopLevelWindow(root) ? root : nullptr;
}

std::wstring GetProcessAppUserModelId(DWORD pid) {
    std::wstring appId;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        return appId;
    }

    UINT length = 0;
    LONG result = GetApplicationUserModelId(process, &length, nullptr);
    if (result == ERROR_INSUFFICIENT_BUFFER && length > 1) {
        std::wstring buffer(length, L'\0');
        result = GetApplicationUserModelId(process, &length, buffer.data());
        if (result == ERROR_SUCCESS) {
            if (length && buffer[length - 1] == L'\0') {
                --length;
            }
            buffer.resize(length);
            appId = std::move(buffer);
        }
    }

    CloseHandle(process);
    return appId;
}

std::wstring GetAppIdFromAutomationId(const std::wstring& automationId) {
    constexpr wchar_t prefix[] = L"Appid:";
    if (_wcsnicmp(automationId.c_str(), prefix, ARRAYSIZE(prefix) - 1) != 0) {
        return L"";
    }

    const wchar_t* value = automationId.c_str() + ARRAYSIZE(prefix) - 1;
    while (iswspace(*value)) {
        ++value;
    }
    return value;
}

struct ExactAppIdMatchContext {
    std::wstring appId;
    HWND firstWindow = nullptr;
    DWORD matchedPid = 0;
    bool ambiguous = false;
};

BOOL CALLBACK EnumWindowsForExactAppIdMatch(HWND hwnd, LPARAM lParam) {
    auto* context = reinterpret_cast<ExactAppIdMatchContext*>(lParam);
    if (!context || !IsLikelyUserFacingPrimaryWindow(hwnd)) {
        return TRUE;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid || pid == GetCurrentProcessId() ||
        _wcsicmp(GetProcessAppUserModelId(pid).c_str(),
                 context->appId.c_str()) != 0) {
        return TRUE;
    }

    if (!context->matchedPid) {
        context->matchedPid = pid;
        context->firstWindow = hwnd;
    } else if (context->matchedPid != pid) {
        context->ambiguous = true;
        return FALSE;
    }
    return TRUE;
}

HWND TryGetWindowFromExactAppId(const std::wstring& automationId) {
    ExactAppIdMatchContext context;
    context.appId = GetAppIdFromAutomationId(automationId);
    if (context.appId.empty()) {
        return nullptr;
    }

    EnumWindows(EnumWindowsForExactAppIdMatch,
                reinterpret_cast<LPARAM>(&context));
    return !context.ambiguous ? context.firstWindow : nullptr;
}

std::wstring NormalizeIdentityText(const std::wstring& value) {
    std::wstring normalized;
    normalized.reserve(value.size());
    bool pendingSpace = false;
    for (wchar_t ch : value) {
        if (iswalnum(ch)) {
            if (pendingSpace && !normalized.empty()) {
                normalized.push_back(L' ');
            }
            normalized.push_back(static_cast<wchar_t>(towlower(ch)));
            pendingSpace = false;
        } else {
            pendingSpace = true;
        }
    }
    return normalized;
}

std::wstring GetTaskbarAppName(const std::wstring& accessibleName) {
    std::wstring name = accessibleName;
    const std::wstring lower = ToLower(name);
    const size_t runningSuffix = lower.rfind(L" - ");
    if (runningSuffix != std::wstring::npos) {
        const std::wstring suffix = lower.substr(runningSuffix + 3);
        if (suffix.find(L"running window") != std::wstring::npos ||
            suffix.find(L"pinned") != std::wstring::npos) {
            name.resize(runningSuffix);
        }
    }
    return NormalizeIdentityText(name);
}

struct UniqueProcessMatchContext {
    std::wstring appName;
    HWND firstWindow = nullptr;
    DWORD matchedPid = 0;
    bool ambiguous = false;
};

bool ContainsWholeIdentityPhrase(const std::wstring& text,
                                 const std::wstring& phrase) {
    size_t position = text.find(phrase);
    while (position != std::wstring::npos) {
        const bool startsAtBoundary =
            position == 0 || text[position - 1] == L' ';
        const size_t end = position + phrase.size();
        const bool endsAtBoundary =
            end == text.size() || text[end] == L' ';
        if (startsAtBoundary && endsAtBoundary) {
            return true;
        }
        position = text.find(phrase, position + 1);
    }
    return false;
}

BOOL CALLBACK EnumWindowsForUniqueProcessMatch(HWND hwnd, LPARAM lParam) {
    auto* context = reinterpret_cast<UniqueProcessMatchContext*>(lParam);
    if (!context || !IsLikelyUserFacingPrimaryWindow(hwnd)) {
        return TRUE;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid || pid == GetCurrentProcessId()) {
        return TRUE;
    }

    std::wstring exeName = GetModuleBaseNameForPid(pid);
    const size_t extension = exeName.find_last_of(L'.');
    if (extension != std::wstring::npos) {
        exeName.resize(extension);
    }

    const std::wstring normalizedExe = NormalizeIdentityText(exeName);
    const std::wstring normalizedTitle =
        NormalizeIdentityText(GetWindowTextString(hwnd));
    const bool exactExe = !normalizedExe.empty() &&
                          normalizedExe == context->appName;
    const bool titleContainsApp =
        ContainsWholeIdentityPhrase(normalizedTitle, context->appName);
    if (!exactExe && !titleContainsApp) {
        return TRUE;
    }

    if (!context->matchedPid) {
        context->matchedPid = pid;
        context->firstWindow = hwnd;
    } else if (context->matchedPid != pid) {
        context->ambiguous = true;
        return FALSE;
    }
    return TRUE;
}

HWND TryGetUniqueProcessWindowFromDescriptor(
    const UiaButtonDescriptor& descriptor) {
    UniqueProcessMatchContext context;
    context.appName = GetTaskbarAppName(descriptor.name);
    if (context.appName.empty()) {
        return nullptr;
    }

    EnumWindows(EnumWindowsForUniqueProcessMatch,
                reinterpret_cast<LPARAM>(&context));
    return !context.ambiguous ? context.firstWindow : nullptr;
}

struct UniqueHiddenProcessContext {
    HWND firstWindow = nullptr;
    DWORD matchedPid = 0;
    bool ambiguous = false;
};

BOOL CALLBACK EnumWindowsForUniqueHiddenProcess(HWND hwnd, LPARAM lParam) {
    auto* context = reinterpret_cast<UniqueHiddenProcessContext*>(lParam);
    if (!context || !IsLikelyUserFacingPrimaryWindow(hwnd)) {
        return TRUE;
    }

    DWORD affinity = WDA_NONE;
    if (!GetWindowDisplayAffinity(hwnd, &affinity) ||
        (affinity != kWdaExcludeFromCapture && affinity != kWdaMonitor)) {
        return TRUE;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid || pid == GetCurrentProcessId()) {
        return TRUE;
    }

    if (!context->matchedPid) {
        context->matchedPid = pid;
        context->firstWindow = hwnd;
    } else if (context->matchedPid != pid) {
        context->ambiguous = true;
        return FALSE;
    }
    return TRUE;
}

HWND TryGetOnlyCaptureHiddenProcessWindow() {
    UniqueHiddenProcessContext context;
    EnumWindows(EnumWindowsForUniqueHiddenProcess,
                reinterpret_cast<LPARAM>(&context));
    return !context.ambiguous ? context.firstWindow : nullptr;
}

std::wstring DescribeWindowForUser(HWND hwnd);

bool ResolveTaskbarButtonTargetWindow(POINT pt,
                                      HWND* targetWindow,
                                      std::wstring* displayName) {
    if (!targetWindow) {
        return false;
    }

    *targetWindow = nullptr;
    if (displayName) {
        displayName->clear();
    }

    HWND taskbarRoot = nullptr;
    if (!IsPointOnTaskbar(pt, &taskbarRoot)) {
        Wh_Log(L"ResolveTaskbarButtonTargetWindow: point (%ld,%ld) is not on taskbar",
            pt.x, pt.y);
        return false;
    }

    if (!g_uia) {
        if (FAILED(CoCreateInstance(CLSID_CUIAutomation8, nullptr,
                                    CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&g_uia)))) {
            g_uia.Reset();
            if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr,
                                        CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&g_uia)))) {
                Wh_Log(L"ResolveTaskbarButtonTargetWindow: failed to create UI Automation");
                return false;
            }
        }
    }

    ComPtr<IUIAutomationElement> elementAtPoint;
    if (FAILED(g_uia->ElementFromPoint(pt, &elementAtPoint)) || !elementAtPoint) {
        Wh_Log(L"ResolveTaskbarButtonTargetWindow: UIA ElementFromPoint failed at (%ld,%ld)",
            pt.x, pt.y);
        return false;
    }

    ComPtr<IUIAutomationTreeWalker> controlWalker;
    if (FAILED(g_uia->get_ControlViewWalker(&controlWalker)) || !controlWalker) {
        Wh_Log(L"ResolveTaskbarButtonTargetWindow: failed to get control-view walker");
        return false;
    }

    UiaButtonDescriptor bestDescriptor;
    ComPtr<IUIAutomationElement> current = elementAtPoint;
    for (int depth = 0; current && depth < 16; ++depth) {
        if (HWND directWindow = TryGetDirectWindowFromElement(current.Get(), taskbarRoot)) {
            *targetWindow = directWindow;
            if (displayName) {
                const std::wstring title = GetWindowTextString(directWindow);
                if (!title.empty()) {
                    *displayName = title;
                } else {
                    DWORD pid = 0;
                    GetWindowThreadProcessId(directWindow, &pid);
                    *displayName = GetModuleBaseNameForPid(pid);
                }
            }
            Wh_Log(L"ResolveTaskbarButtonTargetWindow: direct UIA native hwnd match -> hwnd=0x%p label=%s",
                directWindow, displayName ? displayName->c_str() : L"");
            return true;
        }

        UiaButtonDescriptor descriptor;
        descriptor.name = GetElementString(current.Get(),
                                           &IUIAutomationElement::get_CurrentName);
        descriptor.automationId =
            GetElementString(current.Get(),
                             &IUIAutomationElement::get_CurrentAutomationId);
        descriptor.className =
            GetElementString(current.Get(),
                             &IUIAutomationElement::get_CurrentClassName);
        descriptor.frameworkId =
            GetElementString(current.Get(),
                             &IUIAutomationElement::get_CurrentFrameworkId);
        descriptor.helpText =
            GetElementString(current.Get(),
                             &IUIAutomationElement::get_CurrentHelpText);
        descriptor.itemStatus =
            GetElementString(current.Get(),
                             &IUIAutomationElement::get_CurrentItemStatus);

        if (HWND automationIdWindow = TryGetWindowFromAutomationId(
                descriptor.automationId, taskbarRoot)) {
            *targetWindow = automationIdWindow;
            if (displayName) {
                *displayName = DescribeWindowForUser(automationIdWindow);
            }
            Wh_Log(L"ResolveTaskbarButtonTargetWindow: AutomationId hwnd match -> hwnd=0x%p automationId='%s'",
                automationIdWindow, descriptor.automationId.c_str());
            return true;
        }

        if (HWND appIdWindow =
                TryGetWindowFromExactAppId(descriptor.automationId)) {
            *targetWindow = appIdWindow;
            if (displayName) {
                *displayName = DescribeWindowForUser(appIdWindow);
            }
            DWORD pid = 0;
            GetWindowThreadProcessId(appIdWindow, &pid);
            Wh_Log(L"ResolveTaskbarButtonTargetWindow: exact AppUserModelID match -> hwnd=0x%p pid=%lu automationId='%s'",
                appIdWindow, pid, descriptor.automationId.c_str());
            return true;
        }

        if (bestDescriptor.name.empty() &&
            LooksLikeTaskbarButtonElement(descriptor,
                                          GetElementControlType(current.Get()))) {
            bestDescriptor = descriptor;
            Wh_Log(L"ResolveTaskbarButtonTargetWindow: selected taskbar descriptor name='%s' automationId='%s' class='%s' framework='%s' help='%s' status='%s'",
                bestDescriptor.name.c_str(),
                bestDescriptor.automationId.c_str(),
                bestDescriptor.className.c_str(),
                bestDescriptor.frameworkId.c_str(),
                bestDescriptor.helpText.c_str(),
                bestDescriptor.itemStatus.c_str());
        }

        ComPtr<IUIAutomationElement> parent;
        if (FAILED(controlWalker->GetParentElement(current.Get(), &parent)) ||
            !parent) {
            break;
        }
        current = parent;
    }

    if (HWND uniqueProcessWindow =
            TryGetUniqueProcessWindowFromDescriptor(bestDescriptor)) {
        *targetWindow = uniqueProcessWindow;
        if (displayName) {
            *displayName = DescribeWindowForUser(uniqueProcessWindow);
        }
        DWORD pid = 0;
        GetWindowThreadProcessId(uniqueProcessWindow, &pid);
        Wh_Log(L"ResolveTaskbarButtonTargetWindow: unique process match -> hwnd=0x%p pid=%lu label='%s'",
            uniqueProcessWindow, pid,
            displayName ? displayName->c_str() : L"");
        return true;
    }

    // UIA can occasionally return an empty taskbar element. Only use this
    // recovery path to unhide: never choose an ordinary visible window, and
    // refuse to guess if capture-hidden windows belong to multiple processes.
    if (bestDescriptor.name.empty() &&
        bestDescriptor.automationId.empty()) {
        if (HWND hiddenWindow = TryGetOnlyCaptureHiddenProcessWindow()) {
            *targetWindow = hiddenWindow;
            if (displayName) {
                *displayName = DescribeWindowForUser(hiddenWindow);
            }
            DWORD pid = 0;
            GetWindowThreadProcessId(hiddenWindow, &pid);
            Wh_Log(L"ResolveTaskbarButtonTargetWindow: empty UIA recovery selected only capture-hidden process hwnd=0x%p pid=%lu label='%s'",
                hiddenWindow, pid,
                displayName ? displayName->c_str() : L"");
            return true;
        }
    }

    Wh_Log(L"ResolveTaskbarButtonTargetWindow: no unambiguous target found for taskbar button name='%s' automationId='%s'",
        bestDescriptor.name.c_str(), bestDescriptor.automationId.c_str());
    return false;
}

std::wstring DescribeWindowForUser(HWND hwnd) {
    std::wstring title = GetWindowTextString(hwnd);
    if (!title.empty()) {
        return title;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    std::wstring exeBase = GetModuleBaseNameForPid(pid);
    return exeBase.empty() ? L"Application" : exeBase;
}

bool IsWindowManagedByMod(HWND hwnd) {
    std::scoped_lock lock(g_windowAffinityMutex);
    return g_managedWindowOriginalAffinity.find(hwnd) !=
           g_managedWindowOriginalAffinity.end();
}

bool HasManagedWindows() {
    std::scoped_lock lock(g_windowAffinityMutex);
    return !g_managedWindowOriginalAffinity.empty();
}

void RememberManagedWindowAffinity(HWND hwnd, DWORD originalAffinity) {
    std::scoped_lock lock(g_windowAffinityMutex);
    const auto [it, inserted] =
        g_managedWindowOriginalAffinity.emplace(hwnd, originalAffinity);
    Wh_Log(L"RememberManagedWindowAffinity: hwnd=0x%p originalAffinity=0x%08X inserted=%d stored=0x%08X",
        hwnd, originalAffinity, inserted ? 1 : 0, it->second);
}

bool GetManagedWindowOriginalAffinity(HWND hwnd, DWORD* originalAffinity) {
    if (!originalAffinity) {
        return false;
    }

    std::scoped_lock lock(g_windowAffinityMutex);
    auto it = g_managedWindowOriginalAffinity.find(hwnd);
    if (it == g_managedWindowOriginalAffinity.end()) {
        return false;
    }

    *originalAffinity = it->second;
    return true;
}

void ForgetManagedWindowAffinity(HWND hwnd) {
    std::scoped_lock lock(g_windowAffinityMutex);
    const size_t erased = g_managedWindowOriginalAffinity.erase(hwnd);
    Wh_Log(L"ForgetManagedWindowAffinity: hwnd=0x%p erased=%zu remaining=%zu", hwnd,
        erased, g_managedWindowOriginalAffinity.size());
}

void ApplyHiddenBorderIndicator(HWND hwnd, bool hidden) {
    hwnd = GetAncestor(hwnd, GA_ROOT);
    if (!IsWindow(hwnd)) {
        return;
    }

    COLORREF color = g_settings.hiddenBorderColor;
    if (hidden) {
        if (!g_settings.showHiddenBorder) {
            return;
        }

        COLORREF originalColor = kDwmDefaultColor;
        if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_BORDER_COLOR,
                                            &originalColor,
                                            sizeof(originalColor)))) {
            std::scoped_lock lock(g_windowAffinityMutex);
            g_managedWindowOriginalBorderColor.emplace(hwnd, originalColor);
        }
    } else {
        std::scoped_lock lock(g_windowAffinityMutex);
        auto it = g_managedWindowOriginalBorderColor.find(hwnd);
        if (it != g_managedWindowOriginalBorderColor.end()) {
            color = it->second;
        } else {
            color = kDwmDefaultColor;
        }
    }

    const HRESULT result = DwmSetWindowAttribute(
        hwnd, DWMWA_BORDER_COLOR, &color, sizeof(color));
    if (SUCCEEDED(result)) {
        if (!hidden) {
            std::scoped_lock lock(g_windowAffinityMutex);
            g_managedWindowOriginalBorderColor.erase(hwnd);
        }
        Wh_Log(L"ApplyHiddenBorderIndicator: DWM border hwnd=0x%p hidden=%d color=0x%08X",
            hwnd, hidden ? 1 : 0, color);
    } else {
        Wh_Log(L"ApplyHiddenBorderIndicator: DwmSetWindowAttribute failed hwnd=0x%p hidden=%d hr=0x%08X",
            hwnd, hidden ? 1 : 0, result);
        if (hidden) {
            std::scoped_lock lock(g_windowAffinityMutex);
            g_managedWindowOriginalBorderColor.erase(hwnd);
        }
    }
}

bool IsWindowCurrentlyHiddenFromCapture(HWND hwnd) {
    hwnd = GetAncestor(hwnd, GA_ROOT);
    if (!IsWindow(hwnd)) {
        return false;
    }

    DWORD currentAffinity = WDA_NONE;
    if (!GetWindowDisplayAffinity(hwnd, &currentAffinity)) {
        return false;
    }

    return currentAffinity == kWdaExcludeFromCapture ||
           currentAffinity == kWdaMonitor;
}

ToggleResult ApplyDisplayAffinityForWindow(HWND hwnd,
                                           bool hide,
                                           bool allowUnmanagedRestore = false) {
    hwnd = GetAncestor(hwnd, GA_ROOT);
    if (!IsWindow(hwnd)) {
        Wh_Log(L"ApplyDisplayAffinityForWindow: invalid hwnd");
        return ToggleResult::Failed;
    }

    DWORD currentAffinity = WDA_NONE;
    if (!GetWindowDisplayAffinity(hwnd, &currentAffinity)) {
        currentAffinity = WDA_NONE;
    }

    DWORD desiredAffinity = hide ? kWdaExcludeFromCapture : WDA_NONE;
    bool newlyManaged = false;
    if (!hide) {
        DWORD originalAffinity = WDA_NONE;
        if (GetManagedWindowOriginalAffinity(hwnd, &originalAffinity)) {
            desiredAffinity = originalAffinity;
            Wh_Log(L"ApplyDisplayAffinityForWindow: restoring managed hwnd=0x%p to originalAffinity=0x%08X",
                hwnd, originalAffinity);
        } else if (allowUnmanagedRestore &&
                   (currentAffinity == kWdaExcludeFromCapture ||
                    currentAffinity == kWdaMonitor)) {
            desiredAffinity = WDA_NONE;
            Wh_Log(L"ApplyDisplayAffinityForWindow: restoring unmanaged hidden hwnd=0x%p to visible state",
                hwnd);
        } else {
            Wh_Log(L"ApplyDisplayAffinityForWindow: refusing to restore unmanaged hwnd=0x%p",
                hwnd);
            return ToggleResult::Failed;
        }
    } else if (!IsWindowManagedByMod(hwnd)) {
        RememberManagedWindowAffinity(hwnd, currentAffinity);
        newlyManaged = true;
    }

    Wh_Log(L"ApplyDisplayAffinityForWindow: hwnd=0x%p hide=%d currentAffinity=0x%08X desiredAffinity=0x%08X exStyle=0x%p",
        hwnd, hide ? 1 : 0, currentAffinity, desiredAffinity,
        reinterpret_cast<void*>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)));

    if (SetWindowDisplayAffinity(hwnd, desiredAffinity)) {
        if (!hide) {
            ApplyHiddenBorderIndicator(hwnd, false);
            ForgetManagedWindowAffinity(hwnd);
        } else {
            ApplyHiddenBorderIndicator(hwnd, true);
        }
        Wh_Log(L"ApplyDisplayAffinityForWindow: SetWindowDisplayAffinity succeeded hwnd=0x%p result=%s",
            hwnd, hide ? L"Hidden" : L"Unhidden");
        return hide ? ToggleResult::Hidden : ToggleResult::Unhidden;
    }

    const DWORD initialError = GetLastError();
    Wh_Log(L"ApplyDisplayAffinityForWindow: SetWindowDisplayAffinity failed hwnd=0x%p gle=%lu",
        hwnd, initialError);

    if (hide) {
        if (SetWindowDisplayAffinity(hwnd, kWdaMonitor)) {
            ApplyHiddenBorderIndicator(hwnd, true);
            Wh_Log(L"ApplyDisplayAffinityForWindow: compatibility fallback WDA_MONITOR succeeded hwnd=0x%p",
                hwnd);
            return ToggleResult::HiddenCompatibility;
        }

        const DWORD compatibilityError = GetLastError();
        Wh_Log(L"ApplyDisplayAffinityForWindow: compatibility fallback WDA_MONITOR failed hwnd=0x%p gle=%lu exStyle=0x%p",
            hwnd, compatibilityError,
            reinterpret_cast<void*>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)));
    }

    if (hide && newlyManaged) {
        ForgetManagedWindowAffinity(hwnd);
    }

    SetLastError(initialError);
    return ToggleResult::Failed;
}

struct ProcessAffinityContext {
    bool hide = false;
    bool allowUnmanagedRestore = false;
    bool anySucceeded = false;
    bool anyCompatibility = false;
};

BOOL CALLBACK EnumCurrentProcessWindowsForAffinity(HWND hwnd, LPARAM lParam) {
    auto* context = reinterpret_cast<ProcessAffinityContext*>(lParam);
    if (!context) {
        return TRUE;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) {
        return TRUE;
    }

    if (!IsLikelyUserFacingPrimaryWindow(hwnd)) {
        return TRUE;
    }

    const ToggleResult result = ApplyDisplayAffinityForWindow(
        hwnd, context->hide, context->allowUnmanagedRestore);
    if (result == ToggleResult::Hidden || result == ToggleResult::Unhidden) {
        context->anySucceeded = true;
    } else if (result == ToggleResult::HiddenCompatibility) {
        context->anySucceeded = true;
        context->anyCompatibility = true;
    }

    return TRUE;
}

ToggleResult ApplyDisplayAffinityForCurrentProcess(
    bool hide, bool allowUnmanagedRestore = false) {
    ProcessAffinityContext context;
    context.hide = hide;
    context.allowUnmanagedRestore = allowUnmanagedRestore;
    EnumWindows(EnumCurrentProcessWindowsForAffinity,
                reinterpret_cast<LPARAM>(&context));

    if (!context.anySucceeded) {
        Wh_Log(L"ApplyDisplayAffinityForCurrentProcess: no eligible windows updated hide=%d pid=%lu",
            hide ? 1 : 0, GetCurrentProcessId());
        return ToggleResult::Failed;
    }

    if (hide) {
        return context.anyCompatibility ? ToggleResult::HiddenCompatibility
                                        : ToggleResult::Hidden;
    }
    return ToggleResult::Unhidden;
}

ToggleResult ToggleDisplayAffinityForCurrentProcess(HWND sourceWindow) {
    sourceWindow = GetAncestor(sourceWindow, GA_ROOT);
    const bool hasManagedWindows = HasManagedWindows();
    const bool sourceHidden =
        sourceWindow && IsWindowCurrentlyHiddenFromCapture(sourceWindow);
    const bool hide = !hasManagedWindows && !sourceHidden;
    const bool allowUnmanagedRestore = !hasManagedWindows && sourceHidden;

    Wh_Log(L"ToggleDisplayAffinityForCurrentProcess: pid=%lu managed=%d source=0x%p sourceHidden=%d hide=%d allowUnmanagedRestore=%d",
        GetCurrentProcessId(), hasManagedWindows ? 1 : 0, sourceWindow,
        sourceHidden ? 1 : 0, hide ? 1 : 0,
        allowUnmanagedRestore ? 1 : 0);
    return ApplyDisplayAffinityForCurrentProcess(hide, allowUnmanagedRestore);
}

std::wstring GetReceiverWindowName(DWORD pid) {
    wchar_t name[96];
    swprintf_s(name, L"Windhawk.HideFromScreenCapture.Receiver.%lu", pid);
    return name;
}

LRESULT CALLBACK ReceiverWindowProc(HWND hwnd,
                                    UINT msg,
                                    WPARAM wParam,
                                    LPARAM lParam) {
    UNREFERENCED_PARAMETER(lParam);

    if (msg == g_toggleMessage) {
        if (g_unloading.load() || g_processIsProtected.load()) {
            return static_cast<LRESULT>(ToggleResult::Failed);
        }

        HWND sourceWindow = reinterpret_cast<HWND>(wParam);
        DWORD sourcePid = 0;
        GetWindowThreadProcessId(sourceWindow, &sourcePid);
        if (sourcePid != GetCurrentProcessId()) {
            return static_cast<LRESULT>(ToggleResult::Failed);
        }

        Wh_Log(L"ReceiverWindowProc: received toggle source=0x%p pid=%lu",
               sourceWindow, sourcePid);
        return static_cast<LRESULT>(
            ToggleDisplayAffinityForCurrentProcess(sourceWindow));
    }

    if (msg == kStopReceiverMessage) {
        DestroyWindow(hwnd);
        return 0;
    }

    if (msg == WM_DESTROY) {
        if (g_receiverWindow.load() == hwnd) {
            g_receiverWindow.store(nullptr);
        }
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

DWORD WINAPI ReceiverThreadMain(LPVOID) {
    HINSTANCE instance = reinterpret_cast<HINSTANCE>(&__ImageBase);
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = ReceiverWindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kReceiverWindowClassName;

    ATOM classAtom = RegisterClassW(&windowClass);
    if (!classAtom) {
        Wh_Log(L"ReceiverThreadMain: RegisterClassW failed gle=%lu",
               GetLastError());
        SetEvent(g_receiverReadyEvent);
        return 0;
    }

    const std::wstring windowName =
        GetReceiverWindowName(GetCurrentProcessId());
    HWND receiverWindow = CreateWindowExW(
        0, kReceiverWindowClassName, windowName.c_str(), 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, instance, nullptr);
    g_receiverWindow.store(receiverWindow);
    if (!receiverWindow) {
        Wh_Log(L"ReceiverThreadMain: CreateWindowExW failed gle=%lu",
               GetLastError());
        if (classAtom) {
            UnregisterClassW(kReceiverWindowClassName, instance);
        }
        SetEvent(g_receiverReadyEvent);
        return 0;
    }

    if (!ChangeWindowMessageFilterEx(receiverWindow, g_toggleMessage,
                                     MSGFLT_ALLOW, nullptr)) {
        Wh_Log(L"ReceiverThreadMain: ChangeWindowMessageFilterEx failed msg=0x%X gle=%lu",
               g_toggleMessage, GetLastError());
    }

    Wh_Log(L"ReceiverThreadMain: receiver ready hwnd=0x%p pid=%lu",
           receiverWindow, GetCurrentProcessId());
    SetEvent(g_receiverReadyEvent);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (receiverWindow && IsWindow(receiverWindow)) {
        DestroyWindow(receiverWindow);
    }
    g_receiverWindow.store(nullptr);
    UnregisterClassW(kReceiverWindowClassName, instance);
    return 0;
}

bool StartReceiverThread() {
    g_receiverReadyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_receiverReadyEvent) {
        Wh_Log(L"StartReceiverThread: CreateEventW failed gle=%lu",
               GetLastError());
        return false;
    }

    g_receiverThread = CreateThread(nullptr, 0, ReceiverThreadMain, nullptr, 0,
                                    &g_receiverThreadId);
    if (!g_receiverThread) {
        Wh_Log(L"StartReceiverThread: CreateThread failed gle=%lu",
               GetLastError());
        CloseHandle(g_receiverReadyEvent);
        g_receiverReadyEvent = nullptr;
        return false;
    }

    WaitForSingleObject(g_receiverReadyEvent, INFINITE);
    CloseHandle(g_receiverReadyEvent);
    g_receiverReadyEvent = nullptr;
    return g_receiverWindow.load() != nullptr;
}

void StopReceiverThread() {
    HWND receiverWindow = g_receiverWindow.load();
    if (receiverWindow) {
        PostMessageW(receiverWindow, kStopReceiverMessage, 0, 0);
    } else if (g_receiverThreadId) {
        PostThreadMessageW(g_receiverThreadId, WM_QUIT, 0, 0);
    }

    if (g_receiverThread) {
        WaitForSingleObject(g_receiverThread, INFINITE);
        CloseHandle(g_receiverThread);
        g_receiverThread = nullptr;
    }
    g_receiverThreadId = 0;
    g_receiverWindow.store(nullptr);
}

bool SendToggleMessageToWindow(HWND hwnd,
                               ToggleResult* resultOut,
                               std::wstring* displayNameOut) {
    if (!EnsureToggleMessageRegistered()) {
        Wh_Log(L"SendToggleMessageToWindow: toggle message unavailable");
        return false;
    }

    hwnd = GetAncestor(hwnd, GA_ROOT);
    if (!IsWindow(hwnd)) {
        Wh_Log(L"SendToggleMessageToWindow: invalid hwnd");
        return false;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    const std::wstring receiverName = GetReceiverWindowName(pid);
    HWND receiver = FindWindowExW(HWND_MESSAGE, nullptr,
                                  kReceiverWindowClassName,
                                  receiverName.c_str());
    if (!receiver) {
        Wh_Log(L"SendToggleMessageToWindow: receiver not found pid=%lu",
               pid);
        return false;
    }

    ULONG_PTR result = 0;
    if (!SendMessageTimeoutW(receiver, g_toggleMessage,
                             reinterpret_cast<WPARAM>(hwnd), 0,
                             SMTO_ABORTIFHUNG | SMTO_BLOCK,
                             kToggleSendTimeoutMs, &result)) {
        Wh_Log(L"SendToggleMessageToWindow: send failed receiver=0x%p pid=%lu gle=%lu",
               receiver, pid, GetLastError());
        return false;
    }

    if (resultOut) {
        *resultOut = static_cast<ToggleResult>(result);
    }
    if (displayNameOut) {
        *displayNameOut = DescribeWindowForUser(hwnd);
    }

    Wh_Log(L"SendToggleMessageToWindow: receiver=0x%p pid=%lu result=%llu",
           receiver, pid, static_cast<unsigned long long>(result));
    return true;
}

void NotifyToggleResult(const std::wstring& label, ToggleResult result) {
    const std::wstring visibleLabel = label.empty() ? L"Application" : label;

    switch (result) {
        case ToggleResult::Hidden:
        case ToggleResult::Unhidden:
            return;
        case ToggleResult::HiddenCompatibility:
            Wh_Log(L"NotifyToggleResult: compatibility mode for '%s'",
                visibleLabel.c_str());
            break;
        default:
            Wh_Log(L"NotifyToggleResult: failed for '%s'", visibleLabel.c_str());
            break;
    }
}

bool ToggleWindowFromTaskbarPoint(POINT pt, bool* swallowed) {
    if (swallowed) {
        *swallowed = false;
    }

    HWND taskbarRoot = nullptr;
    if (!IsPointOnTaskbar(pt, &taskbarRoot) || !taskbarRoot) {
        Wh_Log(L"ToggleWindowFromTaskbarPoint: point wasn't on taskbar");
        return false;
    }

    Wh_Log(L"ToggleWindowFromTaskbarPoint: taskbar hit root=0x%p", taskbarRoot);

    HWND targetWindow = nullptr;
    std::wstring taskbarLabel;
    if (!ResolveTaskbarButtonTargetWindow(pt, &targetWindow, &taskbarLabel) ||
        !targetWindow) {
        Wh_Log(L"ToggleWindowFromTaskbarPoint: failed to resolve taskbar button target");
        return false;
    }

    std::wstring protectedImagePath;
    if (IsProtectedWindowTarget(targetWindow, &protectedImagePath)) {
        Wh_Log(L"ToggleWindowFromTaskbarPoint: protected target hwnd=0x%p path='%s'",
            targetWindow, protectedImagePath.c_str());
        if (swallowed) {
            *swallowed = true;
        }
        return true;
    }

    Wh_Log(L"ToggleWindowFromTaskbarPoint: resolved target hwnd=0x%p label='%s'",
        targetWindow, taskbarLabel.c_str());

    ToggleResult result = ToggleResult::Failed;
    std::wstring actualWindowLabel;
    if (!SendToggleMessageToWindow(targetWindow, &result, &actualWindowLabel)) {
        Wh_Log(L"ToggleWindowFromTaskbarPoint: send toggle message failed hwnd=0x%p",
            targetWindow);
        NotifyToggleResult(taskbarLabel, ToggleResult::Failed);
        if (swallowed) {
            *swallowed = true;
        }
        return true;
    }

    Wh_Log(L"ToggleWindowFromTaskbarPoint: toggle completed hwnd=0x%p result=%u actualLabel='%s'",
        targetWindow, static_cast<unsigned>(result), actualWindowLabel.c_str());

    NotifyToggleResult(!actualWindowLabel.empty() ? actualWindowLabel
                                                  : taskbarLabel,
                       result);
    if (swallowed) {
        *swallowed = true;
    }
    return true;
}

LRESULT CALLBACK ExplorerMouseHookProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code < 0 || g_unloading.load()) {
        return CallNextHookEx(g_explorerMouseHook, code, wParam, lParam);
    }

    auto* mouse = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
    if (!mouse) {
        return CallNextHookEx(g_explorerMouseHook, code, wParam, lParam);
    }

    if (wParam == WM_MBUTTONUP) {
        const DWORD swallowStart = g_swallowStartTick.exchange(0);
        if (swallowStart && GetTickCount() - swallowStart <= kSwallowWindowMs) {
            return 1;
        }
        return CallNextHookEx(g_explorerMouseHook, code, wParam, lParam);
    }

    if (wParam != WM_MBUTTONDOWN || !TriggerModeHasMouse() ||
        !IsModifierSatisfied() || !IsPointOnTaskbar(mouse->pt, nullptr)) {
        return CallNextHookEx(g_explorerMouseHook, code, wParam, lParam);
    }

    if (!g_explorerWorkerThreadId ||
        !PostThreadMessageW(g_explorerWorkerThreadId, kExplorerToggleMessage,
                            static_cast<WPARAM>(mouse->pt.x),
                            static_cast<LPARAM>(mouse->pt.y))) {
        return CallNextHookEx(g_explorerMouseHook, code, wParam, lParam);
    }

    g_swallowStartTick.store(GetTickCount());
    return 1;
}

LRESULT CALLBACK ExplorerKeyboardHookProc(int code,
                                          WPARAM wParam,
                                          LPARAM lParam) {
    if (code < 0 || g_unloading.load() || !TriggerModeHasHotkey()) {
        return CallNextHookEx(g_explorerKeyboardHook, code, wParam, lParam);
    }

    auto* keyboard = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
    if (!keyboard ||
        static_cast<int>(keyboard->vkCode) != GetConfiguredHotkeyVk()) {
        return CallNextHookEx(g_explorerKeyboardHook, code, wParam, lParam);
    }

    if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
        g_hotkeyDown.store(false);
        if (g_hotkeySwallowed.exchange(false)) {
            return 1;
        }
        return CallNextHookEx(g_explorerKeyboardHook, code, wParam, lParam);
    }

    if (wParam != WM_KEYDOWN && wParam != WM_SYSKEYDOWN) {
        return CallNextHookEx(g_explorerKeyboardHook, code, wParam, lParam);
    }

    if (g_hotkeyDown.exchange(true)) {
        if (g_hotkeySwallowed.load()) {
            return 1;
        }
        return CallNextHookEx(g_explorerKeyboardHook, code, wParam, lParam);
    }

    POINT pt{};
    if (!GetCursorPos(&pt) || !IsPointOnTaskbar(pt, nullptr)) {
        return CallNextHookEx(g_explorerKeyboardHook, code, wParam, lParam);
    }

    if (!g_explorerWorkerThreadId ||
        !PostThreadMessageW(g_explorerWorkerThreadId, kExplorerToggleMessage,
                            static_cast<WPARAM>(pt.x),
                            static_cast<LPARAM>(pt.y))) {
        return CallNextHookEx(g_explorerKeyboardHook, code, wParam, lParam);
    }

    g_hotkeySwallowed.store(true);
    return 1;
}

void EnsureExplorerHooksInstalled(HMODULE thisModule) {
    if (!TriggerModeHasMouse()) {
        if (g_explorerMouseHook) {
            UnhookWindowsHookEx(g_explorerMouseHook);
            g_explorerMouseHook = nullptr;
            Wh_Log(L"EnsureExplorerHooksInstalled: low-level mouse hook removed");
        }
    } else if (!g_explorerMouseHook) {
        g_explorerMouseHook = SetWindowsHookExW(WH_MOUSE_LL,
                                                ExplorerMouseHookProc,
                                                thisModule, 0);
        if (!g_explorerMouseHook) {
            Wh_Log(L"EnsureExplorerHooksInstalled: SetWindowsHookExW mouse failed gle=%lu",
                GetLastError());
        } else {
            Wh_Log(L"EnsureExplorerHooksInstalled: low-level mouse hook installed");
        }
    }

    if (!TriggerModeHasHotkey()) {
        if (g_explorerKeyboardHook) {
            UnhookWindowsHookEx(g_explorerKeyboardHook);
            g_explorerKeyboardHook = nullptr;
            Wh_Log(L"EnsureExplorerHooksInstalled: low-level keyboard hook removed");
        }
        return;
    }

    if (!g_explorerKeyboardHook) {
        g_explorerKeyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL,
                                                   ExplorerKeyboardHookProc,
                                                   thisModule, 0);
        if (!g_explorerKeyboardHook) {
            Wh_Log(L"EnsureExplorerHooksInstalled: SetWindowsHookExW keyboard failed gle=%lu",
                GetLastError());
        } else {
            Wh_Log(L"EnsureExplorerHooksInstalled: low-level keyboard hook installed");
        }
    }
}

DWORD WINAPI ExplorerWorkerThreadMain(LPVOID) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninitCo = SUCCEEDED(hr);
    Wh_Log(L"ExplorerWorkerThreadMain: starting CoInitializeEx hr=0x%08X", hr);

    // Force creation of the thread message queue before hooks can post work.
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    SetEvent(g_explorerWorkerReadyEvent);

    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == kExplorerToggleMessage) {
            POINT pt{static_cast<LONG>(msg.wParam),
                     static_cast<LONG>(msg.lParam)};
            bool swallowed = false;
            ToggleWindowFromTaskbarPoint(pt, &swallowed);
        }
    }

    g_uia.Reset();
    if (uninitCo) {
        CoUninitialize();
    }
    return 0;
}

DWORD WINAPI ExplorerHookThreadMain(LPVOID) {
    Wh_Log(L"ExplorerHookThreadMain: starting hook-only message pump");

    EnsureToggleMessageRegistered();

    HMODULE thisModule = reinterpret_cast<HMODULE>(&__ImageBase);
    SetTimer(nullptr, kExplorerHookRetryTimerId, kExplorerHookRetryIntervalMs,
             nullptr);
    EnsureExplorerHooksInstalled(thisModule);

    MSG msg;
    while (!g_unloading.load() && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_TIMER &&
            msg.wParam == kExplorerHookRetryTimerId) {
            EnsureExplorerHooksInstalled(thisModule);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    KillTimer(nullptr, kExplorerHookRetryTimerId);

    if (g_explorerMouseHook) {
        UnhookWindowsHookEx(g_explorerMouseHook);
        g_explorerMouseHook = nullptr;
        Wh_Log(L"ExplorerHookThreadMain: low-level mouse hook removed");
    }

    if (g_explorerKeyboardHook) {
        UnhookWindowsHookEx(g_explorerKeyboardHook);
        g_explorerKeyboardHook = nullptr;
        Wh_Log(L"ExplorerHookThreadMain: low-level keyboard hook removed");
    }

    return 0;
}

bool IsCurrentProcessExplorer() {
    wchar_t modulePath[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, modulePath, ARRAYSIZE(modulePath))) {
        return false;
    }

    const wchar_t* fileName = PathFindFileNameW(modulePath);
    return fileName && _wcsicmp(fileName, L"explorer.exe") == 0;
}

void InstallPerProcessReceiver() {
    if (!EnsureToggleMessageRegistered()) {
        Wh_Log(L"InstallPerProcessReceiver: toggle message unavailable");
        return;
    }

    if (g_processIsProtected.load()) {
        Wh_Log(L"InstallPerProcessReceiver: skipped protected pid=%lu",
               GetCurrentProcessId());
        return;
    }

    if (!StartReceiverThread()) {
        Wh_Log(L"InstallPerProcessReceiver: receiver startup failed pid=%lu",
               GetCurrentProcessId());
    }
}

}

BOOL Wh_ModInit() {
    LoadSettings();
    g_processIsExplorer.store(IsCurrentProcessExplorer());
    g_processIsProtected.store(
        IsProtectedProcessPath(GetProcessImagePathForPid(GetCurrentProcessId())));
    Wh_Log(L"Wh_ModInit: pid=%lu explorer=%d protected=%d",
        GetCurrentProcessId(), g_processIsExplorer.load() ? 1 : 0,
        g_processIsProtected.load() ? 1 : 0);
    InstallPerProcessReceiver();

    if (g_processIsExplorer.load()) {
        g_unloading.store(false);
        g_explorerWorkerReadyEvent =
            CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (g_explorerWorkerReadyEvent) {
            g_explorerWorkerThread =
                CreateThread(nullptr, 0, ExplorerWorkerThreadMain, nullptr, 0,
                             &g_explorerWorkerThreadId);
        }

        if (g_explorerWorkerThread) {
            WaitForSingleObject(g_explorerWorkerReadyEvent, INFINITE);
            g_explorerHookThread =
                CreateThread(nullptr, 0, ExplorerHookThreadMain, nullptr, 0,
                             &g_explorerHookThreadId);
        } else {
            Wh_Log(L"Wh_ModInit: explorer worker thread startup failed gle=%lu",
                   GetLastError());
        }

        if (g_explorerWorkerReadyEvent) {
            CloseHandle(g_explorerWorkerReadyEvent);
            g_explorerWorkerReadyEvent = nullptr;
        }
        Wh_Log(L"Wh_ModInit: explorer worker handle=0x%p tid=%lu hook handle=0x%p tid=%lu",
            g_explorerWorkerThread, g_explorerWorkerThreadId,
            g_explorerHookThread, g_explorerHookThreadId);
    }

    return TRUE;
}

void Wh_ModUninit() {
    g_unloading.store(true);
    Wh_Log(L"Wh_ModUninit: pid=%lu explorer=%d", GetCurrentProcessId(),
        g_processIsExplorer.load() ? 1 : 0);

    if (g_explorerHookThreadId) {
        PostThreadMessageW(g_explorerHookThreadId, WM_QUIT, 0, 0);
    }

    if (g_explorerHookThread) {
        WaitForSingleObject(g_explorerHookThread, INFINITE);
        CloseHandle(g_explorerHookThread);
        g_explorerHookThread = nullptr;
        g_explorerHookThreadId = 0;
    }

    if (g_explorerWorkerThreadId) {
        PostThreadMessageW(g_explorerWorkerThreadId, WM_QUIT, 0, 0);
    }

    if (g_explorerWorkerThread) {
        WaitForSingleObject(g_explorerWorkerThread, INFINITE);
        CloseHandle(g_explorerWorkerThread);
        g_explorerWorkerThread = nullptr;
    }
    g_explorerWorkerThreadId = 0;

    Wh_Log(L"Wh_ModUninit: restoring all eligible windows for pid=%lu",
           GetCurrentProcessId());
    ApplyDisplayAffinityForCurrentProcess(false);
    StopReceiverThread();
}

BOOL Wh_ModSettingsChanged(BOOL* bReload) {
    if (bReload) {
        *bReload = TRUE;
    }

    Wh_Log(L"Wh_ModSettingsChanged: reloading");
    return TRUE;
}
