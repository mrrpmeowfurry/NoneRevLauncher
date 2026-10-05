// NoneRevLauncher - Our own tiny bootstrapper :3
//
// what it does:
//  - register "nonerev-launcher:" url protcol (per user)
//    as long as it's not "roblox-player:" it won't have issue with actual roblox installation
//  - when the site opens "nonerev-launcher:1+launchmode:play+gameinfo:<ticket>+placelauncherurl:<url>
//    it ask the site for current client version (<site>/setup/version), downloads <site>/setup/<version>-NoneRevPlayer.zip
//    if that version is not installed it will downoad and unzip at %LOCALAPPDATA%\NoneRev\Versions\<version>\
//    then start RobloxPlayerBeta.exe with the ticket
//  - run with no args will install/update and then open the site, while "--uninstall" will remove everything
//  - also registers "discord-<app id>:" so Discord's Join button can start us: we then ask Discord which
//    server the friend is in and open the game page on the site, which presses Play on that server
//  - studio: "nonerev-launcher:1+launchmode:edit+gameinfo:<ticket>+script:<edit.ashx url>" or "--studio"
//    installs the newest studio zip to %LOCALAPPDATA%\NoneRev\Studio\<version>\ and opens RobloxStudioBeta.exe

#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wininet.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <commctrl.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <map>
#include <stdexcept>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include "miniz.h"
#include "resource.h"

#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#ifndef NONEREV_DEFAULT_BASE_URL
// NONEREV_DEFAULT_BASE_URL should be set in the vcxproj
// if not then just go for localhost
#define NONEREV_DEFAULT_BASE_URL "http://localhost"
#endif

#ifndef NONEREV_DISCORD_APP_ID
#define NONEREV_DISCORD_APP_ID ""
#endif

static const wchar_t* kProtocol = L"nonerev-launcher";
static const wchar_t* kAppName = L"NoneRev";
static const wchar_t* kLauncherExe = L"NoneRevLauncher.exe";
static const wchar_t* kPlayerExe = L"RobloxPlayerBeta.exe";
static const wchar_t* kStudioExe = L"RobloxStudioBeta.exe";
static const wchar_t* kRegKey = L"Software\\NoneRev\\Launcher";

static std::wstring toW(const std::string& s)
{
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static std::string toA(const std::wstring& w)
{
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
}

static std::string trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// %XX decoding, site encode placelauncherurl with encodeURIComponent
static std::string urlDecode(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2]))
        {
            out += (char)strtol(s.substr(i + 1, 2).c_str(), NULL, 16);
            i += 2;
        }
        else if (s[i] == '+')
            out += ' ';
        else
            out += s[i];
    }
    return out;
}

static std::string baseOf(const std::string& url)
{
    size_t p = url.find("://");
    if (p == std::string::npos) return "";
    size_t q = url.find('/', p + 3);
    return q == std::string::npos ? url : url.substr(0, q);
}

static std::map<std::string, std::string> parseProtocolArgs(const std::string& arg)
{
    std::map<std::string, std::string> m;
    std::string s = arg;
    size_t colon = s.find(':');
    if (colon != std::string::npos) s = s.substr(colon + 1);
    size_t start = 0;
    while (start <= s.size())
    {
        size_t plus = s.find('+', start);
        std::string part = s.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
        size_t c = part.find(':');
        if (c != std::string::npos)
            m[part.substr(0, c)] = urlDecode(part.substr(c + 1));
        if (plus == std::string::npos) break;
        start = plus + 1;
    }
    return m;
}

static std::wstring installDir()
{
    wchar_t path[MAX_PATH];
    SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, path);
    std::wstring dir = std::wstring(path) + L"\\NoneRev";
    CreateDirectoryW(dir.c_str(), NULL);
    return dir;
}

static std::wstring versionsDir()
{
    std::wstring dir = installDir() + L"\\Versions";
    CreateDirectoryW(dir.c_str(), NULL);
    return dir;
}

// kept out of Versions, the client cleanup deletes everything in there but its own version
static std::wstring studioDir()
{
    std::wstring dir = installDir() + L"\\Studio";
    CreateDirectoryW(dir.c_str(), NULL);
    return dir;
}

static std::wstring thisExe()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    return path;
}

static bool fileExists(const std::wstring& p)
{
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static void createDirs(const std::wstring& path)
{
    for (size_t i = 3; i < path.size(); ++i)
    {
        if (path[i] == L'\\' || path[i] == L'/')
            CreateDirectoryW(path.substr(0, i).c_str(), NULL);
    }
    CreateDirectoryW(path.c_str(), NULL);
}

static void deleteTree(const std::wstring& dir)
{
    std::vector<wchar_t> buf(dir.begin(), dir.end());
    buf.push_back(0);
    buf.push_back(0);
    SHFILEOPSTRUCTW op = { 0 };
    op.wFunc = FO_DELETE;
    op.pFrom = &buf[0];
    op.fFlags = FOF_NO_UI;
    SHFileOperationW(&op);
}

static void regSet(const std::wstring& key, const wchar_t* name, const std::wstring& value)
{
    HKEY h;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, NULL, 0, KEY_WRITE, NULL, &h, NULL) == ERROR_SUCCESS)
    {
        RegSetValueExW(h, name, 0, REG_SZ, (const BYTE*)value.c_str(), (DWORD)((value.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(h);
    }
}

static std::wstring regGet(const std::wstring& key, const wchar_t* name)
{
    HKEY h;
    wchar_t buf[2048] = { 0 };
    DWORD size = sizeof(buf);
    std::wstring out;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_READ, &h) == ERROR_SUCCESS)
    {
        if (RegQueryValueExW(h, name, NULL, NULL, (BYTE*)buf, &size) == ERROR_SUCCESS)
            out = buf;
        RegCloseKey(h);
    }
    return out;
}

