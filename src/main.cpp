// warp-lite: terminal mínima para Windows con pestañas y paneles al estilo Warp.
// Win32 + ConPTY + libvterm + GDI. Sin IA, sin bloques, sin cuenta: solo terminal.
// Extra respecto a Warp: al reabrir, cada panel recupera su última orden escrita
// en el prompt (sin ejecutarla).

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "vterm.h"

// Hack, la fuente por defecto de Warp, va dentro del .exe (third_party/hack, MIT/Bitstream Vera).
__asm__(".section .rdata,\"dr\"\n.balign 4\n"
        ".global hack_regular\nhack_regular:\n.incbin \"third_party/hack/Hack-Regular.ttf\"\n.global hack_regular_end\nhack_regular_end:\n"
        ".global hack_bold\nhack_bold:\n.incbin \"third_party/hack/Hack-Bold.ttf\"\n.global hack_bold_end\nhack_bold_end:\n"
        ".global hack_italic\nhack_italic:\n.incbin \"third_party/hack/Hack-Italic.ttf\"\n.global hack_italic_end\nhack_italic_end:\n"
        ".global hack_bolditalic\nhack_bolditalic:\n.incbin \"third_party/hack/Hack-BoldItalic.ttf\"\n"
        ".global hack_bolditalic_end\nhack_bolditalic_end:\n.text\n");
extern "C" const unsigned char hack_regular[], hack_regular_end[], hack_bold[], hack_bold_end[], hack_italic[],
    hack_italic_end[], hack_bolditalic[], hack_bolditalic_end[];

#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif

// ---------------------------------------------------------------------------
// ConPTY (cargado en tiempo de ejecución: Windows 10 1809+)

typedef HRESULT(WINAPI* PFN_CreatePseudoConsole)(COORD, HANDLE, HANDLE, DWORD, void**);
typedef HRESULT(WINAPI* PFN_ResizePseudoConsole)(void*, COORD);
typedef void(WINAPI* PFN_ClosePseudoConsole)(void*);
static PFN_CreatePseudoConsole pCreatePseudoConsole;
static PFN_ResizePseudoConsole pResizePseudoConsole;
static PFN_ClosePseudoConsole pClosePseudoConsole;

// ---------------------------------------------------------------------------
// Aspecto: tema «Dark» de Warp (app/src/themes/default_themes.rs)

static const COLORREF kBg = RGB(0x05, 0x05, 0x05);
static const COLORREF kFg = RGB(0xff, 0xff, 0xff);
static const COLORREF kTitleBg = RGB(0x11, 0x11, 0x11);
static const COLORREF kTabActiveBg = RGB(0x24, 0x24, 0x24);
static const COLORREF kTabText = RGB(0x9b, 0x9b, 0x9b);
static const COLORREF kHdrActiveBg = RGB(0x1a, 0x1a, 0x1a);
static const COLORREF kAccent = RGB(0x19, 0xaa, 0xd8);
static const COLORREF kDivider = RGB(0x3a, 0x3a, 0x3a);
static const COLORREF kSelBg = RGB(0x1d, 0x4f, 0x63);
static const COLORREF kCloseHover = RGB(0xc4, 0x2b, 0x1c);
static const uint32_t kAnsi[16] = {0x616161, 0xFF8272, 0xB4FA72, 0xFEFDC2, 0xA5D5FE, 0xFF8FFD,
                                   0xD0D1FE, 0xF1F1F1, 0x8E8E8E, 0xFFC4BD, 0xD6FCB9, 0xFEFDD5,
                                   0xC1E3FE, 0xFFB1FE, 0xE5E6FE, 0xFEFFFF};
static const int kGap = 1;           // separación entre paneles (px)
static const int kPad = 4;           // margen interior de cada panel (px)
static const size_t kScrollback = 5000;

enum { WM_APP_DIRTY = WM_APP + 1, WM_APP_EXITED, WM_APP_SCRIPT };
enum { TIMER_RESTORE = 1, TIMER_SCRIPT = 2, TIMER_TOAST = 3 };

// ---------------------------------------------------------------------------
// Modelo: pestañas -> árbol de paneles

struct Pane;

struct Node {
  Node* parent = nullptr;
  Node* a = nullptr;
  Node* b = nullptr;
  bool vertical = false;  // true: a | b (lado a lado); false: a encima de b
  float ratio = 0.5f;
  Pane* pane = nullptr;   // solo en hojas
  RECT rc{};
  bool leaf() const { return pane != nullptr; }
};

struct Tab {
  Node* root = nullptr;
  Pane* active = nullptr;
  std::wstring name;  // puesto a mano (doble clic); vacío = título de la shell
};

struct Pane {
  Node* node = nullptr;
  void* hpc = nullptr;
  HANDLE inW = nullptr, outR = nullptr, proc = nullptr, reader = nullptr, wait = nullptr;
  VTerm* vt = nullptr;
  VTermScreen* scr = nullptr;
  CRITICAL_SECTION cs;       // protege vt y el estado de pantalla
  CRITICAL_SECTION writeCs;  // ordena las escrituras a la shell
  std::string outBuf;        // bytes generados por libvterm pendientes de enviar
  int rows = 24, cols = 80;
  RECT rc{};       // panel entero (cabecera incluida)
  RECT content{};  // zona de texto
  VTermPos cursor{};
  bool cursorVisible = true;
  bool altScreen = false;
  int mouseMode = 0;
  std::string titleBuf;
  std::wstring title;
  std::deque<std::vector<VTermScreenCell>> sb;
  int scrollOff = 0;
  // Última orden: dónde empezó a escribirse la línea actual.
  std::string lastCmd;  // UTF-8
  // Proceso activo (como la cabecera de Warp): orden lanzada y si la shell tiene un hijo vivo.
  DWORD pid = 0;
  std::string submittedCmd;
  std::wstring runningCmd;
  int lineRow = -1, lineCol = -1;
  // Restauración tras reiniciar.
  std::string restoreCmd;
  DWORD spawnTick = 0;
  std::atomic<DWORD> firstOut{0}, lastOut{0};
};

static HWND g_hwnd;
static std::vector<Tab> g_tabs;
static int g_activeTab = 0;
static std::set<Pane*> g_live;
static std::atomic<bool> g_dirtyPosted{false};
static HFONT g_fonts[4];  // normal, negrita, cursiva, negrita+cursiva
static HFONT g_uiFont;
static HFONT g_iconFont;  // Segoe MDL2 Assets: botones de ventana
static const int kDefaultFontPx = 13;  // DEFAULT_MONOSPACE_FONT_SIZE de Warp
static int g_fontPx = kDefaultFontPx;
static int g_zoom = 100;  // escala de toda la interfaz, en %
static int g_dpi = 96;
// Renombrado de pestaña en curso.
static int g_renameTab = -1;
static std::wstring g_renameText;
// Aviso breve en la barra de título (p. ej. la escala).
static std::wstring g_toast;
static int g_cellW = 8, g_cellH = 16;
static std::wstring g_sessionPath;
static bool g_focused = true;

// Selección (coordenadas de vista dentro de un panel).
static Pane* g_selPane = nullptr;
static VTermPos g_selA{}, g_selB{};
static bool g_selecting = false, g_hasSel = false;
// Arrastre de divisores y de paneles (por su cabecera).
static Node* g_dragNode = nullptr;
static Pane* g_dragPane = nullptr;
static Pane* g_dropTarget = nullptr;
static bool g_layoutHeaders = false;
// Barra de título propia.
enum { HIT_NONE, HIT_TAB, HIT_TABCLOSE, HIT_PLUS, HIT_MIN, HIT_MAX, HIT_CLOSE };
static int g_hoverHit = HIT_NONE, g_hoverTab = -1, g_pressedHit = HIT_NONE;

// Guion de prueba (--script) para capturas automáticas.
static std::vector<std::wstring> g_script;
static size_t g_scriptPos = 0;

static int ScaledDpi() { return MulDiv(g_dpi, g_zoom, 100); }
static int Px(int v) { return MulDiv(v, ScaledDpi(), 96); }
static int TabBarH() { return Px(38); }
static int PaneHdrH() { return std::max(Px(26), g_cellH + Px(8)); }
static Tab& CurTab() { return g_tabs[g_activeTab]; }

// ---------------------------------------------------------------------------
// Utilidades de texto

static void AppendUtf16(std::wstring& s, uint32_t cp) {
  if (cp < 0x10000) {
    s += (wchar_t)cp;
  } else {
    cp -= 0x10000;
    s += (wchar_t)(0xD800 + (cp >> 10));
    s += (wchar_t)(0xDC00 + (cp & 0x3FF));
  }
}

static void AppendUtf8(std::string& s, uint32_t cp) {
  if (cp < 0x80) {
    s += (char)cp;
  } else if (cp < 0x800) {
    s += (char)(0xC0 | (cp >> 6));
    s += (char)(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    s += (char)(0xE0 | (cp >> 12));
    s += (char)(0x80 | ((cp >> 6) & 0x3F));
    s += (char)(0x80 | (cp & 0x3F));
  } else {
    s += (char)(0xF0 | (cp >> 18));
    s += (char)(0x80 | ((cp >> 12) & 0x3F));
    s += (char)(0x80 | ((cp >> 6) & 0x3F));
    s += (char)(0x80 | (cp & 0x3F));
  }
}

static std::string ToUtf8(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s(n, 0);
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
  return s;
}

static std::wstring ToWide(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n, 0);
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}

static std::string HexEncode(const std::string& s) {
  static const char* d = "0123456789abcdef";
  std::string o;
  for (unsigned char c : s) {
    o += d[c >> 4];
    o += d[c & 15];
  }
  return o.empty() ? "-" : o;
}

static std::string HexDecode(const std::string& h) {
  std::string o;
  if (h == "-") return o;
  for (size_t i = 0; i + 1 < h.size(); i += 2) o += (char)strtol(h.substr(i, 2).c_str(), nullptr, 16);
  return o;
}

// ---------------------------------------------------------------------------
// Panel: libvterm + ConPTY

static void BlankCell(VTermScreenCell* c) {
  memset(c, 0, sizeof *c);
  c->width = 1;
  c->fg.type = VTERM_COLOR_RGB | VTERM_COLOR_DEFAULT_FG;
  c->bg.type = VTERM_COLOR_RGB | VTERM_COLOR_DEFAULT_BG;
}

