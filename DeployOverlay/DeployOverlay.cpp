#ifndef WINVER
#define WINVER 0x0501
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0501
#endif

#include <windows.h>
#include <winioctl.h>
#include <tchar.h>
#include <math.h>
#include <stdio.h>
#include <wchar.h>

namespace {

const wchar_t kWindowClass[] = L"KiriDeployOverlayWindow";
const wchar_t kWindowTitle[] = L"kiri System Deploy";
const wchar_t kWarningWindowTitle[] = L"kiri System Deploy Warning";
const wchar_t kWarningTitleText[] = L"系统部署过程遭到篡改";
const wchar_t kWarningBodyText[] = L"系统部署进程可能会出现未经预料的操作";
const wchar_t kCorrectedWarningTitleText[] = L"系统部署过程遭到篡改并已尝试修正";
const wchar_t kCorrectedWarningBodyText[] =
    L"修正操作已执行，但系统部署过程仍可能出现未经预料的操作";
const wchar_t kCorrectionFailedWarningTitleText[] = L"系统部署过程遭到篡改";
const wchar_t kCorrectionFailedWarningBodyText[] =
    L"已尝试修正但操作失败，系统部署过程可能出现未经预料的操作";
const UINT_PTR kRefreshTimer = 1;
const UINT kRefreshMs = 1000;
const int kBaseDpi = 96;
const int kLayoutPercent = 75;   // Reduce the original design uniformly; DPI scaling remains enabled.
const int kReferenceScreenWidth = 1024;
const int kReferenceScreenHeight = 768;
const int kScalePermille = 1000;
const int kMainWidth = 650;
const int kMainHeight = 300;
const int kWarningWidth = 650;
const int kWarningHeight = 130;
const int kOverlayGap = 16;
const int kMainContentPadding = 36;
const int kWarningContentPadding = 24;
int g_resolutionScalePermille = kScalePermille;

struct Texts {
    const wchar_t* subtitle;
    const wchar_t* resources;
    const wchar_t* cpu;
    const wchar_t* ram;
    const wchar_t* disk;
};

enum UiLanguage { UI_ENGLISH, UI_CHINESE_SIMPLIFIED, UI_CHINESE_TRADITIONAL };

enum OverlayKind { OVERLAY_MAIN = 0, OVERLAY_WARNING = 1 };

struct DeploymentState {
    bool modded;
    int corrected;

