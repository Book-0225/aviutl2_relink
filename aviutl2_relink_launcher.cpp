#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "plugin2.h"

namespace {

constexpr wchar_t PLUGIN_NAME[] = L"aviutl2_relink launcher";
constexpr wchar_t PLUGIN_INFORMATION[] =
    L"aviutl2_relink launcher v0.0.5 by BOOK25";
constexpr wchar_t MENU_OPEN_CURRENT[] =
    L"aviutl2_relink\\現在のプロジェクトを開く";
constexpr wchar_t MENU_PICK_PROJECT[] =
    L"aviutl2_relink\\aup2 を選んで開く";
constexpr wchar_t MENU_OPEN_TOOL[] = L"aviutl2_relink\\ツールだけ起動";
constexpr wchar_t EXE_NAME[] = L"aviutl2_relink.exe";

COMMON_PLUGIN_TABLE g_commonPluginTable = {
    PLUGIN_NAME,
    PLUGIN_INFORMATION,
};

HOST_APP_TABLE *g_host = nullptr;
EDIT_HANDLE *g_editHandle = nullptr;

HWND OwnerWindow() {
  if (!g_editHandle || !g_editHandle->get_host_app_window)
    return nullptr;
  return g_editHandle->get_host_app_window();
}

std::filesystem::path ModuleDirectory() {
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&g_commonPluginTable),
                          &module)) {
    return std::filesystem::current_path();
  }

  std::vector<wchar_t> buffer(MAX_PATH);
  for (;;) {
    DWORD size =
        GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (size == 0)
      return std::filesystem::current_path();
    if (size + 1 < buffer.size())
      return std::filesystem::path(buffer.data()).parent_path();
    buffer.resize(buffer.size() * 2);
  }
}

std::optional<std::filesystem::path> FindRelinkExe() {
  std::vector<std::filesystem::path> candidates;
  auto moduleDir = ModuleDirectory();
  candidates.push_back(moduleDir / EXE_NAME);
  candidates.push_back(moduleDir.parent_path() / EXE_NAME);
  candidates.push_back(std::filesystem::current_path() / EXE_NAME);

  for (const auto &candidate : candidates) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(candidate, ec))
      return candidate;
  }
  return std::nullopt;
}

std::optional<std::filesystem::path> PickProjectFile(
    HWND owner, const std::filesystem::path &initialFolder) {
  std::vector<wchar_t> buffer(32768, L'\0');

  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = owner;
  ofn.lpstrFilter =
      L"AviUtl2 Project (*.aup2)\0*.aup2\0All Files (*.*)\0*.*\0";
  ofn.lpstrFile = buffer.data();
  ofn.nMaxFile = static_cast<DWORD>(buffer.size());
  ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

  std::wstring initialFolderText = initialFolder.wstring();
  if (!initialFolderText.empty())
    ofn.lpstrInitialDir = initialFolderText.c_str();

  if (!GetOpenFileNameW(&ofn))
    return std::nullopt;

  return std::filesystem::path(buffer.data());
}

std::wstring QuoteForCommandLine(const std::wstring &value) {
  std::wstring quoted = L"\"";
  size_t backslashes = 0;
  for (wchar_t ch : value) {
    if (ch == L'\\') {
      ++backslashes;
      continue;
    }
    if (ch == L'"') {
      quoted.append(backslashes * 2 + 1, L'\\');
      quoted.push_back(ch);
    } else {
      quoted.append(backslashes, L'\\');
      quoted.push_back(ch);
    }
    backslashes = 0;
  }
  quoted.append(backslashes * 2, L'\\');
  quoted.push_back(L'"');
  return quoted;
}

bool LaunchRelink(const std::filesystem::path &exePath,
                  const std::optional<std::filesystem::path> &projectPath) {
  std::wstring commandLine = QuoteForCommandLine(exePath.wstring());
  if (projectPath) {
    commandLine += L" ";
    commandLine += QuoteForCommandLine(projectPath->wstring());
  }

  STARTUPINFOW startupInfo{};
  startupInfo.cb = sizeof(startupInfo);
  PROCESS_INFORMATION processInfo{};
  std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
  mutableCommand.push_back(L'\0');

  BOOL ok = CreateProcessW(exePath.c_str(), mutableCommand.data(), nullptr,
                           nullptr, FALSE, 0, nullptr,
                           exePath.parent_path().c_str(), &startupInfo,
                           &processInfo);
  if (!ok)
    return false;

  CloseHandle(processInfo.hThread);
  CloseHandle(processInfo.hProcess);
  return true;
}