static void RequestRepaint() {
  if (!g_dirtyPosted.exchange(true)) PostMessage(g_hwnd, WM_APP_DIRTY, 0, 0);
}

// libvterm escribe aquí con el lock tomado; FlushOut lo envía ya sin él, para que
// una escritura bloqueada nunca impida al hilo lector vaciar la salida.
static void OnVtOutput(const char* s, size_t len, void* user) {
  ((Pane*)user)->outBuf.append(s, len);
}

static void FlushOut(Pane* p) {
  EnterCriticalSection(&p->writeCs);
  EnterCriticalSection(&p->cs);
  std::string data;
  data.swap(p->outBuf);
  LeaveCriticalSection(&p->cs);
  DWORD w;
  if (!data.empty() && p->inW) WriteFile(p->inW, data.data(), (DWORD)data.size(), &w, nullptr);
  LeaveCriticalSection(&p->writeCs);
}

static int CbMoveCursor(VTermPos pos, VTermPos, int, void* user) {
  ((Pane*)user)->cursor = pos;
  return 1;
}

static int CbSetTermProp(VTermProp prop, VTermValue* v, void* user) {
  Pane* p = (Pane*)user;
  switch (prop) {
    case VTERM_PROP_CURSORVISIBLE: p->cursorVisible = v->boolean; break;
    case VTERM_PROP_ALTSCREEN: p->altScreen = v->boolean; p->scrollOff = 0; break;
    case VTERM_PROP_MOUSE: p->mouseMode = v->number; break;
    case VTERM_PROP_TITLE:
      if (v->string.initial) p->titleBuf.clear();
      p->titleBuf.append(v->string.str, v->string.len);
      if (v->string.final) p->title = ToWide(p->titleBuf);
      break;
    default: break;
  }
  return 1;
}

static int CbPushLine(int cols, const VTermScreenCell* cells, void* user) {
  Pane* p = (Pane*)user;
  p->sb.emplace_back(cells, cells + cols);
  if (p->sb.size() > kScrollback) p->sb.pop_front();
  if (p->scrollOff > 0) p->scrollOff = std::min<int>(p->scrollOff + 1, (int)p->sb.size());
  return 1;
}

static int CbPopLine(int cols, VTermScreenCell* cells, void* user) {
  Pane* p = (Pane*)user;
  if (p->sb.empty()) return 0;
  auto& line = p->sb.back();
  for (int i = 0; i < cols; i++) {
    if (i < (int)line.size()) cells[i] = line[i];
    else BlankCell(&cells[i]);
  }
  p->sb.pop_back();
  if (p->scrollOff > (int)p->sb.size()) p->scrollOff = (int)p->sb.size();
  return 1;
}

static int CbSbClear(void* user) {
  Pane* p = (Pane*)user;
  p->sb.clear();
  p->scrollOff = 0;
  return 1;
}

static Pane* NewPane() {
  Pane* p = new Pane();
  InitializeCriticalSection(&p->cs);
  InitializeCriticalSection(&p->writeCs);
  p->vt = vterm_new(p->rows, p->cols);
  vterm_set_utf8(p->vt, 1);
  vterm_output_set_callback(p->vt, OnVtOutput, p);
  p->scr = vterm_obtain_screen(p->vt);
  static VTermScreenCallbacks cbs = [] {
    VTermScreenCallbacks c{};
    c.movecursor = CbMoveCursor;
    c.settermprop = CbSetTermProp;
    c.sb_pushline = CbPushLine;
    c.sb_popline = CbPopLine;
    c.sb_clear = CbSbClear;
    return c;
  }();
  vterm_screen_set_callbacks(p->scr, &cbs, p);
  vterm_screen_enable_altscreen(p->scr, 1);
  vterm_screen_enable_reflow(p->scr, true);
  VTermState* st = vterm_obtain_state(p->vt);
  VTermColor fg, bg;
  vterm_color_rgb(&fg, GetRValue(kFg), GetGValue(kFg), GetBValue(kFg));
  vterm_color_rgb(&bg, GetRValue(kBg), GetGValue(kBg), GetBValue(kBg));
  vterm_state_set_default_colors(st, &fg, &bg);
  for (int i = 0; i < 16; i++) {
    VTermColor c;
    vterm_color_rgb(&c, kAnsi[i] >> 16, (kAnsi[i] >> 8) & 0xff, kAnsi[i] & 0xff);
    vterm_state_set_palette_color(st, i, &c);
  }
  vterm_screen_reset(p->scr, 1);
  g_live.insert(p);
  return p;
}

static std::wstring ShellCommand() {
  wchar_t buf[MAX_PATH * 2];
  DWORD n = GetEnvironmentVariableW(L"WARPLITE_SHELL", buf, MAX_PATH * 2);
  if (n > 0 && n < MAX_PATH * 2) return buf;
  if (SearchPathW(nullptr, L"pwsh.exe", nullptr, MAX_PATH, buf, nullptr)) return L"pwsh.exe -NoLogo";
  return L"powershell.exe -NoLogo";
}

static VOID CALLBACK OnProcessExit(PVOID ctx, BOOLEAN) {
  PostMessage(g_hwnd, WM_APP_EXITED, 0, (LPARAM)ctx);
}

static DWORD WINAPI ReaderThread(LPVOID arg) {
  Pane* p = (Pane*)arg;
  static thread_local char buf[1 << 16];
  DWORD n;
  while (ReadFile(p->outR, buf, sizeof buf, &n, nullptr) && n) {
    EnterCriticalSection(&p->cs);
    vterm_input_write(p->vt, buf, n);
    bool reply = !p->outBuf.empty();
    LeaveCriticalSection(&p->cs);
    if (reply) FlushOut(p);
    DWORD now = GetTickCount();
    if (!p->firstOut) p->firstOut = now;
    p->lastOut = now;
    RequestRepaint();
  }
  return 0;
}

static bool SpawnPane(Pane* p) {
  HANDLE inR = nullptr, outW = nullptr;
  if (!CreatePipe(&inR, &p->inW, nullptr, 0)) return false;
  if (!CreatePipe(&p->outR, &outW, nullptr, 0)) return false;
  COORD size{(SHORT)p->cols, (SHORT)p->rows};
  HRESULT hr = pCreatePseudoConsole(size, inR, outW, 0, &p->hpc);
  CloseHandle(inR);
  CloseHandle(outW);
  if (FAILED(hr)) return false;

  STARTUPINFOEXW si{};
  si.StartupInfo.cb = sizeof si;
  // Sin heredar los std handles del proceso padre (p. ej. al lanzarlo desde ssh).
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  SIZE_T attrSize = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
  si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0, attrSize);
  InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attrSize);
  UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, p->hpc,
                            sizeof(p->hpc), nullptr, nullptr);
  std::wstring cmd = ShellCommand();
  wchar_t home[MAX_PATH] = L"";
  GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
  PROCESS_INFORMATION pi{};
  BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                           EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT, nullptr,
                           home[0] ? home : nullptr, &si.StartupInfo, &pi);
  DeleteProcThreadAttributeList(si.lpAttributeList);
  HeapFree(GetProcessHeap(), 0, si.lpAttributeList);
  if (!ok) return false;
  CloseHandle(pi.hThread);
  p->proc = pi.hProcess;
  p->pid = pi.dwProcessId;
  p->spawnTick = GetTickCount();
  p->reader = CreateThread(nullptr, 0, ReaderThread, p, 0, nullptr);
  RegisterWaitForSingleObject(&p->wait, p->proc, OnProcessExit, p, INFINITE, WT_EXECUTEONLYONCE);
  return true;
}

static void DestroyPane(Pane* p) {
  g_live.erase(p);
  if (g_selPane == p) g_selPane = nullptr, g_hasSel = false;
  if (g_dragPane == p || g_dropTarget == p) g_dragPane = g_dropTarget = nullptr;
  if (p->wait) UnregisterWaitEx(p->wait, INVALID_HANDLE_VALUE);
  if (p->hpc) pClosePseudoConsole(p->hpc);
  if (p->proc) {
    if (WaitForSingleObject(p->proc, 0) == WAIT_TIMEOUT) TerminateProcess(p->proc, 0);
    CloseHandle(p->proc);
  }
  if (p->reader) {
    WaitForSingleObject(p->reader, 2000);
    CloseHandle(p->reader);
  }
  if (p->inW) CloseHandle(p->inW);
  if (p->outR) CloseHandle(p->outR);
  vterm_free(p->vt);
  DeleteCriticalSection(&p->cs);
  DeleteCriticalSection(&p->writeCs);
  delete p;
}

static void ResizePane(Pane* p, RECT r) {
  p->rc = p->content = r;
  if (g_layoutHeaders) p->content.top += PaneHdrH();
  RECT& c = p->content;
  int cols = std::max(2, (int)(c.right - c.left - 2 * kPad) / g_cellW);
  int rows = std::max(1, (int)(c.bottom - c.top - 2 * kPad) / g_cellH);
  if (cols == p->cols && rows == p->rows) return;
  p->cols = cols;
  p->rows = rows;
  EnterCriticalSection(&p->cs);
  vterm_set_size(p->vt, rows, cols);
  LeaveCriticalSection(&p->cs);
  if (p->hpc) pResizePseudoConsole(p->hpc, COORD{(SHORT)cols, (SHORT)rows});
}

// ---------------------------------------------------------------------------
// Árbol de paneles

static void LayoutNode(Node* n, RECT r) {
  n->rc = r;
  if (n->leaf()) {
    ResizePane(n->pane, r);
    return;
  }
  if (n->vertical) {
    int w = r.right - r.left - kGap;
    int wa = (int)(w * n->ratio);
    LayoutNode(n->a, {r.left, r.top, r.left + wa, r.bottom});
    LayoutNode(n->b, {r.left + wa + kGap, r.top, r.right, r.bottom});
  } else {
    int h = r.bottom - r.top - kGap;
    int ha = (int)(h * n->ratio);
    LayoutNode(n->a, {r.left, r.top, r.right, r.top + ha});
    LayoutNode(n->b, {r.left, r.top + ha + kGap, r.right, r.bottom});
  }
}