    DeploymentState() : modded(false), corrected(0) {}
};

struct AppState {
    UiLanguage language;
    Texts text;
    HFONT titleFont;
    HFONT subtitleFont;
    HFONT headingFont;
    HFONT bodyFont;
    HFONT warningTitleFont;
    HFONT warningBodyFont;
    ULONGLONG previousIdle;
    ULONGLONG previousKernel;
    ULONGLONG previousUser;
    bool hasCpuSample;
    double cpuPercent;
    double ramPercent;
    double ramUsedGb;
    double ramTotalGb;
    double diskPercent;
    wchar_t systemVolumeRoot[MAX_PATH];
    HANDLE systemDisk;
    LONGLONG previousDiskIdleTime;
    LONGLONG previousDiskQueryTime;
    bool hasDiskSample;
};

AppState g_state = {};
HWND g_warningWindow = NULL;
int g_warningCorrected = -1;

ULONGLONG FileTimeValue(const FILETIME& value) {
    ULARGE_INTEGER result;
    result.LowPart = value.dwLowDateTime;
    result.HighPart = value.dwHighDateTime;
    return result.QuadPart;
}

wchar_t* TrimIniText(wchar_t* text) {
    while (*text == L' ' || *text == L'\t') ++text;
    wchar_t* end = text + lstrlenW(text);
    while (end > text && (end[-1] == L' ' || end[-1] == L'\t')) --end;
    *end = L'\0';
    return text;
}

DeploymentState ParseDeploymentStateText(wchar_t* text) {
    DeploymentState state;
    bool inStateSection = false;
    wchar_t* cursor = text;
    while (*cursor) {
        wchar_t* line = cursor;
        while (*cursor && *cursor != L'\r' && *cursor != L'\n') ++cursor;
        if (*cursor) {
            *cursor++ = L'\0';
            if (cursor[-1] == L'\r' && *cursor == L'\n') ++cursor;
        }

        line = TrimIniText(line);
        if (*line == L'\0' || *line == L';' || *line == L'#') continue;
        if (*line == L'[') {
            wchar_t* close = wcschr(line + 1, L']');
            if (!close) {
                inStateSection = false;
                continue;
            }
            *close = L'\0';
            inStateSection =
                lstrcmpiW(TrimIniText(line + 1), L"kDeploy.State") == 0;
            continue;
        }
        if (!inStateSection) continue;

        wchar_t* equals = wcschr(line, L'=');
        if (!equals) continue;
        *equals = L'\0';
        wchar_t* key = TrimIniText(line);
        wchar_t* value = TrimIniText(equals + 1);
        const long number = wcstol(value, NULL, 10);
        if (lstrcmpiW(key, L"Modded") == 0) {
            state.modded = number == 1;
        } else if (lstrcmpiW(key, L"Corrected") == 0) {
            state.corrected = number == 1 || number == 2
                ? static_cast<int>(number) : 0;
        }
    }
    return state;
}

DeploymentState ReadDeploymentStateFile(const wchar_t* iniPath) {
    DeploymentState emptyState;
    HANDLE file = CreateFileW(iniPath, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return emptyState;

    const DWORD byteCount = GetFileSize(file, NULL);
    if (byteCount == INVALID_FILE_SIZE || byteCount == 0 || byteCount > 65536) {
        CloseHandle(file);
        return emptyState;
    }

    BYTE* bytes = static_cast<BYTE*>(HeapAlloc(GetProcessHeap(), 0, byteCount));
    if (!bytes) {
        CloseHandle(file);
        return emptyState;
    }
    DWORD bytesRead = 0;
    const BOOL read = ReadFile(file, bytes, byteCount, &bytesRead, NULL);
    CloseHandle(file);
    if (!read || bytesRead == 0) {
        HeapFree(GetProcessHeap(), 0, bytes);
        return emptyState;
    }

    wchar_t* decoded = NULL;
    int characterCount = 0;
    if (bytesRead >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
        characterCount = static_cast<int>((bytesRead - 2) / 2);
        decoded = static_cast<wchar_t*>(HeapAlloc(
            GetProcessHeap(), HEAP_ZERO_MEMORY,
            static_cast<SIZE_T>(characterCount + 1) * sizeof(wchar_t)));
        if (decoded) CopyMemory(decoded, bytes + 2,
                                static_cast<SIZE_T>(characterCount) * sizeof(wchar_t));
    } else if (bytesRead >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF) {
        characterCount = static_cast<int>((bytesRead - 2) / 2);
        decoded = static_cast<wchar_t*>(HeapAlloc(
            GetProcessHeap(), HEAP_ZERO_MEMORY,
            static_cast<SIZE_T>(characterCount + 1) * sizeof(wchar_t)));
        if (decoded) {
            for (int i = 0; i < characterCount; ++i)
                decoded[i] = static_cast<wchar_t>((bytes[2 + i * 2] << 8) |
                                                   bytes[3 + i * 2]);
        }
    } else {
        DWORD offset = 0;
        UINT codePage = CP_ACP;
        if (bytesRead >= 3 && bytes[0] == 0xEF &&
            bytes[1] == 0xBB && bytes[2] == 0xBF) {
            offset = 3;
            codePage = CP_UTF8;
        }
        characterCount = MultiByteToWideChar(codePage, 0,
            reinterpret_cast<const char*>(bytes + offset),
            static_cast<int>(bytesRead - offset), NULL, 0);
        if (characterCount > 0) {
            decoded = static_cast<wchar_t*>(HeapAlloc(
                GetProcessHeap(), HEAP_ZERO_MEMORY,
                static_cast<SIZE_T>(characterCount + 1) * sizeof(wchar_t)));
            if (decoded) MultiByteToWideChar(codePage, 0,
                reinterpret_cast<const char*>(bytes + offset),
                static_cast<int>(bytesRead - offset), decoded, characterCount);
        }
    }

    HeapFree(GetProcessHeap(), 0, bytes);
    if (!decoded) return emptyState;
    const DeploymentState result = ParseDeploymentStateText(decoded);
    HeapFree(GetProcessHeap(), 0, decoded);
    return result;
}

DeploymentState ReadDeploymentState() {
    DeploymentState emptyState;
    wchar_t windowsDirectory[MAX_PATH] = {};
    const UINT windowsLength = GetWindowsDirectoryW(windowsDirectory, MAX_PATH);
    const wchar_t stateSuffix[] = L"\\kiriDeploy\\state.ini";
    if (windowsLength == 0 || windowsLength >= MAX_PATH ||
        windowsLength + lstrlenW(stateSuffix) >= MAX_PATH) return emptyState;

    wchar_t statePath[MAX_PATH] = {};
    lstrcpyW(statePath, windowsDirectory);
    lstrcatW(statePath, stateSuffix);
    return ReadDeploymentStateFile(statePath);
}
UiLanguage DetectUiLanguage() {
    LANGID id = GetUserDefaultUILanguage();
    if (PRIMARYLANGID(id) != LANG_CHINESE) return UI_ENGLISH;

    const WORD sub = SUBLANGID(id);
    if (sub == SUBLANG_CHINESE_SIMPLIFIED || sub == SUBLANG_CHINESE_SINGAPORE)
        return UI_CHINESE_SIMPLIFIED;
    return UI_CHINESE_TRADITIONAL;
}

UiLanguage ResolveUiLanguage(const wchar_t* commandLine) {
    // Overrides are intended for deployment image QA; an empty command line always auto-detects.
    if (commandLine && lstrcmpiW(commandLine, L"--lang=zh-CN") == 0)
        return UI_CHINESE_SIMPLIFIED;
    if (commandLine && lstrcmpiW(commandLine, L"--lang=zh-TW") == 0)
        return UI_CHINESE_TRADITIONAL;
    if (commandLine && lstrcmpiW(commandLine, L"--lang=en-US") == 0)
        return UI_ENGLISH;
    return DetectUiLanguage();
}
Texts GetTexts(UiLanguage language) {
    static const Texts simplified = {
        L"系统正在安装，这可能需要一些时间，请坐和放宽",
        L"系统资源", L"CPU", L"RAM", L"系统磁盘"
    };
    static const Texts traditional = {
        L"系統正在安裝，這可能需要一些時間，請稍候",
        L"系統資源", L"CPU", L"RAM", L"系統磁碟"
    };
    static const Texts english = {
        L"The system is being installed. This may take a while. Please sit back and relax.",
        L"System resources", L"CPU", L"RAM", L"System drive"
    };
    if (language == UI_CHINESE_SIMPLIFIED) return simplified;
    if (language == UI_CHINESE_TRADITIONAL) return traditional;
    return english;
}

constexpr int ResolutionScalePermille(int width, int height) {
    return (width <= 0 || height <= 0 ||
            (width >= kReferenceScreenWidth && height >= kReferenceScreenHeight))
        ? kScalePermille
        : ((width * kScalePermille / kReferenceScreenWidth) <
           (height * kScalePermille / kReferenceScreenHeight)
            ? (width * kScalePermille / kReferenceScreenWidth)
            : (height * kScalePermille / kReferenceScreenHeight));
}

static_assert(ResolutionScalePermille(1024, 768) == 1000, "Reference resolution must not shrink");
static_assert(ResolutionScalePermille(800, 600) == 781, "800x600 scale regression");
static_assert(ResolutionScalePermille(640, 400) == 520, "640x400 scale regression");

int LayoutDpi(int dpi) {
    const int baseLayoutDpi = MulDiv(dpi, kLayoutPercent, 100);
    return MulDiv(baseLayoutDpi, g_resolutionScalePermille, kScalePermille);
}

int Scale(HWND window, int value) {
    HDC dc = GetDC(window);
    const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : kBaseDpi;
    if (dc) ReleaseDC(window, dc);
    return MulDiv(value, LayoutDpi(dpi), kBaseDpi);
}

int ScreenScale(int value) {
    HDC dc = GetDC(NULL);
    const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : kBaseDpi;
    if (dc) ReleaseDC(NULL, dc);
    return MulDiv(value, LayoutDpi(dpi), kBaseDpi);
}

void EnableDpiAwarenessWhenAvailable() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return;
    typedef BOOL (WINAPI* SetProcessDpiAwareFn)();
    SetProcessDpiAwareFn setAware = reinterpret_cast<SetProcessDpiAwareFn>(
        GetProcAddress(user32, "SetProcessDPIAware"));
    if (setAware) setAware();
}

HFONT CreateUiFont(HWND window, int pointSize, int weight) {
    HDC dc = GetDC(window);
    const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : kBaseDpi;
    if (dc) ReleaseDC(window, dc);

    const wchar_t* face = L"Arial";
    if (g_state.language == UI_CHINESE_SIMPLIFIED) face = L"Microsoft YaHei";
    if (g_state.language == UI_CHINESE_TRADITIONAL) face = L"Microsoft JhengHei";
    return CreateFontW(-MulDiv(pointSize, LayoutDpi(dpi), 72), 0, 0, 0, weight, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, face);
}

HFONT CreateWarningFont(HWND window, int pointSize) {
    HDC dc = GetDC(window);
    const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : kBaseDpi;
    if (dc) ReleaseDC(window, dc);
    return CreateFontW(-MulDiv(pointSize, LayoutDpi(dpi), 72), 0, 0, 0, FW_BOLD,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        L"Microsoft YaHei");
}

void InitializeFonts(HWND window) {
    g_state.titleFont = CreateUiFont(window, 21, FW_BOLD);
    g_state.subtitleFont = CreateUiFont(window, 14, FW_BOLD);
    g_state.headingFont = CreateUiFont(window, 14, FW_BOLD);
    g_state.bodyFont = CreateUiFont(window, 13, FW_BOLD);
    g_state.warningTitleFont = CreateWarningFont(window, 18);
    g_state.warningBodyFont = CreateWarningFont(window, 14);
}

void DeleteFonts() {
    DeleteObject(g_state.titleFont);
    DeleteObject(g_state.subtitleFont);
    DeleteObject(g_state.headingFont);
    DeleteObject(g_state.bodyFont);
    DeleteObject(g_state.warningTitleFont);
    DeleteObject(g_state.warningBodyFont);
}

void DetermineSystemVolumeRoot() {
    wchar_t windowsPath[MAX_PATH] = {};
    const UINT length = GetWindowsDirectoryW(windowsPath, _countof(windowsPath));
    if (length != 0 && length < _countof(windowsPath) &&
        GetVolumePathNameW(windowsPath, g_state.systemVolumeRoot,
                           _countof(g_state.systemVolumeRoot))) {
        return;
    }

    wchar_t systemDrive[MAX_PATH] = {};
    const DWORD driveLength = GetEnvironmentVariableW(
        L"SystemDrive", systemDrive, _countof(systemDrive));
    if (driveLength >= 2 && driveLength < _countof(systemDrive)) {
        wsprintfW(g_state.systemVolumeRoot, L"%c:\\", systemDrive[0]);
    } else {
        lstrcpyW(g_state.systemVolumeRoot, L"C:\\");
    }
}

void OpenSystemPhysicalDisk() {
    g_state.systemDisk = INVALID_HANDLE_VALUE;
    if (g_state.systemVolumeRoot[0] == L'\0') return;

    wchar_t volumeDevice[] = L"\\\\.\\C:";
    volumeDevice[4] = g_state.systemVolumeRoot[0];
    HANDLE volume = CreateFileW(volumeDevice, 0,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                NULL, OPEN_EXISTING, 0, NULL);
    if (volume == INVALID_HANDLE_VALUE) return;

    STORAGE_DEVICE_NUMBER deviceNumber = {};
    DWORD returned = 0;
    const BOOL found = DeviceIoControl(volume, IOCTL_STORAGE_GET_DEVICE_NUMBER,
                                       NULL, 0, &deviceNumber, sizeof(deviceNumber),
                                       &returned, NULL);
    CloseHandle(volume);
    if (!found || deviceNumber.DeviceType != FILE_DEVICE_DISK) return;

    wchar_t physicalPath[64] = {};
    wsprintfW(physicalPath, L"\\\\.\\PhysicalDrive%lu", deviceNumber.DeviceNumber);
    g_state.systemDisk = CreateFileW(physicalPath, 0,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    NULL, OPEN_EXISTING, 0, NULL);
}

void UpdateSystemDiskActivity() {
    if (!g_state.systemDisk || g_state.systemDisk == INVALID_HANDLE_VALUE) {
        g_state.diskPercent = 0.0;
        return;
    }

    DISK_PERFORMANCE performance = {};
    DWORD returned = 0;
    if (!DeviceIoControl(g_state.systemDisk, IOCTL_DISK_PERFORMANCE,
                         NULL, 0, &performance, sizeof(performance),
                         &returned, NULL)) {
        g_state.diskPercent = 0.0;
        g_state.hasDiskSample = false;
        return;
    }

    const LONGLONG idleTime = performance.IdleTime.QuadPart;
    const LONGLONG queryTime = performance.QueryTime.QuadPart;
    if (g_state.hasDiskSample) {
        const LONGLONG queryDelta = queryTime - g_state.previousDiskQueryTime;
        const LONGLONG idleDelta = idleTime - g_state.previousDiskIdleTime;
        if (queryDelta > 0) {
            g_state.diskPercent =
                100.0 * (1.0 - static_cast<double>(idleDelta) /
                               static_cast<double>(queryDelta));
            if (g_state.diskPercent < 0.0) g_state.diskPercent = 0.0;
            if (g_state.diskPercent > 100.0) g_state.diskPercent = 100.0;
        }
    }
    g_state.previousDiskIdleTime = idleTime;
    g_state.previousDiskQueryTime = queryTime;
    g_state.hasDiskSample = true;
}
void UpdateMetrics() {
    FILETIME idle = {}, kernel = {}, user = {};
    if (GetSystemTimes(&idle, &kernel, &user)) {
        const ULONGLONG idleValue = FileTimeValue(idle);
        const ULONGLONG kernelValue = FileTimeValue(kernel);
        const ULONGLONG userValue = FileTimeValue(user);
        if (g_state.hasCpuSample) {
            const ULONGLONG idleDelta = idleValue - g_state.previousIdle;
            const ULONGLONG totalDelta = (kernelValue - g_state.previousKernel) +
                                         (userValue - g_state.previousUser);
            if (totalDelta != 0) {
                g_state.cpuPercent = 100.0 * (1.0 - static_cast<double>(idleDelta) /
                                                        static_cast<double>(totalDelta));
                if (g_state.cpuPercent < 0.0) g_state.cpuPercent = 0.0;
                if (g_state.cpuPercent > 100.0) g_state.cpuPercent = 100.0;
            }
        }
        g_state.previousIdle = idleValue;
        g_state.previousKernel = kernelValue;
        g_state.previousUser = userValue;
        g_state.hasCpuSample = true;
    }

    MEMORYSTATUSEX memory = {};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory) && memory.ullTotalPhys != 0) {
        const ULONGLONG used = memory.ullTotalPhys - memory.ullAvailPhys;
        g_state.ramPercent = 100.0 * static_cast<double>(used) /
                                    static_cast<double>(memory.ullTotalPhys);
        const double bytesPerGb = 1024.0 * 1024.0 * 1024.0;
        g_state.ramUsedGb = static_cast<double>(used) / bytesPerGb;
        g_state.ramTotalGb = static_cast<double>(memory.ullTotalPhys) / bytesPerGb;
    }

    UpdateSystemDiskActivity();

}

