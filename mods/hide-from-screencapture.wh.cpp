// ==WindhawkMod==
// @id              hide-from-screencapture
// @name            Hide From Screencapture
// @description     Toggle screen-capture exclusion for Windows 11 taskbar apps, with a optional hidden-window border.
// @version         1.0.0
// @author          AjaxFNC
// @architecture    x86-64
// @include         *
// @compilerOptions -lole32 -loleaut32 -luuid -lshell32 -lpropsys -lruntimeobject -luiautomationcore -lcomctl32 -ldwmapi -lshlwapi
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Capture Toggle
Hide supported apps from screen capture from the Windows 11 taskbar without
touching known anti-cheat and game installs.

## Safety
Using capture-hiding tricks on games can cause instability, crashes, or even
anti-cheat trouble such as an in-game ban.

To reduce that risk, this mod refuses to operate on a built-in protected list
covering common anti-cheat environments such as Riot Vanguard, Easy Anti-Cheat,
Easy Anti-Cheat EOS, and BattlEye, along with known protected executables.

You can disable that protection list in the mod settings, but doing so may
cause game crashes, anti-cheat problems, or in-game bans.

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
  $name: Show a gray border around hidden windows
  $description: Applies a gray border to windows that this mod currently hides from capture.

- DisableProtectionList: false
  $name: Disable protected-app blocking
  $description: Allows the mod to target apps that are normally blocked. This may cause crashes, anti-cheat issues, or in-game bans.

- TriggerModeV2: MiddleClick
  $name: Trigger mode
  $options:
  - MiddleClick: Middle click
  - HoverHotkey: Hover hotkey
  - Both: Middle click and hover hotkey

- TriggerModifierV2: None
  $name: Modifier for middle click
  $description: Optional modifier required for the middle-click trigger.
  $options:
  - None: None
  - Ctrl: Ctrl
  - Shift: Shift
  - Alt: Alt
  - Win: Win

- HoverHotkeyV2: F8
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
#include <dwmapi.h>
#include <propkey.h>
#include <propsys.h>
#include <propvarutil.h>
#include <shlwapi.h>
#include <uiautomation.h>
#include <commctrl.h>
#include <wrl.h>

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

extern "C" IMAGE_DOS_HEADER __ImageBase;

constexpr wchar_t kToggleMessageName[] =
    L"Windhawk.HideFromScreenCapture.ToggleMessage.v1";
constexpr wchar_t kSubclassPropName[] =
    L"Windhawk.HideFromScreenCapture.Subclassed";
constexpr wchar_t kOriginalWndProcPropName[] =
    L"Windhawk.HideFromScreenCapture.OriginalWndProc";
constexpr UINT kSwallowWindowMs = 500;
constexpr UINT kToggleSendTimeoutMs = 700;
constexpr COLORREF kHiddenBorderColor = RGB(136, 136, 136);
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

struct WindowCandidate {
    HWND hwnd = nullptr;
    DWORD pid = 0;
    std::wstring title;
    std::wstring exeBaseName;
    std::wstring appUserModelId;
    int score = 0;
    bool foregroundProcess = false;
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
    bool disableProtectionList = false;
    TriggerMode triggerMode = TriggerMode::MiddleClick;
    TriggerModifier triggerModifier = TriggerModifier::None;
    HoverHotkey hoverHotkey = HoverHotkey::F8;
};

using CreateWindowExW_t = decltype(&CreateWindowExW);
CreateWindowExW_t g_originalCreateWindowExW = nullptr;

UINT g_toggleMessage = 0;

std::atomic<bool> g_processIsExplorer{false};
std::atomic<bool> g_processIsProtected{false};
std::atomic<bool> g_unloading{false};
std::atomic<bool> g_processHidden{false};
std::mutex g_windowAffinityMutex;
std::unordered_map<HWND, DWORD> g_managedWindowOriginalAffinity;
std::unordered_set<HWND> g_windowsWithHiddenBorder;
ModSettings g_settings;

HANDLE g_explorerHookThread = nullptr;
DWORD g_explorerHookThreadId = 0;
HHOOK g_explorerMouseHook = nullptr;
HHOOK g_explorerKeyboardHook = nullptr;
ComPtr<IUIAutomation> g_uia;

std::atomic<DWORD> g_swallowStartTick{0};
std::atomic<bool> g_hotkeyDown{false};

void Log(const wchar_t* fmt, ...);

template <size_t N>
bool SettingEquals(PCWSTR value, const wchar_t (&literal)[N]) {
    return value && wcscmp(value, literal) == 0;
}

