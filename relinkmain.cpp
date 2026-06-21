#define WIN32_LEAN_AND_MEAN
// clang-format off
#include "Aup2Parser.h"
#include <windows.h>
// clang-format on
#include <Uxtheme.h>
#include <algorithm>
#include <chrono>
#include <commctrl.h>
#include <commdlg.h>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <shellapi.h>
#include <shobjidl.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

constexpr wchar_t APP_TITLE_BASE[] = L"aviutl2_relink v0.0.7";
constexpr wchar_t COPY_RULES_FILE[] = L"aviutl2_relink.copy.ini";

constexpr int32_t ID_BTN_OPEN = 101;
constexpr int32_t ID_EDIT_FILEPATH = 102;
constexpr int32_t ID_LIST = 103;
constexpr int32_t ID_BTN_CHECK = 104;
constexpr int32_t ID_BTN_EDITSEL = 105;
constexpr int32_t ID_BTN_SAVE = 106;
constexpr int32_t ID_BTN_REPLACEROOT = 107;
constexpr int32_t ID_BTN_COPYFILES = 108;
constexpr int32_t ID_BTN_CHECKEXPORT = 109;
constexpr int32_t ID_BTN_RELINK_PROJECT_FILES = 110;
constexpr int32_t ID_EDIT_FILTER = 111;
constexpr int32_t ID_CHECK_NGONLY = 112;
constexpr int32_t ID_STATIC_FILTER = 113;
constexpr int32_t COL_STATUS = 0;
constexpr int32_t COL_KEY = 1;
constexpr int32_t COL_PATH = 2;
constexpr uint32_t WM_COPYFILES_DONE = WM_APP + 1;
constexpr uint32_t WM_CHECK_DONE = WM_APP + 2;
constexpr uint32_t WM_ROOTSCAN_DONE = WM_APP + 3;
constexpr uint32_t WM_COPYPLAN_DONE = WM_APP + 4;

constexpr int32_t ID_SEL_LIST = 1001;

enum class CopyAction { Copy,
                        Skip,
                        Ask };
enum class PreserveTreeMode { Flat,
                              Drive,
                              CommonRoot };
enum class UpdatePathsMode { Ask,
                             Yes,
                             No };
enum class OverwriteMode { Ask,
                           Yes,
                           No };

struct CopyRule {
    CopyAction action = CopyAction::Ask;
    std::wstring pattern;
    std::wstring reason;
};

struct CopySettings {
    CopyAction defaultAction = CopyAction::Ask;
    PreserveTreeMode preserveTree = PreserveTreeMode::Drive;
    UpdatePathsMode updatePathsAfterCopy = UpdatePathsMode::Ask;
    OverwriteMode overwrite = OverwriteMode::Ask;
    UpdatePathsMode exportLog = UpdatePathsMode::Ask;
    UpdatePathsMode exportResult = UpdatePathsMode::Ask;
    UpdatePathsMode saveAfterUpdate = UpdatePathsMode::Ask;
    std::wstring projectSideFolder = L"{project_name}_files";
    std::vector<CopyRule> rules;
};

struct CopyCandidate {
    std::filesystem::path source;
    CopyAction action = CopyAction::Ask;
    std::wstring reason;
    bool exists = false;
};

struct CopyPlanItem {
    std::filesystem::path source;
    std::filesystem::path dest;
    CopyAction action = CopyAction::Ask;
    std::wstring reason;
    std::wstring result;
    std::wstring detail;
    bool exists = false;
    bool destExists = false;
};

struct CopyPlan {
    std::filesystem::path destRoot;
    std::vector<CopyPlanItem> items;
};

struct CopyPlanScanContext {
    CopySettings settings;
    CopyPlan plan;
};

struct CopyExecutionResult {
    std::filesystem::path destRoot;
    std::vector<CopyPlanItem> records;
    std::map<std::wstring, std::filesystem::path> copiedPaths;
    int32_t copied = 0;
    int32_t skippedExisting = 0;
    int32_t failed = 0;
    int32_t missing = 0;
    int32_t excluded = 0;
    int32_t skippedAsk = 0;
    bool updatePaths = false;
    UpdatePathsMode exportLog = UpdatePathsMode::No;
    UpdatePathsMode exportResult = UpdatePathsMode::No;
    UpdatePathsMode saveAfterUpdate = UpdatePathsMode::No;
};

struct RelinkPlanItem {
    PathEntry* entry = nullptr;
    std::filesystem::path newPath;
};

struct DetectedRootMapping {
    std::filesystem::path oldRoot;
    std::filesystem::path newRoot;
    int32_t matchCount = 0;
    int32_t totalEntries = 0;
};

struct SelectionDialogState {
    const std::vector<DetectedRootMapping>* mappings = nullptr;
    int32_t result = -1;
    HWND hList = nullptr;
};

static HWND g_hWnd = nullptr;
static HWND g_hList = nullptr;
static Aup2Document g_doc;
static bool g_loaded = false;
static bool g_dirty = false;
static bool g_copyInProgress = false;
static bool g_scanInProgress = false;
static bool g_checkExportPending = false;
static std::vector<CheckResult> g_checkResults;
static std::unordered_map<const PathEntry*, bool> g_checkExistsByEntry;
static std::vector<size_t> g_visibleEntries;
static int32_t g_dpi = USER_DEFAULT_SCREEN_DPI;
static HFONT g_hFont = nullptr;

static bool IsBusy() {
    return g_copyInProgress || g_scanInProgress;
}

static int32_t ScaleForDpi(int32_t value, int32_t dpi) {
    return MulDiv(value, dpi, USER_DEFAULT_SCREEN_DPI);
}

static HFONT CreateUiFont(int32_t dpi) {
    NONCLIENTMETRICS ncm{};
    ncm.cbSize = sizeof(ncm);
    if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm,
                                    0, dpi)) {
        if (!SystemParametersInfo(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
            return nullptr;
    }
    return CreateFontIndirect(&ncm.lfMessageFont);
}

static BOOL CALLBACK SetFontProc(HWND hwnd, LPARAM lParam) {
    SendMessage(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(reinterpret_cast<HFONT>(lParam)), TRUE);
    return TRUE;
}

static void ApplyFontToChildren() {
    if (!g_hFont)
        return;
    SendMessage(g_hWnd, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFont), TRUE);
    EnumChildWindows(g_hWnd, SetFontProc, reinterpret_cast<LPARAM>(g_hFont));
}

static int32_t TaskMsgBox(HWND owner, const std::wstring& text, const std::wstring& title, UINT mbFlags) {
    std::wstring mainInstruction;
    std::wstring content = text;
    size_t splitPos = text.find(L"\n\n");
    if (splitPos != std::wstring::npos) {
        mainInstruction = text.substr(0, splitPos);
        content = text.substr(splitPos + 2);
    }

    TASKDIALOGCONFIG cfg{};
    cfg.cbSize = sizeof(cfg);
    cfg.hwndParent = owner;
    cfg.dwFlags = TDF_SIZE_TO_CONTENT | TDF_ALLOW_DIALOG_CANCELLATION;
    cfg.pszWindowTitle = title.c_str();
    cfg.pszMainInstruction = mainInstruction.empty() ? nullptr : mainInstruction.c_str();
    cfg.pszContent = content.c_str();

    switch (mbFlags & 0xF0) {
        case MB_ICONERROR:
            cfg.pszMainIcon = TD_ERROR_ICON;
            break;
        case MB_ICONWARNING:
            cfg.pszMainIcon = TD_WARNING_ICON;
            break;
        case MB_ICONQUESTION:
        case MB_ICONINFORMATION:
            cfg.pszMainIcon = TD_INFORMATION_ICON;
            break;
        default:
            break;
    }

    int32_t defaultButton = IDOK;
    switch (mbFlags & 0x0F) {
        case MB_YESNO:
            cfg.dwCommonButtons = TDCBF_YES_BUTTON | TDCBF_NO_BUTTON;
            defaultButton = IDYES;
            break;
        case MB_YESNOCANCEL:
            cfg.dwCommonButtons =
                TDCBF_YES_BUTTON | TDCBF_NO_BUTTON | TDCBF_CANCEL_BUTTON;
            defaultButton = IDYES;
            break;
        case MB_OKCANCEL:
            cfg.dwCommonButtons = TDCBF_OK_BUTTON | TDCBF_CANCEL_BUTTON;
            defaultButton = IDOK;
            break;
        case MB_OK:
        default:
            cfg.dwCommonButtons = TDCBF_OK_BUTTON;
            defaultButton = IDOK;
            break;
    }

    switch (mbFlags & 0xF00) {
        case MB_DEFBUTTON2:
            if (cfg.dwCommonButtons & TDCBF_NO_BUTTON)
                defaultButton = IDNO;
            else if (cfg.dwCommonButtons & TDCBF_CANCEL_BUTTON)
                defaultButton = IDCANCEL;
            break;
        case MB_DEFBUTTON3:
            if (cfg.dwCommonButtons & TDCBF_CANCEL_BUTTON)
                defaultButton = IDCANCEL;
            break;
        default:
            break;
    }
    cfg.nDefaultButton = defaultButton;

    int32_t pressedButton = IDCANCEL;
    HRESULT hr = TaskDialogIndirect(&cfg, &pressedButton, nullptr, nullptr);
    if (FAILED(hr))
        return TaskMsgBox(owner, text.c_str(), title.c_str(), mbFlags);

    return pressedButton;
}

static std::wstring ToWide(const std::string& s) {
    if (s.empty())
        return {};

    int32_t n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                                    static_cast<int32_t>(s.size()), nullptr, 0);
    if (n <= 0)
        throw std::runtime_error("Failed to convert UTF-8 string to UTF-16.");

    std::wstring w(n, L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                            static_cast<int32_t>(s.size()), w.data(), n) != n) {
        throw std::runtime_error("Failed to convert UTF-8 string to UTF-16.");
    }
    return w;
}

static std::string ToUtf8(const std::wstring& w) {
    if (w.empty())
        return {};

    int32_t n =
        WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int32_t>(w.size()),
                            nullptr, 0, nullptr, nullptr);
    if (n <= 0)
        throw std::runtime_error("Failed to convert UTF-16 string to UTF-8.");

    std::string s(n, '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int32_t>(w.size()),
                            s.data(), n, nullptr, nullptr) != n) {
        throw std::runtime_error("Failed to convert UTF-16 string to UTF-8.");
    }
    return s;
}

static std::filesystem::path
NormalizePathForCompare(const std::filesystem::path& path) {
    std::error_code ec;
    auto normalized = path.lexically_normal();

    auto weak = std::filesystem::weakly_canonical(normalized, ec);
    if (!ec)
        normalized = weak;

    return normalized;
}

static bool PathsReferToSameLocation(const std::filesystem::path& lhs,
                                     const std::filesystem::path& rhs) {
    auto left = NormalizePathForCompare(lhs).wstring();
    auto right = NormalizePathForCompare(rhs).wstring();
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) ==
           CSTR_EQUAL;
}

static HMENU ControlId(int32_t id) {
    return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id));
}