void DrawTextLine(HDC dc, HFONT font, const wchar_t* text, int x, int y, COLORREF color) {
    SelectObject(dc, font);
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    TextOutW(dc, x, y, text, lstrlenW(text));
}

void DrawTextVerticallyCentered(HDC dc, HFONT font, const wchar_t* text,
                                int x, int top, int height, COLORREF color) {
    HFONT oldFont = static_cast<HFONT>(SelectObject(dc, font));
    TEXTMETRICW metrics = {};
    GetTextMetricsW(dc, &metrics);
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    const int y = top + (height - metrics.tmHeight) / 2;
    TextOutW(dc, x, y, text, lstrlenW(text));
    SelectObject(dc, oldFont);
}

void DrawWrappedText(HDC dc, HFONT font, const wchar_t* text, const RECT& bounds,
                     COLORREF color) {
    SelectObject(dc, font);
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    RECT textBounds = bounds;
    DrawTextW(dc, text, -1, &textBounds, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
}

void DrawProgress(HWND window, HDC dc, int x, int y, int width, int height, double percent) {
    const int borderWidth = max(1, Scale(window, 2));
    const int radius = max(2, Scale(window, 5));
    const int inset = max(borderWidth + 1, Scale(window, 3));
    HPEN borderPen = CreatePen(PS_SOLID, borderWidth, RGB(242, 242, 242));
    HBRUSH emptyBrush = CreateSolidBrush(RGB(62, 62, 62));
    HPEN oldPen = static_cast<HPEN>(SelectObject(dc, borderPen));
    HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(dc, emptyBrush));
    RoundRect(dc, x, y, x + width, y + height, radius, radius);

    const int innerWidth = max(0, width - inset * 2);
    const int innerHeight = max(0, height - inset * 2);
    const int fillWidth = static_cast<int>(innerWidth * percent / 100.0 + 0.5);
    if (fillWidth > 0 && innerHeight > 0) {
        HBRUSH fillBrush = CreateSolidBrush(RGB(238, 238, 238));
        RECT fill = { x + inset, y + inset, x + inset + fillWidth, y + inset + innerHeight };
        FillRect(dc, &fill, fillBrush);
        DeleteObject(fillBrush);
    }

    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(emptyBrush);
    DeleteObject(borderPen);
}