void LoadSettings() {
    g_settings.showHiddenBorder = Wh_GetIntSetting(L"ShowHiddenBorder") != 0;
    g_settings.disableProtectionList =
        Wh_GetIntSetting(L"DisableProtectionList") != 0;

    PCWSTR triggerModeSetting = Wh_GetStringSetting(L"TriggerModeV2");
    if (!triggerModeSetting || !*triggerModeSetting) {
        Wh_FreeStringSetting(triggerModeSetting);
        const int legacyTriggerMode = Wh_GetIntSetting(L"TriggerMode");
        switch (legacyTriggerMode) {
            case 1:
                g_settings.triggerMode = TriggerMode::HoverHotkey;
                break;
            case 2:
                g_settings.triggerMode = TriggerMode::Both;
                break;
            case 0:
            default:
                g_settings.triggerMode = TriggerMode::MiddleClick;
                break;
        }
    } else if (SettingEquals(triggerModeSetting, L"HoverHotkey")) {
        g_settings.triggerMode = TriggerMode::HoverHotkey;
        Wh_FreeStringSetting(triggerModeSetting);
    } else if (SettingEquals(triggerModeSetting, L"Both")) {
        g_settings.triggerMode = TriggerMode::Both;
        Wh_FreeStringSetting(triggerModeSetting);
    } else {
        g_settings.triggerMode = TriggerMode::MiddleClick;
        Wh_FreeStringSetting(triggerModeSetting);
    }

    PCWSTR triggerModifierSetting = Wh_GetStringSetting(L"TriggerModifierV2");
    if (!triggerModifierSetting || !*triggerModifierSetting) {
        Wh_FreeStringSetting(triggerModifierSetting);
        const int legacyTriggerModifier = Wh_GetIntSetting(L"TriggerModifier");
        switch (legacyTriggerModifier) {
            case 1:
                g_settings.triggerModifier = TriggerModifier::Ctrl;
                break;
            case 2:
                g_settings.triggerModifier = TriggerModifier::Shift;
                break;
            case 3:
                g_settings.triggerModifier = TriggerModifier::Alt;
                break;
            case 4:
                g_settings.triggerModifier = TriggerModifier::Win;
                break;
            case 0:
            default:
                g_settings.triggerModifier = TriggerModifier::None;
                break;
        }
    } else if (SettingEquals(triggerModifierSetting, L"Ctrl")) {
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
    if (triggerModifierSetting && *triggerModifierSetting) {
        Wh_FreeStringSetting(triggerModifierSetting);
    }

    PCWSTR hoverHotkeySetting = Wh_GetStringSetting(L"HoverHotkeyV2");
    if (!hoverHotkeySetting || !*hoverHotkeySetting) {
        Wh_FreeStringSetting(hoverHotkeySetting);
        const int legacyHoverHotkey = Wh_GetIntSetting(L"HoverHotkey");
        switch (legacyHoverHotkey) {
            case 1:
                g_settings.hoverHotkey = HoverHotkey::F9;
                break;
            case 2:
                g_settings.hoverHotkey = HoverHotkey::F10;
                break;
            case 3:
                g_settings.hoverHotkey = HoverHotkey::F11;
                break;
            case 4:
                g_settings.hoverHotkey = HoverHotkey::F12;
                break;
            case 5:
                g_settings.hoverHotkey = HoverHotkey::Pause;
                break;
            case 6:
                g_settings.hoverHotkey = HoverHotkey::ScrollLock;
                break;
            case 0:
            default:
                g_settings.hoverHotkey = HoverHotkey::F8;
                break;
        }
    } else if (SettingEquals(hoverHotkeySetting, L"F9")) {
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
    if (hoverHotkeySetting && *hoverHotkeySetting) {
        Wh_FreeStringSetting(hoverHotkeySetting);
    }

    Log(L"LoadSettings: showHiddenBorder=%d disableProtectionList=%d triggerMode=%d triggerModifier=%d hoverHotkey=%d",
        g_settings.showHiddenBorder ? 1 : 0,
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
        Log(L"EnsureToggleMessageRegistered: RegisterWindowMessageW failed gle=%lu",
            GetLastError());
        return 0;
    }

    g_toggleMessage = msg;
    Log(L"EnsureToggleMessageRegistered: registered msg=0x%X pid=%lu", msg,
        GetCurrentProcessId());
    return msg;
}

void Log(const wchar_t* fmt, ...) {
    wchar_t buffer[2048];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buffer, _countof(buffer), _TRUNCATE, fmt, args);
    va_end(args);
    Wh_Log(L"%s", buffer);
}

std::wstring ToLower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
    return value;
}

std::wstring Trim(std::wstring value) {
    const auto notSpace = [](wchar_t ch) { return !iswspace(ch); };
    const auto first = std::find_if(value.begin(), value.end(), notSpace);
    if (first == value.end()) {
        return L"";
    }
    const auto last = std::find_if(value.rbegin(), value.rend(), notSpace).base();
    return std::wstring(first, last);
}

std::wstring NormalizeForMatch(const std::wstring& value) {
    std::wstring lower = ToLower(value);
    std::wstring normalized;
    normalized.reserve(lower.size());
    bool previousWasSpace = false;

    for (wchar_t ch : lower) {
        if (iswalnum(ch)) {
            normalized.push_back(ch);
            previousWasSpace = false;
        } else if (!previousWasSpace) {
            normalized.push_back(L' ');
            previousWasSpace = true;
        }
    }

    return Trim(normalized);
}

bool ContainsNormalized(const std::wstring& haystack,
                        const std::wstring& needle) {
    const std::wstring normHaystack = NormalizeForMatch(haystack);
    const std::wstring normNeedle = NormalizeForMatch(needle);
    return !normNeedle.empty() &&
           normHaystack.find(normNeedle) != std::wstring::npos;
}