static void Relayout() {
  if (!g_hwnd || g_tabs.empty()) return;
  RECT rc;
  GetClientRect(g_hwnd, &rc);
  rc.top += TabBarH();
  for (auto& t : g_tabs) {
    g_layoutHeaders = !t.root->leaf();  // cabeceras solo con el panel dividido
    LayoutNode(t.root, rc);
  }
  InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void Leaves(Node* n, std::vector<Pane*>& out) {
  if (n->leaf()) out.push_back(n->pane);
  else {
    Leaves(n->a, out);
    Leaves(n->b, out);
  }
}

// Como Warp: los paneles de una misma fila o columna se reparten el espacio a partes iguales.
static int Units(Node* n, bool vertical) {
  if (n->leaf() || n->vertical != vertical) return 1;
  return Units(n->a, vertical) + Units(n->b, vertical);
}

static void Equalize(Node* n) {
  if (n->leaf()) return;
  int a = Units(n->a, n->vertical), b = Units(n->b, n->vertical);
  n->ratio = (float)a / (a + b);
  Equalize(n->a);
  Equalize(n->b);
}

static Node* RootOf(Node* n) {
  while (n->parent) n = n->parent;
  return n;
}

static int TabIndexOf(Pane* p) {
  Node* root = RootOf(p->node);
  for (size_t i = 0; i < g_tabs.size(); i++)
    if (g_tabs[i].root == root) return (int)i;
  return -1;
}

static void ClearSelection() {
  g_hasSel = false;
  g_selecting = false;
  g_selPane = nullptr;
}

static Pane* SplitActive(bool vertical) {
  Tab& t = CurTab();
  Node* leaf = t.active->node;
  Node* old = new Node();
  old->pane = leaf->pane;
  old->parent = leaf;
  old->pane->node = old;
  Pane* np = NewPane();
  Node* nn = new Node();
  nn->pane = np;
  nn->parent = leaf;
  np->node = nn;
  leaf->pane = nullptr;
  leaf->a = old;
  leaf->b = nn;
  leaf->vertical = vertical;
  t.active = np;
  Equalize(t.root);
  Relayout();
  SpawnPane(np);
  return np;
}

static Pane* NewTab(int at = -1) {
  Pane* p = NewPane();
  Node* n = new Node();
  n->pane = p;
  p->node = n;
  Tab t{n, p};
  if (at < 0 || at > (int)g_tabs.size()) at = (int)g_tabs.size();
  g_tabs.insert(g_tabs.begin() + at, t);
  g_activeTab = at;
  Relayout();
  SpawnPane(p);
  return p;
}

static void ClosePane(Pane* p) {
  int ti = TabIndexOf(p);
  if (ti < 0) return;
  Tab& t = g_tabs[ti];
  Node* n = p->node;
  if (!n->parent) {
    g_tabs.erase(g_tabs.begin() + ti);
    delete n;
    DestroyPane(p);
    if (g_tabs.empty()) {
      // Último panel cerrado: salimos sin pisar la sesión guardada.
      DestroyWindow(g_hwnd);
      return;
    }
    if (g_activeTab >= (int)g_tabs.size()) g_activeTab = (int)g_tabs.size() - 1;
    else if (g_activeTab > ti) g_activeTab--;
  } else {
    Node* par = n->parent;
    Node* sib = par->a == n ? par->b : par->a;
    par->pane = sib->pane;
    par->a = sib->a;
    par->b = sib->b;
    par->vertical = sib->vertical;
    par->ratio = sib->ratio;
    if (par->pane) par->pane->node = par;
    if (par->a) par->a->parent = par;
    if (par->b) par->b->parent = par;
    delete sib;
    delete n;
    if (t.active == p) {
      std::vector<Pane*> ls;
      Leaves(par, ls);
      t.active = ls.front();
    }
    Equalize(t.root);
    DestroyPane(p);
  }
  Relayout();
}

// Panel vecino en una dirección (dx,dy), como Ctrl+Alt+flechas en Warp.
static Pane* Neighbor(Pane* from, int dx, int dy) {
  std::vector<Pane*> ls;
  Leaves(CurTab().root, ls);
  RECT a = from->node->rc;
  int cx = (a.left + a.right) / 2, cy = (a.top + a.bottom) / 2;
  Pane* best = nullptr;
  long bestDist = LONG_MAX;
  for (Pane* p : ls) {
    if (p == from) continue;
    RECT b = p->node->rc;
    bool ok;
    long dist;
    if (dx) {
      ok = (dx > 0 ? b.left >= a.right : b.right <= a.left) && b.top < a.bottom && b.bottom > a.top;
      dist = labs((dx > 0 ? b.left - a.right : a.left - b.right)) * 4 +
             labs((b.top + b.bottom) / 2 - cy);
    } else {
      ok = (dy > 0 ? b.top >= a.bottom : b.bottom <= a.top) && b.left < a.right && b.right > a.left;
      dist = labs((dy > 0 ? b.top - a.bottom : a.top - b.bottom)) * 4 +
             labs((b.left + b.right) / 2 - cx);
    }
    if (ok && dist < bestDist) bestDist = dist, best = p;
  }
  return best;
}

static void SwapPanes(Pane* x, Pane* y) {
  Node* nx = x->node;
  Node* ny = y->node;
  nx->pane = y;
  ny->pane = x;
  x->node = ny;
  y->node = nx;
  Relayout();
}

static void CyclePane(int dir) {
  std::vector<Pane*> ls;
  Leaves(CurTab().root, ls);
  auto it = std::find(ls.begin(), ls.end(), CurTab().active);
  int i = (int)(it - ls.begin());
  i = (i + dir + (int)ls.size()) % (int)ls.size();
  CurTab().active = ls[i];
}

// ---------------------------------------------------------------------------
// Sesión: pestañas, divisiones y última orden de cada panel

static void SaveNode(std::ostringstream& o, Node* n) {
  if (n->leaf()) {
    o << "L " << HexEncode(n->pane->lastCmd) << "\n";
  } else {
    o << "S " << (n->vertical ? 1 : 0) << " " << n->ratio << "\n";
    SaveNode(o, n->a);
    SaveNode(o, n->b);
  }
}

static void SaveSession() {
  if (g_tabs.empty() || g_sessionPath.empty()) return;
  std::ostringstream o;
  o << "BW1\nactive " << g_activeTab << "\nui " << g_zoom << " " << g_fontPx << "\n";
  for (auto& t : g_tabs) {
    std::vector<Pane*> ls;
    Leaves(t.root, ls);
    int ai = (int)(std::find(ls.begin(), ls.end(), t.active) - ls.begin());
    o << "tab " << ai << " " << HexEncode(ToUtf8(t.name)) << "\n";
    SaveNode(o, t.root);
  }
  std::string data = o.str();
  std::wstring tmp = g_sessionPath + L".tmp";
  HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return;
  DWORD w;
  WriteFile(f, data.data(), (DWORD)data.size(), &w, nullptr);
  CloseHandle(f);
  MoveFileExW(tmp.c_str(), g_sessionPath.c_str(), MOVEFILE_REPLACE_EXISTING);
}

static Node* LoadNode(std::istringstream& in, std::vector<Pane*>& created) {
  std::string kind;
  if (!(in >> kind)) return nullptr;
  Node* n = new Node();
  if (kind == "L") {
    std::string hex;
    in >> hex;
    Pane* p = NewPane();
    p->lastCmd = HexDecode(hex);
    p->restoreCmd = p->lastCmd;
    p->node = n;
    n->pane = p;
    created.push_back(p);
    return n;
  }
  int v;
  in >> v >> n->ratio;
  n->vertical = v != 0;
  n->ratio = std::clamp(n->ratio, 0.05f, 0.95f);
  n->a = LoadNode(in, created);
  n->b = n->a ? LoadNode(in, created) : nullptr;
  if (!n->a || !n->b) return nullptr;
  n->a->parent = n->b->parent = n;
  return n;
}

static bool LoadSession() {
  HANDLE f = CreateFileW(g_sessionPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0,
                         nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  std::string data(GetFileSize(f, nullptr), 0);
  DWORD r = 0;
  ReadFile(f, data.data(), (DWORD)data.size(), &r, nullptr);
  CloseHandle(f);
  std::istringstream in(data);
  std::string magic, word;
  int active = 0;
  if (!(in >> magic) || magic != "BW1" || !(in >> word >> active)) return false;
  std::vector<Pane*> created;
  while (in >> word) {
    if (word == "ui") {
      in >> g_zoom >> g_fontPx;
      g_zoom = std::clamp(g_zoom, 50, 300);
      g_fontPx = std::clamp(g_fontPx, 6, 48);
      continue;
    }
    if (word != "tab") break;
    int ai;
    in >> ai;
    std::string name;
    auto pos = in.tellg();
    if (in >> name && name != "S" && name != "L") {  // sesiones antiguas no traen nombre
      name = HexDecode(name);
    } else {
      name.clear();
      in.clear();
      in.seekg(pos);
    }
    size_t first = created.size();
    Node* root = LoadNode(in, created);
    if (!root) break;
    Tab t{root, created[std::min(first + std::max(ai, 0), created.size() - 1)], ToWide(name)};
    g_tabs.push_back(t);
  }
  if (g_tabs.empty()) return false;
  g_activeTab = std::clamp(active, 0, (int)g_tabs.size() - 1);
  Relayout();
  for (auto& t : g_tabs) {
    std::vector<Pane*> ls;
    Leaves(t.root, ls);
    for (Pane* p : ls) SpawnPane(p);
  }
  return true;
}

// ---------------------------------------------------------------------------
// Última orden escrita

static void CellAt(Pane* p, int row, int col, VTermScreenCell* c) {
  int line = row - p->scrollOff;
  if (line >= 0) {
    if (!vterm_screen_get_cell(p->scr, VTermPos{line, col}, c)) BlankCell(c);
    return;
  }
  int idx = (int)p->sb.size() + line;
  if (idx < 0 || col >= (int)p->sb[idx].size()) {
    BlankCell(c);
    return;
  }
  *c = p->sb[idx][col];
}

static bool IsGhost(Pane* p, VTermScreenCell& c) {
  // Sugerencias en línea de PSReadLine: cursiva o gris.
  if (c.attrs.italic) return true;
  if (VTERM_COLOR_IS_DEFAULT_FG(&c.fg)) return false;
  VTermColor col = c.fg;
  vterm_screen_convert_color_to_rgb(p->scr, &col);
  return col.rgb.red == col.rgb.green && col.rgb.green == col.rgb.blue && col.rgb.red < 0xA0;
}

// Se llama con el lock tomado, justo antes de enviar Enter.
static std::string ReadTypedLine(Pane* p) {
  if (p->lineRow < 0 || p->altScreen) return {};
  int r0 = p->lineRow, c0 = p->lineCol;
  int r1 = std::max(r0, p->cursor.row);
  std::string out;
  int cutAt = -1;  // posición en `out` donde empieza una sugerencia fantasma
  for (int r = r0; r <= r1 && r < p->rows; r++) {
    for (int c = (r == r0 ? c0 : 0); c < p->cols; c++) {
      VTermScreenCell cell;
      if (!vterm_screen_get_cell(p->scr, VTermPos{r, c}, &cell)) continue;
      if (cell.chars[0] == (uint32_t)-1) continue;
      bool afterCursor = r > p->cursor.row || (r == p->cursor.row && c >= p->cursor.col);
      if (afterCursor && cutAt < 0 && cell.chars[0] && cell.chars[0] != ' ') {
        if (IsGhost(p, cell)) cutAt = (int)out.size();
        else cutAt = -2;  // texto real tras el cursor: no hay fantasma
      }
      if (!cell.chars[0]) out += ' ';
      for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i]; i++) AppendUtf8(out, cell.chars[i]);
    }
  }
  if (cutAt >= 0) out.resize(cutAt);
  while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) out.pop_back();
  size_t s = out.find_first_not_of(' ');
  return s == std::string::npos ? std::string() : out.substr(s);
}