static void UpdateWindowTitle() {
    std::wstring title = APP_TITLE_BASE;
    if (g_loaded) {
        title += L" - ";
        title += g_doc.sourcePath.filename().wstring();
    }
    if (g_copyInProgress)
        title += L" - コピー中";
    else if (g_scanInProgress)
        title += L" - 確認中";
    if (g_dirty)
        title += L" *";
    SetWindowText(g_hWnd, title.c_str());
}

static void UpdateControlStates() {
    bool busy = IsBusy();
    EnableWindow(GetDlgItem(g_hWnd, ID_BTN_OPEN), !busy);
    EnableWindow(GetDlgItem(g_hWnd, ID_BTN_CHECK), g_loaded && !busy);
    EnableWindow(GetDlgItem(g_hWnd, ID_BTN_CHECKEXPORT), g_loaded && !busy);
    EnableWindow(GetDlgItem(g_hWnd, ID_BTN_EDITSEL), g_loaded && !busy);
    EnableWindow(GetDlgItem(g_hWnd, ID_BTN_REPLACEROOT), g_loaded && !busy);
    EnableWindow(GetDlgItem(g_hWnd, ID_BTN_RELINK_PROJECT_FILES), g_loaded && !busy);
    EnableWindow(GetDlgItem(g_hWnd, ID_BTN_COPYFILES), g_loaded && !busy);
    EnableWindow(GetDlgItem(g_hWnd, ID_BTN_SAVE), g_loaded && g_dirty && !busy);
    EnableWindow(GetDlgItem(g_hWnd, ID_EDIT_FILTER), g_loaded && !busy);
    EnableWindow(GetDlgItem(g_hWnd, ID_CHECK_NGONLY), g_loaded && !busy);
}

static void SetDirty(bool dirty) {
    g_dirty = dirty;
    UpdateWindowTitle();
    UpdateControlStates();
}

static std::wstring EnsureTrailingSeparator(std::wstring path) {
    if (path.empty())
        return path;
    if (path.back() != L'\\' && path.back() != L'/')
        path.push_back(L'\\');
    return path;
}

static bool StartsWithPathPrefix(const std::wstring& value,
                                 const std::wstring& prefix) {
    if (value.size() < prefix.size())
        return false;
    return CompareStringOrdinal(
               value.c_str(), static_cast<int32_t>(prefix.size()), prefix.c_str(),
               static_cast<int32_t>(prefix.size()), TRUE) == CSTR_EQUAL;
}

static std::wstring Trim(const std::wstring& s) {
    size_t first = s.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos)
        return {};
    size_t last = s.find_last_not_of(L" \t\r\n");
    return s.substr(first, last - first + 1);
}

static std::wstring ToLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
    return s;
}

static std::wstring NormalizeSlashes(std::wstring s) {
    std::replace(s.begin(), s.end(), L'\\', L'/');
    return s;
}

static bool EqualsIgnoreCase(const std::wstring& lhs, const std::wstring& rhs) {
    return CompareStringOrdinal(lhs.c_str(), -1, rhs.c_str(), -1, TRUE) ==
           CSTR_EQUAL;
}

static std::optional<CopyAction> ParseCopyAction(const std::wstring& value) {
    auto v = ToLower(Trim(value));
    if (v == L"copy")
        return CopyAction::Copy;
    if (v == L"skip")
        return CopyAction::Skip;
    if (v == L"ask")
        return CopyAction::Ask;
    return std::nullopt;
}

static std::optional<PreserveTreeMode>
ParsePreserveTreeMode(const std::wstring& value) {
    auto v = ToLower(Trim(value));
    if (v == L"flat")
        return PreserveTreeMode::Flat;
    if (v == L"drive")
        return PreserveTreeMode::Drive;
    if (v == L"common_root")
        return PreserveTreeMode::CommonRoot;
    return std::nullopt;
}

static std::optional<UpdatePathsMode>
ParseUpdatePathsMode(const std::wstring& value) {
    auto v = ToLower(Trim(value));
    if (v == L"ask")
        return UpdatePathsMode::Ask;
    if (v == L"yes" || v == L"true" || v == L"1")
        return UpdatePathsMode::Yes;
    if (v == L"no" || v == L"false" || v == L"0")
        return UpdatePathsMode::No;
    return std::nullopt;
}

static std::optional<OverwriteMode>
ParseOverwriteMode(const std::wstring& value) {
    auto v = ToLower(Trim(value));
    if (v == L"ask")
        return OverwriteMode::Ask;
    if (v == L"yes" || v == L"true" || v == L"1")
        return OverwriteMode::Yes;
    if (v == L"no" || v == L"false" || v == L"0")
        return OverwriteMode::No;
    return std::nullopt;
}

static bool WildcardMatch(const wchar_t* pattern, const wchar_t* text) {
    while (*pattern) {
        if (*pattern == L'*') {
            int32_t starCount = 0;
            while (*pattern == L'*') {
                ++starCount;
                ++pattern;
            }

            bool crossDirectories = starCount >= 2;
            if (!*pattern) {
                if (crossDirectories)
                    return true;
                while (*text) {
                    if (*text == L'/')
                        return false;
                    ++text;
                }
                return true;
            }

            const wchar_t* scan = text;
            for (;;) {
                if (WildcardMatch(pattern, scan))
                    return true;
                if (!*scan || (!crossDirectories && *scan == L'/'))
                    return false;
                ++scan;
            }
        }

        if (!*text)
            return false;

        if (*pattern == L'?') {
            if (*text == L'/')
                return false;
        } else if (towlower(*pattern) != towlower(*text)) {
            return false;
        }

        ++pattern;
        ++text;
    }

    return *text == L'\0';
}

static bool PatternMatchesPath(const std::wstring& pattern,
                               const std::filesystem::path& path) {
    std::wstring normalizedPattern = NormalizeSlashes(Trim(pattern));
    if (normalizedPattern.empty())
        return false;

    std::wstring full = NormalizeSlashes(path.lexically_normal().wstring());
    std::wstring filename = NormalizeSlashes(path.filename().wstring());
    bool pathPattern = normalizedPattern.find(L'/') != std::wstring::npos ||
                       normalizedPattern.find(L':') != std::wstring::npos;

    const std::wstring& target = pathPattern ? full : filename;
    return WildcardMatch(normalizedPattern.c_str(), target.c_str());
}

static std::wstring GetControlText(HWND hwnd) {
    int32_t len = GetWindowTextLength(hwnd);
    if (len <= 0)
        return {};

    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    GetWindowText(hwnd, text.data(), len + 1);
    text.resize(static_cast<size_t>(len));
    return text;
}

static void SetCheckResults(std::vector<CheckResult> results) {
    g_checkResults = std::move(results);
    g_checkExistsByEntry.clear();
    g_checkExistsByEntry.reserve(g_checkResults.size());
    for (const auto& r : g_checkResults)
        g_checkExistsByEntry.emplace(r.entry, r.exists);
}

static void ClearCheckResults() {
    g_checkResults.clear();
    g_checkExistsByEntry.clear();
}

static bool IsEntryMissing(const PathEntry& entry) {
    auto it = g_checkExistsByEntry.find(&entry);
    if (it == g_checkExistsByEntry.end())
        return false;
    return !it->second;
}

static bool EntryMatchesCurrentFilter(const PathEntry& entry) {
    HWND ngOnly = GetDlgItem(g_hWnd, ID_CHECK_NGONLY);
    if (ngOnly && SendMessage(ngOnly, BM_GETCHECK, 0, 0) == BST_CHECKED &&
        !IsEntryMissing(entry)) {
        return false;
    }

    HWND filterEdit = GetDlgItem(g_hWnd, ID_EDIT_FILTER);
    std::wstring filter = ToLower(Trim(GetControlText(filterEdit)));
    if (filter.empty())
        return true;

    std::wstring key = ToLower(ToWide(entry.key));
    std::wstring path = ToLower(ToWide(entry.path));
    return key.find(filter) != std::wstring::npos ||
           path.find(filter) != std::wstring::npos;
}

static PathEntry* EntryFromVisibleRow(int32_t row) {
    if (row < 0 || row >= static_cast<int32_t>(g_visibleEntries.size()))
        return nullptr;

    size_t entryIndex = g_visibleEntries[static_cast<size_t>(row)];
    if (entryIndex >= g_doc.entries.size())
        return nullptr;
    return &g_doc.entries[entryIndex];
}

static std::filesystem::path GetExeDirectory() {
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;) {
        DWORD n =
            GetModuleFileName(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0)
            return std::filesystem::current_path();
        if (n < buf.size() - 1)
            return std::filesystem::path(buf.data()).parent_path();
        buf.resize(buf.size() * 2);
    }
}

static std::wstring ExpandProjectFolderTemplate(std::wstring value) {
    std::wstring projectName = g_doc.sourcePath.stem().wstring();
    size_t pos = 0;
    while ((pos = value.find(L"{project_name}", pos)) != std::wstring::npos) {
        value.replace(pos, 14, projectName);
        pos += projectName.size();
    }
    return value;
}

static bool WriteDefaultCopyRules(const std::filesystem::path& path) {
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs)
        return false;

    ofs << "# aviutl2_relink copy rules\n"
           "# Lines are evaluated from top to bottom. Actions: copy, skip, ask.\n"
           "# Pattern without a slash matches file name. Pattern with slash "
           "matches full path.\n"
           "\n"
           "default_action=ask\n"
           "preserve_tree=drive\n"
           "project_side_folder={project_name}_files\n"
           "update_paths_after_copy=ask\n"
           "overwrite=ask\n"
           "export_log=ask\n"
           "export_result=ask\n"
           "save_after_update=ask\n"
           "\n"
           "skip=*.dll|binary/plugin\n"
           "skip=*.exe|executable\n"
           "skip=*.bat|script\n"
           "skip=*.cmd|script\n"
           "skip=*.ps1|script\n"
           "skip=*.auf|AviUtl plugin\n"
           "skip=*.aui|AviUtl plugin\n"
           "skip=*.auo|AviUtl plugin\n"
           "skip=*.auf2|AviUtl2 plugin\n"
           "skip=*.aui2|AviUtl2 plugin\n"
           "skip=*.auo2|AviUtl2 plugin\n"
           "skip=*.aux2|AviUtl2 plugin\n"
           "skip=C:/Program Files/**|installed application/plugin area\n"
           "skip=C:/Program Files (x86)/**|installed application/plugin area\n"
           "\n"
           "ask=*.lua|script\n"
           "ask=*.js|script\n"
           "ask=*.vbs|script\n"
           "ask=*.json|settings or data\n"
           "ask=*.ini|settings\n"
           "ask=*.cfg|settings\n"
           "ask=*.ttf|font\n"
           "ask=*.otf|font\n"
           "\n"
           "copy=*.mp4\n"
           "copy=*.mov\n"
           "copy=*.avi\n"
           "copy=*.mkv\n"
           "copy=*.webm\n"
           "copy=*.wav\n"
           "copy=*.mp3\n"
           "copy=*.flac\n"
           "copy=*.ogg\n"
           "copy=*.m4a\n"
           "copy=*.png\n"
           "copy=*.jpg\n"
           "copy=*.jpeg\n"
           "copy=*.webp\n"
           "copy=*.bmp\n"
           "copy=*.gif\n"
           "copy=*.tif\n"
           "copy=*.tiff\n"
           "copy=*.psd\n"
           "copy=*.txt\n"
           "copy=*.srt\n"
           "copy=*.ass\n";
    return ofs.good();
}