std::wstring EscapeXml(const std::wstring& text) {
    std::wstring escaped;
    escaped.reserve(text.size() + 16);
    for (wchar_t ch : text) {
        switch (ch) {
            case L'&':
                escaped += L"&amp;";
                break;
            case L'<':
                escaped += L"&lt;";
                break;
            case L'>':
                escaped += L"&gt;";
                break;
            case L'"':
                escaped += L"&quot;";
                break;
            case L'\'':
                escaped += L"&apos;";
                break;
            default:
                escaped.push_back(ch);
                break;
        }
    }
    return escaped;
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

std::wstring GetWindowAppUserModelId(HWND hwnd) {
    std::wstring appId;
    ComPtr<IPropertyStore> propertyStore;
    if (FAILED(SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&propertyStore)))) {
        return appId;
    }

    PROPVARIANT value;
    PropVariantInit(&value);
    if (SUCCEEDED(propertyStore->GetValue(PKEY_AppUserModel_ID, &value)) &&
        value.vt == VT_LPWSTR && value.pwszVal) {
        appId = value.pwszVal;
    }
    PropVariantClear(&value);
    return appId;
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

bool IsSubclassableTopLevelWindow(HWND hwnd) {
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

int ScoreCandidate(const WindowCandidate& candidate,
                   const UiaButtonDescriptor& descriptor,
                   DWORD foregroundPid) {
    int score = 0;
    const std::wstring title = candidate.title;
    const std::wstring exeBase = candidate.exeBaseName;
    const std::wstring appId = candidate.appUserModelId;

    if (!descriptor.name.empty()) {
        if (ContainsNormalized(title, descriptor.name) ||
            ContainsNormalized(descriptor.name, title)) {
            score += 80;
        }

        if (ContainsNormalized(exeBase, descriptor.name) ||
            ContainsNormalized(descriptor.name, exeBase)) {
            score += 45;
        }
    }

    const std::wstring combinedHints =
        descriptor.automationId + L" " + descriptor.helpText + L" " +
        descriptor.itemStatus + L" " + descriptor.className;

    if (!appId.empty() && !combinedHints.empty()) {
        if (ContainsNormalized(combinedHints, appId) ||
            ContainsNormalized(appId, combinedHints)) {
            score += 100;
        }
    }

    if (!descriptor.helpText.empty()) {
        if (ContainsNormalized(title, descriptor.helpText) ||
            ContainsNormalized(descriptor.helpText, title)) {
            score += 55;
        }
    }

    if (!descriptor.itemStatus.empty()) {
        if (ContainsNormalized(title, descriptor.itemStatus) ||
            ContainsNormalized(descriptor.itemStatus, title)) {
            score += 55;
        }
    }

    if (candidate.pid == foregroundPid) {
        score += 25;
    }

    if (GetForegroundWindow() == candidate.hwnd) {
        score += 20;
    }

    if (!title.empty()) {
        score += 10;
    }

    return score;
}

struct FindWindowContext {
    UiaButtonDescriptor descriptor;
    DWORD foregroundPid = 0;
    WindowCandidate best;
};

struct ProcessMessageTargetContext {
    DWORD pid = 0;
    HWND preferred = nullptr;
    HWND fallback = nullptr;
};

BOOL CALLBACK EnumWindowsForTaskbarMatch(HWND hwnd, LPARAM lParam) {
    auto* context = reinterpret_cast<FindWindowContext*>(lParam);
    if (!context || !IsCandidateTopLevelWindow(hwnd)) {
        return TRUE;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid || pid == GetCurrentProcessId()) {
        return TRUE;
    }

    WindowCandidate candidate;
    candidate.hwnd = hwnd;
    candidate.pid = pid;
    candidate.title = GetWindowTextString(hwnd);
    candidate.exeBaseName = GetModuleBaseNameForPid(pid);
    candidate.appUserModelId = GetWindowAppUserModelId(hwnd);
    candidate.foregroundProcess = (pid == context->foregroundPid);
    candidate.score = ScoreCandidate(candidate, context->descriptor,
                                     context->foregroundPid);

    if (candidate.score > context->best.score) {
        context->best = candidate;
    }

    return TRUE;
}

BOOL CALLBACK EnumWindowsForProcessMessageTarget(HWND hwnd, LPARAM lParam) {
    auto* context = reinterpret_cast<ProcessMessageTargetContext*>(lParam);
    if (!context || !IsCandidateTopLevelWindow(hwnd)) {
        return TRUE;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != context->pid) {
        return TRUE;
    }

    if (hwnd == context->preferred) {
        return TRUE;
    }

    context->fallback = hwnd;
    return FALSE;
}

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
        Log(L"ResolveTaskbarButtonTargetWindow: point (%ld,%ld) is not on taskbar",
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
                Log(L"ResolveTaskbarButtonTargetWindow: failed to create UI Automation");
                return false;
            }
        }
    }

    ComPtr<IUIAutomationElement> elementAtPoint;
    if (FAILED(g_uia->ElementFromPoint(pt, &elementAtPoint)) || !elementAtPoint) {
        Log(L"ResolveTaskbarButtonTargetWindow: UIA ElementFromPoint failed at (%ld,%ld)",
            pt.x, pt.y);
        return false;
    }

    ComPtr<IUIAutomationTreeWalker> controlWalker;
    if (FAILED(g_uia->get_ControlViewWalker(&controlWalker)) || !controlWalker) {
        Log(L"ResolveTaskbarButtonTargetWindow: failed to get control-view walker");
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
            Log(L"ResolveTaskbarButtonTargetWindow: direct UIA native hwnd match -> hwnd=0x%p label=%s",
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

        if (bestDescriptor.name.empty() &&
            LooksLikeTaskbarButtonElement(descriptor,
                                          GetElementControlType(current.Get()))) {
            bestDescriptor = descriptor;
            Log(L"ResolveTaskbarButtonTargetWindow: selected taskbar descriptor name='%s' automationId='%s' class='%s' framework='%s' help='%s' status='%s'",
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

    if (bestDescriptor.name.empty() && bestDescriptor.helpText.empty()) {
        Log(L"ResolveTaskbarButtonTargetWindow: no usable taskbar descriptor found");
        return false;
    }

    DWORD foregroundPid = 0;
    HWND foregroundWindow = GetForegroundWindow();
    if (foregroundWindow) {
        GetWindowThreadProcessId(foregroundWindow, &foregroundPid);
    }

    FindWindowContext context;
    context.descriptor = bestDescriptor;
    context.foregroundPid = foregroundPid;
    EnumWindows(EnumWindowsForTaskbarMatch, reinterpret_cast<LPARAM>(&context));

    if (!context.best.hwnd || context.best.score < 35) {
        Log(L"ResolveTaskbarButtonTargetWindow: no window match found for descriptor name='%s' help='%s' bestScore=%d",
            bestDescriptor.name.c_str(), bestDescriptor.helpText.c_str(),
            context.best.score);
        return false;
    }

    *targetWindow = context.best.hwnd;
    if (displayName) {
        *displayName = !context.best.title.empty()
                           ? context.best.title
                           : (!context.best.exeBaseName.empty()
                                  ? context.best.exeBaseName
                                  : bestDescriptor.name);
    }
    Log(L"ResolveTaskbarButtonTargetWindow: scored match -> hwnd=0x%p pid=%lu score=%d title='%s' exe='%s' appid='%s'",
        context.best.hwnd, context.best.pid, context.best.score,
        context.best.title.c_str(), context.best.exeBaseName.c_str(),
        context.best.appUserModelId.c_str());
    return true;
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

bool IsWindowBorderedByMod(HWND hwnd) {
    std::scoped_lock lock(g_windowAffinityMutex);
    return g_windowsWithHiddenBorder.find(hwnd) != g_windowsWithHiddenBorder.end();
}

bool HasManagedWindows() {
    std::scoped_lock lock(g_windowAffinityMutex);
    return !g_managedWindowOriginalAffinity.empty();
}

bool IsProcessHiddenByMod() {
    return HasManagedWindows() || g_processHidden.load();
}

void RememberManagedWindowAffinity(HWND hwnd, DWORD originalAffinity) {
    std::scoped_lock lock(g_windowAffinityMutex);
    const auto [it, inserted] =
        g_managedWindowOriginalAffinity.emplace(hwnd, originalAffinity);
    Log(L"RememberManagedWindowAffinity: hwnd=0x%p originalAffinity=0x%08X inserted=%d stored=0x%08X",
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
    Log(L"ForgetManagedWindowAffinity: hwnd=0x%p erased=%zu remaining=%zu", hwnd,
        erased, g_managedWindowOriginalAffinity.size());
}

void ForgetManagedWindowBorder(HWND hwnd) {
    std::scoped_lock lock(g_windowAffinityMutex);
    g_windowsWithHiddenBorder.erase(hwnd);
}

void SetManagedWindowBorderState(HWND hwnd, bool bordered) {
    std::scoped_lock lock(g_windowAffinityMutex);
    if (bordered) {
        g_windowsWithHiddenBorder.insert(hwnd);
    } else {
        g_windowsWithHiddenBorder.erase(hwnd);
    }
}

void ApplyHiddenBorderIndicator(HWND hwnd, bool hidden) {
    hwnd = GetAncestor(hwnd, GA_ROOT);
    if (!IsWindow(hwnd)) {
        return;
    }

    if (!g_settings.showHiddenBorder) {
        hidden = false;
    }

    const COLORREF color = hidden ? kHiddenBorderColor : kDwmDefaultColor;
    if (SUCCEEDED(DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &color,
                                        sizeof(color)))) {
        SetManagedWindowBorderState(hwnd, hidden);
        Log(L"ApplyHiddenBorderIndicator: hwnd=0x%p hidden=%d color=0x%08X",
            hwnd, hidden ? 1 : 0, color);
    } else {
        Log(L"ApplyHiddenBorderIndicator: DwmSetWindowAttribute failed hwnd=0x%p hidden=%d",
            hwnd, hidden ? 1 : 0);
    }
}

BOOL CALLBACK EnumCurrentProcessWindowsForBorderRefresh(HWND hwnd, LPARAM lParam) {
    const bool hideBorder = lParam != 0;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) {
        return TRUE;
    }

    if (!IsLikelyUserFacingPrimaryWindow(hwnd)) {
        return TRUE;
    }

    if (hideBorder) {
        if (IsWindowBorderedByMod(hwnd)) {
            ApplyHiddenBorderIndicator(hwnd, false);
        }
    } else if (IsWindowManagedByMod(hwnd)) {
        ApplyHiddenBorderIndicator(hwnd, true);
    }

    return TRUE;
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
        Log(L"ApplyDisplayAffinityForWindow: invalid hwnd");
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
            Log(L"ApplyDisplayAffinityForWindow: restoring managed hwnd=0x%p to originalAffinity=0x%08X",
                hwnd, originalAffinity);
        } else if (allowUnmanagedRestore &&
                   (currentAffinity == kWdaExcludeFromCapture ||
                    currentAffinity == kWdaMonitor)) {
            desiredAffinity = WDA_NONE;
            Log(L"ApplyDisplayAffinityForWindow: restoring unmanaged hidden hwnd=0x%p to visible state",
                hwnd);
        } else {
            Log(L"ApplyDisplayAffinityForWindow: refusing to restore unmanaged hwnd=0x%p",
                hwnd);
            return ToggleResult::Failed;
        }
    } else if (!IsWindowManagedByMod(hwnd)) {
        RememberManagedWindowAffinity(hwnd, currentAffinity);
        newlyManaged = true;
    }

    Log(L"ApplyDisplayAffinityForWindow: hwnd=0x%p hide=%d currentAffinity=0x%08X desiredAffinity=0x%08X exStyle=0x%p",
        hwnd, hide ? 1 : 0, currentAffinity, desiredAffinity,
        reinterpret_cast<void*>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)));

    if (SetWindowDisplayAffinity(hwnd, desiredAffinity)) {
        if (!hide) {
            ApplyHiddenBorderIndicator(hwnd, false);
            ForgetManagedWindowAffinity(hwnd);
            ForgetManagedWindowBorder(hwnd);
        } else {
            ApplyHiddenBorderIndicator(hwnd, true);
        }
        Log(L"ApplyDisplayAffinityForWindow: SetWindowDisplayAffinity succeeded hwnd=0x%p result=%s",
            hwnd, hide ? L"Hidden" : L"Unhidden");
        return hide ? ToggleResult::Hidden : ToggleResult::Unhidden;
    }

    const DWORD initialError = GetLastError();
    if (hide && newlyManaged) {
        ForgetManagedWindowAffinity(hwnd);
    }
    Log(L"ApplyDisplayAffinityForWindow: SetWindowDisplayAffinity failed hwnd=0x%p gle=%lu",
        hwnd, initialError);

    if (hide) {
        if (SetWindowDisplayAffinity(hwnd, kWdaMonitor)) {
            ApplyHiddenBorderIndicator(hwnd, true);
            Log(L"ApplyDisplayAffinityForWindow: compatibility fallback WDA_MONITOR succeeded hwnd=0x%p",
                hwnd);
            return ToggleResult::HiddenCompatibility;
        }

        const DWORD compatibilityError = GetLastError();
        Log(L"ApplyDisplayAffinityForWindow: compatibility fallback WDA_MONITOR failed hwnd=0x%p gle=%lu exStyle=0x%p",
            hwnd, compatibilityError,
            reinterpret_cast<void*>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)));
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
        Log(L"ApplyDisplayAffinityForCurrentProcess: no eligible windows updated hide=%d pid=%lu",
            hide ? 1 : 0, GetCurrentProcessId());
        return ToggleResult::Failed;
    }

    g_processHidden.store(hide && context.anySucceeded);
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

    Log(L"ToggleDisplayAffinityForCurrentProcess: pid=%lu managed=%d source=0x%p sourceHidden=%d hide=%d allowUnmanagedRestore=%d",
        GetCurrentProcessId(), hasManagedWindows ? 1 : 0, sourceWindow,
        sourceHidden ? 1 : 0, hide ? 1 : 0,
        allowUnmanagedRestore ? 1 : 0);
    return ApplyDisplayAffinityForCurrentProcess(hide, allowUnmanagedRestore);
}

