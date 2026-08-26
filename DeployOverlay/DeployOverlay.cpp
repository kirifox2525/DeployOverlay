#ifndef WINVER
#define WINVER 0x0501
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0501
#endif

#include <windows.h>
#include <tchar.h>
#include <math.h>
#include <stdio.h>

namespace {

const wchar_t kWindowClass[] = L"KiriDeployOverlayWindow";
const wchar_t kWindowTitle[] = L"kiri System Deploy";
const UINT_PTR kRefreshTimer = 1;
const UINT kRefreshMs = 1000;
const int kBaseDpi = 96;
const int kLayoutPercent = 75;   // Reduce the original design uniformly; DPI scaling remains enabled.
const int kReferenceScreenWidth = 1024;
const int kReferenceScreenHeight = 768;
const int kScalePermille = 1000;
int g_resolutionScalePermille = kScalePermille;

struct Texts {
    const wchar_t* subtitle;
    const wchar_t* resources;
    const wchar_t* cpu;
    const wchar_t* ram;
    const wchar_t* disk;
};

enum UiLanguage { UI_ENGLISH, UI_CHINESE_SIMPLIFIED, UI_CHINESE_TRADITIONAL };

struct AppState {
    UiLanguage language;
    Texts text;
    HFONT titleFont;
    HFONT subtitleFont;
    HFONT headingFont;
    HFONT bodyFont;
    ULONGLONG previousIdle;
    ULONGLONG previousKernel;
    ULONGLONG previousUser;
    bool hasCpuSample;
    double cpuPercent;
    double ramPercent;
    double ramUsedGb;
    double ramTotalGb;
    double diskPercent;
    HANDLE deploymentProcess;
    bool sysprepCleanupPending;
    wchar_t systemDrive[4];
};

AppState g_state = {};

ULONGLONG FileTimeValue(const FILETIME& value) {
    ULARGE_INTEGER result;
    result.LowPart = value.dwLowDateTime;
    result.HighPart = value.dwHighDateTime;
    return result.QuadPart;
}

constexpr bool UsesLegacyMiniSetup(DWORD majorVersion, DWORD minorVersion) {
    return majorVersion == 5 && minorVersion >= 1;
}

static_assert(UsesLegacyMiniSetup(5, 1), "Windows XP must use setup.exe mini setup");
static_assert(UsesLegacyMiniSetup(5, 2), "Windows Server 2003 must use setup.exe mini setup");
static_assert(!UsesLegacyMiniSetup(6, 0), "Windows Vista must use WinDeploy.exe");
static_assert(!UsesLegacyMiniSetup(10, 0), "Modern Windows must use WinDeploy.exe");

HANDLE StartSystemDeployment() {
    struct NativeVersionInfo {
        ULONG size;
        ULONG majorVersion;
        ULONG minorVersion;
        ULONG buildNumber;
        ULONG platformId;
        WCHAR servicePack[128];
    };
    typedef LONG (WINAPI* RtlGetVersionFn)(NativeVersionInfo*);

    bool useLegacyMiniSetup = false;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    RtlGetVersionFn rtlGetVersion = ntdll
        ? reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"))
        : NULL;
    if (rtlGetVersion) {
        NativeVersionInfo versionInfo = {};
        versionInfo.size = sizeof(versionInfo);
        if (rtlGetVersion(&versionInfo) >= 0) {
            useLegacyMiniSetup = UsesLegacyMiniSetup(
                versionInfo.majorVersion, versionInfo.minorVersion);
        }
    }

    wchar_t windowsDirectory[MAX_PATH] = {};
    const UINT windowsLength = GetWindowsDirectoryW(windowsDirectory, MAX_PATH);
    if (windowsLength == 0 || windowsLength >= MAX_PATH) return NULL;

    const wchar_t* executableSuffix = useLegacyMiniSetup
        ? L"\\System32\\setup.exe"
        : L"\\System32\\Oobe\\WinDeploy.exe";
    const wchar_t* workingSuffix = useLegacyMiniSetup
        ? L"\\System32"
        : L"\\System32\\Oobe";
    if (windowsLength + lstrlenW(executableSuffix) >= MAX_PATH ||
        windowsLength + lstrlenW(workingSuffix) >= MAX_PATH) return NULL;

    wchar_t executablePath[MAX_PATH] = {};
    wchar_t workingDirectory[MAX_PATH] = {};
    lstrcpyW(executablePath, windowsDirectory);
    lstrcatW(executablePath, executableSuffix);
    lstrcpyW(workingDirectory, windowsDirectory);
    lstrcatW(workingDirectory, workingSuffix);

    wchar_t commandLine[MAX_PATH + 64] = {};
    if (useLegacyMiniSetup) {
        wsprintfW(commandLine, L"\"%s\" -newsetup -mini", executablePath);
    } else {
        wsprintfW(commandLine, L"\"%s\"", executablePath);
    }

    // A 32-bit overlay on 64-bit Windows would otherwise be redirected to SysWOW64.
    typedef BOOL (WINAPI* DisableWow64RedirectionFn)(PVOID*);
    typedef BOOL (WINAPI* RevertWow64RedirectionFn)(PVOID);
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    DisableWow64RedirectionFn disableRedirection = kernel32
        ? reinterpret_cast<DisableWow64RedirectionFn>(
            GetProcAddress(kernel32, "Wow64DisableWow64FsRedirection"))
        : NULL;
    RevertWow64RedirectionFn revertRedirection = kernel32
        ? reinterpret_cast<RevertWow64RedirectionFn>(
            GetProcAddress(kernel32, "Wow64RevertWow64FsRedirection"))
        : NULL;

    PVOID previousRedirection = NULL;
    const BOOL redirectionDisabled = disableRedirection && revertRedirection
        ? disableRedirection(&previousRedirection)
        : FALSE;

    STARTUPINFOW startupInfo = {};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo = {};
    HANDLE processHandle = NULL;
    if (CreateProcessW(executablePath, commandLine, NULL, NULL, FALSE, 0, NULL,
                       workingDirectory, &startupInfo, &processInfo)) {
        CloseHandle(processInfo.hThread);
        processHandle = processInfo.hProcess;
    }

    if (redirectionDisabled) revertRedirection(previousRedirection);
    return processHandle;
}

bool DeleteDirectoryTree(const wchar_t* directory) {
    const DWORD rootAttributes = GetFileAttributesW(directory);
    if (rootAttributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }
    if (!(rootAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    if (rootAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        SetFileAttributesW(directory, FILE_ATTRIBUTE_NORMAL);
        return RemoveDirectoryW(directory) != FALSE;
    }

    wchar_t searchPath[MAX_PATH] = {};
    const int directoryLength = lstrlenW(directory);
    if (directoryLength + 2 >= MAX_PATH) return false;
    lstrcpyW(searchPath, directory);
    if (directoryLength > 0 && directory[directoryLength - 1] != L'\\')
        lstrcatW(searchPath, L"\\");
    lstrcatW(searchPath, L"*");

    bool success = true;
    WIN32_FIND_DATAW entry = {};
    HANDLE findHandle = FindFirstFileW(searchPath, &entry);
    if (findHandle != INVALID_HANDLE_VALUE) {
        do {
            if (lstrcmpW(entry.cFileName, L".") == 0 ||
                lstrcmpW(entry.cFileName, L"..") == 0) continue;

            wchar_t childPath[MAX_PATH] = {};
            const int childLength = lstrlenW(entry.cFileName);
            if (directoryLength + 1 + childLength >= MAX_PATH) {
                success = false;
                continue;
            }
            lstrcpyW(childPath, directory);
            if (directoryLength > 0 && directory[directoryLength - 1] != L'\\')
                lstrcatW(childPath, L"\\");
            lstrcatW(childPath, entry.cFileName);

            if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                    SetFileAttributesW(childPath, FILE_ATTRIBUTE_NORMAL);
                    if (!RemoveDirectoryW(childPath)) success = false;
                } else if (!DeleteDirectoryTree(childPath)) {
                    success = false;
                }
            } else {
                SetFileAttributesW(childPath, FILE_ATTRIBUTE_NORMAL);
                if (!DeleteFileW(childPath)) success = false;
            }
        } while (FindNextFileW(findHandle, &entry));
        FindClose(findHandle);
    } else {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND) success = false;
    }

