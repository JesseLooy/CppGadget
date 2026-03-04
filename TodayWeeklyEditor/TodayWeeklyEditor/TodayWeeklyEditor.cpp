// TodoWeeklyEditor.cpp - Write tasks into weekly list.txt (split by "date + weekday")
// Features:
// - Pick any date (DateTimePicker), write tasks to that day's section
// - Keeps format: header line "YYYY-MM-DD 星期X" then tasks lines "[ ] ..."/"[x] ..."
// - Auto archive old list.txt when a new week starts:
//   moves list.txt to "<m>.<d>-<m>.<d>任务.txt" (in same folder) based on dates in file
// - UTF-8 read/write with BOM (Notepad friendly)

#include <windows.h>
#include <commctrl.h>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>

#pragma comment(lib, "comctl32.lib")

static const wchar_t* kFilePath =
L"C:\\Users\\LiuNeng\\Desktop\\todolist\\list.txt";

// UI IDs
static const int ID_DATEPICK = 1001;
static const int ID_EDIT = 1002;
static const int ID_SAVE = 1003;
static const int ID_LOAD = 1004;
static const int ID_HOTKEY_F2 = 3001;

static HWND g_hwnd = nullptr;
static HWND g_hDate = nullptr;
static HWND g_hEdit = nullptr;

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
static void ToggleShowHideEditor(HWND hwnd) {
    if (IsWindowVisible(hwnd)) {
        ShowWindow(hwnd, SW_HIDE); // 隐藏到后台
    }
    else {
        ShowWindow(hwnd, SW_SHOW);
        ShowWindow(hwnd, SW_RESTORE);
        SetForegroundWindow(hwnd);

        // 更稳地置顶一下再取消置顶（避免被挡住）
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    }
}
static inline void TrimEndCR(std::wstring& s) { if (!s.empty() && s.back() == L'\r') s.pop_back(); }
static inline bool IsAllSpace(const std::wstring& s) {
    for (wchar_t c : s) if (c != L' ' && c != L'\t') return false;
    return true;
}
static std::wstring ToLowerW(std::wstring s) { for (auto& ch : s) ch = (wchar_t)towlower(ch); return s; }
static bool ContainsNoCase(const std::wstring& hay, const std::wstring& needle) {
    return ToLowerW(hay).find(ToLowerW(needle)) != std::wstring::npos;
}
static inline bool HasAnyDigit(const std::wstring& s) {
    for (wchar_t c : s) if (c >= L'0' && c <= L'9') return true;
    return false;
}

// ---------------- Date utils ----------------
struct YMD { int y = 0, m = 0, d = 0; };

static bool IsLeap(int y) { return (y % 400 == 0) || (y % 4 == 0 && y % 100 != 0); }
static int DaysInMonth(int y, int m) {
    static int dm[] = { 0,31,28,31,30,31,30,31,31,30,31,30,31 };
    if (m == 2) return dm[m] + (IsLeap(y) ? 1 : 0);
    return dm[m];
}

