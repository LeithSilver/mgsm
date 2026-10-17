// First-run setup window (Windows: GDI, drag and drop, file dialog). The install itself is installer.cpp (portable).
#include "installer.h"

#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace fs = std::filesystem;

namespace {
fs::path ExeDir(const std::string& exe_dir) { return fs::u8path(exe_dir); }
}

// ---- the setup window ---------------------------------------------------------------------------------------
namespace {
constexpr UINT WM_APP_PROGRESS = WM_APP + 40, WM_APP_DONE = WM_APP + 41;
constexpr int ID_BROWSE = 101, ID_ROM = 102;

struct SetupState {
    std::string exe_dir;
    fs::path rom;               // a package found in ROM\ next to the exe
    std::mutex mutex;
    std::string status, error;
    float fraction = 0.0f;
    std::atomic<bool> busy{false};
    bool done = false;
    HWND hwnd = nullptr, browse = nullptr, rom_button = nullptr;
    HFONT title_font = nullptr, body_font = nullptr, small_font = nullptr;
    float scale = 1.0f;
    std::thread worker;
};
SetupState* g_setup = nullptr;

const COLORREF kBg = RGB(16, 18, 20), kText = RGB(222, 226, 222), kDim = RGB(140, 150, 140), kAccent = RGB(98, 160, 92),
               kError = RGB(230, 110, 100), kPanel = RGB(28, 32, 34);

int S(int v) { return static_cast<int>(v * g_setup->scale + 0.5f); }

void Start(const fs::path& package) {
    SetupState& st = *g_setup;
    if (st.busy) return;
    if (st.worker.joinable()) st.worker.join();
    st.busy = true;
    { std::lock_guard<std::mutex> l(st.mutex); st.error.clear(); st.status = "Starting..."; st.fraction = 0.0f; }
    EnableWindow(st.browse, FALSE);
    if (st.rom_button) EnableWindow(st.rom_button, FALSE);
    InvalidateRect(st.hwnd, nullptr, FALSE);
    st.worker = std::thread([package] {
        SetupState& s = *g_setup;
        std::string err;
        const bool ok = Installer::Install(package, s.exe_dir, [](float f, const std::string& msg) {
            { std::lock_guard<std::mutex> l(g_setup->mutex); g_setup->fraction = f; g_setup->status = msg; }
            PostMessage(g_setup->hwnd, WM_APP_PROGRESS, 0, 0);
        }, &err);
        { std::lock_guard<std::mutex> l(s.mutex); if (!ok) s.error = err; }
        PostMessage(s.hwnd, WM_APP_DONE, ok ? 1 : 0, 0);
    });
}

void Browse(HWND hwnd) {
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"N-Gage game package (*.n-gage)\0*.n-gage\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Select Metal.Gear.Solid.n-gage";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn)) Start(file);
}

void Text(HDC dc, const std::wstring& s, RECT r, HFONT font, COLORREF c, UINT flags) {
    SelectObject(dc, font);
    SetTextColor(dc, c);
    ::DrawTextW(dc, s.c_str(), -1, &r, flags);
}

std::wstring Wide(const std::string& s) {
    std::wstring w(s.size(), L'\0');
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), static_cast<int>(w.size()));
    w.resize(n > 0 ? static_cast<size_t>(n) : 0);
    return w;
}

