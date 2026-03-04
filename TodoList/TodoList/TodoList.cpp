// TodoWeekly.cpp - Win32 Todo List (weekly file, split by "date + weekday", show today's section only)
// Features:
// - Auto wrap + variable item height, no ellipsis
// - Checkbox (custom drawn), click to toggle
// - Persist state back to file, only updates today's section
// - Global hotkey F8 show/hide (background running when hidden)
// - Pop up on resume from sleep/hibernate
// - Fixed window size (no drag resize)
// - Content edge-to-edge (no gaps)

#include <windows.h>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>

static const wchar_t* kFilePath =
L"C:\\Users\\LiuNeng\\Desktop\\todolist\\list.txt";

static const int ID_LISTBOX = 501;
static const int ID_HOTKEY_F8 = 1001;

// Fixed window outer size
static const int WIN_W = 450;
static const int WIN_H = 500;

struct TodoItem {
    std::wstring text; // without [x]/[ ]
    bool done = false;
};

static HWND g_hwnd = nullptr;
static HWND g_hList = nullptr;
static std::wstring g_globalTitle = L"";  // optional file title
static std::vector<TodoItem> g_items;     // today's items only

// old listbox proc for subclass
static WNDPROC g_oldListProc = nullptr;

// ---------------- UTF-8 <-> UTF-16 helpers ----------------
static std::wstring Utf8ToWide(const std::string& s, UINT cp = CP_UTF8) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(cp, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring ws(len, L'\0');
    MultiByteToWideChar(cp, 0, s.c_str(), (int)s.size(), ws.data(), len);
    return ws;
}
static std::string WideToUtf8(const std::wstring& ws, UINT cp = CP_UTF8) {
    if (ws.empty()) return "";
    int len = WideCharToMultiByte(cp, 0, ws.c_str(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
    std::string s(len, '\0');
    WideCharToMultiByte(cp, 0, ws.c_str(), (int)ws.size(), s.data(), len, nullptr, nullptr);
    return s;
}

static inline void TrimEndCR(std::wstring& s) { if (!s.empty() && s.back() == L'\r') s.pop_back(); }
static inline bool IsAllSpace(const std::wstring& s) {
    for (wchar_t c : s) if (c != L' ' && c != L'\t') return false;
    return true;
}
static inline bool StartsWith(const std::wstring& s, const wchar_t* prefix) {
    size_t n = wcslen(prefix);
    return s.size() >= n && wcsncmp(s.c_str(), prefix, n) == 0;
}
static inline std::wstring ToLowerW(std::wstring s) {
    for (auto& ch : s) ch = (wchar_t)towlower(ch);
    return s;
}
static inline bool ContainsNoCase(const std::wstring& hay, const std::wstring& needle) {
    return ToLowerW(hay).find(ToLowerW(needle)) != std::wstring::npos;
}
static inline bool HasAnyDigit(const std::wstring& s) {
    for (wchar_t c : s) if (c >= L'0' && c <= L'9') return true;
    return false;
}

// ---------------- Today strings ----------------
static std::wstring GetTodayDateYMD() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buf[32];
    swprintf_s(buf, L"%04u-%02u-%02u", st.wYear, st.wMonth, st.wDay);
    return buf;
}
static std::wstring GetTodayDateMD_dash() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buf[16];
    swprintf_s(buf, L"%02u-%02u", st.wMonth, st.wDay);
    return buf;
}
static std::wstring GetTodayDateMD_slash() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buf[16];
    swprintf_s(buf, L"%02u/%02u", st.wMonth, st.wDay);
    return buf;
}
static std::wstring GetTodayWeekdayCN() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    // wDayOfWeek: 0=Sunday ... 6=Saturday
    static const wchar_t* mapCN[7] = { L"星期日", L"星期一", L"星期二", L"星期三", L"星期四", L"星期五", L"星期六" };
    return mapCN[st.wDayOfWeek];
}
static std::wstring GetTodayWeekdayCN2() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    static const wchar_t* mapCN2[7] = { L"周日", L"周一", L"周二", L"周三", L"周四", L"周五", L"周六" };
    return mapCN2[st.wDayOfWeek];
}
static std::wstring GetTodayWeekdayEN3() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    static const wchar_t* mapEN3[7] = { L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat" };
    return mapEN3[st.wDayOfWeek];
}
static std::wstring GetTodayWeekdayENFull() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    static const wchar_t* mapEN[7] = { L"Sunday", L"Monday", L"Tuesday", L"Wednesday", L"Thursday", L"Friday", L"Saturday" };
    return mapEN[st.wDayOfWeek];
}