static CopySettings LoadCopySettings() {
    CopySettings settings;
    auto configPath = GetExeDirectory() / COPY_RULES_FILE;

    if (!std::filesystem::exists(configPath)) {
        if (WriteDefaultCopyRules(configPath)) {
            std::wstring msg = std::wstring(COPY_RULES_FILE) +
                               L" を作成しました。\n必要に応じて編集できます。\n\n  " +
                               configPath.wstring();
            TaskMsgBox(g_hWnd, msg, L"コピー設定", MB_ICONINFORMATION);
        } else {
            std::wstring msg =
                L"設定ファイルの作成に失敗しました。既定の動作で続行します。\n\n  " +
                configPath.wstring();
            TaskMsgBox(g_hWnd, msg, L"コピー設定", MB_ICONWARNING);
        }
    }

    std::ifstream ifs(configPath, std::ios::binary);
    if (!ifs)
        return settings;

    std::string lineUtf8;
    while (std::getline(ifs, lineUtf8)) {
        std::wstring line;
        try {
            line = ToWide(lineUtf8);
        } catch (...) {
            continue;
        }
        line = Trim(line);
        if (line.empty() || line.front() == L'#' || line.front() == L';')
            continue;

        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos)
            continue;

        std::wstring key = ToLower(Trim(line.substr(0, eq)));
        std::wstring value = Trim(line.substr(eq + 1));

        if (key == L"default_action") {
            if (auto parsed = ParseCopyAction(value))
                settings.defaultAction = *parsed;
        } else if (key == L"preserve_tree") {
            if (auto parsed = ParsePreserveTreeMode(value))
                settings.preserveTree = *parsed;
        } else if (key == L"project_side_folder") {
            if (!value.empty())
                settings.projectSideFolder = value;
        } else if (key == L"update_paths_after_copy") {
            if (auto parsed = ParseUpdatePathsMode(value))
                settings.updatePathsAfterCopy = *parsed;
        } else if (key == L"overwrite") {
            if (auto parsed = ParseOverwriteMode(value))
                settings.overwrite = *parsed;
        } else if (key == L"export_log") {
            if (auto parsed = ParseUpdatePathsMode(value))
                settings.exportLog = *parsed;
        } else if (key == L"export_result") {
            if (auto parsed = ParseUpdatePathsMode(value))
                settings.exportResult = *parsed;
        } else if (key == L"save_after_update") {
            if (auto parsed = ParseUpdatePathsMode(value))
                settings.saveAfterUpdate = *parsed;
        } else if (key == L"copy" || key == L"skip" || key == L"ask") {
            CopyRule rule;
            rule.action = *ParseCopyAction(key);
            size_t pipe = value.find(L'|');
            rule.pattern = Trim(value.substr(0, pipe));
            if (pipe != std::wstring::npos)
                rule.reason = Trim(value.substr(pipe + 1));
            if (!rule.pattern.empty())
                settings.rules.push_back(std::move(rule));
        }
    }

    return settings;
}

static CopyCandidate ClassifyCopyCandidate(const std::filesystem::path& source,
                                           const CopySettings& settings) {
    CopyCandidate candidate;
    candidate.source = source;
    candidate.action = settings.defaultAction;

    std::error_code ec;
    candidate.exists = std::filesystem::is_regular_file(source, ec);
    if (ec)
        candidate.exists = false;

    for (const auto& rule : settings.rules) {
        if (!PatternMatchesPath(rule.pattern, source))
            continue;
        candidate.action = rule.action;
        candidate.reason = rule.reason;
        break;
    }

    return candidate;
}

static std::wstring SanitizePathPart(std::wstring value) {
    for (auto& ch : value) {
        if (ch == L':' || ch == L'<' || ch == L'>' || ch == L'"' || ch == L'|' ||
            ch == L'?' || ch == L'*')
            ch = L'_';
    }
    return value;
}

static std::filesystem::path
RelativePathForCopy(const std::filesystem::path& source, PreserveTreeMode mode,
                    const std::filesystem::path& commonRoot) {
    if (mode == PreserveTreeMode::Flat)
        return source.filename();

    if (mode == PreserveTreeMode::CommonRoot && !commonRoot.empty()) {
        std::error_code ec;
        auto rel = std::filesystem::relative(source, commonRoot, ec);
        if (!ec && !rel.empty() && rel.native().find(L"..") != 0)
            return rel;
    }

    std::filesystem::path result;
    std::wstring root = source.root_name().wstring();
    if (!root.empty())
        result /= SanitizePathPart(root);

    auto relative = source.relative_path();
    if (!relative.empty())
        result /= relative;
    else
        result /= source.filename();

    return result;
}

static std::optional<std::filesystem::path>
RelativePathAfterFolder(const std::filesystem::path& path,
                        const std::wstring& folderName) {
    if (folderName.empty())
        return std::nullopt;

    auto normalized = path.lexically_normal();
    bool found = false;
    std::filesystem::path relative;

    for (const auto& part : normalized) {
        if (EqualsIgnoreCase(part.wstring(), folderName)) {
            found = true;
            relative.clear();
            continue;
        }

        if (found) {
            relative /= part;
            continue;
        }
    }

    if (!found || relative.empty())
        return std::nullopt;
    return relative;
}

static std::filesystem::path
ComputeCommonRoot(const std::vector<CopyCandidate>& candidates) {
    std::filesystem::path common;
    for (const auto& candidate : candidates) {
        if (!candidate.exists || candidate.action == CopyAction::Skip)
            continue;

        auto parent = candidate.source.parent_path();
        if (common.empty()) {
            common = parent;
            continue;
        }

        std::vector<std::filesystem::path> left;
        std::vector<std::filesystem::path> right;
        for (const auto& part : common)
            left.push_back(part);
        for (const auto& part : parent)
            right.push_back(part);

        std::filesystem::path next;
        size_t count = (std::min)(left.size(), right.size());
        for (size_t i = 0; i < count; ++i) {
            if (!EqualsIgnoreCase(left[i].wstring(), right[i].wstring()))
                break;
            next /= left[i];
        }
        common = next;
    }
    return common;
}

static std::vector<CopyCandidate>
BuildCopyCandidates(const CopySettings& settings) {
    std::vector<CopyCandidate> candidates;
    std::vector<std::wstring> seen;

    for (const auto& entry : g_doc.entries) {
        if (entry.isProjectFile)
            continue;

        std::filesystem::path source =
            NormalizePathForCompare(PathFromUtf8(entry.path));
        std::wstring sourceText = source.wstring();
        bool duplicate = false;
        for (const auto& item : seen) {
            if (EqualsIgnoreCase(item, sourceText)) {
                duplicate = true;
                break;
            }
        }
        if (duplicate)
            continue;

        seen.push_back(sourceText);
        candidates.push_back(ClassifyCopyCandidate(source, settings));
    }

    return candidates;
}

static CopyPlan BuildCopyPlan(const CopySettings& settings,
                              const std::filesystem::path& destRoot) {
    CopyPlan plan;
    plan.destRoot = destRoot;
    auto candidates = BuildCopyCandidates(settings);
    auto commonRoot = ComputeCommonRoot(candidates);

    std::map<std::wstring, int32_t> destCounts;
    for (const auto& candidate : candidates) {
        CopyPlanItem item;
        item.source = candidate.source;
        item.action = candidate.action;
        item.reason = candidate.reason;
        item.exists = candidate.exists;

        auto relative = RelativePathForCopy(candidate.source, settings.preserveTree,
                                            commonRoot);
        item.dest = destRoot / relative;

        std::wstring destKey = ToLower(item.dest.lexically_normal().wstring());
        int32_t& count = destCounts[destKey];
        if (count > 0) {
            auto parent = item.dest.parent_path();
            auto stem = item.dest.stem().wstring();
            auto ext = item.dest.extension().wstring();
            item.dest = parent / std::format(L"{} ({}){}", stem, count + 1, ext);
        }
        ++count;

        std::error_code ec;
        item.destExists = std::filesystem::exists(item.dest, ec);
        plan.items.push_back(std::move(item));
    }

    return plan;
}

static std::wstring DescribeFirstItems(const CopyPlan& plan, CopyAction action,
                                       int32_t maxItems) {
    std::wstring text;
    int32_t count = 0;
    for (const auto& item : plan.items) {
        if (item.action != action)
            continue;
        if (count >= maxItems)
            break;
        text += L"\n  ";
        text += item.source.filename().wstring();
        if (!item.reason.empty()) {
            text += L" (";
            text += item.reason;
            text += L")";
        }
        ++count;
    }
    return text;
}

static std::wstring CopyActionName(CopyAction action) {
    switch (action) {
        case CopyAction::Copy:
            return L"copy";
        case CopyAction::Skip:
            return L"skip";
        case CopyAction::Ask:
            return L"ask";
    }
    return L"unknown";
}

static bool ResolveAskSetting(UpdatePathsMode mode, const std::wstring& message,
                              const std::wstring& title) {
    if (mode == UpdatePathsMode::Yes)
        return true;
    if (mode == UpdatePathsMode::No)
        return false;
    return TaskMsgBox(g_hWnd, message.c_str(), title.c_str(),
                      MB_YESNO | MB_ICONQUESTION) == IDYES;
}

static std::wstring MakeTimestamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_s(&local, &t);
    return std::format(L"{:04}{:02}{:02}_{:02}{:02}{:02}", local.tm_year + 1900,
                       local.tm_mon + 1, local.tm_mday, local.tm_hour,
                       local.tm_min, local.tm_sec);
}

static std::string CsvCell(const std::wstring& value) {
    std::string utf8 = ToUtf8(value);
    if (!utf8.empty()) {
        char c = utf8.front();
        if (c == '=' || c == '+' || c == '-' || c == '@')
            utf8.insert(utf8.begin(), '\'');
    }
    bool quote = utf8.find_first_of(",\"\r\n") != std::string::npos;
    if (!quote)
        return utf8;

    std::string out = "\"";
    for (char ch : utf8) {
        if (ch == '"')
            out += "\"\"";
        else
            out += ch;
    }
    out += "\"";
    return out;
}

static bool WriteTextUtf8(const std::filesystem::path& path,
                          const std::wstring& text) {
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs)
        return false;
    ofs << "\xEF\xBB\xBF";
    ofs << ToUtf8(text);
    return ofs.good();
}