// Convert YMD to a serial day count (days since 1970-01-01) for comparisons
static long long ToSerial(const YMD& t) {
    // simple civil-from-epoch style
    int y = t.y, m = t.m, d = t.d;
    // shift March to start for easier calc
    if (m <= 2) { y--; m += 12; }
    long long era = y / 400;
    long long yoe = y - era * 400;
    long long doy = (153 * (m - 3) + 2) / 5 + d - 1;
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    // 1970-01-01 is 719468 in this scheme; but we just need relative
    return era * 146097 + doe;
}
static int WeekdaySun0(const YMD& t) {
    // Zeller-like using serial: pick known reference.
    // We can use Windows: create SYSTEMTIME and use SystemTimeToFileTime + FileTimeToSystemTime? too heavy.
    // Here use serial modulo with a known mapping:
    // For 1970-01-01 (Thursday). Let's compute weekday from serial diff to 1970-01-01.
    YMD ref{ 1970,1,1 };
    long long diff = ToSerial(t) - ToSerial(ref);
    int wd = (4 + (int)(diff % 7) + 7) % 7; // Thu=4 if Sun=0
    return wd;
}
static YMD AddDays(YMD t, int delta) {
    int y = t.y, m = t.m, d = t.d;
    int dd = delta;
    if (dd > 0) {
        while (dd--) {
            d++;
            if (d > DaysInMonth(y, m)) { d = 1; m++; if (m > 12) { m = 1; y++; } }
        }
    }
    else if (dd < 0) {
        while (dd++) {
            d--;
            if (d < 1) { m--; if (m < 1) { m = 12; y--; } d = DaysInMonth(y, m); }
        }
    }
    return YMD{ y,m,d };
}
static YMD GetMondayOfWeek(const YMD& t) {
    // weekday: Sun=0..Sat=6. We want Monday start.
    int wd = WeekdaySun0(t);
    int offsetToMonday = (wd == 0) ? -6 : (1 - wd); // if Sunday, go back 6 days
    return AddDays(t, offsetToMonday);
}
static std::wstring WeekdayCN_FromYMD(const YMD& t) {
    static const wchar_t* mapCN[7] = { L"星期日", L"星期一", L"星期二", L"星期三", L"星期四", L"星期五", L"星期六" };
    return mapCN[WeekdaySun0(t)];
}
static std::wstring FormatYMD(const YMD& t) {
    wchar_t buf[32];
    swprintf_s(buf, L"%04d-%02d-%02d", t.y, t.m, t.d);
    return buf;
}
static std::wstring FormatMD_Dot(const YMD& t) {
    wchar_t buf[32];
    swprintf_s(buf, L"%d.%d", t.m, t.d); // no leading zeros like your example 3.1
    return buf;
}
static YMD TodayYMD() {
    SYSTEMTIME st{}; GetLocalTime(&st);
    return YMD{ (int)st.wYear,(int)st.wMonth,(int)st.wDay };
}

// Parse date from a header line: supports "YYYY-MM-DD", "MM-DD", "MM/DD"
static bool TryParseDateFromLine(const std::wstring& line, YMD& out) {
    // Search for patterns by scanning digits
    // 1) YYYY-MM-DD
    for (size_t i = 0; i + 9 < line.size(); ++i) {
        if (iswdigit(line[i]) && iswdigit(line[i + 1]) && iswdigit(line[i + 2]) && iswdigit(line[i + 3]) &&
            (line[i + 4] == L'-' || line[i + 4] == L'/' || line[i + 4] == L'.') &&
            iswdigit(line[i + 5]) && iswdigit(line[i + 6]) &&
            (line[i + 7] == L'-' || line[i + 7] == L'/' || line[i + 7] == L'.') &&
            iswdigit(line[i + 8]) && iswdigit(line[i + 9])) {
            int y = (line[i] - L'0') * 1000 + (line[i + 1] - L'0') * 100 + (line[i + 2] - L'0') * 10 + (line[i + 3] - L'0');
            int m = (line[i + 5] - L'0') * 10 + (line[i + 6] - L'0');
            int d = (line[i + 8] - L'0') * 10 + (line[i + 9] - L'0');
            if (m >= 1 && m <= 12 && d >= 1 && d <= 31) { out = YMD{ y,m,d }; return true; }
        }
    }
    // 2) MM-DD / MM/DD
    for (size_t i = 0; i + 4 < line.size(); ++i) {
        if (iswdigit(line[i]) && iswdigit(line[i + 1]) &&
            (line[i + 2] == L'-' || line[i + 2] == L'/' || line[i + 2] == L'.') &&
            iswdigit(line[i + 3]) && iswdigit(line[i + 4])) {
            int m = (line[i] - L'0') * 10 + (line[i + 1] - L'0');
            int d = (line[i + 3] - L'0') * 10 + (line[i + 4] - L'0');
            if (m >= 1 && m <= 12 && d >= 1 && d <= 31) {
                int y = TodayYMD().y; // assume current year
                out = YMD{ y,m,d };
                return true;
            }
        }
    }
    return false;
}

// Section header detection: contains weekday token + digits
static bool IsSectionHeader(const std::wstring& line) {
    if (line.empty() || IsAllSpace(line)) return false;
    if (!HasAnyDigit(line)) return false;

    std::wstring lower = ToLowerW(line);
    static const wchar_t* wds[] = {
        L"星期一", L"星期二", L"星期三", L"星期四", L"星期五", L"星期六", L"星期日",
        L"周一", L"周二", L"周三", L"周四", L"周五", L"周六", L"周日",
        L"mon", L"tue", L"wed", L"thu", L"fri", L"sat", L"sun",
        L"monday", L"tuesday", L"wednesday", L"thursday", L"friday", L"saturday", L"sunday"
    };
    for (auto wd : wds) {
        if (lower.find(ToLowerW(wd)) != std::wstring::npos) return true;
    }
    return false;
}