// ---------------- Section header detection ----------------
// header line should contain weekday token + some date-like digits
static bool IsSectionHeader(const std::wstring& line) {
    if (line.empty() || IsAllSpace(line)) return false;

    // weekday tokens (CN + EN)
    static const wchar_t* wds[] = {
        L"星期一", L"星期二", L"星期三", L"星期四", L"星期五", L"星期六", L"星期日",
        L"周一", L"周二", L"周三", L"周四", L"周五", L"周六", L"周日",
        L"mon", L"tue", L"wed", L"thu", L"fri", L"sat", L"sun",
        L"monday", L"tuesday", L"wednesday", L"thursday", L"friday", L"saturday", L"sunday"
    };

    bool hasWeek = false;
    std::wstring lower = ToLowerW(line);
    for (auto wd : wds) {
        if (lower.find(ToLowerW(wd)) != std::wstring::npos) { hasWeek = true; break; }
    }
    if (!hasWeek) return false;

    // date-like: must have digits; often includes '-' or '/' or '.'
    if (!HasAnyDigit(line)) return false;
    return true;
}

// match today priority: date exact > month-day > weekday
static int MatchTodayScore(const std::wstring& headerLine) {
    std::wstring ymd = GetTodayDateYMD();
    std::wstring md1 = GetTodayDateMD_dash();
    std::wstring md2 = GetTodayDateMD_slash();

    std::wstring wdCN = GetTodayWeekdayCN();
    std::wstring wdCN2 = GetTodayWeekdayCN2();
    std::wstring wdEN3 = GetTodayWeekdayEN3();
    std::wstring wdEN = GetTodayWeekdayENFull();

    int score = 0;
    if (ContainsNoCase(headerLine, ymd)) score = (((score) > (100)) ? (score) : (100));
    if (ContainsNoCase(headerLine, md1) || ContainsNoCase(headerLine, md2)) score = (((score) > (80)) ? (score) : (80));

    if (ContainsNoCase(headerLine, wdCN) || ContainsNoCase(headerLine, wdCN2) ||
        ContainsNoCase(headerLine, wdEN3) || ContainsNoCase(headerLine, wdEN)) {
        score = (((score) > (60)) ? (score) : (60));
    }
    return score;
}

// ---------------- Parse task line ([x]/[ ] optional) ----------------
static bool ParseTaskLine(const std::wstring& line, TodoItem& out) {
    if (line.empty() || IsAllSpace(line)) return false;

    TodoItem item;
    if (StartsWith(line, L"[x] ") || StartsWith(line, L"[X] ")) {
        item.done = true;
        item.text = line.substr(4);
    }
    else if (StartsWith(line, L"[ ] ")) {
        item.done = false;
        item.text = line.substr(4);
    }
    else {
        item.done = false;
        item.text = line;
    }
    if (item.text.empty() || IsAllSpace(item.text)) return false;
    out = std::move(item);
    return true;
}

// ---------------- Load whole file lines (UTF-8) ----------------
static bool LoadAllLinesUTF8(const wchar_t* path, std::vector<std::wstring>& outLines) {
    outLines.clear();

    std::ifstream fin(path, std::ios::binary);
    if (!fin.is_open()) return false;

    std::string bytes((std::istreambuf_iterator<char>(fin)), std::istreambuf_iterator<char>());

    // UTF-8 BOM
    if (bytes.size() >= 3 &&
        (unsigned char)bytes[0] == 0xEF &&
        (unsigned char)bytes[1] == 0xBB &&
        (unsigned char)bytes[2] == 0xBF) {
        bytes.erase(0, 3);
    }

    const UINT CODEPAGE = CP_UTF8; // if GBK: change to CP_ACP or 936
    std::wstring content = Utf8ToWide(bytes, CODEPAGE);

    size_t start = 0;
    while (start <= content.size()) {
        size_t pos = content.find(L'\n', start);
        std::wstring line = (pos == std::wstring::npos) ? content.substr(start) : content.substr(start, pos - start);
        TrimEndCR(line);
        outLines.push_back(line);
        if (pos == std::wstring::npos) break;
        start = pos + 1;
    }
    return true;
}