static const int kWinW = 500;
static const int kWinH = 320;
static const int kButtonW = 130;
static const int kButtonH = 44;
enum
{
    ID_BUTTON = 1001,
    WM_LAUNCHER_DONE = WM_APP + 1,
    // the worker thread never touches the controls, it posts these to the window
    WM_UI_STATUS,       // lParam: new std::wstring
    WM_UI_PROGRESS,     // wParam: percent
    WM_UI_MARQUEE,
    WM_UI_CANCEL,       // wParam: show
    WM_UI_SUCCESS       // lParam: new std::pair<std::wstring, std::wstring>
};

// set by the Cancel button / closing the window, the worker checks it between chunks
static volatile LONG g_cancel = 0;
struct Cancelled : std::runtime_error { Cancelled() : std::runtime_error("cancelled") {} };
static void checkCancel()
{
    if (g_cancel) throw Cancelled();
}

static HWND g_wnd = NULL, g_logo = NULL, g_label = NULL, g_bar = NULL, g_button = NULL, g_title = NULL, g_subtitle = NULL;
static HBITMAP g_bmpLogo = NULL, g_bmpCancel = NULL, g_bmpCancelOn = NULL, g_bmpOk = NULL, g_bmpOkOn = NULL;
static HFONT g_fontMsg = NULL, g_fontTitle = NULL, g_fontSub = NULL;
static HBRUSH g_white = NULL;
static WNDPROC g_oldButtonProc = NULL;
static bool g_buttonIsOk = false;
static bool g_finished = false;

static void uiSetStatus(const std::wstring& text)
{
    if (g_label) SetWindowTextW(g_label, text.c_str());
}

static void uiSetProgress(int percent)
{
    if (!g_bar) return;
    LONG style = GetWindowLongW(g_bar, GWL_STYLE);
    if (style & PBS_MARQUEE)
    {
        SendMessage(g_bar, PBM_SETMARQUEE, FALSE, 0);
        SetWindowLongW(g_bar, GWL_STYLE, style & ~PBS_MARQUEE);
    }
    SendMessage(g_bar, PBM_SETPOS, percent < 0 ? 0 : (percent > 100 ? 100 : percent), 0);
}

static void uiSetMarquee()
{
    if (!g_bar) return;
    LONG style = GetWindowLongW(g_bar, GWL_STYLE);
    if (!(style & PBS_MARQUEE))
    {
        SetWindowLongW(g_bar, GWL_STYLE, style | PBS_MARQUEE);
        SendMessage(g_bar, PBM_SETMARQUEE, TRUE, 30);
    }
}

static void uiShowCancel(bool show)
{
    if (g_button && !g_buttonIsOk) ShowWindow(g_button, show ? SW_SHOW : SW_HIDE);
}

static void uiShowSuccess(const std::wstring& title, const std::wstring& subtitle)
{
    ShowWindow(g_label, SW_HIDE);
    ShowWindow(g_bar, SW_HIDE);
    SetWindowTextW(g_title, title.c_str());
    SetWindowTextW(g_subtitle, subtitle.c_str());
    ShowWindow(g_title, SW_SHOW);
    ShowWindow(g_subtitle, SW_SHOW);
    g_buttonIsOk = true;
    SendMessage(g_button, BM_SETIMAGE, (WPARAM)IMAGE_BITMAP, (LPARAM)g_bmpOk);
    ShowWindow(g_button, SW_SHOW);
    InvalidateRect(g_wnd, NULL, TRUE);
}

static void uiCancel()
{
    InterlockedExchange(&g_cancel, 1);
    EnableWindow(g_button, FALSE);
    uiSetStatus(L"Cancelling...");
}

static void setStatus(const std::wstring& text)
{
    std::wstring* s = new std::wstring(text);
    if (!g_wnd || !PostMessage(g_wnd, WM_UI_STATUS, 0, (LPARAM)s)) delete s;
}

static void setProgress(int percent)
{
    if (g_wnd) PostMessage(g_wnd, WM_UI_PROGRESS, (WPARAM)percent, 0);
}

static void setMarquee()
{
    if (g_wnd) PostMessage(g_wnd, WM_UI_MARQUEE, 0, 0);
}

static void showCancel(bool show)
{
    if (g_wnd) PostMessage(g_wnd, WM_UI_CANCEL, show ? 1 : 0, 0);
}

static void showSuccess(const std::wstring& title, const std::wstring& subtitle)
{
    std::pair<std::wstring, std::wstring>* p = new std::pair<std::wstring, std::wstring>(title, subtitle);
    if (!g_wnd || !PostMessage(g_wnd, WM_UI_SUCCESS, 0, (LPARAM)p)) delete p;
}

// button is bitmap so just swap it for the -ON one while the mouse is over it
static LRESULT CALLBACK buttonProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    static bool tracking = false;
    if (msg == WM_MOUSEMOVE && !tracking)
    {
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, h, 0 };
        tracking = TrackMouseEvent(&tme) != 0;
        SendMessage(h, BM_SETIMAGE, (WPARAM)IMAGE_BITMAP, (LPARAM)(g_buttonIsOk ? g_bmpOkOn : g_bmpCancelOn));
    }
    else if (msg == WM_MOUSELEAVE)
    {
        tracking = false;
        SendMessage(h, BM_SETIMAGE, (WPARAM)IMAGE_BITMAP, (LPARAM)(g_buttonIsOk ? g_bmpOk : g_bmpCancel));
    }
    return CallWindowProcW(g_oldButtonProc, h, msg, w, l);
}