LRESULT CALLBACK ToggleSubclassProc(HWND hwnd,
                                    UINT msg,
                                    WPARAM wParam,
                                    LPARAM lParam) {
    EnsureToggleMessageRegistered();

    WNDPROC originalWndProc = reinterpret_cast<WNDPROC>(
        GetPropW(hwnd, kOriginalWndProcPropName));
    if (!originalWndProc) {
        originalWndProc = DefWindowProcW;
    }

    if (msg == g_toggleMessage) {
        UNREFERENCED_PARAMETER(wParam);
        UNREFERENCED_PARAMETER(lParam);
        Log(L"ToggleSubclassProc: received toggle message hwnd=0x%p", hwnd);
        return static_cast<LRESULT>(
            ToggleDisplayAffinityForCurrentProcess(hwnd));
    }

    if (msg == WM_NCDESTROY) {
        if (IsWindowBorderedByMod(hwnd)) {
            ApplyHiddenBorderIndicator(hwnd, false);
        }
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(originalWndProc));
        ForgetManagedWindowAffinity(hwnd);
        ForgetManagedWindowBorder(hwnd);
        RemovePropW(hwnd, kOriginalWndProcPropName);
        RemovePropW(hwnd, kSubclassPropName);
    }

    return CallWindowProcW(originalWndProc, hwnd, msg, wParam, lParam);
}