void Paint(HWND hwnd) {
    SetupState& st = *g_setup;
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(hwnd, &ps);
    RECT cr; GetClientRect(hwnd, &cr);
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, cr.right, cr.bottom);
    HGDIOBJ old_bmp = SelectObject(dc, bmp);
    HBRUSH bg = CreateSolidBrush(kBg);
    FillRect(dc, &cr, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);
    const int m = S(32), w = cr.right;
    Text(dc, L"METAL GEAR SOLID MOBILE", {m, S(28), w - m, S(70)}, st.title_font, kText, DT_LEFT | DT_SINGLELINE);
    Text(dc, L"First-time setup", {m, S(68), w - m, S(92)}, st.body_font, kAccent, DT_LEFT | DT_SINGLELINE);
    Text(dc, L"This port runs the game from your own copy of Metal Gear Solid Mobile for N-Gage 2.0. "
                 L"Drop Metal.Gear.Solid.n-gage onto this window, or choose it with Browse. The game files are "
                 L"extracted into the game and textures folders next to the program; your file isn't changed.",
             {m, S(104), w - m, S(200)}, st.body_font, kText, DT_LEFT | DT_WORDBREAK);

    // drop zone
    RECT zone = {m, S(208), w - m, S(276)};
    HBRUSH panel = CreateSolidBrush(kPanel);
    FillRect(dc, &zone, panel);
    DeleteObject(panel);
    HPEN pen = CreatePen(PS_DOT, 1, kDim);
    HGDIOBJ old_pen = SelectObject(dc, pen);
    HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, zone.left, zone.top, zone.right, zone.bottom);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(pen);
    Text(dc, st.busy ? L"Installing..." : L"Drop Metal.Gear.Solid.n-gage here", zone, st.body_font, kDim, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    std::string status, error; float fraction;
    { std::lock_guard<std::mutex> l(st.mutex); status = st.status; error = st.error; fraction = st.fraction; }
    const int py = cr.bottom - S(58);
    if (st.busy || st.done) {
        RECT bar = {m, py, w - m, py + S(10)};
        HBRUSH b1 = CreateSolidBrush(kPanel);
        FillRect(dc, &bar, b1);
        DeleteObject(b1);
        RECT fill = bar;
        fill.right = bar.left + static_cast<int>((bar.right - bar.left) * (std::min)(1.0f, (std::max)(0.0f, fraction)));
        HBRUSH b2 = CreateSolidBrush(kAccent);
        FillRect(dc, &fill, b2);
        DeleteObject(b2);
    }
    if (!error.empty()) Text(dc, Wide(error), {m, py - S(4), w - m, cr.bottom - S(8)}, st.small_font, kError, DT_LEFT | DT_WORDBREAK);
    else if (!status.empty()) Text(dc, Wide(status), {m, py + S(16), w - m, cr.bottom - S(8)}, st.small_font, kDim, DT_LEFT | DT_SINGLELINE);

    BitBlt(wdc, 0, 0, cr.right, cr.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old_bmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

void DrawButton(const DRAWITEMSTRUCT* di) {
    const bool primary = di->CtlID == ID_ROM || !g_setup->rom_button;
    const bool disabled = (di->itemState & ODS_DISABLED) != 0, pressed = (di->itemState & ODS_SELECTED) != 0;
    HBRUSH bg = CreateSolidBrush(kBg);
    FillRect(di->hDC, &di->rcItem, bg);
    DeleteObject(bg);
    COLORREF fill = primary ? kAccent : kPanel;
    if (disabled) fill = RGB(40, 44, 46);
    else if (pressed) fill = primary ? RGB(78, 135, 72) : RGB(44, 50, 52);
    HBRUSH b = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, primary && !disabled ? fill : RGB(70, 78, 74));
    HGDIOBJ ob = SelectObject(di->hDC, b), op = SelectObject(di->hDC, pen);
    RoundRect(di->hDC, di->rcItem.left, di->rcItem.top, di->rcItem.right, di->rcItem.bottom, S(8), S(8));
    SelectObject(di->hDC, ob); SelectObject(di->hDC, op);
    DeleteObject(b); DeleteObject(pen);
    wchar_t text[256];
    GetWindowTextW(di->hwndItem, text, 256);
    SetBkMode(di->hDC, TRANSPARENT);
    SetTextColor(di->hDC, disabled ? kDim : (primary ? RGB(12, 16, 12) : kText));
    SelectObject(di->hDC, g_setup->body_font);
    RECT r = di->rcItem;
    ::DrawTextW(di->hDC, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

LRESULT CALLBACK SetupProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SetupState* st = g_setup;
    switch (msg) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: Paint(hwnd); return 0;
        case WM_DRAWITEM: DrawButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lp)); return TRUE;
        case WM_COMMAND:
            if (LOWORD(wp) == ID_BROWSE) Browse(hwnd);
            else if (LOWORD(wp) == ID_ROM) Start(st->rom);
            return 0;
        case WM_DROPFILES: {
            HDROP drop = reinterpret_cast<HDROP>(wp);
            wchar_t file[MAX_PATH] = {};
            if (DragQueryFileW(drop, 0, file, MAX_PATH)) Start(file);
            DragFinish(drop);
            return 0;
        }
        case WM_APP_PROGRESS: InvalidateRect(hwnd, nullptr, FALSE); return 0;
        case WM_APP_DONE:
            st->busy = false;
            if (wp) { st->done = true; InvalidateRect(hwnd, nullptr, FALSE); UpdateWindow(hwnd); Sleep(400); DestroyWindow(hwnd); return 0; }
            EnableWindow(st->browse, TRUE);
            if (st->rom_button) EnableWindow(st->rom_button, TRUE);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_CLOSE:
            if (st->busy) return 0;   // let the install finish (it can't be interrupted safely)
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
}