static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    switch (msg)
    {
    case WM_NCCALCSIZE:
        return 0;
    case WM_NCHITTEST:
    {
        LRESULT hit = DefWindowProcW(h, msg, w, l);
        return hit == HTCLIENT ? HTCAPTION : hit;
    }
    case WM_CTLCOLORSTATIC:
        SetTextColor((HDC)w, RGB(25, 25, 25));
        SetBkMode((HDC)w, TRANSPARENT);
        return (LRESULT)g_white;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(h, &ps);
        FillRect(hdc, &ps.rcPaint, g_white);
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(184, 184, 184));
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        RECT rc;
        GetClientRect(h, &rc);
        Rectangle(hdc, 0, 0, rc.right, rc.bottom);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ACTIVATE:
        InvalidateRect(h, NULL, FALSE);
        break;
    case WM_COMMAND:
        if (LOWORD(w) == ID_BUTTON)
        {
            if (g_buttonIsOk) DestroyWindow(h);
            else uiCancel();
        }
        break;
    case WM_UI_STATUS:
    {
        std::wstring* s = (std::wstring*)l;
        uiSetStatus(*s);
        delete s;
        return 0;
    }
    case WM_UI_PROGRESS:
        uiSetProgress((int)w);
        return 0;
    case WM_UI_MARQUEE:
        uiSetMarquee();
        return 0;
    case WM_UI_CANCEL:
        uiShowCancel(w != 0);
        return 0;
    case WM_UI_SUCCESS:
    {
        std::pair<std::wstring, std::wstring>* p = (std::pair<std::wstring, std::wstring>*)l;
        uiShowSuccess(p->first, p->second);
        delete p;
        return 0;
    }
    case WM_LAUNCHER_DONE:
        DestroyWindow(h);
        return 0;
    case WM_CLOSE:
        // the worker cleans up after itself and then closes us
        if (g_finished) DestroyWindow(h);
        else uiCancel();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

static HFONT makeFont(const wchar_t* face, int points, int weight)
{
    HDC hdc = GetDC(NULL);
    int height = -MulDiv(points, GetDeviceCaps(hdc, LOGPIXELSY), 72);
    ReleaseDC(NULL, hdc);
    return CreateFontW(height, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, face);
}

static void createWindow()
{
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    HINSTANCE inst = GetModuleHandle(NULL);

    g_white = CreateSolidBrush(RGB(255, 255, 255));
    g_bmpLogo = LoadBitmapW(inst, MAKEINTRESOURCEW(IDB_LOGO));
    g_bmpCancel = LoadBitmapW(inst, MAKEINTRESOURCEW(IDB_BTN_CANCEL));
    g_bmpCancelOn = LoadBitmapW(inst, MAKEINTRESOURCEW(IDB_BTN_CANCEL_ON));
    g_bmpOk = LoadBitmapW(inst, MAKEINTRESOURCEW(IDB_BTN_OK));
    g_bmpOkOn = LoadBitmapW(inst, MAKEINTRESOURCEW(IDB_BTN_OK_ON));
    g_fontMsg = makeFont(L"Tahoma", 11, FW_NORMAL);
    g_fontTitle = makeFont(L"Segoe UI", 20, FW_LIGHT);
    g_fontSub = makeFont(L"Segoe UI", 12, FW_NORMAL);

    WNDCLASSW wc = { 0 };
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = g_white;
    wc.lpszClassName = L"NoneRevLauncherWindow";
    RegisterClassW(&wc);

    int x = (GetSystemMetrics(SM_CXSCREEN) - kWinW) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - kWinH) / 2;
    g_wnd = CreateWindowExW(0, wc.lpszClassName, kAppName, WS_POPUP | WS_THICKFRAME | WS_VISIBLE,
        x, y, kWinW, kWinH, NULL, NULL, inst, NULL);

    BITMAP bm = { 0 };
    GetObject(g_bmpLogo, sizeof(bm), &bm);
    g_logo = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_BITMAP,
        (kWinW - bm.bmWidth) / 2, 70, bm.bmWidth, bm.bmHeight, g_wnd, NULL, inst, NULL);
    SendMessage(g_logo, STM_SETIMAGE, (WPARAM)IMAGE_BITMAP, (LPARAM)g_bmpLogo);

    g_label = CreateWindowExW(0, L"STATIC", L"Please wait...", WS_CHILD | WS_VISIBLE | SS_CENTER,
        5, 200, kWinW - 10, 20, g_wnd, NULL, inst, NULL);
    SendMessage(g_label, WM_SETFONT, (WPARAM)g_fontMsg, TRUE);

    g_bar = CreateWindowExW(0, PROGRESS_CLASSW, NULL, WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
        24, 242, 452, 20, g_wnd, NULL, inst, NULL);
    SendMessage(g_bar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));

    // success screen, hidden til need
    g_title = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_CENTER, 5, 178, kWinW - 10, 40, g_wnd, NULL, inst, NULL);
    SendMessage(g_title, WM_SETFONT, (WPARAM)g_fontTitle, TRUE);
    g_subtitle = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_CENTER, 5, 222, kWinW - 10, 24, g_wnd, NULL, inst, NULL);
    SendMessage(g_subtitle, WM_SETFONT, (WPARAM)g_fontSub, TRUE);

    g_button = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | BS_FLAT | BS_BITMAP,
        (kWinW - kButtonW) / 2, kWinH - 55, kButtonW, kButtonH, g_wnd, (HMENU)ID_BUTTON, inst, NULL);
    SendMessage(g_button, BM_SETIMAGE, (WPARAM)IMAGE_BITMAP, (LPARAM)g_bmpCancel);
    g_oldButtonProc = (WNDPROC)SetWindowLongPtrW(g_button, GWLP_WNDPROC, (LONG_PTR)buttonProc);

    setMarquee();
}