// ---------------- Load/Save lines (UTF-8 BOM) ----------------
static bool LoadAllLinesUTF8(const wchar_t* path, std::vector<std::wstring>& outLines) {
    outLines.clear();
    std::ifstream fin(path, std::ios::binary);
    if (!fin.is_open()) return false;

    std::string bytes((std::istreambuf_iterator<char>(fin)), std::istreambuf_iterator<char>());
    if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF && (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF) {
        bytes.erase(0, 3);
    }
    const UINT CODEPAGE = CP_UTF8;
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
    const UINT CODEPAGE = CP_UTF8;
    std::ofstream fout(path, std::ios::binary | std::ios::trunc);
    if (!fout.is_open()) return false;

    unsigned char bom[3] = { 0xEF,0xBB,0xBF };
    fout.write((const char*)bom, 3);

    for (const auto& wline : lines) {
        std::string u8 = WideToUtf8(wline, CODEPAGE);
        fout.write(u8.c_str(), (std::streamsize)u8.size());
        fout.write("\r\n", 2);
    }
    return true;
}

// ---------------- Archive logic ----------------
// If list.txt contains no section date in current week, but has dates in a previous week, archive it.
static std::wstring GetFolderOfPath(const std::wstring& full) {
    size_t p = full.find_last_of(L"\\/");
    if (p == std::wstring::npos) return L".";
    return full.substr(0, p);
}
static std::wstring JoinPath(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    wchar_t sep = L'\\';
    if (dir.back() == L'\\' || dir.back() == L'/') return dir + name;
    return dir + sep + name;
}

static bool FileExistsW(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES) && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static bool RotateIfNewWeek() {
    std::vector<std::wstring> lines;
    if (!LoadAllLinesUTF8(kFilePath, lines)) return false; // no file -> nothing to rotate

    // collect all parseable dates from headers
    std::vector<YMD> dates;
    for (const auto& line : lines) {
        if (!IsSectionHeader(line)) continue;
        YMD d{};
        if (TryParseDateFromLine(line, d)) dates.push_back(d);
    }
    if (dates.empty()) return false; // cannot decide -> do nothing

    // compute current week Monday..Sunday
    YMD today = TodayYMD();
    YMD curMon = GetMondayOfWeek(today);
    long long curMonS = ToSerial(curMon);
    long long curSunS = ToSerial(AddDays(curMon, 6));

    // if any date in current week => no rotate
    bool hasCurrentWeek = false;
    for (const auto& d : dates) {
        long long s = ToSerial(d);
        if (s >= curMonS && s <= curSunS) { hasCurrentWeek = true; break; }
    }
    if (hasCurrentWeek) return false;

    // otherwise archive: name by min/max date in file
    auto minmax = std::minmax_element(dates.begin(), dates.end(),
        [](const YMD& a, const YMD& b) { return ToSerial(a) < ToSerial(b); });
    YMD dMin = *minmax.first;
    YMD dMax = *minmax.second;

    std::wstring baseName = FormatMD_Dot(dMin) + L"-" + FormatMD_Dot(dMax) + L"任务.txt";

    std::wstring folder = GetFolderOfPath(kFilePath);
    std::wstring dst = JoinPath(folder, baseName);

    // avoid overwrite
    if (FileExistsW(dst)) {
        int k = 1;
        while (true) {
            std::wstring alt = FormatMD_Dot(dMin) + L"-" + FormatMD_Dot(dMax) + L"任务(" + std::to_wstring(k) + L").txt";
            dst = JoinPath(folder, alt);
            if (!FileExistsW(dst)) break;
            k++;
        }
    }

    // Move file
    if (!MoveFileW(kFilePath, dst.c_str())) {
        // If fail (e.g. file in use), try copy + delete
        if (!CopyFileW(kFilePath, dst.c_str(), TRUE)) return false;
        DeleteFileW(kFilePath);
    }
    return true;
}
static inline bool StartsWith(const std::wstring& s, const wchar_t* prefix) {
    size_t n = wcslen(prefix);
    return s.size() >= n && wcsncmp(s.c_str(), prefix, n) == 0;
}
// ---------------- Update a day's section ----------------
static std::vector<std::wstring> NormalizeTaskLinesFromEdit(const std::wstring& editText) {
    // Split by lines; keep user lines, ensure prefix [ ] / [x]
    std::vector<std::wstring> out;
    size_t start = 0;
    while (start <= editText.size()) {
        size_t pos = editText.find(L'\n', start);
        std::wstring line = (pos == std::wstring::npos) ? editText.substr(start) : editText.substr(start, pos - start);
        TrimEndCR(line);
        if (!line.empty() && !IsAllSpace(line)) {
            // If already starts with [x]/[ ] keep; else prefix [ ]
            if (StartsWith(line, L"[x] ") || StartsWith(line, L"[X] ") || StartsWith(line, L"[ ] ")) {
                // normalize [X] to [x]
                if (StartsWith(line, L"[X] ")) line[1] = L'x';
                out.push_back(line);
            }
            else {
                out.push_back(L"[ ] " + line);
            }
        }
        if (pos == std::wstring::npos) break;
        start = pos + 1;
    }
    return out;
}