void ShowExeNotFound(HWND owner) {
  MessageBoxW(owner,
              L"aviutl2_relink.exe が見つかりません。\n"
              L"このプラグインと同じフォルダに配置してください。",
              PLUGIN_NAME, MB_ICONERROR);
}

std::optional<std::filesystem::path> CurrentProjectPath(EDIT_SECTION *edit) {
  if (!edit || !g_editHandle)
    return std::nullopt;

  PROJECT_FILE *project = edit->get_project_file(g_editHandle);
  if (!project || !project->get_project_file_path)
    return std::nullopt;

  LPCWSTR projectPathText = project->get_project_file_path();
  if (!projectPathText || projectPathText[0] == L'\0')
    return std::nullopt;

  return std::filesystem::path(projectPathText);
}

void OpenCurrentProject(EDIT_SECTION *edit) {
  HWND owner = OwnerWindow();
  auto projectPath = CurrentProjectPath(edit);
  if (!projectPath) {
    MessageBoxW(owner, L"プロジェクトファイルを取得できませんでした。",
                PLUGIN_NAME, MB_ICONERROR);
    return;
  }

  auto exePath = FindRelinkExe();
  if (!exePath) {
    ShowExeNotFound(owner);
    return;
  }

  int answer = MessageBoxW(
      owner,
      L"aviutl2_relink.exe で現在のプロジェクトファイルを開きます。\n\n"
      L"AviUtl2 側に未保存の変更がある場合、外部ツールには最後に保存した内容だけが表示されます。\n"
      L"外部ツールで保存したあと、AviUtl2 側で上書き保存すると変更が戻ることがあります。",
      PLUGIN_NAME, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
  if (answer != IDYES)
    return;

  if (!LaunchRelink(*exePath, projectPath)) {
    MessageBoxW(owner, L"aviutl2_relink.exe の起動に失敗しました。",
                PLUGIN_NAME, MB_ICONERROR);
  }
}

void PickAndOpenProject(EDIT_SECTION *edit) {
  HWND owner = OwnerWindow();
  auto exePath = FindRelinkExe();
  if (!exePath) {
    ShowExeNotFound(owner);
    return;
  }

  std::filesystem::path initialFolder = exePath->parent_path();
  if (auto current = CurrentProjectPath(edit))
    initialFolder = current->parent_path();

  auto picked = PickProjectFile(owner, initialFolder);
  if (!picked)
    return;

  if (!LaunchRelink(*exePath, picked)) {
    MessageBoxW(owner, L"aviutl2_relink.exe の起動に失敗しました。",
                PLUGIN_NAME, MB_ICONERROR);
  }
}

void OpenToolOnly(EDIT_SECTION *) {
  HWND owner = OwnerWindow();
  auto exePath = FindRelinkExe();
  if (!exePath) {
    ShowExeNotFound(owner);
    return;
  }

  if (!LaunchRelink(*exePath, std::nullopt)) {
    MessageBoxW(owner, L"aviutl2_relink.exe の起動に失敗しました。",
                PLUGIN_NAME, MB_ICONERROR);
  }
}
} // namespace

EXTERN_C __declspec(dllexport) DWORD RequiredVersion() { return 2004500; }

EXTERN_C __declspec(dllexport) COMMON_PLUGIN_TABLE *GetCommonPluginTable() {
  return &g_commonPluginTable;
}

EXTERN_C __declspec(dllexport) bool InitializePlugin(DWORD) { return true; }

EXTERN_C __declspec(dllexport) void UninitializePlugin() {}

EXTERN_C __declspec(dllexport) void RegisterPlugin(HOST_APP_TABLE *host) {
  g_host = host;
  if (!g_host)
    return;

  g_editHandle = g_host->create_edit_handle();
  g_host->register_edit_menu(MENU_OPEN_CURRENT, OpenCurrentProject);
  g_host->register_edit_menu(MENU_PICK_PROJECT, PickAndOpenProject);
  g_host->register_edit_menu(MENU_OPEN_TOOL, OpenToolOnly);
}