static HINTERNET openInternet()
{
    HINTERNET h = InternetOpenW(L"NoneRevLauncher/1.0", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!h) throw std::runtime_error("InternetOpen failed");
    // a stalled server should end in an error, not a window stuck forever
    DWORD connectMs = 15000, receiveMs = 30000;
    InternetSetOptionW(h, INTERNET_OPTION_CONNECT_TIMEOUT, &connectMs, sizeof(connectMs));
    InternetSetOptionW(h, INTERNET_OPTION_RECEIVE_TIMEOUT, &receiveMs, sizeof(receiveMs));
    return h;
}

static std::string httpGetString(const std::string& url)
{
    HINTERNET inet = openInternet();
    HINTERNET req = InternetOpenUrlA(inet, url.c_str(), NULL, 0, INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (!req) { InternetCloseHandle(inet); throw std::runtime_error("Could not reach " + url); }

    DWORD status = 0, size = sizeof(status);
    HttpQueryInfoA(req, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &size, NULL);
    std::string out;
    char buf[8192];
    DWORD read = 0;
    while (InternetReadFile(req, buf, sizeof(buf), &read) && read > 0)
        out.append(buf, read);
    InternetCloseHandle(req);
    InternetCloseHandle(inet);
    if (status != 200)
    {
        char msg[64];
        sprintf_s(msg, "HTTP %lu for ", status);
        throw std::runtime_error(std::string(msg) + url);
    }
    return out;
}

static void httpDownload(const std::string& url, const std::wstring& dest, const std::wstring& what)
{
    HINTERNET inet = openInternet();
    HINTERNET req = InternetOpenUrlA(inet, url.c_str(), NULL, 0, INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (!req) { InternetCloseHandle(inet); throw std::runtime_error("Could not download " + url); }

    DWORD status = 0, size = sizeof(status);
    HttpQueryInfoA(req, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &size, NULL);
    if (status != 200)
    {
        InternetCloseHandle(req); InternetCloseHandle(inet);
        char msg[64];
        sprintf_s(msg, "HTTP %lu for ", status);
        throw std::runtime_error(std::string(msg) + url);
    }
    DWORD total = 0; size = sizeof(total);
    HttpQueryInfoA(req, HTTP_QUERY_CONTENT_LENGTH | HTTP_QUERY_FLAG_NUMBER, &total, &size, NULL);

    HANDLE file = CreateFileW(dest.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) { InternetCloseHandle(req); InternetCloseHandle(inet); throw std::runtime_error("Could not create download file"); }

    std::vector<char> buf(256 * 1024);
    DWORD read = 0, done = 0, written = 0;
    bool ok = true;
    setStatus(what);
    bool cancelled = false;
    for (;;)
    {
        if (g_cancel) { cancelled = true; break; }
        if (!InternetReadFile(req, &buf[0], (DWORD)buf.size(), &read)) { ok = false; break; }
        if (read == 0) break;
        if (!WriteFile(file, &buf[0], read, &written, NULL) || written != read) { ok = false; break; }
        done += read;
        if (total > 0)
            setProgress((int)((unsigned long long)done * 100 / total));
    }
    CloseHandle(file);
    InternetCloseHandle(req);
    InternetCloseHandle(inet);
    if (cancelled || !ok || (total > 0 && done != total))
    {
        DeleteFileW(dest.c_str());
        if (cancelled) throw Cancelled();
        throw std::runtime_error("Download was cut short, try again");
    }
}

static size_t writeToHandle(void* file, mz_uint64, const void* buf, size_t n)
{
    DWORD written = 0;
    return WriteFile((HANDLE)file, buf, (DWORD)n, &written, NULL) ? written : 0;
}

static void unzip(const std::wstring& zipPath, const std::wstring& destDir)
{
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    // open it ourselves so user folders with non-ascii names work (8.3 short names can be turned off)
    FILE* f = NULL;
    if (_wfopen_s(&f, zipPath.c_str(), L"rb") != 0 || !f) throw std::runtime_error("Could not open the downloaded zip");
    _fseeki64(f, 0, SEEK_END);
    mz_uint64 zipSize = (mz_uint64)_ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    if (!mz_zip_reader_init_cfile(&zip, f, zipSize, 0))
    {
        fclose(f);
        throw std::runtime_error("The downloaded file is not a valid zip");
    }
    // reader_end first, then the file it reads from
    struct CloseZip
    {
        mz_zip_archive* zip; FILE* f;
        ~CloseZip() { mz_zip_reader_end(zip); fclose(f); }
    } closeZip = { &zip, f };

    mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; ++i)
    {
        checkCancel();
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
        std::wstring rel = toW(st.m_filename);
        for (size_t k = 0; k < rel.size(); ++k) if (rel[k] == L'/') rel[k] = L'\\';
        // no zip slip: nothing that climbs out, is absolute or names a drive
        if (rel.empty() || rel.find(L"..") != std::wstring::npos || rel[0] == L'\\' || rel.find(L':') != std::wstring::npos)
            continue;
        std::wstring out = destDir + L"\\" + rel;
        // .net's ZipFile (package.ps1) write stuff without the dos dir attribute, so
        // miniz doesnt spot them, a trailing slash is a folder no matter what the attrs say
        bool isDir = mz_zip_reader_is_file_a_directory(&zip, i) != 0 || (!rel.empty() && rel[rel.size() - 1] == L'\\');
        if (isDir)
        {
            createDirs(out);
            continue;
        }
        size_t slash = out.find_last_of(L'\\');
        if (slash != std::wstring::npos) createDirs(out.substr(0, slash));

        HANDLE file = CreateFileW(out.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Could not write " + std::string(st.m_filename));
        // straight to disk, no copy of the whole file in memory
        mz_bool extracted = mz_zip_reader_extract_to_callback(&zip, i, writeToHandle, file, 0);
        CloseHandle(file);
        if (!extracted) throw std::runtime_error("Could not extract " + std::string(st.m_filename) + ", the disk may be full");

        if (i % 50 == 0)
            setProgress((int)(i * 100 / count));
    }
}

static void writeAppSettings(const std::wstring& versionDir, const std::string& baseUrl)
{
    std::string xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
        "<Settings>\r\n"
        "  <BaseUrl>" + baseUrl + "/</BaseUrl>\r\n"
        "  <ContentFolder>content</ContentFolder>\r\n"
        "  <SilentCrashReport>0</SilentCrashReport>\r\n"
        "  <HideChatWindow>0</HideChatWindow>\r\n"
        "  <DiscordAppId>" NONEREV_DISCORD_APP_ID "</DiscordAppId>\r\n"
        "</Settings>\r\n";
    HANDLE file = CreateFileW((versionDir + L"\\AppSettings.xml").c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Could not write AppSettings.xml");
    DWORD written = 0;
    WriteFile(file, xml.c_str(), (DWORD)xml.size(), &written, NULL);
    CloseHandle(file);
}

static std::wstring discordProtocol()
{
    std::string id = NONEREV_DISCORD_APP_ID;
    return id.empty() ? L"" : L"discord-" + toW(id);
}

static void registerProtocol(const std::wstring& exe)
{
    std::wstring key = std::wstring(L"Software\\Classes\\") + kProtocol;
    regSet(key, NULL, L"URL:NoneRev Launcher Protocol");
    regSet(key, L"URL Protocol", L"");
    regSet(key + L"\\DefaultIcon", NULL, L"\"" + exe + L"\",0");
    regSet(key + L"\\shell\\open\\command", NULL, L"\"" + exe + L"\" \"%1\"");

    // Discord looks this one up when someone presses Join and the game is not running yet
    std::wstring discord = discordProtocol();
    if (!discord.empty())
    {
        std::wstring dkey = L"Software\\Classes\\" + discord;
        regSet(dkey, NULL, L"URL:Run NoneRev from Discord");
        regSet(dkey, L"URL Protocol", L"");
        regSet(dkey + L"\\DefaultIcon", NULL, L"\"" + exe + L"\",0");
        regSet(dkey + L"\\shell\\open\\command", NULL, L"\"" + exe + L"\" \"%1\"");
    }
}

static void unregisterProtocol()
{
    SHDeleteKeyW(HKEY_CURRENT_USER, (std::wstring(L"Software\\Classes\\") + kProtocol).c_str());
    std::wstring discord = discordProtocol();
    if (!discord.empty())
        SHDeleteKeyW(HKEY_CURRENT_USER, (L"Software\\Classes\\" + discord).c_str());
}

static const wchar_t* kUninstallKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\NoneRev";

static void registerUninstaller(const std::wstring& exe, const std::wstring& dir)
{
    regSet(kUninstallKey, L"DisplayName", L"NoneRev");
    regSet(kUninstallKey, L"DisplayIcon", L"\"" + exe + L"\",0");
    regSet(kUninstallKey, L"Publisher", L"NoneRev");
    regSet(kUninstallKey, L"InstallLocation", dir);
    regSet(kUninstallKey, L"UninstallString", L"\"" + exe + L"\" --uninstall");
    regSet(kUninstallKey, L"DisplayVersion", L"1.0");
    HKEY h;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, NULL, 0, KEY_WRITE, NULL, &h, NULL) == ERROR_SUCCESS)
    {
        DWORD one = 1;
        RegSetValueExW(h, L"NoModify", 0, REG_DWORD, (const BYTE*)&one, sizeof(one));
        RegSetValueExW(h, L"NoRepair", 0, REG_DWORD, (const BYTE*)&one, sizeof(one));
        RegCloseKey(h);
    }
}

// our exe lives in the install folder, so a copy in %TEMP% waits for us to exit and then removes it
static void scheduleSelfDelete()
{
    wchar_t tmp[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, tmp)) return;
    std::wstring helper = std::wstring(tmp) + L"NoneRevUninstall.exe";
    if (!CopyFileW(thisExe().c_str(), helper.c_str(), FALSE)) return;

    wchar_t pid[16];
    swprintf_s(pid, L"%lu", GetCurrentProcessId());
    std::wstring cmd = L"\"" + helper + L"\" --remove-install " + pid;
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    if (CreateProcessW(helper.c_str(), &buf[0], NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
    {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

// the %TEMP% copy: wait until the launcher has really exited, then delete the install folder.
// only ever installDir(), never a path from the command line
static void finishUninstall(DWORD pid)
{
    HANDLE launcher = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (launcher)
    {
        WaitForSingleObject(launcher, 60000);
        CloseHandle(launcher);
    }
    deleteTree(installDir());
}

static void createStudioShortcut(const std::wstring& launcher);

static std::wstring installSelf()
{
    std::wstring dir = installDir();
    std::wstring target = dir + L"\\" + kLauncherExe;
    std::wstring me = thisExe();
    if (_wcsicmp(me.c_str(), target.c_str()) != 0)
        CopyFileW(me.c_str(), target.c_str(), FALSE);
    registerProtocol(target);
    createStudioShortcut(target);
    registerUninstaller(target, dir);
    regSet(kRegKey, L"InstallDir", dir);
    return target;
}

static std::string fetchVersion(const std::string& url)
{
    std::string version = trim(httpGetString(url));
    if (version.empty() || version.size() > 40) throw std::runtime_error("The site did not give a version: " + url);
    for (size_t i = 0; i < version.size(); ++i)
        if (!(isalnum((unsigned char)version[i]) || version[i] == '.' || version[i] == '-' || version[i] == '_'))
            throw std::runtime_error("Weird version from the site: " + version);
    return version;
}

// download + unzip into vdir unless that version is already there. true when it installed something
static bool installVersion(const std::wstring& vdir, const std::string& version, const std::string& zipUrl,
                           const std::wstring& zipName, const wchar_t* exe, const wchar_t* what)
{
    std::wstring marker = vdir + L"\\installed.txt";
    if (fileExists(marker) && fileExists(vdir + L"\\" + exe))
        return false;

    // unzip next to it and rename when complete, so a crash or cancel never leaves a half install behind
    std::wstring zip = installDir() + L"\\" + toW(version) + zipName;
    std::wstring partial = vdir + L".partial";
    try
    {
        showCancel(true);
        setProgress(0);
        httpDownload(zipUrl, zip, what);
        setProgress(0);
        deleteTree(partial);
        createDirs(partial);
        unzip(zip, partial);
        showCancel(false);
        DeleteFileW(zip.c_str());
        if (!fileExists(partial + L"\\" + exe)) throw std::runtime_error("The zip did not contain " + toA(exe));

        HANDLE done = CreateFileW((partial + L"\\installed.txt").c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (done != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            WriteFile(done, version.c_str(), (DWORD)version.size(), &written, NULL);
            CloseHandle(done);
        }
        deleteTree(vdir);
        if (!MoveFileW(partial.c_str(), vdir.c_str()))
            throw std::runtime_error("Could not finish installing, close NoneRev and try again");
    }
    catch (...)
    {
        showCancel(false);
        DeleteFileW(zip.c_str());
        deleteTree(partial);
        throw;
    }
    return true;
}

static void removeOtherVersions(const std::wstring& root, const std::wstring& keep)
{
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((root + L"\\*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return;
    do
    {
        std::wstring name = fd.cFileName;
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && name != L"." && name != L".." && name != keep)
            deleteTree(root + L"\\" + name);
    } while (FindNextFileW(find, &fd));
    FindClose(find);
}

static std::wstring ensureClient(const std::string& baseUrl)
{
    setStatus(L"Getting the latest NoneRev...");
    setMarquee();
    std::string version = fetchVersion(baseUrl + "/setup/version");
    std::wstring vdir = versionsDir() + L"\\" + toW(version);
    if (installVersion(vdir, version, baseUrl + "/setup/" + version + "-NoneRevPlayer.zip", L"-NoneRevPlayer.zip", kPlayerExe, L"Installing NoneRev..."))
        removeOtherVersions(versionsDir(), toW(version));
    setStatus(L"Configuring NoneRev...");
    setProgress(100);
    writeAppSettings(vdir, baseUrl);
    regSet(kRegKey, L"Version", toW(version));
    regSet(kRegKey, L"BaseUrl", toW(baseUrl));
    return vdir;
}

static std::wstring ensureStudio(const std::string& baseUrl)
{
    setStatus(L"Getting the latest NoneRev Studio...");
    setMarquee();
    std::string version = fetchVersion(baseUrl + "/setup/version?os=studio");
    std::wstring vdir = studioDir() + L"\\" + toW(version);
    // the site only links the newest studio zip, which is the version we just got
    if (installVersion(vdir, version, baseUrl + "/setup/download?os=studio", L"-NoneRevStudio.zip", kStudioExe, L"Installing NoneRev Studio..."))
        removeOtherVersions(studioDir(), toW(version));
    setStatus(L"Configuring NoneRev Studio...");
    setProgress(100);
    writeAppSettings(vdir, baseUrl);
    return vdir;
}

static void startProcess(const std::wstring& vdir, const wchar_t* exeName, const std::wstring& args)
{
    std::wstring exe = vdir + L"\\" + exeName;
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    if (!CreateProcessW(exe.c_str(), &buf[0], NULL, NULL, FALSE, 0, NULL, vdir.c_str(), &si, &pi))
        throw std::runtime_error("Could not start " + toA(exeName));
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

static void launchPlayer(const std::wstring& vdir, const std::string& baseUrl, const std::string& ticket, const std::string& placeLauncherUrl)
{
    setStatus(L"Starting NoneRev...");
    setProgress(100);
    startProcess(vdir, kPlayerExe, L"--play -a \"" + toW(baseUrl) + L"/Login/Negotiate.ashx\" -t \"" + toW(ticket) + L"\" -j \"" + toW(placeLauncherUrl) + L"\"");
}

// same arguments the old roblox studio bootstrapper passed: -ide|-build, -url + -ticket to log in, -script to open a place
static void launchStudio(const std::wstring& vdir, const std::string& baseUrl, const std::string& mode, const std::string& ticket, const std::string& script)
{
    setStatus(L"Starting NoneRev Studio...");
    setProgress(100);
    std::wstring args = mode == "build" ? L"-build" : L"-ide";
    if (!ticket.empty())
        args += L" -url \"" + toW(baseUrl) + L"/Login/Negotiate.ashx\" -ticket \"" + toW(ticket) + L"\"";
    if (!script.empty())
        args += L" -script \"" + toW(script) + L"\"";
    startProcess(vdir, kStudioExe, args);
}

// Start menu entry that runs "NoneRevLauncher.exe --studio"
static std::wstring studioShortcutPath()
{
    wchar_t path[MAX_PATH];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_PROGRAMS, NULL, 0, path))) return L"";
    return std::wstring(path) + L"\\NoneRev Studio.lnk";
}

static void createStudioShortcut(const std::wstring& launcher)
{
    std::wstring lnk = studioShortcutPath();
    if (lnk.empty()) return;
    CoInitialize(NULL);
    IShellLinkW* link = NULL;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&link)))
    {
        link->SetPath(launcher.c_str());
        link->SetArguments(L"--studio");
        link->SetIconLocation(launcher.c_str(), 0);
        link->SetDescription(L"NoneRev Studio");
        IPersistFile* file = NULL;
        if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, (void**)&file)))
        {
            file->Save(lnk.c_str(), TRUE);
            file->Release();
        }
        link->Release();
    }
    CoUninitialize();
}