bool MaybeSubclassWindow(HWND hwnd) {
    if (!IsWindow(hwnd)) {
        return false;
    }

    if (g_processIsProtected.load()) {
        return false;
    }

    if (!EnsureToggleMessageRegistered()) {
        Log(L"MaybeSubclassWindow: toggle message unavailable hwnd=0x%p", hwnd);
        return false;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) {
        return false;
    }

    if (GetPropW(hwnd, kSubclassPropName)) {
        return true;
    }

    if (!IsLikelyUserFacingPrimaryWindow(hwnd)) {
        return false;
    }

    WNDPROC originalWndProc = reinterpret_cast<WNDPROC>(
        GetWindowLongPtrW(hwnd, GWLP_WNDPROC));
    if (!originalWndProc) {
        Log(L"MaybeSubclassWindow: GetWindowLongPtrW(GWLP_WNDPROC) failed hwnd=0x%p gle=%lu",
            hwnd, GetLastError());
        return false;
    }

    if (!SetPropW(hwnd, kOriginalWndProcPropName,
                  reinterpret_cast<HANDLE>(originalWndProc))) {
        Log(L"MaybeSubclassWindow: SetPropW(original wndproc) failed hwnd=0x%p gle=%lu",
            hwnd, GetLastError());
        return false;
    }

    const LONG_PTR previousWndProc = SetWindowLongPtrW(
        hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ToggleSubclassProc));
    if (!previousWndProc) {
        const DWORD gle = GetLastError();
        RemovePropW(hwnd, kOriginalWndProcPropName);
        Log(L"MaybeSubclassWindow: SetWindowLongPtrW(GWLP_WNDPROC) failed hwnd=0x%p gle=%lu",
            hwnd, gle);
        return false;
    }

    if (!ChangeWindowMessageFilterEx(hwnd, g_toggleMessage, MSGFLT_ALLOW,
                                     nullptr)) {
        const DWORD gle = GetLastError();
        Log(L"MaybeSubclassWindow: ChangeWindowMessageFilterEx failed hwnd=0x%p msg=0x%X gle=%lu",
            hwnd, g_toggleMessage, gle);
    } else {
        Log(L"MaybeSubclassWindow: allowed toggle message through UIPI hwnd=0x%p msg=0x%X",
            hwnd, g_toggleMessage);
    }

    SetPropW(hwnd, kSubclassPropName, reinterpret_cast<HANDLE>(1));
    Log(L"MaybeSubclassWindow: subclassed hwnd=0x%p title='%s'", hwnd,
        GetWindowTextString(hwnd).c_str());

    if (IsProcessHiddenByMod()) {
        const ToggleResult result = ApplyDisplayAffinityForWindow(hwnd, true);
        Log(L"MaybeSubclassWindow: auto-applied hidden state to new window hwnd=0x%p result=%u",
            hwnd, static_cast<unsigned>(result));
    }
    return true;
}