static void MarkLineStart(Pane* p) {
  if (p->lineRow >= 0 || p->altScreen) return;
  p->lineRow = p->cursor.row;
  p->lineCol = p->cursor.col;
}

// ---------------------------------------------------------------------------
// Entrada

static VTermModifier Mods() {
  int m = VTERM_MOD_NONE;
  if (GetKeyState(VK_SHIFT) & 0x8000) m |= VTERM_MOD_SHIFT;
  if (GetKeyState(VK_CONTROL) & 0x8000) m |= VTERM_MOD_CTRL;
  if (GetKeyState(VK_MENU) & 0x8000) m |= VTERM_MOD_ALT;
  return (VTermModifier)m;
}

static void ToBottom(Pane* p) {
  p->scrollOff = 0;
  if (g_selPane == p) ClearSelection();
}

static void SendKey(Pane* p, VTermKey key, VTermModifier mod) {
  EnterCriticalSection(&p->cs);
  ToBottom(p);
  if (key == VTERM_KEY_ENTER) {
    std::string cmd = ReadTypedLine(p);
    p->lineRow = p->lineCol = -1;
    if (!cmd.empty()) p->submittedCmd = cmd;
    if (!cmd.empty() && cmd != "exit" && cmd != "logout") p->lastCmd = cmd;
    LeaveCriticalSection(&p->cs);
    if (!cmd.empty()) SaveSession();
    EnterCriticalSection(&p->cs);
  } else {
    MarkLineStart(p);
  }
  vterm_keyboard_key(p->vt, key, mod);
  LeaveCriticalSection(&p->cs);
  FlushOut(p);
}

static void SendChar(Pane* p, uint32_t cp, VTermModifier mod) {
  EnterCriticalSection(&p->cs);
  ToBottom(p);
  if (cp == 3) p->lineRow = p->lineCol = -1;  // Ctrl+C descarta la línea
  else MarkLineStart(p);
  vterm_keyboard_unichar(p->vt, cp, mod);
  LeaveCriticalSection(&p->cs);
  FlushOut(p);
}

static void SendText(Pane* p, const std::wstring& text) {
  for (size_t i = 0; i < text.size(); i++) {
    uint32_t cp = text[i];
    if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < text.size())
      cp = 0x10000 + ((cp - 0xD800) << 10) + (text[++i] - 0xDC00);
    SendChar(p, cp, VTERM_MOD_NONE);
  }
}

static std::wstring SelectionText();

static void CopySelection() {
  std::wstring s = SelectionText();
  if (s.empty() || !OpenClipboard(g_hwnd)) return;
  EmptyClipboard();
  HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t));
  memcpy(GlobalLock(h), s.c_str(), (s.size() + 1) * sizeof(wchar_t));
  GlobalUnlock(h);
  SetClipboardData(CF_UNICODETEXT, h);
  CloseClipboard();
}

static void Paste(Pane* p) {
  if (!OpenClipboard(g_hwnd)) return;
  std::wstring w;
  if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
    w = (const wchar_t*)GlobalLock(h);
    GlobalUnlock(h);
  }
  CloseClipboard();
  // Sin ESC ni otros controles: un "\e[201~" pegado no puede salirse del pegado protegido.
  std::wstring norm;
  for (size_t i = 0; i < w.size(); i++) {
    wchar_t ch = w[i];
    if (ch == L'\r' && i + 1 < w.size() && w[i + 1] == L'\n') continue;
    if (ch == L'\n') ch = L'\r';
    if ((ch < 0x20 && ch != L'\t' && ch != L'\r') || ch == 0x7f || (ch >= 0x80 && ch < 0xa0)) continue;
    norm += ch;
  }
  if (norm.empty()) return;
  std::string u = ToUtf8(norm);
  EnterCriticalSection(&p->cs);
  ToBottom(p);
  MarkLineStart(p);
  vterm_keyboard_start_paste(p->vt);
  p->outBuf += u;
  vterm_keyboard_end_paste(p->vt);
  LeaveCriticalSection(&p->cs);
  FlushOut(p);
}

static void MakeFonts();

static void Toast(const std::wstring& t) {
  g_toast = t;
  SetTimer(g_hwnd, TIMER_TOAST, 1500, nullptr);
  InvalidateRect(g_hwnd, nullptr, FALSE);
}

// Como Warp: Ctrl +/-/0 escala toda la interfaz; Ctrl+Shift +/- solo el tamaño de letra.
static void SetZoom(int z) {
  g_zoom = std::clamp(z, 50, 300);
  MakeFonts();
  Relayout();
  Toast(L"Escala " + std::to_wstring(g_zoom) + L" %");
  SaveSession();
}

static void SetFontSize(int px) {
  g_fontPx = std::clamp(px, 6, 48);
  MakeFonts();
  Relayout();
  Toast(L"Letra " + std::to_wstring(g_fontPx) + L" px");
  SaveSession();
}

static std::wstring ShortTitle(Pane* p);