static bool SaveAllLinesUTF8(const wchar_t* path, const std::vector<std::wstring>& lines) {
    const UINT CODEPAGE = CP_UTF8; // keep consistent
    std::ofstream fout(path, std::ios::binary | std::ios::trunc);
    if (!fout.is_open()) return false;

    // UTF-8 BOM for Notepad
    unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
    fout.write((const char*)bom, 3);

    for (size_t i = 0; i < lines.size(); ++i) {
        std::string u8 = WideToUtf8(lines[i], CODEPAGE);
        fout.write(u8.c_str(), (std::streamsize)u8.size());
        fout.write("\r\n", 2);
    }
    return true;
}

// ---------------- Load today's section from weekly file ----------------
// returns: window title and today's items; also outputs the chosen header line (for saving back)
static bool LoadTodayFromWeeklyFile(const wchar_t* path,
    std::wstring& outWindowTitle,
    std::wstring& outChosenHeaderLine,
    std::vector<TodoItem>& outItems) {
    outItems.clear();
    outChosenHeaderLine.clear();

    std::vector<std::wstring> lines;
    if (!LoadAllLinesUTF8(path, lines)) return false;

    // optional global title: first non-empty line that is NOT a section header
    std::wstring globalTitle;
    size_t idx = 0;
    while (idx < lines.size() && (lines[idx].empty() || IsAllSpace(lines[idx]))) idx++;
    if (idx < lines.size() && !IsSectionHeader(lines[idx])) {
        globalTitle = lines[idx];
        idx++;
    }

    // find best matching header
    int bestScore = 0;
    size_t bestHeaderIndex = (size_t)-1;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (!IsSectionHeader(lines[i])) continue;
        int score = MatchTodayScore(lines[i]);
        if (score > bestScore) {
            bestScore = score;
            bestHeaderIndex = i;
        }
    }

    // If no section found, show empty for today
    std::wstring todayYMD = GetTodayDateYMD();
    std::wstring todayWD = GetTodayWeekdayCN();
    if (bestHeaderIndex == (size_t)-1) {
        outWindowTitle = (!globalTitle.empty() ? globalTitle + L" - " : L"") + todayYMD + L" " + todayWD;
        outChosenHeaderLine = todayYMD + L" " + todayWD; // used when saving (append new section)
        return true;
    }

    outChosenHeaderLine = lines[bestHeaderIndex];

    // collect tasks until next header
    for (size_t i = bestHeaderIndex + 1; i < lines.size(); ++i) {
        if (IsSectionHeader(lines[i])) break;
        TodoItem item;
        if (ParseTaskLine(lines[i], item)) outItems.push_back(std::move(item));
    }

    // window title
    if (!globalTitle.empty())
        outWindowTitle = globalTitle + L" - " + outChosenHeaderLine;
    else
        outWindowTitle = outChosenHeaderLine;

    return true;
}

// ---------------- Save today's section back (only today's section) ----------------
static bool SaveTodayToWeeklyFile(const wchar_t* path,
    const std::wstring& chosenHeaderLine,
    const std::vector<TodoItem>& items) {
    std::vector<std::wstring> lines;
    LoadAllLinesUTF8(path, lines); // if failed, lines empty -> create new

    // Build today's task lines
    std::vector<std::wstring> newTaskLines;
    newTaskLines.reserve(items.size());
    for (const auto& it : items) {
        std::wstring line = it.done ? L"[x] " : L"[ ] ";
        line += it.text;
        newTaskLines.push_back(std::move(line));
    }

    // Find the section by exact header match first (ignore case)
    size_t headerIndex = (size_t)-1;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (ToLowerW(lines[i]) == ToLowerW(chosenHeaderLine)) {
            headerIndex = i;
            break;
        }
    }

    // If not found exact, try find the best match again (same rule as load) but must be "today-like"
    if (headerIndex == (size_t)-1) {
        int bestScore = 0;
        size_t best = (size_t)-1;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (!IsSectionHeader(lines[i])) continue;
            int score = MatchTodayScore(lines[i]);
            if (score > bestScore) { bestScore = score; best = i; }
        }
        // accept if it has at least weekday match
        if (bestScore >= 60) headerIndex = best;
    }

    if (headerIndex == (size_t)-1) {
        // Append a new section at end
        if (!lines.empty() && !(lines.back().empty() || IsAllSpace(lines.back())))
            lines.push_back(L""); // blank line between sections
        lines.push_back(chosenHeaderLine);
        for (auto& t : newTaskLines) lines.push_back(t);
        return SaveAllLinesUTF8(path, lines);
    }

    // Determine section end (next header or end)
    size_t start = headerIndex + 1;
    size_t end = start;
    while (end < lines.size() && !IsSectionHeader(lines[end])) end++;

    // Replace lines[start, end) with newTaskLines
    std::vector<std::wstring> updated;
    updated.reserve(lines.size() - (end - start) + newTaskLines.size());

    for (size_t i = 0; i < start; ++i) updated.push_back(lines[i]);
    for (auto& t : newTaskLines) updated.push_back(t);
    for (size_t i = end; i < lines.size(); ++i) updated.push_back(lines[i]);

    return SaveAllLinesUTF8(path, updated);
}