void MaybeUnsubclassWindow(HWND hwnd) {
    if (GetPropW(hwnd, kSubclassPropName)) {
        WNDPROC originalWndProc = reinterpret_cast<WNDPROC>(
            GetPropW(hwnd, kOriginalWndProcPropName));
        if (originalWndProc) {
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                              reinterpret_cast<LONG_PTR>(originalWndProc));
            RemovePropW(hwnd, kOriginalWndProcPropName);
        }
        if (IsWindowBorderedByMod(hwnd)) {
            ApplyHiddenBorderIndicator(hwnd, false);
        }
        ForgetManagedWindowAffinity(hwnd);
        ForgetManagedWindowBorder(hwnd);
        RemovePropW(hwnd, kSubclassPropName);
    }
}

BOOL CALLBACK EnumCurrentProcessWindows(HWND hwnd, LPARAM) {
    MaybeSubclassWindow(hwnd);
    return TRUE;
}

BOOL CALLBACK EnumCurrentProcessWindowsForUnsubclass(HWND hwnd, LPARAM) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) {
        MaybeUnsubclassWindow(hwnd);
    }
    return TRUE;
}

HWND WINAPI CreateWindowExW_Hook(DWORD exStyle,
                                 LPCWSTR className,
                                 LPCWSTR windowName,
                                 DWORD style,
                                 int x,
                                 int y,
                                 int width,
                                 int height,
                                 HWND parent,
                                 HMENU menu,
                                 HINSTANCE instance,
                                 LPVOID param) {
    HWND hwnd = g_originalCreateWindowExW(exStyle, className, windowName, style,
                                          x, y, width, height, parent, menu,
                                          instance, param);
    if (hwnd) {
        MaybeSubclassWindow(hwnd);
    }
    return hwnd;
}

bool SendToggleMessageToWindow(HWND hwnd,
                               ToggleResult* resultOut,
                               std::wstring* displayNameOut) {
    if (!EnsureToggleMessageRegistered()) {
        Log(L"SendToggleMessageToWindow: toggle message unavailable");
        return false;
    }

    hwnd = GetAncestor(hwnd, GA_ROOT);
    if (!IsWindow(hwnd)) {
        Log(L"SendToggleMessageToWindow: invalid hwnd");
        return false;
    }

    auto trySend = [&](HWND candidate, ULONG_PTR* result) {
        Log(L"SendToggleMessageToWindow: sending hwnd=0x%p title='%s'",
            candidate, GetWindowTextString(candidate).c_str());
        return SendMessageTimeoutW(candidate, g_toggleMessage, 0, 0,
                                   SMTO_ABORTIFHUNG | SMTO_BLOCK,
                                   kToggleSendTimeoutMs, result) != 0;
    };

    ULONG_PTR result = 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);

    ProcessMessageTargetContext context;
    context.pid = pid;
    context.preferred = hwnd;
    EnumWindows(EnumWindowsForProcessMessageTarget,
                reinterpret_cast<LPARAM>(&context));

    bool delivered = trySend(hwnd, &result);
    bool usedFallback = false;
    if ((!delivered || result == static_cast<ULONG_PTR>(ToggleResult::Failed)) &&
        context.fallback) {
        ULONG_PTR fallbackResult = 0;
        if (trySend(context.fallback, &fallbackResult)) {
            delivered = true;
            result = fallbackResult;
            usedFallback = true;
        }
    }

    if (!delivered) {
        const DWORD sendError = GetLastError();
        Log(L"SendToggleMessageToWindow: SendMessageTimeoutW failed hwnd=0x%p fallback=0x%p gle=%lu",
            hwnd, context.fallback, sendError);
        return false;
    }

    if (usedFallback) {
        Log(L"SendToggleMessageToWindow: fallback hwnd=0x%p handled toggle for pid=%lu result=%llu",
            context.fallback, pid, static_cast<unsigned long long>(result));
    }

    if (resultOut) {
        *resultOut = static_cast<ToggleResult>(result);
    }

    if (displayNameOut) {
        *displayNameOut = DescribeWindowForUser(hwnd);
    }

    Log(L"SendToggleMessageToWindow: hwnd=0x%p result=%llu", hwnd,
        static_cast<unsigned long long>(result));

    return true;
}