// Renombrar pestaña (doble clic): Enter guarda, Esc cancela; vacío vuelve al título de la shell.
static void BeginRename(int i) {
  if (i < 0 || i >= (int)g_tabs.size()) return;
  g_renameTab = i;
  g_renameText = g_tabs[i].name.empty() ? ShortTitle(g_tabs[i].active) : g_tabs[i].name;
  InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void EndRename(bool commit) {
  if (g_renameTab < 0) return;
  if (commit && g_renameTab < (int)g_tabs.size()) {
    std::wstring t = g_renameText;
    size_t a = t.find_first_not_of(L" \t"), b = t.find_last_not_of(L" \t");
    g_tabs[g_renameTab].name = a == std::wstring::npos ? L"" : t.substr(a, b - a + 1);
    SaveSession();
  }
  g_renameTab = -1;
  InvalidateRect(g_hwnd, nullptr, FALSE);
}

// Teclas durante el renombrado; true si se consume.
static bool RenameKey(WPARAM vk) {
  if (vk == VK_RETURN) EndRename(true);
  else if (vk == VK_ESCAPE) EndRename(false);
  else if (vk == VK_BACK) {
    if (!g_renameText.empty()) g_renameText.pop_back();
    if (!g_renameText.empty() && g_renameText.back() >= 0xD800 && g_renameText.back() < 0xDC00)
      g_renameText.pop_back();
    InvalidateRect(g_hwnd, nullptr, FALSE);
  } else return false;
  return true;
}

static void ActivateTab(int i) {
  if (i < 0 || i >= (int)g_tabs.size()) return;
  g_activeTab = i;
  ClearSelection();
  Relayout();
}

// Atajos de Warp en Windows/Linux (app/src/util/bindings.rs y pane_group/mod.rs).
static bool HandleShortcut(WPARAM vk) {
  bool ctrl = GetKeyState(VK_CONTROL) & 0x8000;
  bool shift = GetKeyState(VK_SHIFT) & 0x8000;
  bool alt = GetKeyState(VK_MENU) & 0x8000;
  Pane* act = CurTab().active;
  if (ctrl && shift && !alt) {
    switch (vk) {
      case VK_OEM_PLUS:
      case VK_ADD: SetFontSize(g_fontPx + 1); return true;
      case VK_OEM_MINUS:
      case VK_SUBTRACT: SetFontSize(g_fontPx - 1); return true;
      case 'D': SplitActive(true); return true;    // dividir a la derecha
      case 'E': SplitActive(false); return true;   // dividir hacia abajo
      case 'T': NewTab(g_activeTab + 1); return true;
      case 'W': ClosePane(act); return true;
      case 'C': CopySelection(); return true;
      case 'V': Paste(act); return true;
      case VK_OEM_4: CyclePane(-1); InvalidateRect(g_hwnd, nullptr, FALSE); return true;  // Ctrl+Shift+{
      case VK_OEM_6: CyclePane(+1); InvalidateRect(g_hwnd, nullptr, FALSE); return true;  // Ctrl+Shift+}
      case VK_PRIOR:
      case VK_NEXT: {  // mover pestaña
        int j = g_activeTab + (vk == VK_PRIOR ? -1 : 1);
        if (j < 0 || j >= (int)g_tabs.size()) return true;
        std::swap(g_tabs[g_activeTab], g_tabs[j]);
        g_activeTab = j;
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return true;
      }
      case VK_TAB: ActivateTab((g_activeTab + (int)g_tabs.size() - 1) % (int)g_tabs.size()); return true;
    }
  }
  if (ctrl && alt && (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN)) {
    int dx = vk == VK_LEFT ? -1 : vk == VK_RIGHT ? 1 : 0;
    int dy = vk == VK_UP ? -1 : vk == VK_DOWN ? 1 : 0;
    if (Pane* n = Neighbor(act, dx, dy)) {
      if (shift) SwapPanes(act, n);  // Ctrl+Alt+Shift+flecha: mover el panel
      else CurTab().active = n;
      ClearSelection();
      InvalidateRect(g_hwnd, nullptr, FALSE);
    }
    return true;
  }
  if (ctrl && !shift && !alt) {
    switch (vk) {
      case VK_PRIOR: ActivateTab((g_activeTab + (int)g_tabs.size() - 1) % (int)g_tabs.size()); return true;
      case VK_NEXT:
      case VK_TAB: ActivateTab((g_activeTab + 1) % (int)g_tabs.size()); return true;
      case VK_OEM_PLUS:
      case VK_ADD: SetZoom(g_zoom + 10); return true;
      case VK_OEM_MINUS:
      case VK_SUBTRACT: SetZoom(g_zoom - 10); return true;
      case '0':
      case VK_NUMPAD0: SetZoom(100); return true;
      case 'C':
        if (g_hasSel) {
          CopySelection();
          ClearSelection();
          InvalidateRect(g_hwnd, nullptr, FALSE);
          return true;
        }
        return false;
      case 'V': Paste(act); return true;
    }
    if (vk >= '1' && vk <= '9') {
      ActivateTab(vk == '9' ? (int)g_tabs.size() - 1 : (int)(vk - '1'));
      return true;
    }
  }
  if (shift && !ctrl && !alt && (vk == VK_PRIOR || vk == VK_NEXT) && !act->altScreen) {
    EnterCriticalSection(&act->cs);
    int page = std::max(1, act->rows - 1);
    act->scrollOff = std::clamp(act->scrollOff + (vk == VK_PRIOR ? page : -page), 0, (int)act->sb.size());
    LeaveCriticalSection(&act->cs);
    InvalidateRect(g_hwnd, nullptr, FALSE);
    return true;
  }
  return false;
}

// Devuelve true si la tecla se ha consumido (no se traduce a WM_CHAR).
static bool HandleKeyDown(WPARAM vk, LPARAM) {
  if (g_tabs.empty()) return false;
  if (HandleShortcut(vk)) return true;
  Pane* p = CurTab().active;
  VTermModifier mod = Mods();
  VTermKey key = VTERM_KEY_NONE;
  switch (vk) {
    case VK_RETURN: key = VTERM_KEY_ENTER; break;
    case VK_TAB: key = VTERM_KEY_TAB; break;
    case VK_BACK: key = VTERM_KEY_BACKSPACE; break;
    case VK_ESCAPE: key = VTERM_KEY_ESCAPE; break;
    case VK_UP: key = VTERM_KEY_UP; break;
    case VK_DOWN: key = VTERM_KEY_DOWN; break;
    case VK_LEFT: key = VTERM_KEY_LEFT; break;
    case VK_RIGHT: key = VTERM_KEY_RIGHT; break;
    case VK_INSERT: key = VTERM_KEY_INS; break;
    case VK_DELETE: key = VTERM_KEY_DEL; break;
    case VK_HOME: key = VTERM_KEY_HOME; break;
    case VK_END: key = VTERM_KEY_END; break;
    case VK_PRIOR: key = VTERM_KEY_PAGEUP; break;
    case VK_NEXT: key = VTERM_KEY_PAGEDOWN; break;
    case VK_F4:
      if (mod & VTERM_MOD_ALT) return false;  // Alt+F4 cierra la ventana
      key = (VTermKey)VTERM_KEY_FUNCTION(4);
      break;
    case VK_SPACE:
      if (mod & VTERM_MOD_CTRL) {
        SendChar(p, ' ', mod);
        return true;
      }
      return false;
    default:
      if (vk >= VK_F1 && vk <= VK_F12) key = (VTermKey)VTERM_KEY_FUNCTION(vk - VK_F1 + 1);
      break;
  }
  if (key == VTERM_KEY_NONE) return false;
  SendKey(p, key, mod);
  return true;
}

// ---------------------------------------------------------------------------
// Ratón: selección, divisores, pestañas

static Pane* PaneAt(int x, int y) {
  std::vector<Pane*> ls;
  Leaves(CurTab().root, ls);
  POINT pt{x, y};
  for (Pane* p : ls)
    if (PtInRect(&p->node->rc, pt)) return p;
  return nullptr;
}

static Node* DividerAt(Node* n, int x, int y) {
  if (n->leaf()) return nullptr;
  RECT g;
  if (n->vertical) g = {n->a->rc.right - 2, n->rc.top, n->b->rc.left + 2, n->rc.bottom};
  else g = {n->rc.left, n->a->rc.bottom - 2, n->rc.right, n->b->rc.top + 2};
  POINT pt{x, y};
  if (PtInRect(&g, pt)) return n;
  if (Node* r = DividerAt(n->a, x, y)) return r;
  return DividerAt(n->b, x, y);
}

static VTermPos CellFromPoint(Pane* p, int x, int y) {
  int col = std::clamp((x - (int)p->content.left - kPad) / g_cellW, 0, p->cols - 1);
  int row = std::clamp((y - (int)p->content.top - kPad) / g_cellH, 0, p->rows - 1);
  return VTermPos{row, col};
}

static bool InSelection(Pane* p, int row, int col) {
  if (!g_hasSel || g_selPane != p) return false;
  VTermPos a = g_selA, b = g_selB;
  if (b.row < a.row || (b.row == a.row && b.col < a.col)) std::swap(a, b);
  if (row < a.row || row > b.row) return false;
  if (row == a.row && col < a.col) return false;
  if (row == b.row && col > b.col) return false;
  return true;
}

static std::wstring SelectionText() {
  Pane* p = g_selPane;
  if (!g_hasSel || !p || !g_live.count(p)) return {};
  VTermPos a = g_selA, b = g_selB;
  if (b.row < a.row || (b.row == a.row && b.col < a.col)) std::swap(a, b);
  std::wstring out;
  EnterCriticalSection(&p->cs);
  for (int r = a.row; r <= b.row; r++) {
    std::wstring line;
    for (int c = (r == a.row ? a.col : 0); c <= (r == b.row ? b.col : p->cols - 1); c++) {
      VTermScreenCell cell;
      CellAt(p, r, c, &cell);
      if (cell.chars[0] == (uint32_t)-1) continue;
      if (!cell.chars[0]) line += L' ';
      for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i]; i++) AppendUtf16(line, cell.chars[i]);
    }
    while (!line.empty() && line.back() == L' ') line.pop_back();
    out += line;
    if (r != b.row) out += L"\r\n";
  }
  LeaveCriticalSection(&p->cs);
  return out;
}

static int TabWidth() { return Px(200); }
static int TabsX0() { return Px(8); }
static int WinBtnW() { return Px(46); }

static int TitleHit(int x, int y, int* tab) {
  *tab = -1;
  if (y >= TabBarH()) return HIT_NONE;
  RECT rc;
  GetClientRect(g_hwnd, &rc);
  int bw = WinBtnW();
  if (x >= rc.right - bw) return HIT_CLOSE;
  if (x >= rc.right - 2 * bw) return HIT_MAX;
  if (x >= rc.right - 3 * bw) return HIT_MIN;
  int n = (int)g_tabs.size(), tw = TabWidth(), x0 = TabsX0();
  if (x >= x0 && x < x0 + n * tw && y >= Px(6)) {
    *tab = (x - x0) / tw;
    return x >= x0 + (*tab + 1) * tw - Px(28) ? HIT_TABCLOSE : HIT_TAB;
  }
  if (x >= x0 + n * tw && x < x0 + n * tw + Px(34)) return HIT_PLUS;
  return HIT_NONE;  // resto de la barra: arrastrar la ventana
}

static RECT HeaderRect(Pane* p) { return RECT{p->rc.left, p->rc.top, p->rc.right, p->content.top}; }

// Cabecera de panel bajo el punto; *close indica si es su botón ×.
static Pane* HeaderAt(int x, int y, bool* close) {
  std::vector<Pane*> ls;
  Leaves(CurTab().root, ls);
  POINT pt{x, y};
  for (Pane* p : ls) {
    RECT h = HeaderRect(p);
    if (h.bottom > h.top && PtInRect(&h, pt)) {
      *close = x >= h.right - Px(30);
      return p;
    }
  }
  return nullptr;
}

static void MouseToApp(Pane* p, int x, int y, int button, bool pressed) {
  VTermPos c = CellFromPoint(p, x, y);
  EnterCriticalSection(&p->cs);
  vterm_mouse_move(p->vt, c.row, c.col, Mods());
  if (button) vterm_mouse_button(p->vt, button, pressed, Mods());
  LeaveCriticalSection(&p->cs);
  FlushOut(p);
}