// ---- Discord "Join" -------------------------------------------------------------------------
// Discord started us through the discord-<app id>: protocol because a friend's presence was
// joined and the game was not running. The join secret is not on the command line: we have to
// connect to Discord's local IPC pipe with the same app id, subscribe to ACTIVITY_JOIN, and
// Discord then sends the pending join. The secret is "<placeId>:<jobId>" (DiscordPresence.cpp
// in the client makes it), and the site's /games/start turns that into a Play on that server.

static bool discordWrite(HANDLE pipe, unsigned opcode, const std::string& json)
{
    std::string frame(8, '\0');
    unsigned length = (unsigned)json.size();
    memcpy(&frame[0], &opcode, 4);
    memcpy(&frame[4], &length, 4);
    frame += json;
    DWORD written = 0;
    return WriteFile(pipe, frame.data(), (DWORD)frame.size(), &written, NULL) && written == frame.size();
}

static bool discordRead(HANDLE pipe, unsigned& opcode, std::string& json, DWORD timeoutMs)
{
    DWORD start = GetTickCount();
    for (;;)
    {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, NULL, 0, NULL, &available, NULL)) return false;
        if (available >= 8)
        {
            unsigned char header[8];
            DWORD read = 0;
            if (!ReadFile(pipe, header, 8, &read, NULL) || read != 8) return false;
            unsigned length;
            memcpy(&opcode, header, 4);
            memcpy(&length, header + 4, 4);
            if (length > 65536) return false;
            json.assign(length, '\0');
            DWORD got = 0;
            while (got < length)
            {
                if (!ReadFile(pipe, &json[got], length - got, &read, NULL)) return false;
                got += read;
            }
            return true;
        }
        if (GetTickCount() - start >= timeoutMs) return false;
        Sleep(20);
    }
}