    SetFileAttributesW(directory, FILE_ATTRIBUTE_NORMAL);
    if (!RemoveDirectoryW(directory)) success = false;
    return success;
}

bool DeleteSystemSysprepDirectory() {
    wchar_t systemDrive[4] = {};
    const DWORD length = GetEnvironmentVariableW(L"SYSTEMDRIVE", systemDrive,
                                                  _countof(systemDrive));
    if (length != 2 || systemDrive[1] != L':') return false;

    wchar_t sysprepPath[MAX_PATH] = {};
    lstrcpyW(sysprepPath, systemDrive);
    lstrcatW(sysprepPath, L"\\Sysprep");
    return DeleteDirectoryTree(sysprepPath);
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

void InitializeFonts(HWND window) {
    g_state.titleFont = CreateUiFont(window, 21, FW_BOLD);
    g_state.subtitleFont = CreateUiFont(window, 14, FW_BOLD);
    g_state.headingFont = CreateUiFont(window, 14, FW_BOLD);
    g_state.bodyFont = CreateUiFont(window, 13, FW_BOLD);
}

void DeleteFonts() {
    DeleteObject(g_state.titleFont);
    DeleteObject(g_state.subtitleFont);
    DeleteObject(g_state.headingFont);
    DeleteObject(g_state.bodyFont);
}

void DetermineSystemDrive() {
    wchar_t windowsPath[MAX_PATH] = {};
    if (GetWindowsDirectoryW(windowsPath, MAX_PATH) >= 3) {
        g_state.systemDrive[0] = windowsPath[0];
        g_state.systemDrive[1] = L':';
        g_state.systemDrive[2] = L'\\';
        g_state.systemDrive[3] = L'\0';
    } else {
        lstrcpyW(g_state.systemDrive, L"C:\\");
    }
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

    ULARGE_INTEGER available = {}, total = {}, freeBytes = {};
    if (GetDiskFreeSpaceExW(g_state.systemDrive, &available, &total, &freeBytes) &&
        total.QuadPart != 0) {
        const ULONGLONG used = total.QuadPart - freeBytes.QuadPart;
        g_state.diskPercent = 100.0 * static_cast<double>(used) /
                                     static_cast<double>(total.QuadPart);
    }
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
    const int left = Scale(window, 38);
    DrawTextLine(memoryDc, g_state.titleFont, kWindowTitle, left, Scale(window, 30), white);
    RECT subtitleBounds = { left, Scale(window, 68), client.right - Scale(window, 36),
                            Scale(window, 112) };
    DrawWrappedText(memoryDc, g_state.subtitleFont, g_state.text.subtitle, subtitleBounds, white);
    DrawTextLine(memoryDc, g_state.headingFont, g_state.text.resources, left, Scale(window, 128), white);

    const int labelX = left;
    const int barX = Scale(window, 170);
    const int barWidth = Scale(window, 280);
    const int valueX = Scale(window, 470);
    const int barHeight = Scale(window, 22);
    const int row1 = Scale(window, 168);
    const int row2 = Scale(window, 205);
    const int row3 = Scale(window, 242);

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

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        InitializeFonts(window);
        UpdateMetrics();
        SetTimer(window, kRefreshTimer, kRefreshMs, NULL);
        RECT client = {};
        GetClientRect(window, &client);
        const int radius = Scale(window, 50);
        SetWindowRgn(window, CreateRoundRectRgn(0, 0, client.right + 1, client.bottom + 1,
                                                radius, radius), TRUE);
        return 0;
    }
    case WM_TIMER:
        if (wParam == kRefreshTimer) {
            if (g_state.deploymentProcess &&
                WaitForSingleObject(g_state.deploymentProcess, 0) == WAIT_OBJECT_0) {
                CloseHandle(g_state.deploymentProcess);
                g_state.deploymentProcess = NULL;
                g_state.sysprepCleanupPending = true;
            }
            if (g_state.sysprepCleanupPending && DeleteSystemSysprepDirectory()) {
                g_state.sysprepCleanupPending = false;
                DestroyWindow(window);
                return 0;
            }
            UpdateMetrics();
            InvalidateRect(window, NULL, FALSE);
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        PaintWindow(window);
        return 0;
    case WM_WINDOWPOSCHANGING: {
        WINDOWPOS* windowPosition = reinterpret_cast<WINDOWPOS*>(lParam);
        if (windowPosition && !(windowPosition->flags & SWP_NOMOVE)) {
            const POINT fixedPosition = FixedWindowPosition();
            windowPosition->x = fixedPosition.x;
            windowPosition->y = fixedPosition.y;
        }
        return 0;
    }
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        KillTimer(window, kRefreshTimer);
        if (g_state.deploymentProcess) {
            CloseHandle(g_state.deploymentProcess);
            g_state.deploymentProcess = NULL;
        }
        DeleteFonts();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR commandLine, int) {
    g_state.deploymentProcess = StartSystemDeployment();
    EnableDpiAwarenessWhenAvailable();
    g_resolutionScalePermille = ResolutionScalePermille(
        GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    g_state.language = ResolveUiLanguage(commandLine);
    g_state.text = GetTexts(g_state.language);
    DetermineSystemDrive();

    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursor(NULL, IDC_ARROW);
    windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    windowClass.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&windowClass)) return 1;

    const int width = ScreenScale(650);
    const int height = ScreenScale(300);
    const POINT fixedPosition = FixedWindowPosition();
    HWND window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
        kWindowClass, kWindowTitle, WS_POPUP,
        fixedPosition.x, fixedPosition.y, width, height,
        NULL, NULL, instance, NULL);
    if (!window) return 2;

    ShowWindow(window, SW_SHOWNOACTIVATE);
    UpdateWindow(window);

    MSG message = {};
    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}