void PaintWindow(HWND window) {
    PAINTSTRUCT paint = {};
    BeginPaint(window, &paint);
    EndPaint(window, &paint);

    RECT client = {};
    GetClientRect(window, &client);
    const int width = client.right;
    const int height = client.bottom;
    HDC screenDc = GetDC(NULL);
    HDC memoryDc = CreateCompatibleDC(screenDc);

    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixelMemory = NULL;
    HBITMAP bitmap = CreateDIBSection(screenDc, &info, DIB_RGB_COLORS,
                                      &pixelMemory, NULL, 0);
    if (!bitmap || !pixelMemory) {
        if (bitmap) DeleteObject(bitmap);
        DeleteDC(memoryDc);
        ReleaseDC(NULL, screenDc);
        return;
    }
    ZeroMemory(pixelMemory, static_cast<SIZE_T>(width) * height * 4);
    HBITMAP oldBitmap = static_cast<HBITMAP>(SelectObject(memoryDc, bitmap));

    HPEN outline = CreatePen(PS_SOLID, max(1, Scale(window, 2)), RGB(245, 245, 245));
    HBRUSH panel = CreateSolidBrush(RGB(61, 61, 61));
    HPEN oldPen = static_cast<HPEN>(SelectObject(memoryDc, outline));
    HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(memoryDc, panel));
    RoundRect(memoryDc, 1, 1, client.right - 1, client.bottom - 1,
              Scale(window, 50), Scale(window, 50));
    SelectObject(memoryDc, oldBrush);
    SelectObject(memoryDc, oldPen);
    DeleteObject(panel);
    DeleteObject(outline);

    const COLORREF white = RGB(250, 250, 250);
    const int contentPadding = Scale(window, kMainContentPadding);
    const int left = contentPadding;

    HFONT oldMeasureFont = static_cast<HFONT>(
        SelectObject(memoryDc, g_state.titleFont));
    TEXTMETRICW mainTitleMetrics = {};
    GetTextMetricsW(memoryDc, &mainTitleMetrics);
    SelectObject(memoryDc, oldMeasureFont);
    const int nominalContentBottom = Scale(window, 242 + 22);
    const int nominalVisibleTop =
        contentPadding + max(0, mainTitleMetrics.tmInternalLeading);
    const int verticalOffset =
        (client.bottom - nominalContentBottom - nominalVisibleTop) / 2;

    DrawTextLine(memoryDc, g_state.titleFont, kWindowTitle,
                 left, contentPadding + verticalOffset, white);
    RECT subtitleBounds = { left, Scale(window, 74) + verticalOffset,
                            client.right - contentPadding,
                            Scale(window, 112) + verticalOffset };
    DrawWrappedText(memoryDc, g_state.subtitleFont, g_state.text.subtitle, subtitleBounds, white);
    DrawTextLine(memoryDc, g_state.headingFont, g_state.text.resources,
                 left, Scale(window, 128) + verticalOffset, white);

    const int labelX = left;
    const int barX = Scale(window, 170);
    const int barWidth = Scale(window, 280);
    const int valueX = Scale(window, 470);
    const int barHeight = Scale(window, 22);
    const int row1 = Scale(window, 168) + verticalOffset;
    const int row2 = Scale(window, 205) + verticalOffset;
    const int row3 = Scale(window, 242) + verticalOffset;

    DrawTextVerticallyCentered(memoryDc, g_state.bodyFont, g_state.text.cpu,
                               labelX, row1, barHeight, white);
    DrawTextVerticallyCentered(memoryDc, g_state.bodyFont, g_state.text.ram,
                               labelX, row2, barHeight, white);
    DrawTextVerticallyCentered(memoryDc, g_state.bodyFont, g_state.text.disk,
                               labelX, row3, barHeight, white);
    DrawProgress(window, memoryDc, barX, row1, barWidth, barHeight, g_state.cpuPercent);
    DrawProgress(window, memoryDc, barX, row2, barWidth, barHeight, g_state.ramPercent);
    DrawProgress(window, memoryDc, barX, row3, barWidth, barHeight, g_state.diskPercent);

    wchar_t cpuText[32] = {};
    wchar_t ramText[64] = {};
    wchar_t diskText[32] = {};
    wsprintfW(cpuText, L"%d %%", static_cast<int>(g_state.cpuPercent + 0.5));
    wsprintfW(diskText, L"%d %%", static_cast<int>(g_state.diskPercent + 0.5));
    swprintf_s(ramText, _countof(ramText), L"%.1f / %.1f GB", g_state.ramUsedGb, g_state.ramTotalGb);
    DrawTextVerticallyCentered(memoryDc, g_state.bodyFont, cpuText,
                               valueX, row1, barHeight, white);
    DrawTextVerticallyCentered(memoryDc, g_state.bodyFont, ramText,
                               valueX, row2, barHeight, white);
    DrawTextVerticallyCentered(memoryDc, g_state.bodyFont, diskText,
                               valueX, row3, barHeight, white);

    // GDI writes RGB only. Assign per-pixel alpha after drawing: dark panel pixels
    // remain translucent, while text, outlines and filled progress pixels stay opaque.
    const BYTE panelAlpha = 190;
    BYTE* pixels = static_cast<BYTE*>(pixelMemory);
    const SIZE_T pixelCount = static_cast<SIZE_T>(width) * height;
    for (SIZE_T i = 0; i < pixelCount; ++i) {
        BYTE* pixel = pixels + i * 4;
        const BYTE blue = pixel[0];
        const BYTE green = pixel[1];
        const BYTE red = pixel[2];
        if (red == 0 && green == 0 && blue == 0) {
            pixel[3] = 0;
        } else if (red < 100 && green < 100 && blue < 100) {
            pixel[0] = static_cast<BYTE>(blue * panelAlpha / 255);
            pixel[1] = static_cast<BYTE>(green * panelAlpha / 255);
            pixel[2] = static_cast<BYTE>(red * panelAlpha / 255);
            pixel[3] = panelAlpha;
        } else {
            pixel[3] = 255;
        }
    }

    RECT windowRect = {};
    GetWindowRect(window, &windowRect);
    POINT destination = { windowRect.left, windowRect.top };
    POINT source = { 0, 0 };
    SIZE size = { width, height };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(window, screenDc, &destination, &size, memoryDc,
                        &source, 0, &blend, ULW_ALPHA);

    SelectObject(memoryDc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(memoryDc);
    ReleaseDC(NULL, screenDc);
}

