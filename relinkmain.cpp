#define WIN32_LEAN_AND_MEAN
#include "Aup2Parser.h"
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <filesystem>
#include <format>
#include <shellapi.h>
#include <string>
#include <vector>

constexpr wchar_t APP_TITLE[] = L"aviutl2_relink v0.0.1";

constexpr int32_t ID_BTN_OPEN = 101;
constexpr int32_t ID_EDIT_FILEPATH = 102;
constexpr int32_t ID_LIST = 103;
constexpr int32_t ID_BTN_CHECK = 104;
constexpr int32_t ID_BTN_EDITSEL = 105;
constexpr int32_t ID_BTN_SAVE = 106;
constexpr int32_t COL_STATUS = 0;
constexpr int32_t COL_KEY = 1;
constexpr int32_t COL_PATH = 2;

static HWND g_hWnd = nullptr;
static HWND g_hList = nullptr;
static Aup2Document g_doc;
static bool g_loaded = false;
static std::vector<CheckResult> g_checkResults;

static std::wstring ToWide(const std::string &s) {
  if (s.empty())
    return {};
  int32_t n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(n - 1, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
  return w;
}

static std::string ToUtf8(const std::wstring &w) {
  if (w.empty())
    return {};
  int32_t n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr,
                              nullptr);
  std::string s(n - 1, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
  return s;
}

static void ListViewPopulate() {
  ListView_DeleteAllItems(g_hList);

  for (size_t i = 0; i < g_doc.entries.size(); ++i) {
    const auto &entry = g_doc.entries[i];

    bool exists = false;
    bool checked = !g_checkResults.empty();
    for (const auto &r : g_checkResults) {
      if (r.entry == &entry) {
        exists = r.exists;
        break;
      }
    }

    LVITEM lvi{};
    lvi.mask = LVIF_TEXT;
    lvi.iItem = static_cast<int32_t>(i);
    lvi.iSubItem = COL_STATUS;
    std::wstring status = !checked ? L"-" : (exists ? L"OK" : L"NG");
    lvi.pszText = status.data();
    ListView_InsertItem(g_hList, &lvi);

    std::wstring key = ToWide(entry.key);
    ListView_SetItemText(g_hList, static_cast<int32_t>(i), COL_KEY, key.data());

    std::wstring path = ToWide(entry.path);
    if (entry.isProjectFile)
      path = L"[project] " + path;
    ListView_SetItemText(g_hList, static_cast<int32_t>(i), COL_PATH, path.data());
  }
}

static void OpenFile(const std::filesystem::path &path) {
  auto result = ParseAup2(path);
  if (!result) {
    MessageBox(g_hWnd, L"ファイルの読み込みに失敗しました。", L"エラー",
               MB_ICONERROR);
    return;
  }
  g_doc = std::move(*result);
  g_loaded = true;
  g_checkResults.clear();

  SetWindowText(GetDlgItem(g_hWnd, ID_EDIT_FILEPATH), g_doc.sourcePath.c_str());

  for (auto &entry : g_doc.entries) {
    if (!entry.isProjectFile)
      continue;
    if (PathFromUtf8(entry.path) != g_doc.sourcePath) {
      std::wstring msg =
          std::wstring(L"プロジェクトの記録パスと実際のパスが異なります。\n\n"
                       L"  記録: ") +
          ToWide(entry.path) + L"\n  実際: " + g_doc.sourcePath.wstring() +
          L"\n\n保存時に現在のパスで上書きしますか？";
      if (MessageBox(g_hWnd, msg.c_str(), L"パスの不一致",
                     MB_YESNO | MB_ICONWARNING) == IDYES) {
        entry.path = ToUtf8(g_doc.sourcePath.wstring());
      }
    }
    break;
  }

  ListViewPopulate();
}

static void OnBtnOpen() {
  WCHAR buf[MAX_PATH]{};
  OPENFILENAME ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = g_hWnd;
  ofn.lpstrFilter = L"AviUtl2 Project (*.aup2)\0*.aup2\0All Files (*.*)\0*.*\0";
  ofn.lpstrFile = buf;
  ofn.nMaxFile = MAX_PATH;
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
  if (!GetOpenFileName(&ofn))
    return;
  OpenFile(buf);
}

static void OnBtnEditSel() {
  if (!g_loaded)
    return;

  int32_t sel = ListView_GetNextItem(g_hList, -1, LVNI_SELECTED);
  if (sel < 0 || sel >= static_cast<int32_t>(g_doc.entries.size())) {
    MessageBox(g_hWnd, L"行を選択してください。", L"確認", MB_ICONINFORMATION);
    return;
  }

  auto &selEntry = g_doc.entries[sel];
  const std::string oldPath = selEntry.path;

  WCHAR buf[MAX_PATH]{};
  wcscpy_s(buf, ToWide(oldPath).c_str());

  OPENFILENAME ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = g_hWnd;
  ofn.lpstrFilter = L"All Files (*.*)\0*.*\0";
  ofn.lpstrFile = buf;
  ofn.nMaxFile = MAX_PATH;
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
  if (!GetOpenFileName(&ofn))
    return;

  const std::string newPath = ToUtf8(buf);

  std::vector<size_t> sameIndices;
  for (size_t i = 0; i < g_doc.entries.size(); ++i) {
    if (static_cast<int32_t>(i) == sel)
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
    updateAll = (MessageBox(g_hWnd, msg.c_str(), L"一括変更の確認",
                            MB_YESNO | MB_ICONQUESTION) == IDYES);
  }

  selEntry.path = newPath;

  if (updateAll) {
    for (size_t idx : sameIndices) {
      g_doc.entries[idx].path = newPath;
    }
  }

  g_checkResults.clear();
  ListViewPopulate();
}

static void OnBtnCheck() {
  if (!g_loaded)
    return;
  g_checkResults = CheckPaths(g_doc);
  ListViewPopulate();

  int32_t ng = 0;
  for (const auto &r : g_checkResults)
    if (!r.exists)
      ++ng;
  std::wstring resultMsg =
      ng > 0 ? std::format(L"チェック完了: {} 件中 {} 件が見つかりません。",
                           g_checkResults.size(), ng)
             : std::format(L"チェック完了: {} 件すべて確認できました。",
                           g_checkResults.size());
  MessageBox(g_hWnd, resultMsg.c_str(), L"チェック結果",
             ng > 0 ? MB_ICONWARNING : MB_ICONINFORMATION);
}

static void OnBtnSave() {
  if (!g_loaded)
    return;
  if (!SaveAup2(g_doc, g_doc.sourcePath)) {
    MessageBox(g_hWnd, L"保存に失敗しました。", L"エラー", MB_ICONERROR);
    return;
  }
  MessageBox(g_hWnd, L"保存しました。(バックアップ: .aup2.bak)", L"完了",
             MB_ICONINFORMATION);
}

static void OnDropFiles(HDROP hDrop) {
  WCHAR buf[MAX_PATH]{};
  if (DragQueryFile(hDrop, 0, buf, MAX_PATH))
    OpenFile(buf);
  DragFinish(hDrop);
}

static void OnSize(int32_t cx, int32_t cy) {
  constexpr int32_t MARGIN = 8;
  constexpr int32_t BTN_W = 72;
  constexpr int32_t BTN_H = 24;
  constexpr int32_t ROW_H = 32;
  constexpr int32_t BOTTOM = 36;

  int32_t y = MARGIN;

  MoveWindow(GetDlgItem(g_hWnd, ID_BTN_OPEN), MARGIN, y, BTN_W, BTN_H, TRUE);
  MoveWindow(GetDlgItem(g_hWnd, ID_EDIT_FILEPATH), MARGIN + BTN_W + 4, y,
             cx - MARGIN * 2 - BTN_W - 4, BTN_H, TRUE);
  y += ROW_H;

  int32_t listH = cy - y - BOTTOM - MARGIN;
  MoveWindow(g_hList, MARGIN, y, cx - MARGIN * 2, listH, TRUE);
  y += listH + 4;

  MoveWindow(GetDlgItem(g_hWnd, ID_BTN_CHECK), MARGIN, y, BTN_W, BTN_H, TRUE);
  MoveWindow(GetDlgItem(g_hWnd, ID_BTN_EDITSEL), MARGIN + BTN_W + 4, y, 120,
             BTN_H, TRUE);
  MoveWindow(GetDlgItem(g_hWnd, ID_BTN_SAVE), cx - MARGIN - BTN_W, y, BTN_W,
             BTN_H, TRUE);
}

static LRESULT CALLBACK WndProc(HWND hWnd, uint32_t msg, WPARAM wp, LPARAM lp) {
  try {
    switch (msg) {
    case WM_CREATE: {
      g_hWnd = hWnd;
      HINSTANCE hi = reinterpret_cast<CREATESTRUCT *>(lp)->hInstance;

      CreateWindow(L"BUTTON", L"開く", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0,
                   0, 0, 0, hWnd, reinterpret_cast<HMENU>(ID_BTN_OPEN), hi,
                   nullptr);
      CreateWindow(L"EDIT", L"",
                   WS_CHILD | WS_VISIBLE | WS_BORDER | ES_READONLY |
                       ES_AUTOHSCROLL,
                   0, 0, 0, 0, hWnd, reinterpret_cast<HMENU>(ID_EDIT_FILEPATH),
                   hi, nullptr);

      g_hList = CreateWindow(
          WC_LISTVIEW, L"",
          WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT | LVS_SHOWSELALWAYS, 0,
          0, 0, 0, hWnd, reinterpret_cast<HMENU>(ID_LIST), hi, nullptr);
      ListView_SetExtendedListViewStyle(g_hList, LVS_EX_FULLROWSELECT |
                                                     LVS_EX_GRIDLINES);

      LVCOLUMN lvc{};
      lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
      lvc.iSubItem = COL_STATUS;
      lvc.pszText = const_cast<LPWSTR>(L"状態");
      lvc.cx = 48;
      ListView_InsertColumn(g_hList, COL_STATUS, &lvc);
      lvc.iSubItem = COL_KEY;
      lvc.pszText = const_cast<LPWSTR>(L"キー");
      lvc.cx = 120;
      ListView_InsertColumn(g_hList, COL_KEY, &lvc);
      lvc.iSubItem = COL_PATH;
      lvc.pszText = const_cast<LPWSTR>(L"パス");
      lvc.cx = 600;
      ListView_InsertColumn(g_hList, COL_PATH, &lvc);

      CreateWindow(L"BUTTON", L"チェック",
                   WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hWnd,
                   reinterpret_cast<HMENU>(ID_BTN_CHECK), hi, nullptr);
      CreateWindow(L"BUTTON", L"選択行を変更",
                   WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hWnd,
                   reinterpret_cast<HMENU>(ID_BTN_EDITSEL), hi, nullptr);
      CreateWindow(L"BUTTON", L"保存", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0,
                   0, 0, 0, hWnd, reinterpret_cast<HMENU>(ID_BTN_SAVE), hi,
                   nullptr);

      DragAcceptFiles(hWnd, TRUE);
      return 0;
    }

    case WM_SIZE:
      OnSize(LOWORD(lp), HIWORD(lp));
      return 0;

    case WM_COMMAND:
      switch (LOWORD(wp)) {
      case ID_BTN_OPEN:
        OnBtnOpen();
        break;
      case ID_BTN_CHECK:
        OnBtnCheck();
        break;
      case ID_BTN_EDITSEL:
        OnBtnEditSel();
        break;
      case ID_BTN_SAVE:
        OnBtnSave();
        break;
      }
      return 0;

    case WM_DROPFILES:
      OnDropFiles(reinterpret_cast<HDROP>(wp));
      return 0;

    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    }
    return DefWindowProc(hWnd, msg, wp, lp);
  } catch (const std::exception &e) {
    MessageBoxA(hWnd, e.what(), "予期しないエラー", MB_ICONERROR);
    return 0;
  } catch (...) {
    MessageBox(hWnd, L"予期しないエラーが発生しました。", L"エラー",
               MB_ICONERROR);
    return 0;
  }
}

int32_t WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR lpCmdLine,
                    int32_t nCmdShow) {
  INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES};
  InitCommonControlsEx(&icc);

  WNDCLASS wc{};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = hInstance;
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  wc.lpszClassName = L"aviutl2_relink";
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
  RegisterClass(&wc);

  HWND hWnd = CreateWindow(L"aviutl2_relink", APP_TITLE,
                           WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                           900, 600, nullptr, nullptr, hInstance, nullptr);

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
  return static_cast<int32_t>(msg.wParam);
}