bool Installer::RunSetupWindow(const std::string& exe_dir) {
    SetupState st;
    g_setup = &st;
    st.exe_dir = exe_dir;
    {   // a package already in ROM\ (or next to the exe): the first *.n-gage, whatever its name (Install checks it)
        std::error_code ec;
        for (const fs::path& dir : {ExeDir(exe_dir) / "ROM", ExeDir(exe_dir)}) {
            for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
                std::wstring ext = it->path().extension().wstring();
                for (auto& c : ext) c = towlower(c);
                if (ext == L".n-gage" && it->is_regular_file(ec)) { st.rom = it->path(); break; }
            }
            if (!st.rom.empty()) break;
        }
    }

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = SetupProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"MGSMobileSetup";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0));
    wc.hIconSm = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    RegisterClassExW(&wc);

    HDC screen = GetDC(nullptr);
    st.scale = GetDeviceCaps(screen, LOGPIXELSY) / 96.0f;
    ReleaseDC(nullptr, screen);
    auto font = [&](int px, int weight) {
        return CreateFontW(-S(px), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    };
    st.title_font = font(30, FW_BOLD);
    st.body_font = font(16, FW_NORMAL);
    st.small_font = font(14, FW_NORMAL);

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT wr = {0, 0, S(640), S(420)};
    AdjustWindowRect(&wr, style, FALSE);
    const int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
    st.hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"Metal Gear Solid Mobile - Setup", style,
                              (GetSystemMetrics(SM_CXSCREEN) - ww) / 2, (GetSystemMetrics(SM_CYSCREEN) - wh) / 2, ww, wh,
                              nullptr, nullptr, inst, nullptr);
    if (!st.hwnd) return false;
    const int by = S(292), bh = S(36);
    st.browse = CreateWindowW(L"BUTTON", L"Browse...", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, S(32), by, S(140), bh,
                              st.hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_BROWSE)), inst, nullptr);
    if (!st.rom.empty()) {
        const std::wstring label = L"Install from " + fs::relative(st.rom, ExeDir(exe_dir)).wstring();
        st.rom_button = CreateWindowW(L"BUTTON", label.c_str(), WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, S(184), by, S(424), bh,
                                      st.hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_ROM)), inst, nullptr);
    }
    DragAcceptFiles(st.hwnd, TRUE);
    ShowWindow(st.hwnd, SW_SHOW);
    UpdateWindow(st.hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    if (st.worker.joinable()) st.worker.join();
    DeleteObject(st.title_font); DeleteObject(st.body_font); DeleteObject(st.small_font);
    UnregisterClassW(wc.lpszClassName, inst);
    g_setup = nullptr;
    return st.done;
}