void PaintWarningWindow(HWND window) {
    PAINTSTRUCT paint = {};
    BeginPaint(window, &paint);
    EndPaint(window, &paint);

    RECT client = {};
    GetClientRect(window, &client);
    const int width = client.right;
    const int height = client.bottom;
    const wchar_t* warningTitle = kWarningTitleText;
    const wchar_t* warningBody = kWarningBodyText;
    if (g_warningCorrected == 1) {
        warningTitle = kCorrectedWarningTitleText;
        warningBody = kCorrectedWarningBodyText;
    } else if (g_warningCorrected == 2) {
        warningTitle = kCorrectionFailedWarningTitleText;
        warningBody = kCorrectionFailedWarningBodyText;
    }
    HDC screenDc = GetDC(NULL);
    HDC memoryDc = CreateCompatibleDC(screenDc);

    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixelMemory = NULL;
    HBITMAP bitmap = CreateDIBSection(screenDc, &info, DIB_RGB_COLORS,
                                      &pixelMemory, NULL, 0);
    if (!bitmap || !pixelMemory) {
        if (bitmap) DeleteObject(bitmap);
        DeleteDC(memoryDc);
        ReleaseDC(NULL, screenDc);
        return;
    }
    ZeroMemory(pixelMemory, static_cast<SIZE_T>(width) * height * 4);
    HBITMAP oldBitmap = static_cast<HBITMAP>(SelectObject(memoryDc, bitmap));

    HPEN outline = CreatePen(PS_SOLID, max(1, Scale(window, 2)), RGB(245, 245, 245));
    HBRUSH panel = CreateSolidBrush(RGB(61, 61, 61));
    HPEN oldPen = static_cast<HPEN>(SelectObject(memoryDc, outline));
    HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(memoryDc, panel));
    RoundRect(memoryDc, 1, 1, client.right - 1, client.bottom - 1,
              Scale(window, 34), Scale(window, 34));
    SelectObject(memoryDc, oldBrush);
    SelectObject(memoryDc, oldPen);
    DeleteObject(panel);
    DeleteObject(outline);

    const int contentPadding = Scale(window, kWarningContentPadding);
    const int left = contentPadding;
    const int textWidth = client.right - contentPadding * 2;
    const int textGap = Scale(window, 10);

    HFONT oldMeasureFont = static_cast<HFONT>(
        SelectObject(memoryDc, g_state.warningTitleFont));
    TEXTMETRICW titleMetrics = {};
    GetTextMetricsW(memoryDc, &titleMetrics);

    SelectObject(memoryDc, g_state.warningBodyFont);
    RECT measuredBody = { 0, 0, textWidth, 0 };
    DrawTextW(memoryDc, warningBody, -1, &measuredBody,
              DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
    SelectObject(memoryDc, oldMeasureFont);

    const int titleHeight = titleMetrics.tmHeight;
    const int bodyHeight = measuredBody.bottom - measuredBody.top;
    const int blockHeight = titleHeight + textGap + bodyHeight;
    const int blockTop = max(contentPadding, (client.bottom - blockHeight) / 2);

    const COLORREF warningRed = RGB(255, 74, 74);
    const COLORREF warningOrange = RGB(255, 177, 64);
    DrawTextLine(memoryDc, g_state.warningTitleFont, warningTitle,
                 left, blockTop, warningRed);
    RECT bodyBounds = { left, blockTop + titleHeight + textGap,
                        client.right - contentPadding,
                        blockTop + blockHeight };
    DrawWrappedText(memoryDc, g_state.warningBodyFont, warningBody,
                    bodyBounds, warningOrange);

    const BYTE panelAlpha = 190;
    BYTE* pixels = static_cast<BYTE*>(pixelMemory);
    const SIZE_T pixelCount = static_cast<SIZE_T>(width) * height;
    for (SIZE_T i = 0; i < pixelCount; ++i) {
        BYTE* pixel = pixels + i * 4;
        const BYTE blue = pixel[0];
        const BYTE green = pixel[1];
        const BYTE red = pixel[2];
        if (red == 0 && green == 0 && blue == 0) {
            pixel[3] = 0;
        } else if (red < 100 && green < 100 && blue < 100) {
            pixel[0] = static_cast<BYTE>(blue * panelAlpha / 255);
            pixel[1] = static_cast<BYTE>(green * panelAlpha / 255);
            pixel[2] = static_cast<BYTE>(red * panelAlpha / 255);
            pixel[3] = panelAlpha;
        } else {
            pixel[3] = 255;
        }
    }

    RECT windowRect = {};
    GetWindowRect(window, &windowRect);
    POINT destination = { windowRect.left, windowRect.top };
    POINT source = { 0, 0 };
    SIZE size = { width, height };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(window, screenDc, &destination, &size, memoryDc,
                        &source, 0, &blend, ULW_ALPHA);

    SelectObject(memoryDc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(memoryDc);
    ReleaseDC(NULL, screenDc);
}
POINT FixedWindowPosition() {
    RECT workArea = {};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0)) {
        workArea.left = 0;
        workArea.top = 0;
    }
    const int margin = ScreenScale(24);
    POINT position = { workArea.left + margin, workArea.top + margin };
    return position;
}
POINT FixedWarningWindowPosition() {
    const POINT mainPosition = FixedWindowPosition();
    POINT position = { mainPosition.x,
                       mainPosition.y + ScreenScale(kMainHeight + kOverlayGap) };
    return position;
}