// ---------------- UI: listbox fill/resize ----------------
static void ResizeControls(HWND hwnd) {
    if (!g_hList) return;
    RECT rc{};
    GetClientRect(hwnd, &rc);
    MoveWindow(g_hList, 0, 0, rc.right - rc.left, rc.bottom - rc.top, TRUE);
}

static void FillListBox(HWND hList) {
    SendMessageW(hList, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < (int)g_items.size(); ++i) {
        int idx = (int)SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)g_items[i].text.c_str());
        SendMessageW(hList, LB_SETITEMDATA, idx, (LPARAM)i);
    }
    InvalidateRect(hList, nullptr, TRUE);
}
// 重新读取“当天分区”的任务，并刷新窗口标题 + 列表显示
// 依赖：LoadTodayFromWeeklyFile / g_todayHeader / g_items / g_hList / FillListBox / ResizeControls
static void ReloadTodayAndRefresh(HWND hwnd) {
    if (!hwnd) return;

    std::wstring winTitle;
    std::wstring hdr;
    std::vector<TodoItem> items;

    // 重新从 weekly 格式文件里读取“今天”分区
    if (!LoadTodayFromWeeklyFile(kFilePath, winTitle, hdr, items)) {
        // 读取失败就不破坏现有界面（也可以选择弹窗提示）
        return;
    }

    // 更新全局数据
    auto g_todayHeader = std::move(hdr);
    g_items = std::move(items);

    // 更新窗口标题
    SetWindowTextW(hwnd, winTitle.c_str());

    // 刷新列表内容
    if (g_hList) {
        FillListBox(g_hList);
        ResizeControls(hwnd);      // 贴边/尺寸调整
        InvalidateRect(g_hList, nullptr, TRUE); // 强制重绘一次
    }
}
static void ToggleShowHide(HWND hwnd) {
    if (IsWindowVisible(hwnd)) {
        ShowWindow(hwnd, SW_HIDE); // 后台运行
    }
    else {
        // ★ 现形前先重新读取文件
        ReloadTodayAndRefresh(hwnd);

        ShowWindow(hwnd, SW_SHOW);
        ShowWindow(hwnd, SW_RESTORE);
        SetForegroundWindow(hwnd);

        // 可选：更稳的前置显示
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    }
}

// ---------------- Owner-draw variable height ----------------
static int CalcTextHeight(HDC hdc, HFONT hFont, const std::wstring& text, int width) {
    HFONT old = (HFONT)SelectObject(hdc, hFont);
    RECT r{ 0,0,width,0 };
    DrawTextW(hdc, text.c_str(), (int)text.size(), &r, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
    SelectObject(hdc, old);
    return (((0) > (r.bottom - r.top)) ? (0) : (r.bottom - r.top));
}

// ---------------- ListBox subclass: handle click checkbox ----------------
static LRESULT CALLBACK ListBoxProc(HWND hList, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_SETFOCUS) {
        // 不让 listbox 获得焦点，避免出现焦点项虚线等状态
        if (g_hwnd) SetFocus(g_hwnd);
        return 0;
    }
    if (msg == WM_LBUTTONDOWN) {
        POINT pt{ (short)LOWORD(lParam), (short)HIWORD(lParam) }; // listbox client coords

        LRESULT hit = SendMessageW(hList, LB_ITEMFROMPOINT, 0, MAKELPARAM(pt.x, pt.y));
        int lbIndex = LOWORD(hit);
        BOOL outOfRange = HIWORD(hit);
        if (!outOfRange && lbIndex >= 0) {
            int dataIndex = (int)SendMessageW(hList, LB_GETITEMDATA, lbIndex, 0);
            if (dataIndex >= 0 && dataIndex < (int)g_items.size()) {
                RECT itemRc{};
                SendMessageW(hList, LB_GETITEMRECT, lbIndex, (LPARAM)&itemRc);

                int cb = GetSystemMetrics(SM_CXMENUCHECK);
                int padding = 10; // slightly larger for nicer centering
                RECT cbRc{
                    itemRc.left + padding,
                    itemRc.top + ((itemRc.bottom - itemRc.top) - cb) / 2,
                    itemRc.left + padding + cb,
                    itemRc.top + ((itemRc.bottom - itemRc.top) - cb) / 2 + cb
                };

                if (pt.x >= cbRc.left && pt.x <= cbRc.right && pt.y >= cbRc.top && pt.y <= cbRc.bottom) {
                    g_items[dataIndex].done = !g_items[dataIndex].done;

                    // Save only today's section back to weekly file
                    // We store current window title header part in GWLP_USERDATA? We'll keep it global via window text caching.
                    // Here we use the main window title after last load: "Global - Header" or "Header".
                    // Better: store chosenHeader globally (below).
                    // We'll set that global in WndProc on load.
                    // (see g_todayHeader)
                    extern std::wstring g_todayHeader;
                    SaveTodayToWeeklyFile(kFilePath, g_todayHeader, g_items);

                    InvalidateRect(hList, &itemRc, TRUE);
                    return 0;
                }
            }
        }
    }
    return CallWindowProcW(g_oldListProc, hList, msg, wParam, lParam);
}