static bool WriteCopyLog(const CopyExecutionResult& result,
                         const std::filesystem::path& path, int32_t updated,
                         bool saved) {
    std::wstring text;
    text += L"aviutl2_relink copy log\n";
    text += L"timestamp: " + MakeTimestamp() + L"\n";
    text += L"project: " + g_doc.sourcePath.wstring() + L"\n";
    text += L"destination: " + result.destRoot.wstring() + L"\n\n";
    text += std::format(L"copied: {}\nexisting skipped: {}\nfailed: {}\n"
                        L"missing: {}\nexcluded: {}\nask skipped: {}\n"
                        L"path updated: {}\nauto saved: {}\n\n",
                        result.copied, result.skippedExisting, result.failed,
                        result.missing, result.excluded, result.skippedAsk,
                        updated, saved ? L"yes" : L"no");

    for (const auto& item : result.records) {
        text += L"[";
        text += item.result;
        text += L"] ";
        text += item.source.wstring();
        if (!item.dest.empty()) {
            text += L" -> ";
            text += item.dest.wstring();
        }
        text += L" action=" + CopyActionName(item.action);
        if (!item.reason.empty())
            text += L" reason=" + item.reason;
        if (!item.detail.empty())
            text += L" detail=" + item.detail;
        text += L"\n";
    }

    return WriteTextUtf8(path, text);
}

static bool WriteCopyResultCsv(const CopyExecutionResult& result,
                               const std::filesystem::path& path) {
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs)
        return false;

    ofs << "\xEF\xBB\xBF";
    ofs << "result,action,source,destination,reason,detail\r\n";
    for (const auto& item : result.records) {
        ofs << CsvCell(item.result) << ',' << CsvCell(CopyActionName(item.action))
            << ',' << CsvCell(item.source.wstring()) << ','
            << CsvCell(item.dest.wstring()) << ',' << CsvCell(item.reason) << ','
            << CsvCell(item.detail) << "\r\n";
    }
    return ofs.good();
}

static bool WriteCheckResultCsv(const Aup2Document& doc,
                                const std::vector<CheckResult>& results,
                                const std::filesystem::path& path) {
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs)
        return false;

    ofs << "\xEF\xBB\xBF";
    ofs << "status,key,path,is_project_file,project\r\n";
    for (size_t i = 0; i < doc.entries.size(); ++i) {
        const auto& entry = doc.entries[i];
        bool exists = false;
        if (i < results.size())
            exists = results[i].exists;

        ofs << CsvCell(exists ? L"OK" : L"NG") << ',' << CsvCell(ToWide(entry.key))
            << ',' << CsvCell(ToWide(entry.path)) << ','
            << CsvCell(entry.isProjectFile ? L"yes" : L"no") << ','
            << CsvCell(doc.sourcePath.wstring()) << "\r\n";
    }
    return ofs.good();
}

static std::optional<std::filesystem::path>
PickFolder(const std::wstring& title,
           const std::filesystem::path& initialFolder = {}) {
    IFileDialog* dialog = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(hr))
        return std::nullopt;

    DWORD options = 0;
    hr = dialog->GetOptions(&options);
    if (SUCCEEDED(hr)) {
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                           FOS_PATHMUSTEXIST);
    }
    dialog->SetTitle(title.c_str());

    if (!initialFolder.empty()) {
        IShellItem* folderItem = nullptr;
        hr = SHCreateItemFromParsingName(initialFolder.c_str(), nullptr,
                                         IID_PPV_ARGS(&folderItem));
        if (SUCCEEDED(hr)) {
            dialog->SetFolder(folderItem);
            folderItem->Release();
        }
    }

    hr = dialog->Show(g_hWnd);
    if (FAILED(hr)) {
        dialog->Release();
        return std::nullopt;
    }

    IShellItem* resultItem = nullptr;
    hr = dialog->GetResult(&resultItem);
    dialog->Release();
    if (FAILED(hr))
        return std::nullopt;

    PWSTR displayName = nullptr;
    hr = resultItem->GetDisplayName(SIGDN_FILESYSPATH, &displayName);
    resultItem->Release();
    if (FAILED(hr))
        return std::nullopt;

    std::filesystem::path selected(displayName);
    CoTaskMemFree(displayName);
    return selected;
}

static std::optional<std::filesystem::path>
PickSavePath(const std::wstring& title, const std::wstring& filter,
             const std::filesystem::path& initialFolder,
             const std::wstring& defaultFileName, const wchar_t* defaultExt) {
    std::vector<WCHAR> buf(32768, L'\0');
    if (defaultFileName.size() >= buf.size())
        return std::nullopt;
    wcscpy_s(buf.data(), buf.size(), defaultFileName.c_str());

    OPENFILENAME ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hWnd;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = static_cast<DWORD>(buf.size());
    ofn.lpstrDefExt = defaultExt;
    std::wstring initialFolderText = initialFolder.wstring();
    if (!initialFolderText.empty())
        ofn.lpstrInitialDir = initialFolderText.c_str();
    ofn.lpstrTitle = title.c_str();
    ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
    if (!GetSaveFileName(&ofn))
        return std::nullopt;
    return std::filesystem::path(buf.data());
}

static bool SaveCurrentDocument() {
    if (!g_loaded)
        return true;
    if (g_doc.loadedWriteTime) {
        std::error_code ec;
        auto currentWriteTime =
            std::filesystem::last_write_time(g_doc.sourcePath, ec);
        if (!ec && currentWriteTime != *g_doc.loadedWriteTime) {
            std::wstring msg =
                L"読み込み後にプロジェクトファイルが外部で更新されています。\n\n  " +
                g_doc.sourcePath.wstring() +
                L"\n\nこのまま保存すると、外部で更新された内容を上書きする可能性があ"
                L"ります。\n"
                L"保存を続行しますか？";
            if (TaskMsgBox(g_hWnd, msg.c_str(), L"外部更新の検出",
                           MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
                return false;
            }
        }
    }
    if (!SaveAup2(g_doc, g_doc.sourcePath)) {
        TaskMsgBox(g_hWnd, L"保存に失敗しました。", L"エラー", MB_ICONERROR);
        return false;
    }

    SetDirty(false);
    TaskMsgBox(g_hWnd, L"保存しました。(バックアップ: .aup2.bak)", L"完了",
               MB_ICONINFORMATION);
    return true;
}

static bool ConfirmSaveIfDirty() {
    if (!g_loaded || !g_dirty)
        return true;

    int32_t result = TaskMsgBox(g_hWnd, L"未保存の変更があります。保存しますか？",
                                L"確認", MB_YESNOCANCEL | MB_ICONWARNING);
    if (result == IDCANCEL)
        return false;
    if (result == IDYES)
        return SaveCurrentDocument();
    return true;
}

static void ListViewPopulate() {
    ListView_DeleteAllItems(g_hList);
    g_visibleEntries.clear();

    for (size_t i = 0; i < g_doc.entries.size(); ++i) {
        const auto& entry = g_doc.entries[i];
        if (!EntryMatchesCurrentFilter(entry))
            continue;

        bool exists = false;
        bool checked = !g_checkExistsByEntry.empty();
        if (checked) {
            auto it = g_checkExistsByEntry.find(&entry);
            if (it != g_checkExistsByEntry.end())
                exists = it->second;
        }

        LVITEM lvi{};
        lvi.mask = LVIF_TEXT;
        lvi.iItem = static_cast<int32_t>(g_visibleEntries.size());
        lvi.iSubItem = COL_STATUS;
        std::wstring status = !checked ? L"-" : (exists ? L"OK" : L"NG");
        lvi.pszText = status.data();
        ListView_InsertItem(g_hList, &lvi);
        g_visibleEntries.push_back(i);

        std::wstring key = ToWide(entry.key);
        ListView_SetItemText(g_hList, lvi.iItem, COL_KEY, key.data());

        std::wstring path = ToWide(entry.path);
        if (entry.isProjectFile)
            path = L"[project] " + path;
        ListView_SetItemText(g_hList, lvi.iItem, COL_PATH, path.data());
    }
}

static void OpenFile(const std::filesystem::path& path) {
    auto result = ParseAup2(path);
    if (!result) {
        TaskMsgBox(g_hWnd, L"ファイルの読み込みに失敗しました。", L"エラー",
                   MB_ICONERROR);
        return;
    }
    g_doc = std::move(*result);
    g_loaded = true;
    ClearCheckResults();
    SetDirty(false);

    SetWindowText(GetDlgItem(g_hWnd, ID_EDIT_FILEPATH), g_doc.sourcePath.c_str());

    for (auto& entry : g_doc.entries) {
        if (!entry.isProjectFile)
            continue;
        if (!PathsReferToSameLocation(PathFromUtf8(entry.path), g_doc.sourcePath)) {
            std::wstring msg =
                std::wstring(L"プロジェクトの記録パスと実際のパスが異なります。\n\n"
                             L"  記録: ") +
                ToWide(entry.path) + L"\n  実際: " + g_doc.sourcePath.wstring() +
                L"\n\n保存時に現在のパスで上書きしますか？";
            if (TaskMsgBox(g_hWnd, msg.c_str(), L"パスの不一致",
                           MB_YESNO | MB_ICONWARNING) == IDYES) {
                entry.path = ToUtf8(g_doc.sourcePath.wstring());
            }
        }
        break;
    }

    ListViewPopulate();
    UpdateWindowTitle();
    UpdateControlStates();
}

static void OnBtnOpen() {
    if (!ConfirmSaveIfDirty())
        return;

    std::vector<WCHAR> buf(32768, L'\0');
    OPENFILENAME ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hWnd;
    ofn.lpstrFilter = L"AviUtl2 Project (*.aup2)\0*.aup2\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = static_cast<DWORD>(buf.size());
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileName(&ofn))
        return;
    OpenFile(buf.data());
}

static void OnBtnEditSel() {
    if (!g_loaded)
        return;

    int32_t sel = ListView_GetNextItem(g_hList, -1, LVNI_SELECTED);
    PathEntry* selEntry = EntryFromVisibleRow(sel);
    if (!selEntry) {
        TaskMsgBox(g_hWnd, L"行を選択してください。", L"確認", MB_ICONINFORMATION);
        return;
    }

    const std::string oldPath = selEntry->path;

    std::vector<WCHAR> buf(32768, L'\0');
    std::wstring initialPath = ToWide(oldPath);
    if (initialPath.size() >= buf.size()) {
        TaskMsgBox(g_hWnd, L"初期パスが長すぎるため開けません。", L"エラー",
                   MB_ICONERROR);
        return;
    }
    wcscpy_s(buf.data(), buf.size(), initialPath.c_str());

    OPENFILENAME ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hWnd;
    ofn.lpstrFilter = L"All Files (*.*)\0*.*\0";
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = static_cast<DWORD>(buf.size());
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileName(&ofn))
        return;

    const std::string newPath = ToUtf8(buf.data());

    std::vector<size_t> sameIndices;
    for (size_t i = 0; i < g_doc.entries.size(); ++i) {
        if (&g_doc.entries[i] == selEntry)
            continue;
        if (g_doc.entries[i].isProjectFile)
            continue;
        if (g_doc.entries[i].path == oldPath)
            sameIndices.push_back(i);
    }

    bool updateAll = false;
    if (!sameIndices.empty()) {
        std::wstring msg =
            std::format(L"同じファイルを参照している行が他に {} 件あります。\n"
                        L"まとめて変更しますか？\n\n"
                        L"  元: {}\n  新: {}",
                        sameIndices.size(), ToWide(oldPath), ToWide(newPath));
        updateAll = (TaskMsgBox(g_hWnd, msg.c_str(), L"一括変更の確認",
                                MB_YESNO | MB_ICONQUESTION) == IDYES);
    }

    if (newPath == oldPath)
        return;

    selEntry->path = newPath;

    if (updateAll) {
        for (size_t idx : sameIndices) {
            g_doc.entries[idx].path = newPath;
        }
    }

    ClearCheckResults();
    SetDirty(true);
    ListViewPopulate();
}