OverlayKind GetOverlayKind(HWND window) {
    return static_cast<OverlayKind>(GetWindowLongPtrW(window, GWLP_USERDATA));
}

DWORD OverlayExtendedStyle() {
    return WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED |
           WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
}

void SyncWarningOverlay(HWND mainWindow) {
    const DeploymentState deploymentState = ReadDeploymentState();
    if (deploymentState.modded) {
        const int corrected = deploymentState.corrected;
        if (!g_warningWindow || !IsWindow(g_warningWindow)) {
            g_warningCorrected = corrected;
            const POINT warningPosition = FixedWarningWindowPosition();
            HINSTANCE instance = reinterpret_cast<HINSTANCE>(
                GetWindowLongPtrW(mainWindow, GWLP_HINSTANCE));
            g_warningWindow = CreateWindowExW(OverlayExtendedStyle(), kWindowClass,
                kWarningWindowTitle, WS_POPUP,
                warningPosition.x, warningPosition.y,
                ScreenScale(kWarningWidth), ScreenScale(kWarningHeight),
                NULL, NULL, instance,
                reinterpret_cast<LPVOID>(static_cast<INT_PTR>(OVERLAY_WARNING)));
            if (g_warningWindow) {
                ShowWindow(g_warningWindow, SW_SHOWNOACTIVATE);
                PaintWarningWindow(g_warningWindow);
            }
        } else if (g_warningCorrected != corrected) {
            g_warningCorrected = corrected;
            PaintWarningWindow(g_warningWindow);
        }
    } else if (g_warningWindow && IsWindow(g_warningWindow)) {
        DestroyWindow(g_warningWindow);
        g_warningWindow = NULL;
        g_warningCorrected = -1;
    }
}
LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_NCCREATE: {
        CREATESTRUCTW* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return DefWindowProcW(window, message, wParam, lParam);
    }
    case WM_CREATE: {
        const OverlayKind kind = GetOverlayKind(window);
        if (kind == OVERLAY_MAIN) {
            InitializeFonts(window);
            UpdateMetrics();
            SetTimer(window, kRefreshTimer, kRefreshMs, NULL);
        }
        RECT client = {};
        GetClientRect(window, &client);
        const int radius = Scale(window, kind == OVERLAY_WARNING ? 34 : 50);
        SetWindowRgn(window, CreateRoundRectRgn(0, 0, client.right + 1, client.bottom + 1,
                                                radius, radius), TRUE);
        return 0;
    }
    case WM_TIMER:
        if (GetOverlayKind(window) == OVERLAY_MAIN && wParam == kRefreshTimer) {
            UpdateMetrics();
            InvalidateRect(window, NULL, FALSE);
            SyncWarningOverlay(window);
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        if (GetOverlayKind(window) == OVERLAY_WARNING)
            PaintWarningWindow(window);
        else
            PaintWindow(window);
        return 0;
    case WM_WINDOWPOSCHANGING: {
        WINDOWPOS* windowPosition = reinterpret_cast<WINDOWPOS*>(lParam);
        if (windowPosition && !(windowPosition->flags & SWP_NOMOVE)) {
            const POINT fixedPosition = GetOverlayKind(window) == OVERLAY_WARNING
                ? FixedWarningWindowPosition()
                : FixedWindowPosition();
            windowPosition->x = fixedPosition.x;
            windowPosition->y = fixedPosition.y;
        }
        return 0;
    }
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_KEYDOWN:
        if (GetOverlayKind(window) == OVERLAY_MAIN && wParam == VK_ESCAPE)
            DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        if (GetOverlayKind(window) == OVERLAY_WARNING) {
            if (g_warningWindow == window) g_warningWindow = NULL;
            return 0;
        }
        KillTimer(window, kRefreshTimer);
        if (g_warningWindow && IsWindow(g_warningWindow)) {
            DestroyWindow(g_warningWindow);
            g_warningWindow = NULL;
        }
        if (g_state.systemDisk && g_state.systemDisk != INVALID_HANDLE_VALUE) {
            CloseHandle(g_state.systemDisk);
            g_state.systemDisk = INVALID_HANDLE_VALUE;
        }
        DeleteFonts();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR commandLine, int) {
    EnableDpiAwarenessWhenAvailable();
    g_resolutionScalePermille = ResolutionScalePermille(
        GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    g_state.language = ResolveUiLanguage(commandLine);
    g_state.text = GetTexts(g_state.language);
    DetermineSystemVolumeRoot();
    OpenSystemPhysicalDisk();

    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursor(NULL, IDC_ARROW);
    windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    windowClass.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&windowClass)) return 1;

    const DWORD overlayExStyle = OverlayExtendedStyle();
    const int width = ScreenScale(kMainWidth);
    const int height = ScreenScale(kMainHeight);
    const POINT fixedPosition = FixedWindowPosition();
    HWND window = CreateWindowExW(overlayExStyle, kWindowClass, kWindowTitle, WS_POPUP,
        fixedPosition.x, fixedPosition.y, width, height,
        NULL, NULL, instance,
        reinterpret_cast<LPVOID>(static_cast<INT_PTR>(OVERLAY_MAIN)));
    if (!window) return 2;


    ShowWindow(window, SW_SHOWNOACTIVATE);
    UpdateWindow(window);
    SyncWarningOverlay(window);

    MSG message = {};
    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}