static bool UpdateSectionForDate(const YMD& date, const std::vector<std::wstring>& newTaskLines) {
    std::vector<std::wstring> lines;
    LoadAllLinesUTF8(kFilePath, lines); // if not exist, lines empty

    // Optional global title: if first non-empty line exists and isn't a header, keep it.
    // We will not auto-insert a global title; if file is empty, we just start with header.
    std::wstring ymd = FormatYMD(date);
    std::wstring wd = WeekdayCN_FromYMD(date);
    std::wstring header = ymd + L" " + wd;

    // Find exact header by containing ymd (more tolerant)
    size_t headerIndex = (size_t)-1;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (IsSectionHeader(lines[i]) && ContainsNoCase(lines[i], ymd)) {
            headerIndex = i; break;
        }
    }

    if (headerIndex == (size_t)-1) {
        // append new section
        if (!lines.empty() && !(lines.back().empty() || IsAllSpace(lines.back())))
            lines.push_back(L"");
        lines.push_back(header);
        for (const auto& t : newTaskLines) lines.push_back(t);
        return SaveAllLinesUTF8(kFilePath, lines);
    }

    // replace section body until next header
    size_t start = headerIndex + 1;
    size_t end = start;
    while (end < lines.size() && !IsSectionHeader(lines[end])) end++;

    std::vector<std::wstring> updated;
    updated.reserve(lines.size() - (end - start) + newTaskLines.size());
    for (size_t i = 0; i < start; ++i) updated.push_back(lines[i]);
    for (const auto& t : newTaskLines) updated.push_back(t);
    for (size_t i = end; i < lines.size(); ++i) updated.push_back(lines[i]);

    return SaveAllLinesUTF8(kFilePath, updated);
}

// Load a day's tasks into edit box (for viewing/editing)
static std::wstring LoadSectionTextForDate(const YMD& date) {
    std::vector<std::wstring> lines;
    if (!LoadAllLinesUTF8(kFilePath, lines)) return L"";

    std::wstring ymd = FormatYMD(date);

    size_t headerIndex = (size_t)-1;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (IsSectionHeader(lines[i]) && ContainsNoCase(lines[i], ymd)) {
            headerIndex = i; break;
        }
    }
    if (headerIndex == (size_t)-1) return L"";

    size_t i = headerIndex + 1;
    std::wstring out;
    while (i < lines.size() && !IsSectionHeader(lines[i])) {
        if (!lines[i].empty() && !IsAllSpace(lines[i])) {
            out += lines[i];
            out += L"\r\n";
        }
        i++;
    }
    return out;
}