static std::vector<DetectedRootMapping>
DetectRootMappings(const std::filesystem::path& newRoot) {
    std::vector<std::filesystem::path> sources;
    for (const auto& entry : g_doc.entries) {
        if (entry.isProjectFile)
            continue;
        auto p = NormalizePathForCompare(PathFromUtf8(entry.path));
        bool dup = std::any_of(sources.begin(), sources.end(), [&](const auto& s) {
            return PathsReferToSameLocation(s, p);
        });
        if (!dup)
            sources.push_back(p);
    }

    int32_t total = static_cast<int32_t>(sources.size());
    std::map<std::wstring, DetectedRootMapping> byOldRoot;

    for (const auto& src : sources) {
        std::vector<std::filesystem::path> parts;
        for (const auto& part : src.relative_path())
            parts.push_back(part);
        if (parts.empty())
            continue;

        std::set<std::wstring> seenForThisFile;

        for (size_t strip = 0; strip < parts.size(); ++strip) {
            std::filesystem::path rel;
            for (size_t j = strip; j < parts.size(); ++j)
                rel /= parts[j];

            std::error_code ec;
            if (!std::filesystem::is_regular_file(newRoot / rel, ec))
                continue;

            std::filesystem::path oldRoot = src.root_path();
            for (size_t j = 0; j < strip; ++j)
                oldRoot /= parts[j];

            std::wstring key = ToLower(NormalizePathForCompare(oldRoot).wstring());
            if (seenForThisFile.count(key))
                continue;
            seenForThisFile.insert(key);

            auto& mapping = byOldRoot[key];
            if (mapping.matchCount == 0) {
                mapping.oldRoot = NormalizePathForCompare(oldRoot);
                mapping.newRoot = newRoot;
                mapping.totalEntries = total;
            }
            ++mapping.matchCount;
        }
    }

    std::vector<DetectedRootMapping> result;
    result.reserve(byOldRoot.size());
    for (auto& [k, v] : byOldRoot)
        result.push_back(v);

    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.matchCount > b.matchCount;
    });

    return result;
}

static void CommitSelectionDialog(HWND hWnd, SelectionDialogState* state) {
    if (state) {
        int32_t sel =
            static_cast<int32_t>(SendMessage(state->hList, LB_GETCURSEL, 0, 0));
        state->result = (sel != LB_ERR) ? sel : -1;
    }
    DestroyWindow(hWnd);
}

static LRESULT CALLBACK SelectionDlgWndProc(HWND hWnd, UINT msg, WPARAM wp,
                                            LPARAM lp) {
    auto* state = reinterpret_cast<SelectionDialogState*>(
        GetWindowLongPtr(hWnd, GWLP_USERDATA));

    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCT*>(lp);
            state = reinterpret_cast<SelectionDialogState*>(cs->lpCreateParams);
            SetWindowLongPtr(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            HINSTANCE hi = cs->hInstance;
            std::wstring destLabel =
                L"置換先: " + state->mappings->at(0).newRoot.wstring();
            CreateWindow(L"STATIC", destLabel.c_str(),
                         WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS, 8, 8, 684,
                         18, hWnd, nullptr, hi, nullptr);

            CreateWindow(L"STATIC", L"置換元として適用するフォルダを選択してください:",
                         WS_CHILD | WS_VISIBLE | SS_LEFT, 8, 30, 684, 18, hWnd, nullptr,
                         hi, nullptr);
            state->hList = CreateWindow(
                L"LISTBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_NOTIFY, 8, 52, 684,
                190, hWnd, reinterpret_cast<HMENU>(ID_SEL_LIST), hi, nullptr);

            for (const auto& m : *state->mappings) {
                std::wstring item = std::format(L"[{} / {} 件マッチ]  {}", m.matchCount,
                                                m.totalEntries, m.oldRoot.wstring());
                SendMessage(state->hList, LB_ADDSTRING, 0,
                            reinterpret_cast<LPARAM>(item.c_str()));
            }
            SendMessage(state->hList, LB_SETCURSEL, 0, 0);

            CreateWindow(L"BUTTON", L"適用", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                         8, 252, 88, 28, hWnd, reinterpret_cast<HMENU>(IDOK), hi,
                         nullptr);
            CreateWindow(L"BUTTON", L"キャンセル",
                         WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 104, 252, 110, 28, hWnd,
                         reinterpret_cast<HMENU>(IDCANCEL), hi, nullptr);
            return 0;
        }

        case WM_COMMAND:
            if (LOWORD(wp) == IDOK) {
                CommitSelectionDialog(hWnd, state);
            } else if (LOWORD(wp) == IDCANCEL) {
                DestroyWindow(hWnd);
            } else if (LOWORD(wp) == ID_SEL_LIST && HIWORD(wp) == LBN_DBLCLK) {
                CommitSelectionDialog(hWnd, state);
            }
            return 0;

        case WM_CLOSE:
            DestroyWindow(hWnd);
            return 0;

        case WM_DESTROY:
            return 0;
    }
    return DefWindowProc(hWnd, msg, wp, lp);
}