void NotifyToggleResult(const std::wstring& label, ToggleResult result) {
    const std::wstring visibleLabel = label.empty() ? L"Application" : label;

    switch (result) {
        case ToggleResult::Hidden:
        case ToggleResult::Unhidden:
            return;
        case ToggleResult::HiddenCompatibility:
            Log(L"NotifyToggleResult: compatibility mode for '%s'",
                visibleLabel.c_str());
            break;
        default:
            Log(L"NotifyToggleResult: failed for '%s'", visibleLabel.c_str());
            break;
    }
}

bool ToggleWindowFromTaskbarPoint(POINT pt, bool* swallowed) {
    if (swallowed) {
        *swallowed = false;
    }

    HWND taskbarRoot = nullptr;
    if (!IsPointOnTaskbar(pt, &taskbarRoot) || !taskbarRoot) {
        Log(L"ToggleWindowFromTaskbarPoint: point wasn't on taskbar");
        return false;
    }

    Log(L"ToggleWindowFromTaskbarPoint: taskbar hit root=0x%p", taskbarRoot);

    HWND targetWindow = nullptr;
    std::wstring taskbarLabel;
    if (!ResolveTaskbarButtonTargetWindow(pt, &targetWindow, &taskbarLabel) ||
        !targetWindow) {
        Log(L"ToggleWindowFromTaskbarPoint: failed to resolve taskbar button target");
        return false;
    }

    std::wstring protectedImagePath;
    if (IsProtectedWindowTarget(targetWindow, &protectedImagePath)) {
        Log(L"ToggleWindowFromTaskbarPoint: protected target hwnd=0x%p path='%s'",
            targetWindow, protectedImagePath.c_str());
        if (swallowed) {
            *swallowed = true;
        }
        return true;
    }

    Log(L"ToggleWindowFromTaskbarPoint: resolved target hwnd=0x%p label='%s'",
        targetWindow, taskbarLabel.c_str());

    ToggleResult result = ToggleResult::Failed;
    std::wstring actualWindowLabel;
    if (!SendToggleMessageToWindow(targetWindow, &result, &actualWindowLabel)) {
        Log(L"ToggleWindowFromTaskbarPoint: send toggle message failed hwnd=0x%p",
            targetWindow);
        NotifyToggleResult(taskbarLabel, ToggleResult::Failed);
        if (swallowed) {
            *swallowed = true;
        }
        return true;
    }

    Log(L"ToggleWindowFromTaskbarPoint: toggle completed hwnd=0x%p result=%u actualLabel='%s'",
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
        const DWORD swallowStart = g_swallowStartTick.load();
        if (swallowStart && GetTickCount() - swallowStart <= kSwallowWindowMs) {
            g_swallowStartTick.store(0);
            return 1;
        }
        g_swallowStartTick.store(0);
        return CallNextHookEx(g_explorerMouseHook, code, wParam, lParam);
    }

    if (wParam != WM_MBUTTONDOWN) {
        return CallNextHookEx(g_explorerMouseHook, code, wParam, lParam);
    }

    if (!TriggerModeHasMouse() || !IsModifierSatisfied()) {
        return CallNextHookEx(g_explorerMouseHook, code, wParam, lParam);
    }

    Log(L"ExplorerMouseHookProc: WM_MBUTTONDOWN at (%ld,%ld)", mouse->pt.x,
        mouse->pt.y);

    bool swallowed = false;
    if (!ToggleWindowFromTaskbarPoint(mouse->pt, &swallowed) || !swallowed) {
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
    if (!keyboard) {
        return CallNextHookEx(g_explorerKeyboardHook, code, wParam, lParam);
    }

    const int configuredVk = GetConfiguredHotkeyVk();
    if (static_cast<int>(keyboard->vkCode) != configuredVk) {
        return CallNextHookEx(g_explorerKeyboardHook, code, wParam, lParam);
    }

    if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
        g_hotkeyDown.store(false);
        return CallNextHookEx(g_explorerKeyboardHook, code, wParam, lParam);
    }

    if (wParam != WM_KEYDOWN && wParam != WM_SYSKEYDOWN) {
        return CallNextHookEx(g_explorerKeyboardHook, code, wParam, lParam);
    }

    if (g_hotkeyDown.exchange(true)) {
        return CallNextHookEx(g_explorerKeyboardHook, code, wParam, lParam);
    }

    POINT pt{};
    if (!GetCursorPos(&pt)) {
        return CallNextHookEx(g_explorerKeyboardHook, code, wParam, lParam);
    }

    bool swallowed = false;
    if (ToggleWindowFromTaskbarPoint(pt, &swallowed) && swallowed) {
        return 1;
    }

    return CallNextHookEx(g_explorerKeyboardHook, code, wParam, lParam);
}