static void OnMouseWheel(int x, int y, int delta) {
  Pane* p = PaneAt(x, y);
  if (!p) return;
  int lines = 3 * (delta > 0 ? 1 : -1);
  if (p->mouseMode) {
    MouseToApp(p, x, y, delta > 0 ? 4 : 5, true);
  } else if (p->altScreen) {
    for (int i = 0; i < 3; i++) SendKey(p, delta > 0 ? VTERM_KEY_UP : VTERM_KEY_DOWN, VTERM_MOD_NONE);
  } else {
    EnterCriticalSection(&p->cs);
    p->scrollOff = std::clamp(p->scrollOff + lines, 0, (int)p->sb.size());
    LeaveCriticalSection(&p->cs);
  }
  InvalidateRect(g_hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Dibujo

static void MakeFonts() {
  for (auto& f : g_fonts)
    if (f) DeleteObject(f), f = nullptr;
  if (g_uiFont) DeleteObject(g_uiFont);
  int h = -MulDiv(g_fontPx, ScaledDpi(), 96);
  const wchar_t* face = L"Hack";
  HDC dc = GetDC(nullptr);
  for (int i = 0; i < 4; i++) {
    g_fonts[i] = CreateFontW(h, 0, 0, 0, (i & 1) ? FW_BOLD : FW_NORMAL, (i & 2) ? TRUE : FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             FIXED_PITCH | FF_MODERN, face);
    if (i == 0) {
      HGDIOBJ old = SelectObject(dc, g_fonts[0]);
      wchar_t got[64];
      GetTextFaceW(dc, 64, got);
      SelectObject(dc, old);
      if (_wcsicmp(got, face) != 0) {  // si Hack no cargara: Consolas
        DeleteObject(g_fonts[0]);
        face = L"Consolas";
        i = -1;
        continue;
      }
    }
  }
  g_uiFont = CreateFontW(-Px(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, 0, L"Segoe UI");
  if (g_iconFont) DeleteObject(g_iconFont);
  g_iconFont = CreateFontW(-Px(9), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, 0, L"Segoe MDL2 Assets");
  HGDIOBJ old = SelectObject(dc, g_fonts[0]);
  TEXTMETRICW tm;
  GetTextMetricsW(dc, &tm);
  SIZE sz;
  GetTextExtentPoint32W(dc, L"M", 1, &sz);
  SelectObject(dc, old);
  ReleaseDC(nullptr, dc);
  g_cellW = std::max(1, (int)sz.cx);
  g_cellH = std::max(1, (int)tm.tmHeight);
}

static COLORREF ResolveColor(Pane* p, VTermColor c, bool fg) {
  if (fg && VTERM_COLOR_IS_DEFAULT_FG(&c)) return kFg;
  if (!fg && VTERM_COLOR_IS_DEFAULT_BG(&c)) return kBg;
  vterm_screen_convert_color_to_rgb(p->scr, &c);
  return RGB(c.rgb.red, c.rgb.green, c.rgb.blue);
}

static void Fill(HDC dc, RECT r, COLORREF c) {
  SetBkColor(dc, c);
  ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &r, nullptr, 0, nullptr);
}

struct Style {
  COLORREF fg, bg;
  int font;
  int underline;
  bool strike;
  bool operator!=(const Style& o) const {
    return fg != o.fg || bg != o.bg || font != o.font || underline != o.underline || strike != o.strike;
  }
};

static void DrawPane(HDC dc, Pane* p, bool active, bool showFocus) {
  EnterCriticalSection(&p->cs);
  Fill(dc, p->content, kBg);
  int ox = p->content.left + kPad, oy = p->content.top + kPad;
  std::wstring txt;
  std::vector<INT> dx;
  for (int row = 0; row < p->rows; row++) {
    int y = oy + row * g_cellH;
    int col = 0;
    while (col < p->cols) {
      int start = col;
      Style st{};
      txt.clear();
      dx.clear();
      while (col < p->cols) {
        VTermScreenCell c;
        CellAt(p, row, col, &c);
        if (c.chars[0] == (uint32_t)-1) {  // continuación de carácter ancho
          col++;
          continue;
        }
        Style s;
        s.fg = ResolveColor(p, c.fg, true);
        s.bg = ResolveColor(p, c.bg, false);
        if (c.attrs.reverse) std::swap(s.fg, s.bg);
        if (InSelection(p, row, col)) s.bg = kSelBg;
        s.font = (c.attrs.bold ? 1 : 0) | (c.attrs.italic ? 2 : 0);
        s.underline = c.attrs.underline;
        s.strike = c.attrs.strike;
        if (col != start && s != st) break;
        st = s;
        int w = std::max(1, (int)c.width);
        if (!c.chars[0]) {
          txt += L' ';
          dx.push_back(g_cellW * w);
        } else {
          for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && c.chars[i]; i++) {
            size_t before = txt.size();
            AppendUtf16(txt, c.chars[i]);
            for (size_t k = before; k < txt.size(); k++) dx.push_back(i == 0 && k == before ? g_cellW * w : 0);
          }
        }
        col += w;
      }
      RECT r{ox + start * g_cellW, y, ox + col * g_cellW, y + g_cellH};
      SelectObject(dc, g_fonts[st.font]);
      SetTextColor(dc, st.fg);
      SetBkColor(dc, st.bg);
      ExtTextOutW(dc, r.left, y, ETO_OPAQUE | ETO_CLIPPED, &r, txt.c_str(), (UINT)txt.size(), dx.data());
      if (st.underline || st.strike) {
        int ly = st.strike ? y + g_cellH / 2 : y + g_cellH - 2;
        Fill(dc, RECT{r.left, ly, r.right, ly + 1}, st.fg);
      }
    }
  }
  // Cursor
  if (active && p->cursorVisible && p->scrollOff == 0) {
    RECT cr{ox + p->cursor.col * g_cellW, oy + p->cursor.row * g_cellH, 0, 0};
    cr.right = cr.left + g_cellW;
    cr.bottom = cr.top + g_cellH;
    if (showFocus) {
      VTermScreenCell c;
      CellAt(p, p->cursor.row, p->cursor.col, &c);
      std::wstring ch;
      if (c.chars[0] && c.chars[0] != (uint32_t)-1) AppendUtf16(ch, c.chars[0]);
      SetBkColor(dc, kFg);
      SetTextColor(dc, kBg);
      SelectObject(dc, g_fonts[0]);
      ExtTextOutW(dc, cr.left, cr.top, ETO_OPAQUE | ETO_CLIPPED, &cr, ch.c_str(), (UINT)ch.size(), nullptr);
    } else {
      HBRUSH b = CreateSolidBrush(kFg);
      FrameRect(dc, &cr, b);
      DeleteObject(b);
    }
  }
  LeaveCriticalSection(&p->cs);
}

static void DrawDividers(HDC dc, Node* n) {
  if (n->leaf()) return;
  RECT g = n->vertical ? RECT{n->a->rc.right, n->rc.top, n->b->rc.left, n->rc.bottom}
                       : RECT{n->rc.left, n->a->rc.bottom, n->rc.right, n->b->rc.top};
  Fill(dc, g, kDivider);
  DrawDividers(dc, n->a);
  DrawDividers(dc, n->b);
}

static std::wstring ShortTitle(Pane* p) {
  if (!p->runningCmd.empty()) return p->runningCmd;
  std::wstring t = p->title;
  if (t.empty()) t = L"Terminal";
  size_t slash = t.find_last_of(L"\\/");
  if (slash != std::wstring::npos && slash + 1 < t.size()) t = t.substr(slash + 1);
  return t;
}

// Cada panel cuyo shell tenga un proceso hijo vivo muestra la orden que lo lanzó
// (o el nombre del programa); al terminar vuelve al título normal.
static void UpdateRunning() {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return;
  std::vector<std::pair<DWORD, std::wstring>> children;  // (padre, exe)
  PROCESSENTRY32W pe{sizeof pe};
  for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
    if (_wcsicmp(pe.szExeFile, L"conhost.exe") != 0) children.push_back({pe.th32ParentProcessID, pe.szExeFile});
  CloseHandle(snap);
  bool changed = false;
  for (Pane* p : g_live) {
    if (!p->pid) continue;
    const std::wstring* exe = nullptr;
    for (auto& c : children)
      if (c.first == p->pid) exe = &c.second;
    std::wstring want;
    if (exe) want = p->runningCmd.empty() ? (p->submittedCmd.empty() ? *exe : ToWide(p->submittedCmd)) : p->runningCmd;
    if (want != p->runningCmd) p->runningCmd = want, changed = true;
  }
  if (changed) InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void Text(HDC dc, const std::wstring& s, RECT r, COLORREF c, UINT flags) {
  SetTextColor(dc, c);
  DrawTextW(dc, s.c_str(), (int)s.size(), &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | flags);
}

static void DrawTitleBar(HDC dc, int w) {
  int th = TabBarH();
  Fill(dc, RECT{0, 0, w, th}, kTitleBg);
  SetBkMode(dc, TRANSPARENT);
  SelectObject(dc, g_uiFont);
  int tw = TabWidth(), x0 = TabsX0();
  for (int i = 0; i < (int)g_tabs.size(); i++) {
    RECT r{x0 + i * tw, Px(6), x0 + (i + 1) * tw - Px(2), th};
    bool act = i == g_activeTab, hover = g_hoverTab == i;
    if (act || hover) Fill(dc, r, act ? kTabActiveBg : RGB(0x1a, 0x1a, 0x1a));
    RECT tr{r.left + Px(28), r.top, r.right - Px(28), r.bottom};
    if (i == g_renameTab) {  // editando el nombre: texto con cursor
      Fill(dc, r, kTabActiveBg);
      Fill(dc, RECT{r.left, r.bottom - Px(2), r.right, r.bottom}, kAccent);
      SIZE sz{};
      GetTextExtentPoint32W(dc, g_renameText.c_str(), (int)g_renameText.size(), &sz);
      int cx = std::min((int)(tr.left + tr.right + sz.cx) / 2, (int)tr.right);
      Text(dc, g_renameText, tr, kFg, DT_CENTER);
      Fill(dc, RECT{cx + 1, r.top + Px(9), cx + 1 + std::max(1, Px(1)), r.bottom - Px(9)}, kFg);
      continue;
    }
    const std::wstring& label = g_tabs[i].name.empty() ? ShortTitle(g_tabs[i].active) : g_tabs[i].name;
    Text(dc, label, tr, act ? kFg : kTabText, DT_CENTER | DT_END_ELLIPSIS);
    if (act || hover) {
      RECT xr{r.right - Px(28), r.top, r.right - Px(6), r.bottom};
      bool hx = g_hoverHit == HIT_TABCLOSE && hover;
      SelectObject(dc, g_iconFont);
      Text(dc, L"\uE8BB", xr, hx ? kFg : kTabText, DT_CENTER);
      SelectObject(dc, g_uiFont);
    }
  }
  int px = x0 + (int)g_tabs.size() * tw;
  RECT plus{px, Px(6), px + Px(34), th};
  if (g_hoverHit == HIT_PLUS) Fill(dc, plus, RGB(0x1a, 0x1a, 0x1a));
  Text(dc, L"+", plus, g_hoverHit == HIT_PLUS ? kFg : kTabText, DT_CENTER);
  if (!g_toast.empty()) {
    RECT tr{px + Px(44), 0, w - 3 * WinBtnW() - Px(12), th};
    Text(dc, g_toast, tr, kFg, DT_RIGHT);
  }
  // Minimizar, maximizar/restaurar, cerrar
  SelectObject(dc, g_iconFont);
  int bw = WinBtnW();
  const wchar_t* glyphs[3] = {L"\uE921", IsZoomed(g_hwnd) ? L"\uE923" : L"\uE922", L"\uE8BB"};
  for (int k = 0; k < 3; k++) {
    RECT r{w - (3 - k) * bw, 0, w - (2 - k) * bw, th};
    bool hover = g_hoverHit == HIT_MIN + k;
    if (hover) Fill(dc, r, k == 2 ? kCloseHover : RGB(0x2a, 0x2a, 0x2a));
    Text(dc, glyphs[k], r, hover ? kFg : kTabText, DT_CENTER);
  }
  SetBkMode(dc, OPAQUE);
}

static void DrawPaneHeader(HDC dc, Pane* p, bool active) {
  RECT h = HeaderRect(p);
  if (h.bottom <= h.top) return;
  Fill(dc, h, active ? kHdrActiveBg : kBg);
  SetBkMode(dc, TRANSPARENT);
  SelectObject(dc, g_uiFont);
  RECT tr{h.left + Px(34), h.top, h.right - Px(34), h.bottom};
  Text(dc, ShortTitle(p), tr, active ? kFg : kTabText, DT_CENTER | DT_END_ELLIPSIS);
  RECT xr{h.right - Px(30), h.top, h.right - Px(6), h.bottom};
  SelectObject(dc, g_iconFont);
  Text(dc, L"\uE8BB", xr, kTabText, DT_CENTER);
  SetBkMode(dc, OPAQUE);
}

static void Render(HDC dc, int w, int h) {
  RECT all{0, 0, w, h};
  Fill(dc, all, kBg);
  DrawTitleBar(dc, w);
  if (g_tabs.empty()) return;
  Tab& t = CurTab();
  std::vector<Pane*> ls;
  Leaves(t.root, ls);
  for (Pane* p : ls) {
    DrawPane(dc, p, p == t.active, g_focused);
    DrawPaneHeader(dc, p, p == t.active);
  }
  DrawDividers(dc, t.root);
  if (g_dropTarget) {  // destino al arrastrar un panel por su cabecera
    HBRUSH b = CreateSolidBrush(kAccent);
    RECT r = g_dropTarget->rc;
    for (int i = 0; i < Px(2); i++, InflateRect(&r, -1, -1)) FrameRect(dc, &r, b);
    DeleteObject(b);
  }
}

static bool SaveScreenshot(const std::wstring& path) {
  RECT rc;
  GetClientRect(g_hwnd, &rc);
  int w = rc.right, h = rc.bottom;
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof bi.bmiHeader;
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  void* bits = nullptr;
  HDC dc = CreateCompatibleDC(nullptr);
  HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  HGDIOBJ old = SelectObject(dc, bmp);
  Render(dc, w, h);
  GdiFlush();
  BITMAPFILEHEADER fh{};
  DWORD size = (DWORD)w * h * 4;
  fh.bfType = 0x4D42;
  fh.bfOffBits = sizeof fh + sizeof bi.bmiHeader;
  fh.bfSize = fh.bfOffBits + size;
  HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
  bool ok = f != INVALID_HANDLE_VALUE;
  if (ok) {
    DWORD wr;
    WriteFile(f, &fh, sizeof fh, &wr, nullptr);
    WriteFile(f, &bi.bmiHeader, sizeof bi.bmiHeader, &wr, nullptr);
    WriteFile(f, bits, size, &wr, nullptr);
    CloseHandle(f);
  }
  SelectObject(dc, old);
  DeleteObject(bmp);
  DeleteDC(dc);
  return ok;
}

// ---------------------------------------------------------------------------
// Guion de prueba: pasos separados por '|', uno cada ~1,2 s.
//   type:<texto> enter splitR splitD tab close-pane left right up down swap-<dir> rename:<nombre>
//   zoom:<%> shot:<fichero.bmp> wait quit

static void RunScriptStep() {
  if (g_scriptPos >= g_script.size()) return;
  std::wstring s = g_script[g_scriptPos++];
  Pane* p = g_tabs.empty() ? nullptr : CurTab().active;
  int delay = 1200;
  if (s.rfind(L"type:", 0) == 0 && p) SendText(p, s.substr(5)), delay = 300;
  else if (s == L"enter" && p) SendKey(p, VTERM_KEY_ENTER, VTERM_MOD_NONE);
  else if (s == L"splitR") SplitActive(true), delay = 2500;
  else if (s == L"splitD") SplitActive(false), delay = 2500;
  else if (s == L"tab") NewTab(g_activeTab + 1), delay = 2500;
  else if (s == L"close-pane" && p) ClosePane(p);
  else if (s == L"left" || s == L"right" || s == L"up" || s == L"down") {
    int dx = s == L"left" ? -1 : s == L"right" ? 1 : 0, dy = s == L"up" ? -1 : s == L"down" ? 1 : 0;
    if (Pane* n = Neighbor(p, dx, dy)) CurTab().active = n;
    delay = 200;
  } else if (s.rfind(L"swap-", 0) == 0 && p) {
    std::wstring d = s.substr(5);
    int dx = d == L"left" ? -1 : d == L"right" ? 1 : 0, dy = d == L"up" ? -1 : d == L"down" ? 1 : 0;
    if (Pane* n = Neighbor(p, dx, dy)) SwapPanes(p, n);
    delay = 300;
  } else if (s.rfind(L"rename:", 0) == 0) {
    BeginRename(g_activeTab);
    g_renameText = s.substr(7);
    EndRename(true);
  } else if (s.rfind(L"zoom:", 0) == 0) {
    SetZoom(_wtoi(s.substr(5).c_str()));
    delay = 600;
  } else if (s.rfind(L"shot:", 0) == 0) SaveScreenshot(s.substr(5)), delay = 200;
  else if (s == L"quit") {
    PostMessage(g_hwnd, WM_CLOSE, 0, 0);
    return;
  }
  InvalidateRect(g_hwnd, nullptr, FALSE);
  SetTimer(g_hwnd, TIMER_SCRIPT, delay, nullptr);
}

// ---------------------------------------------------------------------------
// Ventana

static void ApplyDpi(UINT dpi) {
  g_dpi = dpi ? dpi : 96;
  MakeFonts();
}

static UINT WindowDpi(HWND h) {
  typedef UINT(WINAPI * PFN)(HWND);
  static PFN fn = (PFN)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
  return fn ? fn(h) : 96;
}

static int SysMetric(int idx, UINT dpi) {
  typedef int(WINAPI * PFN)(int, UINT);
  static PFN fn = (PFN)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetSystemMetricsForDpi");
  return fn ? fn(idx, dpi) : GetSystemMetrics(idx);
}

static void SetHover(int hit, int tab) {
  if (hit == g_hoverHit && tab == g_hoverTab) return;
  g_hoverHit = hit;
  g_hoverTab = tab;
  RECT r{0, 0, 1 << 15, TabBarH()};
  InvalidateRect(g_hwnd, &r, FALSE);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_NCCALCSIZE:
      // Sin barra de título de Windows: las pestañas ocupan su sitio, como en Warp.
      if (wp) {
        RECT& r = ((NCCALCSIZE_PARAMS*)lp)->rgrc[0];
        UINT dpi = WindowDpi(hwnd);
        int pad = SysMetric(SM_CXPADDEDBORDER, dpi);
        int fx = SysMetric(SM_CXFRAME, dpi) + pad, fy = SysMetric(SM_CYFRAME, dpi) + pad;
        r.left += fx;
        r.right -= fx;
        r.bottom -= fy;
        if (IsZoomed(hwnd)) r.top += fy;
        return 0;
      }
      break;
    case WM_NCHITTEST: {
      LRESULT hit = DefWindowProcW(hwnd, msg, wp, lp);
      if (hit != HTCLIENT) return hit;
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      ScreenToClient(hwnd, &pt);
      int border = Px(6);
      if (!IsZoomed(hwnd) && pt.y < border) {
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (pt.x < 2 * border) return HTTOPLEFT;
        if (pt.x >= rc.right - 2 * border) return HTTOPRIGHT;
        return HTTOP;
      }
      int tab;
      if (pt.y < TabBarH() && TitleHit(pt.x, pt.y, &tab) == HIT_NONE) return HTCAPTION;
      return HTCLIENT;
    }
    case WM_NCMOUSEMOVE:
    case WM_MOUSELEAVE:
      SetHover(HIT_NONE, -1);
      break;
    case WM_CREATE: {
      g_hwnd = hwnd;
      ApplyDpi(WindowDpi(hwnd));
      if (!LoadSession()) NewTab();
      SetTimer(hwnd, TIMER_RESTORE, 100, nullptr);
      if (!g_script.empty()) SetTimer(hwnd, TIMER_SCRIPT, 3000, nullptr);
      return 0;
    }
    case WM_SIZE: Relayout(); return 0;
    case WM_DPICHANGED: {
      ApplyDpi(HIWORD(wp));
      RECT* r = (RECT*)lp;
      SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
      Relayout();
      return 0;
    }
    case WM_APP_DIRTY:
      g_dirtyPosted = false;
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    case WM_APP_EXITED: {
      Pane* p = (Pane*)lp;
      if (g_live.count(p)) ClosePane(p);
      return 0;
    }
    case WM_TIMER:
      if (wp == TIMER_TOAST) {
        KillTimer(hwnd, TIMER_TOAST);
        g_toast.clear();
        InvalidateRect(hwnd, nullptr, FALSE);
      } else if (wp == TIMER_SCRIPT) {
        KillTimer(hwnd, TIMER_SCRIPT);
        RunScriptStep();
      } else if (wp == TIMER_RESTORE) {
        static int ticks = 0;
        if (++ticks % 5 == 0) UpdateRunning();
        // Escribe la última orden en el prompt cuando la shell ha terminado de arrancar.
        DWORD now = GetTickCount();
        for (Pane* p : g_live) {
          if (p->restoreCmd.empty() || !p->proc) continue;
          bool settled = p->firstOut && now - p->lastOut > 400;
          if (!settled && now - p->spawnTick < 8000) continue;
          EnterCriticalSection(&p->cs);
          MarkLineStart(p);
          p->outBuf += p->restoreCmd;  // sin Enter: solo queda escrita
          p->restoreCmd.clear();
          LeaveCriticalSection(&p->cs);
          FlushOut(p);
        }
      }
      return 0;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hwnd, &ps);
      RECT rc;
      GetClientRect(hwnd, &rc);
      HDC mem = CreateCompatibleDC(dc);
      HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
      HGDIOBJ old = SelectObject(mem, bmp);
      Render(mem, rc.right, rc.bottom);
      BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
      SelectObject(mem, old);
      DeleteObject(bmp);
      DeleteDC(mem);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SETFOCUS: g_focused = true; InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_KILLFOCUS: g_focused = false; InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_CHAR:
    case WM_SYSCHAR: {
      if (g_tabs.empty()) return 0;
      if (g_renameTab >= 0) {
        if (wp >= 0x20 && msg == WM_CHAR && g_renameText.size() < 60) g_renameText += (wchar_t)wp;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      static wchar_t high = 0;
      uint32_t cp = (uint32_t)wp;
      if (cp >= 0xD800 && cp < 0xDC00) {
        high = (wchar_t)cp;
        return 0;
      }
      if (cp >= 0xDC00 && cp < 0xE000 && high) cp = 0x10000 + ((high - 0xD800) << 10) + (cp - 0xDC00);
      high = 0;
      SendChar(CurTab().active, cp, msg == WM_SYSCHAR ? VTERM_MOD_ALT : VTERM_MOD_NONE);
      return 0;
    }
    case WM_SYSCOMMAND:
      if ((wp & 0xFFF0) == SC_KEYMENU) return 0;  // Alt no abre el menú de sistema
      break;
    case WM_MOUSEWHEEL: {
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      ScreenToClient(hwnd, &pt);
      if (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) SetZoom(g_zoom + (GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 10 : -10));
      else if (!g_tabs.empty()) OnMouseWheel(pt.x, pt.y, GET_WHEEL_DELTA_WPARAM(wp));
      return 0;
    }
    case WM_LBUTTONDBLCLK: {
      int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp), i;
      if (!g_tabs.empty() && y < TabBarH() && TitleHit(x, y, &i) == HIT_TAB) {
        ActivateTab(i);
        BeginRename(i);
        return 0;
      }
      return WndProc(hwnd, WM_LBUTTONDOWN, wp, lp);
    }
    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_RBUTTONDOWN: {
      int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
      if (g_tabs.empty()) return 0;
      SetFocus(hwnd);
      if (g_renameTab >= 0) EndRename(true);  // clic fuera: guarda el nombre
      if (y < TabBarH()) {
        int i;
        int hit = TitleHit(x, y, &i);
        if ((hit == HIT_TAB && msg == WM_MBUTTONDOWN) || (hit == HIT_TABCLOSE && msg == WM_LBUTTONDOWN)) {
          std::vector<Pane*> ls;
          Leaves(g_tabs[i].root, ls);
          for (Pane* p : ls) ClosePane(p);
        } else if (msg == WM_LBUTTONDOWN) {
          if (hit == HIT_TAB) ActivateTab(i);
          else if (hit == HIT_PLUS) NewTab();
          else if (hit >= HIT_MIN) g_pressedHit = hit;  // se ejecuta al soltar
        }
        return 0;
      }
      if (msg == WM_LBUTTONDOWN) {
        bool close;
        if (Pane* p = HeaderAt(x, y, &close)) {
          if (close) {
            ClosePane(p);
          } else {  // activar y empezar a arrastrar el panel
            CurTab().active = p;
            g_dragPane = p;
            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
          }
          return 0;
        }
      }
      if (msg == WM_LBUTTONDOWN)
        if (Node* d = DividerAt(CurTab().root, x, y)) {
          g_dragNode = d;
          SetCapture(hwnd);
          return 0;
        }
      Pane* p = PaneAt(x, y);
      if (!p) return 0;
      CurTab().active = p;
      bool shift = GetKeyState(VK_SHIFT) & 0x8000;
      if (p->mouseMode && !shift) {
        MouseToApp(p, x, y, msg == WM_LBUTTONDOWN ? 1 : msg == WM_MBUTTONDOWN ? 2 : 3, true);
      } else if (msg == WM_LBUTTONDOWN) {
        g_selPane = p;
        g_selA = g_selB = CellFromPoint(p, x, y);
        g_selecting = true;
        g_hasSel = false;
        SetCapture(hwnd);
      } else if (msg == WM_RBUTTONDOWN) {  // clic derecho: copiar si hay selección, si no pegar
        if (g_hasSel) {
          CopySelection();
          ClearSelection();
        } else {
          Paste(p);
        }
      }
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }
    case WM_MOUSEMOVE: {
      int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
      if (g_tabs.empty()) return 0;
      {
        int tab;
        int hit = y < TabBarH() ? TitleHit(x, y, &tab) : HIT_NONE;
        SetHover(hit, tab);
        TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
      }
      if (g_dragPane) {
        Pane* t = PaneAt(x, y);
        if (t == g_dragPane) t = nullptr;
        if (t != g_dropTarget) g_dropTarget = t, InvalidateRect(hwnd, nullptr, FALSE);
        SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
        return 0;
      }
      if (g_dragNode) {
        RECT r = g_dragNode->rc;
        float ratio = g_dragNode->vertical ? (float)(x - r.left) / std::max(1L, r.right - r.left)
                                           : (float)(y - r.top) / std::max(1L, r.bottom - r.top);
        g_dragNode->ratio = std::clamp(ratio, 0.08f, 0.92f);
        Relayout();
        return 0;
      }
      if (g_selecting && g_selPane) {
        g_selB = CellFromPoint(g_selPane, x, y);
        g_hasSel = g_selA.row != g_selB.row || g_selA.col != g_selB.col;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      if (Pane* p = PaneAt(x, y); p && p->mouseMode >= VTERM_PROP_MOUSE_DRAG && (wp & MK_LBUTTON)) {
        MouseToApp(p, x, y, 0, false);
      }
      Node* d = y >= TabBarH() ? DividerAt(CurTab().root, x, y) : nullptr;
      bool onHeader;
      LPCWSTR cur = d ? (d->vertical ? IDC_SIZEWE : IDC_SIZENS)
                    : (y < TabBarH() || HeaderAt(x, y, &onHeader)) ? IDC_ARROW : IDC_IBEAM;
      SetCursor(LoadCursor(nullptr, cur));
      return 0;
    }
    case WM_LBUTTONUP:
    case WM_MBUTTONUP:
    case WM_RBUTTONUP: {
      int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
      if (g_pressedHit != HIT_NONE) {
        int tab, hit = TitleHit(x, y, &tab);
        if (hit == g_pressedHit) {
          if (hit == HIT_MIN) ShowWindow(hwnd, SW_MINIMIZE);
          else if (hit == HIT_MAX) ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
          else if (hit == HIT_CLOSE) PostMessage(hwnd, WM_CLOSE, 0, 0);
        }
        g_pressedHit = HIT_NONE;
        return 0;
      }
      if (g_dragPane) {  // soltar un panel sobre otro: intercambiarlos
        Pane* target = g_dropTarget;
        Pane* dragged = g_dragPane;
        g_dragPane = g_dropTarget = nullptr;
        ReleaseCapture();
        if (target && g_live.count(target) && g_live.count(dragged)) SwapPanes(dragged, target);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      if (g_dragNode || g_selecting) {
        g_dragNode = nullptr;
        g_selecting = false;
        ReleaseCapture();
        return 0;
      }
      if (g_tabs.empty() || y < TabBarH()) return 0;
      if (Pane* p = PaneAt(x, y); p && p->mouseMode && !(GetKeyState(VK_SHIFT) & 0x8000))
        MouseToApp(p, x, y, msg == WM_LBUTTONUP ? 1 : msg == WM_MBUTTONUP ? 2 : 3, false);
      return 0;
    }
    case WM_SETCURSOR:
      if (LOWORD(lp) == HTCLIENT) return TRUE;  // lo fija WM_MOUSEMOVE
      break;
    case WM_CLOSE:
      SaveSession();
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY: {
      std::vector<Pane*> all(g_live.begin(), g_live.end());
      for (Pane* p : all) DestroyPane(p);
      g_tabs.clear();
      PostQuitMessage(0);
      return 0;
    }
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

static std::wstring SessionPath() {
  wchar_t buf[MAX_PATH];
  if (GetEnvironmentVariableW(L"WARPLITE_SESSION", buf, MAX_PATH)) return buf;
  if (!GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH)) return L"warp-lite-session.txt";
  std::wstring dir = std::wstring(buf) + L"\\warp-lite";
  CreateDirectoryW(dir.c_str(), nullptr);
  std::wstring path = dir + L"\\session.txt";
  // Se llamaba better-warp: conserva la sesión anterior.
  CopyFileW((std::wstring(buf) + L"\\BetterWarp\\session.txt").c_str(), path.c_str(), TRUE);
  return path;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
  HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
  pCreatePseudoConsole = (PFN_CreatePseudoConsole)GetProcAddress(k32, "CreatePseudoConsole");
  pResizePseudoConsole = (PFN_ResizePseudoConsole)GetProcAddress(k32, "ResizePseudoConsole");
  pClosePseudoConsole = (PFN_ClosePseudoConsole)GetProcAddress(k32, "ClosePseudoConsole");
  if (!pCreatePseudoConsole) {
    MessageBoxW(nullptr, L"Hace falta Windows 10 1809 o posterior (ConPTY).", L"warp-lite", MB_ICONERROR);
    return 1;
  }
  typedef BOOL(WINAPI * PFN_Dpi)(HANDLE);
  if (auto fn = (PFN_Dpi)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"))
    fn((HANDLE)-4);  // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2

  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  for (int i = 1; i + 1 < argc; i++) {
    if (!wcscmp(argv[i], L"--script")) {
      std::wstring s = argv[++i];
      size_t pos;
      while ((pos = s.find(L'|')) != std::wstring::npos) g_script.push_back(s.substr(0, pos)), s.erase(0, pos + 1);
      if (!s.empty()) g_script.push_back(s);
    }
  }
  LocalFree(argv);
  g_sessionPath = SessionPath();
  struct { const unsigned char *a, *b; } fonts[] = {{hack_regular, hack_regular_end},
                                                     {hack_bold, hack_bold_end},
                                                     {hack_italic, hack_italic_end},
                                                     {hack_bolditalic, hack_bolditalic_end}};
  for (auto& f : fonts) {
    DWORD n = 0;
    AddFontMemResourceEx((void*)f.a, (DWORD)(f.b - f.a), nullptr, &n);
  }

  WNDCLASSEXW wc{sizeof wc};
  wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursor(nullptr, IDC_IBEAM);
  wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
  wc.lpszClassName = L"WarpLite";
  RegisterClassExW(&wc);
  HWND hwnd = CreateWindowExW(0, L"WarpLite", L"warp-lite", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              1280, 760, nullptr, nullptr, inst, nullptr);
  if (!hwnd) return 1;
  SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
  ShowWindow(hwnd, show);
  UpdateWindow(hwnd);

  MSG m;
  while (GetMessageW(&m, nullptr, 0, 0)) {
    if (g_renameTab >= 0 && (m.message == WM_KEYDOWN || m.message == WM_SYSKEYDOWN)) {
      if (!RenameKey(m.wParam)) TranslateMessage(&m), DispatchMessageW(&m);
      continue;
    }
    if ((m.message == WM_KEYDOWN || m.message == WM_SYSKEYDOWN) && m.hwnd == g_hwnd &&
        HandleKeyDown(m.wParam, m.lParam)) {
      InvalidateRect(g_hwnd, nullptr, FALSE);
      continue;
    }
    TranslateMessage(&m);
    DispatchMessageW(&m);
  }
  return 0;
}