static std::string jsonString(const std::string& json, const char* key)
{
    std::string needle = std::string("\"") + key + "\":\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return "";
    std::string out;
    for (size_t i = pos + needle.size(); i < json.size() && json[i] != '"'; ++i)
    {
        if (json[i] == '\\' && i + 1 < json.size()) ++i;
        out += json[i];
    }
    return out;
}

static void discordJoin(const std::string& baseUrl)
{
    setStatus(L"Asking Discord which game to join...");
    setMarquee();

    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 10 && pipe == INVALID_HANDLE_VALUE; ++i)
    {
        char name[64];
        sprintf_s(name, "\\\\.\\pipe\\discord-ipc-%d", i);
        pipe = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    }
    if (pipe == INVALID_HANDLE_VALUE) throw std::runtime_error("Discord does not seem to be running");

    std::string secret;
    unsigned opcode;
    std::string json;
    bool ok = discordWrite(pipe, 0, std::string("{\"v\":1,\"client_id\":\"") + NONEREV_DISCORD_APP_ID + "\"}")
        && discordRead(pipe, opcode, json, 5000) && opcode == 1 && json.find("\"READY\"") != std::string::npos
        && discordWrite(pipe, 1, "{\"cmd\":\"SUBSCRIBE\",\"evt\":\"ACTIVITY_JOIN\",\"args\":{},\"nonce\":\"join\"}");

    // the join event arrives shortly after the subscription; everything else is answered or ignored
    DWORD deadline = GetTickCount() + 15000;
    while (ok && secret.empty() && GetTickCount() < deadline)
    {
        if (!discordRead(pipe, opcode, json, 1000)) continue;
        if (opcode == 3) discordWrite(pipe, 4, json); // ping
        else if (opcode == 2) break;                  // close
        else if (opcode == 1 && json.find("\"ACTIVITY_JOIN\"") != std::string::npos) secret = jsonString(json, "secret");
    }
    CloseHandle(pipe);

    if (!ok) throw std::runtime_error("Could not talk to Discord. Is the launcher's Discord app id right?");
    if (secret.empty()) throw std::runtime_error("Discord did not say which game to join. Try the Join button again.");

    size_t colon = secret.find(':');
    std::string placeId = secret.substr(0, colon);
    std::string jobId = colon == std::string::npos ? "" : secret.substr(colon + 1);
    for (size_t i = 0; i < placeId.size(); ++i)
        if (!isdigit((unsigned char)placeId[i])) throw std::runtime_error("Discord sent a join we do not understand");
    for (size_t i = 0; i < jobId.size(); ++i)
        if (!(isalnum((unsigned char)jobId[i]) || jobId[i] == '-' || jobId[i] == '_')) throw std::runtime_error("Discord sent a join we do not understand");

    // the site knows who we are and presses Play on that server for us
    std::string url = baseUrl + "/games/start?placeid=" + placeId + (jobId.empty() ? "" : "&jobId=" + jobId);
    ShellExecuteW(NULL, L"open", toW(url).c_str(), NULL, NULL, SW_SHOWNORMAL);
}