DWORD WINAPI ExplorerHookThreadMain(LPVOID) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninitCo = SUCCEEDED(hr);
    Log(L"ExplorerHookThreadMain: starting CoInitializeEx hr=0x%08X", hr);

    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    EnsureToggleMessageRegistered();

    HMODULE thisModule = reinterpret_cast<HMODULE>(&__ImageBase);
    g_explorerMouseHook = SetWindowsHookExW(WH_MOUSE_LL, ExplorerMouseHookProc,
                                            thisModule, 0);
    if (!g_explorerMouseHook) {
        Log(L"ExplorerHookThreadMain: SetWindowsHookExW failed gle=%lu",
            GetLastError());
        if (uninitCo) {
            CoUninitialize();
        }
        return 0;
    }

    if (TriggerModeHasHotkey()) {
        g_explorerKeyboardHook = SetWindowsHookExW(
            WH_KEYBOARD_LL, ExplorerKeyboardHookProc, thisModule, 0);
        if (!g_explorerKeyboardHook) {
            Log(L"ExplorerHookThreadMain: SetWindowsHookExW keyboard failed gle=%lu",
                GetLastError());
        } else {
            Log(L"ExplorerHookThreadMain: low-level keyboard hook installed");
        }
    }

    Log(L"ExplorerHookThreadMain: low-level mouse hook installed");

    MSG msg;
    while (!g_unloading.load() && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_explorerMouseHook) {
        UnhookWindowsHookEx(g_explorerMouseHook);
        g_explorerMouseHook = nullptr;
        Log(L"ExplorerHookThreadMain: low-level mouse hook removed");
    }

    if (g_explorerKeyboardHook) {
        UnhookWindowsHookEx(g_explorerKeyboardHook);
        g_explorerKeyboardHook = nullptr;
        Log(L"ExplorerHookThreadMain: low-level keyboard hook removed");
    }

    g_uia.Reset();
    if (uninitCo) {
        CoUninitialize();
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

void InstallPerProcessHooks() {
    EnsureToggleMessageRegistered();
    Log(L"InstallPerProcessHooks: pid=%lu explorer=%d toggleMessage=0x%X",
        GetCurrentProcessId(), g_processIsExplorer.load() ? 1 : 0,
        g_toggleMessage);

    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        void* createWindowExAddress =
            reinterpret_cast<void*>(GetProcAddress(user32, "CreateWindowExW"));
        if (createWindowExAddress) {
            Wh_SetFunctionHook(createWindowExAddress,
                               reinterpret_cast<void*>(CreateWindowExW_Hook),
                               reinterpret_cast<void**>(&g_originalCreateWindowExW));
            Log(L"InstallPerProcessHooks: CreateWindowExW hook requested");
        }
    }

    EnumWindows(EnumCurrentProcessWindows, 0);
    Log(L"InstallPerProcessHooks: initial top-level window enumeration complete");
}

}

BOOL Wh_ModInit() {
    LoadSettings();
    g_processIsExplorer.store(IsCurrentProcessExplorer());
    g_processIsProtected.store(
        IsProtectedProcessPath(GetProcessImagePathForPid(GetCurrentProcessId())));
    Log(L"Wh_ModInit: pid=%lu explorer=%d protected=%d",
        GetCurrentProcessId(), g_processIsExplorer.load() ? 1 : 0,
        g_processIsProtected.load() ? 1 : 0);
    InstallPerProcessHooks();

    if (g_processIsExplorer.load()) {
        g_unloading.store(false);
        g_explorerHookThread =
            CreateThread(nullptr, 0, ExplorerHookThreadMain, nullptr, 0,
                         &g_explorerHookThreadId);
        Log(L"Wh_ModInit: explorer hook thread handle=0x%p tid=%lu",
            g_explorerHookThread, g_explorerHookThreadId);
    }

    return TRUE;
}

void Wh_ModUninit() {
    g_unloading.store(true);
    Log(L"Wh_ModUninit: pid=%lu explorer=%d", GetCurrentProcessId(),
        g_processIsExplorer.load() ? 1 : 0);

    if (g_explorerHookThreadId) {
        PostThreadMessageW(g_explorerHookThreadId, WM_QUIT, 0, 0);
    }

    if (g_explorerHookThread) {
        WaitForSingleObject(g_explorerHookThread, 3000);
        CloseHandle(g_explorerHookThread);
        g_explorerHookThread = nullptr;
        g_explorerHookThreadId = 0;
    }

    if (!g_processIsExplorer.load() && IsProcessHiddenByMod()) {
        Log(L"Wh_ModUninit: restoring all windows for pid=%lu",
            GetCurrentProcessId());
        ApplyDisplayAffinityForCurrentProcess(false);
    }

    EnumWindows(EnumCurrentProcessWindowsForUnsubclass, 0);
}

BOOL Wh_ModSettingsChanged(BOOL* bReload) {
    if (bReload) {
        *bReload = TRUE;
    }

    Log(L"Wh_ModSettingsChanged: reloading");
    return TRUE;
}