static int32_t
ShowMappingSelectionDialog(const std::vector<DetectedRootMapping>& mappings) {
    HINSTANCE hi = GetModuleHandle(nullptr);

    WNDCLASS wc{};
    wc.lpfnWndProc = SelectionDlgWndProc;
    wc.hInstance = hi;
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"aviutl2_relink_selDlg";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClass(&wc);
    SelectionDialogState state;
    state.mappings = &mappings;

    HWND hDlg =
        CreateWindow(L"aviutl2_relink_selDlg", L"置換元フォルダの選択",
                     WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT,
                     CW_USEDEFAULT, 720, 320, g_hWnd, nullptr, hi, &state);

    if (!hDlg)
        return -1;

    ShowWindow(hDlg, SW_SHOW);
    UpdateWindow(hDlg);
    EnableWindow(g_hWnd, FALSE);

    MSG msg{};
    while (IsWindow(hDlg) && GetMessage(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    EnableWindow(g_hWnd, TRUE);
    SetForegroundWindow(g_hWnd);

    return state.result;
}

static void OnBtnReplaceRoot() {
    if (!g_loaded || IsBusy())
        return;

    std::filesystem::path suggestedDest;
    {
        int32_t sel = ListView_GetNextItem(g_hList, -1, LVNI_SELECTED);
        if (PathEntry* e = EntryFromVisibleRow(sel))
            suggestedDest = PathFromUtf8(e->path).parent_path();
        else if (!g_doc.entries.empty())
            suggestedDest = PathFromUtf8(g_doc.entries.front().path).parent_path();
    }

    auto newRoot = PickFolder(L"置換先フォルダを選択してください", suggestedDest);
    if (!newRoot)
        return;

    g_scanInProgress = true;
    UpdateWindowTitle();
    UpdateControlStates();
    SetCursor(LoadCursor(nullptr, IDC_WAIT));

    std::thread([newRoot = *newRoot]() {
        auto* mappings = new std::vector<DetectedRootMapping>;
        try {
            *mappings = DetectRootMappings(newRoot);
        } catch (...) {
        }
        if (!PostMessage(g_hWnd, WM_ROOTSCAN_DONE, 0, reinterpret_cast<LPARAM>(mappings)))
            delete mappings;
    }).detach();
}

static void FinishRootScan(std::vector<DetectedRootMapping>* mappingsPtr) {
    std::unique_ptr<std::vector<DetectedRootMapping>> mappings(mappingsPtr);
    g_scanInProgress = false;
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
    UpdateWindowTitle();
    UpdateControlStates();

    if (mappings->empty()) {
        TaskMsgBox(
            g_hWnd,
            L"指定フォルダ内に一致するファイル構造が見つかりませんでした。\n\n"
            L"ファイルが正しいフォルダにコピーされているか確認してください。",
            L"ルート一括置換", MB_ICONINFORMATION);
        return;
    }

    int32_t selectedIndex = 0;
    if (static_cast<int32_t>(mappings->size()) > 1) {
        selectedIndex = ShowMappingSelectionDialog(*mappings);
        if (selectedIndex < 0)
            return;
    }

    const DetectedRootMapping& chosen = (*mappings)[static_cast<size_t>(selectedIndex)];
    auto normalizedOld = NormalizePathForCompare(chosen.oldRoot);
    auto normalizedNew = NormalizePathForCompare(chosen.newRoot);
    std::wstring oldRootText = normalizedOld.wstring();
    std::wstring newRootText = normalizedNew.wstring();
    std::wstring oldPrefix = EnsureTrailingSeparator(oldRootText);
    std::wstring newPrefix = EnsureTrailingSeparator(newRootText);

    int32_t unmatchedCount = chosen.totalEntries - chosen.matchCount;
    std::wstring confirmMsg =
        std::format(L"以下のマッピングで参照パスを置換します。\n\n"
                    L"  置換元: {}\n"
                    L"  置換先: {}\n\n"
                    L"  マッチ: {} / {} 件\n"
                    L"  未マッチ（変更なし）: {} 件\n\n"
                    L"実行しますか？",
                    oldRootText, newRootText, chosen.matchCount,
                    chosen.totalEntries, unmatchedCount);

    if (TaskMsgBox(g_hWnd, confirmMsg, L"ルート一括置換", MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;

    int32_t updated = 0;
    for (auto& entry : g_doc.entries) {
        if (entry.isProjectFile)
            continue;

        std::wstring current = NormalizePathForCompare(PathFromUtf8(entry.path)).wstring();
        std::wstring replaced;
        if (PathsReferToSameLocation(current, oldRootText))
            replaced = newRootText;
        else if (StartsWithPathPrefix(current, oldPrefix))
            replaced = newPrefix + current.substr(oldPrefix.size());
        else
            continue;

        std::string newPath = ToUtf8(replaced);
        if (entry.path == newPath)
            continue;

        entry.path = std::move(newPath);
        ++updated;
    }

    if (updated == 0) {
        TaskMsgBox(g_hWnd, L"一致する参照パスはありませんでした。", L"ルート一括置換", MB_ICONINFORMATION);
        return;
    }

    ClearCheckResults();
    SetDirty(true);
    ListViewPopulate();

    TaskMsgBox(g_hWnd, std::format(L"{} 件の参照パスを置換しました。", updated), L"ルート一括置換", MB_ICONINFORMATION);
}

static void OnBtnRelinkProjectFiles() {
    if (!g_loaded)
        return;

    CopySettings settings = LoadCopySettings();
    std::wstring folderName =
        ExpandProjectFolderTemplate(settings.projectSideFolder);
    std::filesystem::path currentRoot =
        g_doc.sourcePath.parent_path() / folderName;

    if (!std::filesystem::exists(currentRoot)) {
        std::wstring msg =
            L"プロジェクト直下の素材フォルダが見つかりません。\n\n  " +
            currentRoot.wstring() + L"\n\n別のフォルダを指定しますか？";
        if (TaskMsgBox(g_hWnd, msg.c_str(), L"直下素材へ再リンク",
                       MB_YESNO | MB_ICONQUESTION) != IDYES) {
            return;
        }

        auto picked = PickFolder(L"現在の素材フォルダを選択してください",
                                 g_doc.sourcePath.parent_path());
        if (!picked)
            return;
        currentRoot = *picked;
        folderName = currentRoot.filename().wstring();
    }

    int32_t updated = 0;
    int32_t alreadyCurrent = 0;
    int32_t noTrace = 0;
    int32_t missing = 0;
    std::vector<RelinkPlanItem> plan;

    for (auto& entry : g_doc.entries) {
        if (entry.isProjectFile)
            continue;

        auto relative =
            RelativePathAfterFolder(PathFromUtf8(entry.path), folderName);
        if (!relative) {
            ++noTrace;
            continue;
        }

        auto newPath = NormalizePathForCompare(currentRoot / *relative);
        if (!std::filesystem::exists(newPath)) {
            ++missing;
            continue;
        }

        if (PathsReferToSameLocation(PathFromUtf8(entry.path), newPath)) {
            ++alreadyCurrent;
            continue;
        }

        plan.push_back(RelinkPlanItem{ &entry, newPath });
    }

    updated = static_cast<int32_t>(plan.size());
    if (updated == 0) {
        std::wstring msg = std::format(L"置換できる参照パスはありませんでした。\n\n"
                                       L"既に現在の場所: {} 件\n"
                                       L"素材フォルダの痕跡なし: {} 件\n"
                                       L"移動先にファイルなし: {} 件",
                                       alreadyCurrent, noTrace, missing);
        TaskMsgBox(g_hWnd, msg.c_str(), L"直下素材へ再リンク", MB_ICONINFORMATION);
        return;
    }

    std::wstring confirmMsg = std::format(
        L"{} 件の参照パスを現在の素材フォルダへ置換します。\n\n"
        L"素材フォルダ:\n  {}\n\n"
        L"既に現在の場所: {} 件\n"
        L"素材フォルダの痕跡なし: {} 件\n"
        L"移動先にファイルなし: {} 件",
        updated, currentRoot.wstring(), alreadyCurrent, noTrace, missing);
    int32_t previewCount = (std::min)(updated, 3);
    if (previewCount > 0) {
        confirmMsg += L"\n\n置換例:";
        for (int32_t i = 0; i < previewCount; ++i) {
            confirmMsg += L"\n  ";
            confirmMsg += ToWide(plan[i].entry->path);
            confirmMsg += L"\n  -> ";
            confirmMsg += plan[i].newPath.wstring();
        }
    }
    confirmMsg += L"\n\n実行しますか？";

    if (TaskMsgBox(g_hWnd, confirmMsg.c_str(), L"直下素材へ再リンク",
                   MB_YESNO | MB_ICONWARNING) != IDYES) {
        return;
    }

    for (const auto& item : plan) {
        item.entry->path = ToUtf8(item.newPath.wstring());
    }

    ClearCheckResults();
    SetDirty(true);
    ListViewPopulate();

    std::wstring msg = std::format(
        L"{} 件の参照パスを現在の素材フォルダへ置換しました。\n\n"
        L"素材フォルダ:\n  {}\n\n"
        L"既に現在の場所: {} 件\n"
        L"素材フォルダの痕跡なし: {} 件\n"
        L"移動先にファイルなし: {} 件",
        updated, currentRoot.wstring(), alreadyCurrent, noTrace, missing);
    TaskMsgBox(g_hWnd, msg.c_str(), L"直下素材へ再リンク", MB_ICONINFORMATION);
}

static void StartCopyWorker(CopyPlan plan, bool copyAskItems,
                            bool overwriteExisting, bool updatePaths,
                            UpdatePathsMode exportLog,
                            UpdatePathsMode exportResult,
                            UpdatePathsMode saveAfterUpdate) {
    g_copyInProgress = true;
    UpdateWindowTitle();
    UpdateControlStates();
    SetCursor(LoadCursor(nullptr, IDC_WAIT));

    std::thread([plan = std::move(plan), copyAskItems, overwriteExisting,
                 updatePaths, exportLog, exportResult,
                 saveAfterUpdate]() mutable {
        auto* result = new CopyExecutionResult;
        result->destRoot = plan.destRoot;
        result->updatePaths = updatePaths;
        result->exportLog = exportLog;
        result->exportResult = exportResult;
        result->saveAfterUpdate = saveAfterUpdate;
        try {
            for (const auto& item : plan.items) {
                CopyPlanItem record = item;
                if (!item.exists) {
                    record.result = L"missing";
                    record.detail = L"source file not found";
                    ++result->missing;
                    result->records.push_back(std::move(record));
                    continue;
                }
                if (item.action == CopyAction::Skip) {
                    record.result = L"excluded";
                    record.detail = L"skipped by rule";
                    ++result->excluded;
                    result->records.push_back(std::move(record));
                    continue;
                }
                if (item.action == CopyAction::Ask && !copyAskItems) {
                    record.result = L"ask_skipped";
                    record.detail = L"ask items were not included";
                    ++result->skippedAsk;
                    result->records.push_back(std::move(record));
                    continue;
                }
                if (item.destExists && !overwriteExisting) {
                    record.result = L"existing_skipped";
                    record.detail = L"destination already exists";
                    ++result->skippedExisting;
                    result->records.push_back(std::move(record));
                    continue;
                }

                std::error_code ec;
                std::filesystem::create_directories(item.dest.parent_path(), ec);
                if (ec) {
                    record.result = L"failed";
                    record.detail = L"failed to create destination folder";
                    ++result->failed;
                    result->records.push_back(std::move(record));
                    continue;
                }

                auto options = overwriteExisting
                                   ? std::filesystem::copy_options::overwrite_existing
                                   : std::filesystem::copy_options::none;
                std::filesystem::copy_file(item.source, item.dest, options, ec);
                if (ec) {
                    record.result = L"failed";
                    record.detail = L"failed to copy file";
                    ++result->failed;
                    result->records.push_back(std::move(record));
                    continue;
                }

                record.result = L"copied";
                record.detail = item.destExists ? L"overwritten" : L"";
                ++result->copied;
                result->copiedPaths[ToLower(
                    NormalizePathForCompare(item.source).wstring())] =
                    NormalizePathForCompare(item.dest);
                result->records.push_back(std::move(record));
            }
        } catch (const std::exception& e) {
            CopyPlanItem record;
            record.result = L"failed";
            record.detail = L"unexpected error: " + ToWide(e.what());
            ++result->failed;
            result->records.push_back(std::move(record));
        } catch (...) {
            CopyPlanItem record;
            record.result = L"failed";
            record.detail = L"unexpected error";
            ++result->failed;
            result->records.push_back(std::move(record));
        }

        if (!PostMessage(g_hWnd, WM_COPYFILES_DONE, 0,
                         reinterpret_cast<LPARAM>(result))) {
            delete result;
        }
    }).detach();
}

static void FinishCopyFiles(CopyExecutionResult& result) {
    g_copyInProgress = false;
    SetCursor(LoadCursor(nullptr, IDC_ARROW));

    int32_t updated = 0;
    if (result.updatePaths && !result.copiedPaths.empty()) {
        for (auto& entry : g_doc.entries) {
            if (entry.isProjectFile)
                continue;
            auto sourceKey =
                ToLower(NormalizePathForCompare(PathFromUtf8(entry.path)).wstring());
            auto it = result.copiedPaths.find(sourceKey);
            if (it == result.copiedPaths.end())
                continue;
            std::string newPath = ToUtf8(it->second.wstring());
            if (entry.path == newPath)
                continue;
            entry.path = std::move(newPath);
            ++updated;
        }
    }

    bool saveRequested = false;
    bool saved = false;
    if (updated > 0) {
        ClearCheckResults();
        SetDirty(true);
        ListViewPopulate();
        saveRequested = ResolveAskSetting(result.saveAfterUpdate,
                                          L"コピー先へ参照パスを変更しました。\n"
                                          L"このままプロジェクトを保存しますか？",
                                          L"素材コピー");
        if (saveRequested) {
            saved = SaveCurrentDocument();
        }
    } else {
        UpdateWindowTitle();
        UpdateControlStates();
    }

    bool logRequested = false;
    bool logExported = false;
    bool resultRequested = false;
    bool resultExported = false;
    std::vector<std::filesystem::path> exportedPaths;
    std::error_code ec;
    std::filesystem::create_directories(result.destRoot, ec);
    std::wstring baseName =
        g_doc.sourcePath.stem().wstring() + L"_copy_" + MakeTimestamp();

    logRequested = ResolveAskSetting(result.exportLog,
                                     L"コピー処理のログを .log で出力しますか？",
                                     L"素材コピー");
    if (logRequested) {
        auto logPath = result.destRoot / (baseName + L".log");
        logExported = WriteCopyLog(result, logPath, updated, saved);
        if (logExported)
            exportedPaths.push_back(logPath);
    }

    resultRequested =
        ResolveAskSetting(result.exportResult,
                          L"コピー結果を .csv で出力しますか？", L"素材コピー");
    if (resultRequested) {
        auto csvPath = result.destRoot / (baseName + L".csv");
        resultExported = WriteCopyResultCsv(result, csvPath);
        if (resultExported)
            exportedPaths.push_back(csvPath);
    }

    std::wstring resultMsg =
        std::format(L"コピー完了: {} 件\n既存スキップ: {} 件\n失敗: {} 件\n"
                    L"参照パス更新: {} 件\n自動保存: {}",
                    result.copied, result.skippedExisting, result.failed, updated,
                    saveRequested ? (saved ? L"成功" : L"失敗") : L"なし");
    if (logRequested)
        resultMsg +=
            std::format(L"\nログ出力: {}", logExported ? L"成功" : L"失敗");
    if (resultRequested)
        resultMsg +=
            std::format(L"\n結果CSV出力: {}", resultExported ? L"成功" : L"失敗");
    for (const auto& path : exportedPaths) {
        resultMsg += L"\n  ";
        resultMsg += path.wstring();
    }
    TaskMsgBox(g_hWnd, resultMsg.c_str(), L"素材コピー",
               (result.failed > 0 || (saveRequested && !saved) ||
                (logRequested && !logExported) ||
                (resultRequested && !resultExported))
                   ? MB_ICONWARNING
                   : MB_ICONINFORMATION);
}

static void OnBtnCopyFiles() {
    if (!g_loaded || IsBusy())
        return;

    CopySettings settings = LoadCopySettings();

    std::filesystem::path destRoot;
    std::wstring suggestedFolder = ExpandProjectFolderTemplate(settings.projectSideFolder);
    auto projectSideDest = g_doc.sourcePath.parent_path() / suggestedFolder;

    std::wstring destMsg =
        L"プロジェクトファイル直下にコピー用フォルダを作成しますか？\n\n  " +
        projectSideDest.wstring() +
        L"\n\nいいえを選ぶと任意フォルダを指定できます。";
    int32_t destChoice = TaskMsgBox(g_hWnd, destMsg, L"素材コピー", MB_YESNOCANCEL | MB_ICONQUESTION);
    if (destChoice == IDCANCEL)
        return;
    if (destChoice == IDYES) {
        destRoot = projectSideDest;
    } else {
        auto picked = PickFolder(L"コピー先フォルダを選択してください", g_doc.sourcePath.parent_path());
        if (!picked)
            return;
        destRoot = *picked;
    }

    g_scanInProgress = true;
    UpdateWindowTitle();
    UpdateControlStates();
    SetCursor(LoadCursor(nullptr, IDC_WAIT));

    std::thread([settings, destRoot]() {
        auto* ctx = new CopyPlanScanContext;
        ctx->settings = settings;
        try {
            ctx->plan = BuildCopyPlan(settings, destRoot);
        } catch (...) {
        }
        if (!PostMessage(g_hWnd, WM_COPYPLAN_DONE, 0, reinterpret_cast<LPARAM>(ctx)))
            delete ctx;
    }).detach();
}

static void FinishCopyPlanScan(CopyPlanScanContext* ctxPtr) {
    std::unique_ptr<CopyPlanScanContext> ctx(ctxPtr);
    g_scanInProgress = false;
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
    UpdateWindowTitle();
    UpdateControlStates();

    CopySettings& settings = ctx->settings;
    CopyPlan& plan = ctx->plan;
    const std::filesystem::path destRoot = plan.destRoot;

    if (plan.items.empty()) {
        TaskMsgBox(g_hWnd, L"コピー候補になる参照ファイルがありません。", L"素材コピー", MB_ICONINFORMATION);
        return;
    }

    int32_t copyCount = 0;
    int32_t askCount = 0;
    int32_t skipCount = 0;
    int32_t missingCount = 0;
    int32_t destExistsCount = 0;
    for (const auto& item : plan.items) {
        if (!item.exists) {
            ++missingCount;
            continue;
        }
        if (item.action == CopyAction::Copy)
            ++copyCount;
        else if (item.action == CopyAction::Ask)
            ++askCount;
        else
            ++skipCount;
        if (item.destExists && item.action != CopyAction::Skip)
            ++destExistsCount;
    }

    bool copyAskItems = false;
    if (askCount > 0) {
        std::wstring askMsg =
            std::format(L"設定で ask になっているファイルが {} 件あります。\n"
                        L"これらもコピー対象に含めますか？",
                        askCount);
        askMsg += DescribeFirstItems(plan, CopyAction::Ask, 8);
        int32_t askChoice = TaskMsgBox(g_hWnd, askMsg, L"素材コピー", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (askChoice == IDCANCEL)
            return;
        copyAskItems = (askChoice == IDYES);
    }

    bool overwriteExisting = false;
    if (destExistsCount > 0) {
        if (settings.overwrite == OverwriteMode::Yes) {
            overwriteExisting = true;
        } else if (settings.overwrite == OverwriteMode::No) {
            overwriteExisting = false;
        } else {
            std::wstring overwriteMsg =
                std::format(L"コピー先に同名ファイルが {} 件あります。\n"
                            L"上書きしますか？\n\n"
                            L"いいえを選ぶと既存ファイルはスキップします。",
                            destExistsCount);
            int32_t overwriteChoice = TaskMsgBox(g_hWnd, overwriteMsg, L"素材コピー", MB_YESNOCANCEL | MB_ICONWARNING);
            if (overwriteChoice == IDCANCEL)
                return;
            overwriteExisting = (overwriteChoice == IDYES);
        }
    }

    int32_t effectiveCopyCount = copyCount + (copyAskItems ? askCount : 0);
    if (effectiveCopyCount == 0) {
        TaskMsgBox(g_hWnd, L"コピー対象がありません。", L"素材コピー", MB_ICONINFORMATION);
        return;
    }

    std::wstring confirmMsg = std::format(
        L"コピー先:\n  {}\n\nコピー: {} 件\n確認扱い: {} 件{}\n除外: {} 件\n"
        L"見つからない: {} 件\n\n実行しますか？",
        destRoot.wstring(), copyCount, askCount,
        copyAskItems ? L" (コピーします)" : L" (スキップします)", skipCount,
        missingCount);
    if (!DescribeFirstItems(plan, CopyAction::Skip, 5).empty()) {
        confirmMsg += L"\n\n除外例:";
        confirmMsg += DescribeFirstItems(plan, CopyAction::Skip, 5);
    }

    if (TaskMsgBox(g_hWnd, confirmMsg, L"素材コピー", MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;

    bool updatePaths = false;
    if (settings.updatePathsAfterCopy == UpdatePathsMode::Yes) {
        updatePaths = true;
    } else if (settings.updatePathsAfterCopy == UpdatePathsMode::Ask) {
        int32_t updateChoice = TaskMsgBox(
            g_hWnd, L"コピーに成功したファイルの参照パスをコピー先へ変更しますか？",
            L"素材コピー", MB_YESNO | MB_ICONQUESTION);
        updatePaths = (updateChoice == IDYES);
    }

    StartCopyWorker(std::move(plan), copyAskItems, overwriteExisting, updatePaths, settings.exportLog, settings.exportResult, settings.saveAfterUpdate);
}

static void StartCheckScan(bool exportAfter) {
    if (!g_loaded || IsBusy())
        return;

    g_checkExportPending = exportAfter;
    g_scanInProgress = true;
    UpdateWindowTitle();
    UpdateControlStates();
    SetCursor(LoadCursor(nullptr, IDC_WAIT));

    std::thread([]() {
        auto* results = new std::vector<CheckResult>;
        try {
            *results = CheckPaths(g_doc);
        } catch (...) {
        }
        if (!PostMessage(g_hWnd, WM_CHECK_DONE, 0, reinterpret_cast<LPARAM>(results)))
            delete results;
    }).detach();
}

static void OnBtnCheck() {
    StartCheckScan(false);
}

static void OnBtnCheckExport() {
    StartCheckScan(true);
}

static void FinishCheck(std::vector<CheckResult>* resultsPtr) {
    std::unique_ptr<std::vector<CheckResult>> owned(resultsPtr);
    g_scanInProgress = false;
    SetCursor(LoadCursor(nullptr, IDC_ARROW));

    SetCheckResults(std::move(*owned));
    ListViewPopulate();
    UpdateWindowTitle();
    UpdateControlStates();

    int32_t ng = 0;
    for (const auto& r : g_checkResults)
        if (!r.exists)
            ++ng;

    bool exportRequested = g_checkExportPending;
    g_checkExportPending = false;

    if (!exportRequested) {
        std::wstring resultMsg =
            ng > 0 ? std::format(L"チェック完了: {} 件中 {} 件が見つかりません。", g_checkResults.size(), ng)
                   : std::format(L"チェック完了: {} 件すべて確認できました。", g_checkResults.size());
        TaskMsgBox(g_hWnd, resultMsg, L"チェック結果", ng > 0 ? MB_ICONWARNING : MB_ICONINFORMATION);
        return;
    }

    std::wstring defaultName = g_doc.sourcePath.stem().wstring() + L"_check_" +
                               MakeTimestamp() + L".csv";
    auto exportPath =
        PickSavePath(L"チェック結果の保存先を選択してください",
                     L"CSV Files (*.csv)\0*.csv\0All Files (*.*)\0*.*\0",
                     g_doc.sourcePath.parent_path(), defaultName, L"csv");
    if (!exportPath)
        return;

    bool exported = WriteCheckResultCsv(g_doc, g_checkResults, *exportPath);
    std::wstring resultMsg = std::format(
        L"チェック完了: {} 件中 {} 件が見つかりません。\n結果CSV出力: {}",
        g_checkResults.size(), ng, exported ? L"成功" : L"失敗");
    if (exported) {
        resultMsg += L"\n  ";
        resultMsg += exportPath->wstring();
    }
    TaskMsgBox(g_hWnd, resultMsg, L"チェック結果",
               (!exported || ng > 0) ? MB_ICONWARNING : MB_ICONINFORMATION);
}

static void OnBtnSave() {
    if (!g_loaded)
        return;
    SaveCurrentDocument();
}

static void OnDropFiles(HDROP hDrop) {
    if (IsBusy()) {
        DragFinish(hDrop);
        TaskMsgBox(g_hWnd, L"処理が完了するまでお待ちください。", L"確認", MB_ICONINFORMATION);
        return;
    }

    if (!ConfirmSaveIfDirty()) {
        DragFinish(hDrop);
        return;
    }

    UINT length = DragQueryFile(hDrop, 0, nullptr, 0);
    if (length > 0) {
        std::vector<WCHAR> buf(length + 1, L'\0');
        if (DragQueryFile(hDrop, 0, buf.data(), length + 1))
            OpenFile(buf.data());
    }
    DragFinish(hDrop);
}

static void OnSize(int32_t cx, int32_t cy) {
    const int32_t MARGIN = ScaleForDpi(8, g_dpi);
    const int32_t BTN_W = ScaleForDpi(72, g_dpi);
    const int32_t MID_BTN_W = ScaleForDpi(128, g_dpi);
    const int32_t RELINK_BTN_W = ScaleForDpi(164, g_dpi);
    const int32_t COPY_BTN_W = ScaleForDpi(112, g_dpi);
    const int32_t CHECK_EXPORT_BTN_W = ScaleForDpi(152, g_dpi);
    const int32_t FILTER_LABEL_W = ScaleForDpi(64, g_dpi);
    const int32_t NG_ONLY_W = ScaleForDpi(96, g_dpi);
    const int32_t BTN_H = ScaleForDpi(24, g_dpi);
    const int32_t ROW_H = ScaleForDpi(32, g_dpi);
    const int32_t BOTTOM = ScaleForDpi(36, g_dpi);

    int32_t y = MARGIN;

    MoveWindow(GetDlgItem(g_hWnd, ID_BTN_OPEN), MARGIN, y, BTN_W, BTN_H, TRUE);
    MoveWindow(GetDlgItem(g_hWnd, ID_EDIT_FILEPATH), MARGIN + BTN_W + 4, y,
               cx - MARGIN * 2 - BTN_W - 4, BTN_H, TRUE);
    y += ROW_H;

    MoveWindow(GetDlgItem(g_hWnd, ID_STATIC_FILTER), MARGIN, y, FILTER_LABEL_W,
               BTN_H, TRUE);
    MoveWindow(GetDlgItem(g_hWnd, ID_EDIT_FILTER), MARGIN + FILTER_LABEL_W, y,
               cx - MARGIN * 2 - FILTER_LABEL_W - NG_ONLY_W - 8, BTN_H, TRUE);
    MoveWindow(GetDlgItem(g_hWnd, ID_CHECK_NGONLY), cx - MARGIN - NG_ONLY_W, y,
               NG_ONLY_W, BTN_H, TRUE);
    y += ROW_H;

    int32_t listH = cy - y - BOTTOM - MARGIN;
    MoveWindow(g_hList, MARGIN, y, cx - MARGIN * 2, listH, TRUE);
    y += listH + 4;

    MoveWindow(GetDlgItem(g_hWnd, ID_BTN_CHECK), MARGIN, y, BTN_W, BTN_H, TRUE);
    MoveWindow(GetDlgItem(g_hWnd, ID_BTN_CHECKEXPORT), MARGIN + BTN_W + 4, y,
               CHECK_EXPORT_BTN_W, BTN_H, TRUE);
    MoveWindow(GetDlgItem(g_hWnd, ID_BTN_EDITSEL),
               MARGIN + BTN_W + 4 + CHECK_EXPORT_BTN_W + 4, y, MID_BTN_W, BTN_H,
               TRUE);
    MoveWindow(GetDlgItem(g_hWnd, ID_BTN_REPLACEROOT),
               MARGIN + BTN_W + 4 + CHECK_EXPORT_BTN_W + 4 + MID_BTN_W + 4, y,
               MID_BTN_W, BTN_H, TRUE);
    MoveWindow(GetDlgItem(g_hWnd, ID_BTN_RELINK_PROJECT_FILES),
               MARGIN + BTN_W + 4 + CHECK_EXPORT_BTN_W + 4 + MID_BTN_W + 4 +
                   MID_BTN_W + 4,
               y, RELINK_BTN_W, BTN_H, TRUE);
    MoveWindow(GetDlgItem(g_hWnd, ID_BTN_COPYFILES),
               MARGIN + BTN_W + 4 + CHECK_EXPORT_BTN_W + 4 + MID_BTN_W + 4 +
                   MID_BTN_W + 4 + RELINK_BTN_W + 4,
               y, COPY_BTN_W, BTN_H, TRUE);
    MoveWindow(GetDlgItem(g_hWnd, ID_BTN_SAVE), cx - MARGIN - BTN_W, y, BTN_W,
               BTN_H, TRUE);
}

static LRESULT CALLBACK WndProc(HWND hWnd, uint32_t msg, WPARAM wp, LPARAM lp) {
    try {
        switch (msg) {
            case WM_CREATE: {
                g_hWnd = hWnd;
                HINSTANCE hi = reinterpret_cast<CREATESTRUCT*>(lp)->hInstance;
                g_dpi = GetDpiForWindow(hWnd);
                g_hFont = CreateUiFont(g_dpi);

                CreateWindow(L"BUTTON", L"開く", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0,
                             0, 0, 0, hWnd, ControlId(ID_BTN_OPEN), hi, nullptr);
                CreateWindow(L"EDIT", L"",
                             WS_CHILD | WS_VISIBLE | WS_BORDER | ES_READONLY |
                                 ES_AUTOHSCROLL,
                             0, 0, 0, 0, hWnd, ControlId(ID_EDIT_FILEPATH), hi, nullptr);
                CreateWindow(L"STATIC", L"絞り込み",
                             WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, 0, 0, 0, 0, hWnd,
                             ControlId(ID_STATIC_FILTER), hi, nullptr);
                CreateWindow(L"EDIT", L"",
                             WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 0, 0, 0,
                             0, hWnd, ControlId(ID_EDIT_FILTER), hi, nullptr);
                CreateWindow(L"BUTTON", L"NGのみ",
                             WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 0, 0, 0, 0, hWnd,
                             ControlId(ID_CHECK_NGONLY), hi, nullptr);

                g_hList = CreateWindow(WC_LISTVIEW, L"",
                                       WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT |
                                           LVS_SHOWSELALWAYS,
                                       0, 0, 0, 0, hWnd, ControlId(ID_LIST), hi, nullptr);
                SetWindowTheme(g_hList, L"Explorer", nullptr);
                ListView_SetExtendedListViewStyle(g_hList, LVS_EX_FULLROWSELECT |
                                                               LVS_EX_GRIDLINES);

                LVCOLUMN lvc{};
                lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
                lvc.iSubItem = COL_STATUS;
                lvc.pszText = const_cast<LPWSTR>(L"状態");
                lvc.cx = ScaleForDpi(48, g_dpi);
                ListView_InsertColumn(g_hList, COL_STATUS, &lvc);
                lvc.iSubItem = COL_KEY;
                lvc.pszText = const_cast<LPWSTR>(L"キー");
                lvc.cx = ScaleForDpi(120, g_dpi);
                ListView_InsertColumn(g_hList, COL_KEY, &lvc);
                lvc.iSubItem = COL_PATH;
                lvc.pszText = const_cast<LPWSTR>(L"パス");
                lvc.cx = ScaleForDpi(600, g_dpi);
                ListView_InsertColumn(g_hList, COL_PATH, &lvc);

                CreateWindow(L"BUTTON", L"チェック",
                             WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hWnd,
                             ControlId(ID_BTN_CHECK), hi, nullptr);
                CreateWindow(L"BUTTON", L"チェック結果出力",
                             WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hWnd,
                             ControlId(ID_BTN_CHECKEXPORT), hi, nullptr);
                CreateWindow(L"BUTTON", L"選択行を変更",
                             WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hWnd,
                             ControlId(ID_BTN_EDITSEL), hi, nullptr);
                CreateWindow(L"BUTTON", L"保存", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0,
                             0, 0, 0, hWnd, ControlId(ID_BTN_SAVE), hi, nullptr);
                CreateWindow(L"BUTTON", L"ルート一括置換",
                             WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hWnd,
                             ControlId(ID_BTN_REPLACEROOT), hi, nullptr);
                CreateWindow(L"BUTTON", L"直下素材へ再リンク",
                             WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hWnd,
                             ControlId(ID_BTN_RELINK_PROJECT_FILES), hi, nullptr);
                CreateWindow(L"BUTTON", L"素材をコピー",
                             WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hWnd,
                             ControlId(ID_BTN_COPYFILES), hi, nullptr);

                ApplyFontToChildren();
                DragAcceptFiles(hWnd, TRUE);
                UpdateWindowTitle();
                UpdateControlStates();
                return 0;
            }

            case WM_GETMINMAXINFO: {
                auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
                mmi->ptMinTrackSize.x = ScaleForDpi(640, g_dpi);
                mmi->ptMinTrackSize.y = ScaleForDpi(420, g_dpi);
                return 0;
            }

            case WM_SIZE:
                OnSize(LOWORD(lp), HIWORD(lp));
                return 0;

            case WM_DPICHANGED: {
                g_dpi = HIWORD(wp);
                HFONT newFont = CreateUiFont(g_dpi);
                if (newFont) {
                    if (g_hFont)
                        DeleteObject(g_hFont);
                    g_hFont = newFont;
                    ApplyFontToChildren();
                }
                auto* suggested = reinterpret_cast<RECT*>(lp);
                SetWindowPos(hWnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left, suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
                return 0;
            }
            case WM_COMMAND:
                if (LOWORD(wp) == ID_EDIT_FILTER && HIWORD(wp) == EN_CHANGE) {
                    if (g_hList)
                        ListViewPopulate();
                    return 0;
                }
                if (LOWORD(wp) == ID_CHECK_NGONLY && HIWORD(wp) == BN_CLICKED) {
                    if (g_hList)
                        ListViewPopulate();
                    return 0;
                }
                switch (LOWORD(wp)) {
                    case ID_BTN_OPEN:
                        OnBtnOpen();
                        break;
                    case ID_BTN_CHECK:
                        OnBtnCheck();
                        break;
                    case ID_BTN_CHECKEXPORT:
                        OnBtnCheckExport();
                        break;
                    case ID_BTN_EDITSEL:
                        OnBtnEditSel();
                        break;
                    case ID_BTN_SAVE:
                        OnBtnSave();
                        break;
                    case ID_BTN_REPLACEROOT:
                        OnBtnReplaceRoot();
                        break;
                    case ID_BTN_RELINK_PROJECT_FILES:
                        OnBtnRelinkProjectFiles();
                        break;
                    case ID_BTN_COPYFILES:
                        OnBtnCopyFiles();
                        break;
                }
                return 0;

            case WM_COPYFILES_DONE: {
                std::unique_ptr<CopyExecutionResult> result(reinterpret_cast<CopyExecutionResult*>(lp));
                if (result)
                    FinishCopyFiles(*result);
                return 0;
            }

            case WM_CHECK_DONE: {
                FinishCheck(reinterpret_cast<std::vector<CheckResult>*>(lp));
                return 0;
            }

            case WM_ROOTSCAN_DONE: {
                FinishRootScan(
                    reinterpret_cast<std::vector<DetectedRootMapping>*>(lp));
                return 0;
            }

            case WM_COPYPLAN_DONE: {
                FinishCopyPlanScan(reinterpret_cast<CopyPlanScanContext*>(lp));
                return 0;
            }

            case WM_CLOSE:
                if (IsBusy()) {
                    TaskMsgBox(hWnd, L"処理が完了するまでお待ちください。", L"確認", MB_ICONINFORMATION);
                    return 0;
                }
                if (!ConfirmSaveIfDirty())
                    return 0;
                DestroyWindow(hWnd);
                return 0;

            case WM_DROPFILES:
                OnDropFiles(reinterpret_cast<HDROP>(wp));
                return 0;

            case WM_DESTROY:
                if (g_hFont) {
                    DeleteObject(g_hFont);
                    g_hFont = nullptr;
                }
                PostQuitMessage(0);
                return 0;
        }
        return DefWindowProc(hWnd, msg, wp, lp);
    } catch (const std::exception& e) {
        MessageBoxA(hWnd, e.what(), "予期しないエラー", MB_ICONERROR);
        return 0;
    } catch (...) {
        TaskMsgBox(hWnd, L"予期しないエラーが発生しました。", L"エラー",
                   MB_ICONERROR);
        return 0;
    }
}

int32_t WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR lpCmdLine,
                        int32_t nCmdShow) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASS wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"aviutl2_relink";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClass(&wc);

    int32_t startupDpi = GetDpiForSystem();
    int32_t initialWidth = MulDiv(900, startupDpi, USER_DEFAULT_SCREEN_DPI);
    int32_t initialHeight = MulDiv(600, startupDpi, USER_DEFAULT_SCREEN_DPI);

    HWND hWnd = CreateWindow(L"aviutl2_relink", APP_TITLE_BASE,
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             initialWidth, initialHeight, nullptr, nullptr,
                             hInstance, nullptr);

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    if (lpCmdLine && lpCmdLine[0] != L'\0') {
        std::wstring arg = lpCmdLine;
        if (arg.front() == L'"' && arg.back() == L'"')
            arg = arg.substr(1, arg.size() - 2);
        OpenFile(arg);
    }

    MSG msg{};
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    CoUninitialize();
    return static_cast<int32_t>(msg.wParam);
}