struct Job
{
    std::string arg;
};

// the site we download from and run things for. never taken from a link: any web page can open one
static std::string siteUrl()
{
    std::string url = NONEREV_DEFAULT_BASE_URL;
    if (url.empty()) url = toA(regGet(kRegKey, L"BaseUrl"));
    return url;
}

static void requireOurSite(const std::string& url, const std::string& site)
{
    if (_stricmp(baseOf(url).c_str(), site.c_str()) != 0)
        throw std::runtime_error("The link points at another site: " + baseOf(url));
}

static DWORD WINAPI worker(LPVOID param)
{
    Job* job = (Job*)param;
    bool stayOpen = false;
    std::string error;
    // one launcher at a time, two of them unzipping into the same folder breaks the install
    HANDLE lock = CreateMutexW(NULL, FALSE, L"Local\\NoneRevLauncherInstall");
    if (lock && WaitForSingleObject(lock, 0) == WAIT_TIMEOUT)
    {
        setStatus(L"Waiting for the other NoneRev window...");
        WaitForSingleObject(lock, INFINITE);
    }
    try
    {
        std::string arg = job->arg;
        std::string site = siteUrl();
        std::map<std::string, std::string> args;
        if (arg.find("nonerev-launcher:") == 0)
            args = parseProtocolArgs(arg);
        std::string mode = args["launchmode"];

        if (arg == "--uninstall")
        {
            setStatus(L"Removing NoneRev...");
            unregisterProtocol();
            SHDeleteKeyW(HKEY_CURRENT_USER, kUninstallKey);
            SHDeleteKeyW(HKEY_CURRENT_USER, L"Software\\NoneRev");
            DeleteFileW(studioShortcutPath().c_str());
            deleteTree(versionsDir());
            scheduleSelfDelete();
            showSuccess(L"NONEREV WAS REMOVED", L"Thanks for playing. Press OK to finish.");
            stayOpen = true;
        }
        else if (arg == "--studio")
        {
            installSelf();
            launchStudio(ensureStudio(site), site, "ide", "", "");
        }
        else if (mode == "edit" || mode == "ide" || mode == "build")
        {
            // studio runs the -script lua with high permissions
            std::string script = args["script"];
            if (!script.empty()) requireOurSite(script, site);
            installSelf();
            launchStudio(ensureStudio(site), site, mode, args["gameinfo"], script);
        }
        else if (arg.find("nonerev-launcher:") == 0)
        {
            std::string placeLauncherUrl = args["placelauncherurl"];
            std::string ticket = args["gameinfo"];
            if (placeLauncherUrl.empty()) throw std::runtime_error("The link has no placelauncherurl");
            if (ticket.empty()) throw std::runtime_error("The link has no login ticket, log in on the site and press Play again");
            requireOurSite(placeLauncherUrl, site);

            installSelf();
            std::wstring vdir = ensureClient(site);
            launchPlayer(vdir, site, ticket, placeLauncherUrl);
        }
        else if (!discordProtocol().empty() && arg.find(toA(discordProtocol())) == 0)
        {
            installSelf();
            ensureClient(site);
            discordJoin(site);
        }
        else
        {
            installSelf();
            ensureClient(site);
            showSuccess(L"NONEREV IS SUCCESSFULLY INSTALLED!", L"Just click the \"Play\" button on any game to join the action!");
            ShellExecuteW(NULL, L"open", toW(site + "/games").c_str(), NULL, NULL, SW_SHOWNORMAL);
            stayOpen = true;
        }
    }
    catch (const Cancelled&)
    {
        stayOpen = false;
    }
    catch (const std::exception& e)
    {
        error = e.what();
    }
    if (lock)
    {
        ReleaseMutex(lock);
        CloseHandle(lock);
    }
    if (!error.empty())
        MessageBoxW(g_wnd, toW(error).c_str(), kAppName, MB_OK | MB_ICONERROR);
    g_finished = true;
    if (g_wnd && !stayOpen) PostMessage(g_wnd, WM_LAUNCHER_DONE, 0, 0);
    delete job;
    return 0;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR cmdLine, int)
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc > 2 && wcscmp(argv[1], L"--remove-install") == 0)
    {
        DWORD pid = wcstoul(argv[2], NULL, 10);
        LocalFree(argv);
        finishUninstall(pid);
        return 0;
    }
    Job* job = new Job();
    if (argc > 1) job->arg = toA(argv[1]);
    LocalFree(argv);

    createWindow();
    HANDLE thread = CreateThread(NULL, 0, worker, job, 0, NULL);
    CloseHandle(thread);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