// Global chosen header line for today (used for saving back)
std::wstring g_todayHeader;

// ---------------- Main Window Proc ----------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_hwnd = hwnd;

        std::wstring winTitle;
        if (!LoadTodayFromWeeklyFile(kFilePath, winTitle, g_todayHeader, g_items)) {
            std::wstring todayYMD = GetTodayDateYMD();
            std::wstring todayWD = GetTodayWeekdayCN();
            g_todayHeader = todayYMD + L" " + todayWD;
            winTitle = g_todayHeader;
            g_items.clear();
            MessageBoxW(hwnd, L"无法读取周任务文件，将以空列表启动。", L"提示", MB_OK | MB_ICONWARNING);
        }
        SetWindowTextW(hwnd, winTitle.c_str());

        g_hList = CreateWindowExW(
            0,
            L"LISTBOX",
            L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOSEL |
            LBS_OWNERDRAWVARIABLE | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
            0, 0, 0, 0,
            hwnd,
            (HMENU)ID_LISTBOX,
            GetModuleHandleW(nullptr),
            nullptr
        );
        SendMessageW(g_hList, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);

        // subclass listbox for click handling
        g_oldListProc = (WNDPROC)SetWindowLongPtrW(g_hList, GWLP_WNDPROC, (LONG_PTR)ListBoxProc);

        FillListBox(g_hList);
        ResizeControls(hwnd);

        if (!RegisterHotKey(hwnd, ID_HOTKEY_F8, MOD_NOREPEAT, VK_F8)) {
            MessageBoxW(hwnd, L"注册全局热键 F8 失败（可能被占用）", L"提示", MB_OK | MB_ICONWARNING);
        }
        return 0;
    }

    case WM_SIZE:
        ResizeControls(hwnd);
        return 0;

    case WM_GETMINMAXINFO: {
        auto* mmi = (MINMAXINFO*)lParam;
        mmi->ptMinTrackSize.x = WIN_W;
        mmi->ptMinTrackSize.y = WIN_H;
        mmi->ptMaxTrackSize.x = WIN_W;
        mmi->ptMaxTrackSize.y = WIN_H;
        return 0;
    }

    case WM_HOTKEY:
        if ((int)wParam == ID_HOTKEY_F8) ToggleShowHide(hwnd);
        return 0;

    case WM_POWERBROADCAST:
        if (wParam == PBT_APMRESUMESUSPEND || wParam == PBT_APMRESUMEAUTOMATIC) {
            ShowWindow(hwnd, SW_SHOW);
            ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
            SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        }
        return TRUE;

    case WM_KEYDOWN:
        // F5 reload today's section from file
        if (wParam == VK_F5) {
            std::wstring winTitle;
            std::vector<TodoItem> items;
            std::wstring hdr;
            if (LoadTodayFromWeeklyFile(kFilePath, winTitle, hdr, items)) {
                g_todayHeader = hdr;
                g_items = std::move(items);
                SetWindowTextW(hwnd, winTitle.c_str());
                FillListBox(g_hList);
                ResizeControls(hwnd);
            }
        }
        return 0;

    case WM_MEASUREITEM: {
        auto* mi = (MEASUREITEMSTRUCT*)lParam;
        if (mi->CtlID != ID_LISTBOX) break;
        if ((int)mi->itemID < 0) { mi->itemHeight = 28; return TRUE; }

        RECT rc{};
        GetClientRect(g_hList, &rc);
        int width = rc.right - rc.left;

        int cb = GetSystemMetrics(SM_CXMENUCHECK);
        int padding = 10;
        int textLeft = cb + padding * 2;
        int textWidth = (((50) > (width - textLeft - padding)) ? (50) : (width - textLeft - padding));

        int dataIndex = (int)SendMessageW(g_hList, LB_GETITEMDATA, mi->itemID, 0);
        std::wstring text = (dataIndex >= 0 && dataIndex < (int)g_items.size()) ? g_items[dataIndex].text : L"";

        HDC hdc = GetDC(g_hList);
        HFONT hFont = (HFONT)SendMessageW(g_hList, WM_GETFONT, 0, 0);

        int textH = CalcTextHeight(hdc, hFont, text, textWidth);

        ReleaseDC(g_hList, hdc);

        int itemH = (((cb) > (textH)) ? (cb) : (textH));
        mi->itemHeight = (((32) > (itemH)) ? (32) : (itemH)); // minimum nicer height
        return TRUE;
    }

    case WM_DRAWITEM: {
        auto* di = (DRAWITEMSTRUCT*)lParam;
        if (di->CtlID != ID_LISTBOX) break;
        if ((int)di->itemID < 0) return TRUE;

        int lbIndex = (int)di->itemID;
        int dataIndex = (int)SendMessageW(g_hList, LB_GETITEMDATA, lbIndex, 0);
        if (dataIndex < 0 || dataIndex >= (int)g_items.size()) return TRUE;

        const TodoItem& item = g_items[dataIndex];
        HDC hdc = di->hDC;
        RECT rc = di->rcItem;



        FillRect(hdc, &rc, GetSysColorBrush(COLOR_WINDOW));

        int cb = GetSystemMetrics(SM_CXMENUCHECK);
        int padding = 10;

        // checkbox rect (vertically centered)
        RECT cbRc{
            rc.left + padding,
            rc.top + ((rc.bottom - rc.top) - cb) / 2,
            rc.left + padding + cb,
            rc.top + ((rc.bottom - rc.top) - cb) / 2 + cb
        };
        UINT state = DFCS_BUTTONCHECK | (item.done ? DFCS_CHECKED : 0);
        DrawFrameControl(hdc, &cbRc, DFC_BUTTON, state);

        // available text rect
        RECT trAvail{
            cbRc.right + padding,
            rc.top + padding,
            rc.right - padding,
            rc.bottom - padding
        };

        // text color
        SetBkMode(hdc, TRANSPARENT);

        SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));

        HFONT hFont = (HFONT)SendMessageW(g_hList, WM_GETFONT, 0, 0);
        HFONT old = (HFONT)SelectObject(hdc, hFont);

        // ---- vertical centering for multi-line block ----
        RECT trCalc = trAvail;
        trCalc.top = 0; trCalc.bottom = 0;
        DrawTextW(hdc, item.text.c_str(), (int)item.text.size(), &trCalc,
            DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);

        int textH = trCalc.bottom - trCalc.top;
        int availH = trAvail.bottom - trAvail.top;
        int offsetY = (availH > textH) ? (availH - textH) / 2 : 0;

        RECT tr = trAvail;
        tr.top += offsetY;
        tr.bottom = tr.top + textH;

        // draw (wrap, no ellipsis)
        DrawTextW(hdc, item.text.c_str(), (int)item.text.size(), &tr, DT_WORDBREAK | DT_NOPREFIX);

        SelectObject(hdc, old);

        return TRUE;
    }

    case WM_DESTROY:
        UnregisterHotKey(hwnd, ID_HOTKEY_F8);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    const wchar_t* kClassName = L"TodoWeekly_OneColumn_Wrap";

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);

    // fixed size, non-resizable: no WS_THICKFRAME, no WS_MAXIMIZEBOX
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX
        | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;

    HWND hwnd = CreateWindowExW(
        0,
        kClassName,
        L"Todo",
        style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        WIN_W, WIN_H,
        nullptr, nullptr, hInst, nullptr
    );
    if (!hwnd) return 0;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}