// ---------------- UI ----------------
static void Layout(HWND hwnd) {
    RECT rc{}; GetClientRect(hwnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;

    int pad = 10;
    int topBarH = 34;

    // Date picker + buttons
    int dateW = 180;
    int btnW = 110;
    int btnH = 28;

    MoveWindow(g_hDate, pad, pad, dateW, 24, TRUE);

    HWND hLoad = GetDlgItem(hwnd, ID_LOAD);
    HWND hSave = GetDlgItem(hwnd, ID_SAVE);

    MoveWindow(hLoad, pad + dateW + 10, pad, btnW, btnH, TRUE);
    MoveWindow(hSave, pad + dateW + 10 + btnW + 8, pad, btnW, btnH, TRUE);

    // Edit fills rest
    int editY = pad + topBarH;
    MoveWindow(g_hEdit, pad, editY, w - pad * 2, h - editY - pad, TRUE);
}

static YMD GetPickedDate() {
    SYSTEMTIME st{};
    DateTime_GetSystemtime(g_hDate, &st);
    return YMD{ (int)st.wYear,(int)st.wMonth,(int)st.wDay };
}

static std::wstring GetEditText(HWND hEdit) {
    int len = GetWindowTextLengthW(hEdit);
    std::wstring s(len, L'\0');
    if (len > 0) GetWindowTextW(hEdit, s.data(), len + 1);
    return s;
}

static void SetEditText(HWND hEdit, const std::wstring& s) {
    SetWindowTextW(hEdit, s.c_str());
}

static void DoLoadDay() {
    YMD d = GetPickedDate();
    std::wstring text = LoadSectionTextForDate(d);
    SetEditText(g_hEdit, text);
}

static void DoSaveDay() {
    // 先检查是否跨周需要归档
    RotateIfNewWeek();

    YMD d = GetPickedDate();
    std::wstring editText = GetEditText(g_hEdit);
    auto taskLines = NormalizeTaskLinesFromEdit(editText);

    if (!UpdateSectionForDate(d, taskLines)) {
        MessageBoxW(g_hwnd, L"写入失败：请检查路径/权限。", L"错误", MB_OK | MB_ICONERROR);
        return;
    }
    MessageBoxW(g_hwnd, L"已保存到本周 list.txt（按日期+星期分区）。", L"保存成功", MB_OK | MB_ICONINFORMATION);
}

// ---------------- WndProc ----------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_hwnd = hwnd;

        INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_DATE_CLASSES };
        InitCommonControlsEx(&icc);

        g_hDate = CreateWindowExW(0, DATETIMEPICK_CLASSW, L"",
            WS_CHILD | WS_VISIBLE | DTS_SHORTDATEFORMAT,
            0, 0, 0, 0, hwnd, (HMENU)ID_DATEPICK, GetModuleHandleW(nullptr), nullptr);

        CreateWindowExW(0, L"BUTTON", L"加载当天",
            WS_CHILD | WS_VISIBLE,
            0, 0, 0, 0, hwnd, (HMENU)ID_LOAD, GetModuleHandleW(nullptr), nullptr);

        CreateWindowExW(0, L"BUTTON", L"保存当天",
            WS_CHILD | WS_VISIBLE,
            0, 0, 0, 0, hwnd, (HMENU)ID_SAVE, GetModuleHandleW(nullptr), nullptr);

        g_hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL,
            0, 0, 0, 0, hwnd, (HMENU)ID_EDIT, GetModuleHandleW(nullptr), nullptr);

        SendMessageW(g_hEdit, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
        SendMessageW(g_hDate, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);

        // 启动时先做一次跨周归档检查（不会误删：只有确认文件不含本周日期才归档）
        RotateIfNewWeek();

        // 默认加载选中日期（今天）
        DoLoadDay();

        Layout(hwnd);
        if (!RegisterHotKey(hwnd, ID_HOTKEY_F2, MOD_NOREPEAT, VK_F2)) {
            MessageBoxW(hwnd, L"注册全局热键 F2 失败（可能被其他软件占用）", L"提示", MB_OK | MB_ICONWARNING);
        }
        return 0;
    }
    case WM_HOTKEY:
        if ((int)wParam == ID_HOTKEY_F2) {
            ToggleShowHideEditor(hwnd);
            return 0;
        }
        break;
    case WM_SIZE:
        Layout(hwnd);
        return 0;

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == ID_LOAD) {
            DoLoadDay();
            return 0;
        }
        if (id == ID_SAVE) {
            DoSaveDay();
            return 0;
        }
        return 0;
    }

    case WM_DESTROY:
        UnregisterHotKey(hwnd, ID_HOTKEY_F2);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    const wchar_t* kClassName = L"TodoWeeklyEditor";

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;

    HWND hwnd = CreateWindowExW(
        0, kClassName, L"本周任务写入器",
        style,
        CW_USEDEFAULT, CW_USEDEFAULT, 780, 560,
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