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
#include <map>
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
static const COLORREF kChipBg = RGB(0x1c, 0x1c, 0x1c);
static const COLORREF kFindBg = RGB(0xc9, 0xa2, 0x27);   // coincidencia
static const COLORREF kFindCur = RGB(0xff, 0x8c, 0x1a);  // coincidencia actual
static const COLORREF kCmdOk = RGB(0x4e, 0xd1, 0x8f);   // orden que existe
static const COLORREF kCmdBad = RGB(0xf0, 0x55, 0x55);  // orden desconocida
static const COLORREF kChipBorder = RGB(0x3a, 0x3a, 0x3a);
static const uint32_t kAnsi[16] = {0x616161, 0xFF8272, 0xB4FA72, 0xFEFDC2, 0xA5D5FE, 0xFF8FFD,
                                   0xD0D1FE, 0xF1F1F1, 0x8E8E8E, 0xFFC4BD, 0xD6FCB9, 0xFEFDD5,
                                   0xC1E3FE, 0xFFB1FE, 0xE5E6FE, 0xFEFFFF};
static const int kGap = 1;           // separación entre paneles (px)
static const int kPad = 4;           // margen interior de cada panel (px)
static const size_t kScrollback = 5000;

enum { WM_APP_DIRTY = WM_APP + 1, WM_APP_EXITED, WM_APP_SCRIPT, WM_APP_LAYOUT };
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
  bool hidden = false;  // oculta (clic derecho → Ocultar); se recupera desde «N ocultas»
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
  // Caja de entrada al estilo Warp (solo con PowerShell integrado): la shell avisa con
  // OSC 9;9;<ruta> cada vez que muestra el prompt; mientras tanto se escribe en la caja.
  bool integrated = false, atPrompt = false;
  std::string oscBuf;
  std::wstring cwd, edit;
  size_t caret = 0;
  std::vector<std::wstring> history;
  // Órdenes que conoce la shell (alias, funciones, cmdlets), para colorear la orden escrita.
  std::set<std::wstring> cmds;
  bool cmdsLoaded = false;
  std::map<std::wstring, bool> existsMemo;
  int histPos = -1;
  RECT input{};
  RECT chip{};          // apartado de la carpeta (clic: selector de carpetas)
  bool resumeRun = false;  // al reabrir, volver a lanzar la orden (estaba en marcha al cerrar)
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
static bool g_renameAll = false;  // texto entero seleccionado: lo que se escriba lo reemplaza
// Aviso breve en la barra de título (p. ej. la escala).
static std::wstring g_toast;

// Textos de la interfaz: inglés por defecto; se cambia con clic derecho en la barra de título.
enum Str { S_TYPE_COMMAND, S_RUNNING, S_ZOOM, S_FONT, S_NEED_WIN10, S_LANGUAGE, S_FIND, S_NO_RESULTS,
           S_SEARCH_DIRS, S_PARENT_DIR, S_RENAME, S_HIDE, S_CLOSE_TAB, S_SHOW_ALL, S_HIDDEN_N, S_COUNT };
static const wchar_t* const kStrings[][S_COUNT] = {
    {L"Type a command", L"Running… keys go to the program", L"Zoom %d%%", L"Font %d px",
     L"Windows 10 1809 or later is required (ConPTY).", L"Language", L"Find", L"No results",
     L"Search directories…", L".. (Parent directory)", L"Rename", L"Hide tab", L"Close tab", L"Show all",
     L"%d hidden"},
    {L"Escribe una orden", L"Ejecutando… las teclas van al programa", L"Escala %d %%", L"Letra %d px",
     L"Hace falta Windows 10 1809 o posterior (ConPTY).", L"Idioma", L"Buscar", L"Sin resultados",
     L"Buscar carpetas…", L".. (Carpeta superior)", L"Renombrar", L"Ocultar pestaña", L"Cerrar pestaña",
     L"Mostrar todas", L"%d ocultas"},
};
static const wchar_t* const kLangNames[] = {L"English", L"Español"};
static const char* const kLangCodes[] = {"en", "es"};
static const int kLangCount = 2;
static int g_lang = 0;
static const wchar_t* T(Str s) { return kStrings[g_lang][s]; }
static std::wstring Tf(Str s, int v) {
  wchar_t b[128];
  swprintf(b, 128, T(s), v);
  return b;
}
// Posición y tamaño de la ventana de la última vez (se guardan en la sesión).
static bool g_hasWinPos = false, g_winMax = false;
static RECT g_winRect{};
static int g_cellW = 8, g_cellH = 16;
static int g_textDy = 0;  // margen vertical del texto dentro de la celda
static std::wstring g_sessionPath;
// Varias ventanas: un proceso por ventana, cada una con su fichero window-<id>.txt en g_windowDir.
static std::wstring g_windowDir;
static bool g_keepSession = false;  // al cerrar, conservar la sesión (salir de todo / apagar Windows)
static UINT g_msgQuitAll;           // mensaje registrado: «cerrar todas guardándolas»
static bool g_focused = true;

// Selección (coordenadas de vista dentro de un panel).
static Pane* g_selPane = nullptr;
static VTermPos g_selA{}, g_selB{};
static bool g_selecting = false, g_hasSel = false;
// Arrastre de divisores y de paneles (por su cabecera).
static Node* g_dragNode = nullptr;
// Enlace bajo el ratón con Ctrl pulsado (Ctrl+clic lo abre).
struct LinkHit {
  Pane* p = nullptr;
  int row = -1, c0 = 0, c1 = 0;
  std::wstring url;
};
static LinkHit g_link;

// Buscar (Ctrl+F): coincidencias en el historial + pantalla del panel activo.
struct FindMatch {
  int line, col, len;  // línea absoluta: 0 = la más antigua del historial
};
struct FindState {
  bool open = false;
  Pane* p = nullptr;
  std::wstring query;
  std::vector<FindMatch> matches;
  int cur = -1;
};
static FindState g_find;

// Selector de carpetas al pulsar el apartado de la ruta, como en Warp.
struct DirEntry {
  std::wstring name;
  bool dir, parent;
};
struct DirPicker {
  bool open = false;
  Pane* p = nullptr;
  std::wstring base, filter;
  std::vector<DirEntry> all, shown;
  int sel = 0, top = 0;
  RECT rc{};
};
static DirPicker g_pick;
static const int kPickRows = 9;
static Pane* g_dragPane = nullptr;
static Pane* g_dropTarget = nullptr;
static bool g_layoutHeaders = false;
// Barra de título propia.
enum { HIT_NONE, HIT_TAB, HIT_TABCLOSE, HIT_PLUS, HIT_MIN, HIT_MAX, HIT_CLOSE, HIT_TOGGLE, HIT_HIDDEN };
static bool g_vertTabs = false;  // pestañas en barra lateral (botón de la izquierda)
static int g_hoverHit = HIT_NONE, g_hoverTab = -1, g_pressedHit = HIT_NONE;
// Pestaña pulsada (para arrastrarla fuera de la ventana).
static int g_tabDrag = -1;
static POINT g_tabDragStart{};

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
    case VTERM_PROP_ALTSCREEN:
      p->altScreen = v->boolean;
      p->scrollOff = 0;
      if (p->integrated) PostMessage(g_hwnd, WM_APP_LAYOUT, 0, (LPARAM)p);  // la caja se oculta a pantalla completa
      break;
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

// OSC 9;9;<ruta> (lo emite el prompt de warp-lite): la shell espera una orden en <ruta>.
static int CbOsc(int command, VTermStringFragment frag, void* user) {
  Pane* p = (Pane*)user;
  if (command != 9) return 0;
  if (frag.initial) p->oscBuf.clear();
  p->oscBuf.append(frag.str, frag.len);
  if (frag.final && p->oscBuf.rfind("9;", 0) == 0) {
    p->cwd = ToWide(p->oscBuf.substr(2));
    bool first = !p->integrated;
    p->integrated = p->atPrompt = true;
    PostMessage(g_hwnd, WM_APP_LAYOUT, first, (LPARAM)p);
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
  static VTermStateFallbacks fallbacks = [] {
    VTermStateFallbacks f{};
    f.osc = CbOsc;
    return f;
  }();
  vterm_screen_set_unrecognised_fallbacks(p->scr, &fallbacks, p);
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

// Prompt de PowerShell que avisa a warp-lite (OSC 9;9;<ruta>) y deja solo un "> " gris.
static std::wstring IntegrationArgs() {
  const wchar_t* script =
      L"function global:prompt { $e = [char]27; $b = [char]7; "
      L"$p = $executionContext.SessionState.Path.CurrentLocation.ProviderPath; "
      // El aviso va aparte: PSReadLine redibuja el texto del prompt y no entiende OSC.
      L"[Console]::Write(\"$e]9;9;$p$b\"); \"$e[90m> $e[0m\" }; "
      // Lista de órdenes para colorear la caja de entrada; se escribe en segundo plano.
      L"$null = Register-EngineEvent -SourceIdentifier PowerShell.OnIdle -MaxTriggerCount 1 -Action { "
      L"try { $f = Join-Path $env:TEMP ('warp-lite-cmds-' + $PID + '.txt'); "
      L"Get-Command -CommandType Alias,Function,Filter,Cmdlet | ForEach-Object Name | "
      L"Set-Content -Encoding UTF8 ($f + '.tmp'); Move-Item -Force ($f + '.tmp') $f } catch {} }";
  std::string bytes((const char*)script, wcslen(script) * sizeof(wchar_t));  // UTF-16LE
  static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::wstring b64;
  for (size_t i = 0; i < bytes.size(); i += 3) {
    uint32_t v = (uint8_t)bytes[i] << 16;
    if (i + 1 < bytes.size()) v |= (uint8_t)bytes[i + 1] << 8;
    if (i + 2 < bytes.size()) v |= (uint8_t)bytes[i + 2];
    b64 += t[v >> 18];
    b64 += t[(v >> 12) & 63];
    b64 += i + 1 < bytes.size() ? t[(v >> 6) & 63] : '=';
    b64 += i + 2 < bytes.size() ? t[v & 63] : '=';
  }
  return L" -NoExit -EncodedCommand " + b64;
}

static std::wstring ShellCommand() {
  wchar_t buf[MAX_PATH * 2];
  DWORD n = GetEnvironmentVariableW(L"WARPLITE_SHELL", buf, MAX_PATH * 2);
  if (n > 0 && n < MAX_PATH * 2) return buf;  // otra shell: sin caja de entrada
  if (SearchPathW(nullptr, L"pwsh.exe", nullptr, MAX_PATH, buf, nullptr)) return L"pwsh.exe -NoLogo" + IntegrationArgs();
  return L"powershell.exe -NoLogo" + IntegrationArgs();
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

// Carpeta de arranque: la última del panel si sigue existiendo; si no, el perfil del usuario.
static const wchar_t* StartDir(Pane* p, const wchar_t* home) {
  DWORD a = p->cwd.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(p->cwd.c_str());
  if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) return p->cwd.c_str();
  return home[0] ? home : nullptr;
}

// Entorno de la shell con TERM/COLORTERM como Warp: ssh los pasa y tmux y compañía usan color.
static std::wstring ShellEnvironment() {
  std::wstring env;
  wchar_t* block = GetEnvironmentStringsW();
  for (wchar_t* v = block; *v; v += wcslen(v) + 1) {
    if (!_wcsnicmp(v, L"TERM=", 5) || !_wcsnicmp(v, L"COLORTERM=", 10)) continue;
    env.append(v);
    env.push_back(L'\0');
  }
  FreeEnvironmentStringsW(block);
  for (const wchar_t* v : {L"TERM=xterm-256color", L"COLORTERM=truecolor"}) {
    env.append(v);
    env.push_back(L'\0');
  }
  env.push_back(L'\0');
  return env;
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
  std::wstring env = ShellEnvironment();
  wchar_t home[MAX_PATH] = L"";
  GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
  PROCESS_INFORMATION pi{};
  BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                           EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT, env.data(),
                           StartDir(p, home), &si.StartupInfo, &pi);
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
  if (g_link.p == p) g_link = LinkHit{};
  if (g_find.p == p) g_find = FindState{};
  if (g_pick.p == p) g_pick = DirPicker{};
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

static int ChipH() { return g_cellH + Px(8); }
static int InputH() { return Px(12) + ChipH() + Px(10) + g_cellH + Px(12); }

static void ResizePane(Pane* p, RECT r) {
  p->rc = p->content = r;
  if (g_layoutHeaders) p->content.top += PaneHdrH();
  p->input = RECT{};
  if (p->integrated && !p->altScreen && p->content.bottom - p->content.top > InputH() + 2 * g_cellH) {
    p->input = RECT{p->content.left, p->content.bottom - InputH(), p->content.right, p->content.bottom};
    p->content.bottom = p->input.top;
  }
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

static int SidebarW() { return g_vertTabs ? Px(220) : 0; }

static void Relayout() {
  if (!g_hwnd || g_tabs.empty()) return;
  RECT rc;
  GetClientRect(g_hwnd, &rc);
  rc.top += TabBarH();
  rc.left += SidebarW();
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
    // «run»: tenía algo en marcha (p. ej. ssh + tmux) y se vuelve a lanzar al reabrir.
    o << "L " << HexEncode(n->pane->lastCmd) << " " << HexEncode(ToUtf8(n->pane->cwd))
      << (n->pane->runningCmd.empty() || n->pane->lastCmd.empty() ? "" : " run") << "\n";
  } else {
    o << "S " << (n->vertical ? 1 : 0) << " " << n->ratio << "\n";
    SaveNode(o, n->a);
    SaveNode(o, n->b);
  }
}

static void SaveNode(std::ostringstream& o, Node* n);

static std::string SessionHeader(int active, const RECT* win, bool max) {
  std::ostringstream o;
  o << "BW1\nactive " << active << "\nui " << g_zoom << " " << g_fontPx << "\nlang " << kLangCodes[g_lang]
    << "\nvtabs " << (g_vertTabs ? 1 : 0) << "\n";
  if (win) o << "win " << win->left << " " << win->top << " " << win->right << " " << win->bottom << " " << (max ? 1 : 0) << "\n";
  return o.str();
}

static bool WriteSessionFile(const std::wstring& path, const std::string& data) {
  std::wstring tmp = path + L".tmp";
  HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  DWORD w;
  WriteFile(f, data.data(), (DWORD)data.size(), &w, nullptr);
  CloseHandle(f);
  return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

static void SaveSession() {
  if (g_tabs.empty() || g_sessionPath.empty()) return;
  std::ostringstream o;
  o << "BW1\nactive " << g_activeTab << "\nui " << g_zoom << " " << g_fontPx << "\nlang " << kLangCodes[g_lang]
    << "\nvtabs " << (g_vertTabs ? 1 : 0) << "\n";
  WINDOWPLACEMENT wp{sizeof wp};
  if (g_hwnd && GetWindowPlacement(g_hwnd, &wp)) {
    RECT& r = wp.rcNormalPosition;
    bool max = wp.showCmd == SW_SHOWMAXIMIZED ||
               (wp.showCmd == SW_SHOWMINIMIZED && (wp.flags & WPF_RESTORETOMAXIMIZED));
    o << "win " << r.left << " " << r.top << " " << r.right << " " << r.bottom << " " << (max ? 1 : 0) << "\n";
  }
  for (auto& t : g_tabs) {
    std::vector<Pane*> ls;
    Leaves(t.root, ls);
    int ai = (int)(std::find(ls.begin(), ls.end(), t.active) - ls.begin());
    o << "tab " << ai << " " << HexEncode(ToUtf8(t.name)) << (t.hidden ? " hidden" : "") << "\n";
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
    std::string hex, dir;
    in >> hex;
    auto pos = in.tellg();  // la carpeta es opcional (sesiones antiguas no la traen)
    if (in >> dir && dir.find_first_not_of("0123456789abcdef-") == std::string::npos) {
      dir = HexDecode(dir);
    } else {
      dir.clear();
      in.clear();
      in.seekg(pos);
    }
    Pane* p = NewPane();
    p->lastCmd = HexDecode(hex);
    p->cwd = ToWide(dir);
    auto pos2 = in.tellg();
    std::string flag;
    if (in >> flag && flag == "run") {
      p->resumeRun = true;
    } else {
      in.clear();
      in.seekg(pos2);
    }
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
    if (word == "vtabs") {
      int v = 0;
      in >> v;
      g_vertTabs = v != 0;
      continue;
    }
    if (word == "lang") {
      std::string code;
      in >> code;
      for (int i = 0; i < kLangCount; i++)
        if (code == kLangCodes[i]) g_lang = i;
      continue;
    }
    if (word == "ui") {
      in >> g_zoom >> g_fontPx;
      g_zoom = std::clamp(g_zoom, 50, 300);
      g_fontPx = std::clamp(g_fontPx, 6, 48);
      continue;
    }
    if (word == "win") {
      int m = 0;
      in >> g_winRect.left >> g_winRect.top >> g_winRect.right >> g_winRect.bottom >> m;
      g_winMax = m != 0;
      g_hasWinPos = g_winRect.right - g_winRect.left > 100 && g_winRect.bottom - g_winRect.top > 100;
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
    bool hidden = false;
    pos = in.tellg();
    std::string flag;
    if (in >> flag && flag == "hidden") {
      hidden = true;
    } else {
      in.clear();
      in.seekg(pos);
    }
    size_t first = created.size();
    Node* root = LoadNode(in, created);
    if (!root) break;
    Tab t{root, created[std::min(first + std::max(ai, 0), created.size() - 1)], ToWide(name), hidden};
    g_tabs.push_back(t);
  }
  if (g_tabs.empty()) return false;
  g_activeTab = std::clamp(active, 0, (int)g_tabs.size() - 1);
  g_tabs[g_activeTab].hidden = false;
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
  if (c.attrs.italic || c.attrs.dim) return true;
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
  if (key == VTERM_KEY_ENTER && !p->integrated) {
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

static bool EditorActive(Pane* p);
static void EditorInsert(Pane* p, const std::wstring& t);

static void Paste(Pane* p) {
  if (!OpenClipboard(g_hwnd)) return;
  std::wstring w;
  if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
    w = (const wchar_t*)GlobalLock(h);
    GlobalUnlock(h);
  }
  CloseClipboard();
  if (EditorActive(p)) {  // en la caja de entrada: una sola línea
    std::wstring t;
    for (wchar_t ch : w) {
      if (ch == L'\r') continue;
      if (ch == L'\n' || ch == L'\t') ch = L' ';
      if (ch < 0x20 || ch == 0x7f || (ch >= 0x80 && ch < 0xa0)) continue;
      t += ch;
    }
    while (!t.empty() && t.back() == L' ') t.pop_back();
    EditorInsert(p, t);
    InvalidateRect(g_hwnd, nullptr, FALSE);
    return;
  }
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
  Toast(Tf(S_ZOOM, g_zoom));
  SaveSession();
}

static void SetFontSize(int px) {
  g_fontPx = std::clamp(px, 6, 48);
  MakeFonts();
  Relayout();
  Toast(Tf(S_FONT, g_fontPx));
  SaveSession();
}

static std::wstring ShortTitle(Pane* p);

// Renombrar pestaña (doble clic): Enter guarda, Esc cancela; vacío vuelve al título de la shell.
static void BeginRename(int i) {
  if (i < 0 || i >= (int)g_tabs.size()) return;
  g_renameTab = i;
  g_renameText = g_tabs[i].name.empty() ? ShortTitle(g_tabs[i].active) : g_tabs[i].name;
  g_renameAll = true;
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
  else if (vk == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
    g_renameAll = true;
    InvalidateRect(g_hwnd, nullptr, FALSE);
  } else if (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_HOME || vk == VK_END) {
    g_renameAll = false;  // sin selección: se sigue escribiendo al final
    InvalidateRect(g_hwnd, nullptr, FALSE);
  } else if (vk == VK_BACK && g_renameAll) {
    g_renameText.clear();
    g_renameAll = false;
    InvalidateRect(g_hwnd, nullptr, FALSE);
  } else if (vk == VK_BACK) {
    if (!g_renameText.empty()) g_renameText.pop_back();
    if (!g_renameText.empty() && g_renameText.back() >= 0xD800 && g_renameText.back() < 0xDC00)
      g_renameText.pop_back();
    InvalidateRect(g_hwnd, nullptr, FALSE);
  } else return false;
  return true;
}

static void ActivateTab(int i) {
  if (i < 0 || i >= (int)g_tabs.size()) return;
  g_tabs[i].hidden = false;  // ir a una pestaña oculta la vuelve a mostrar
  g_activeTab = i;
  ClearSelection();
  Relayout();
}

static void OpenFind(Pane* p);

static int HiddenCount() {
  int n = 0;
  for (auto& t : g_tabs) n += t.hidden;
  return n;
}

// Siguiente pestaña visible en la dirección dir (las ocultas se saltan).
static int NextVisibleTab(int from, int dir) {
  int n = (int)g_tabs.size();
  for (int k = 1; k <= n; k++) {
    int j = ((from + dir * k) % n + n) % n;
    if (!g_tabs[j].hidden) return j;
  }
  return from;
}

static void HideTab(int i) {
  if ((int)g_tabs.size() - HiddenCount() <= 1) return;  // siempre queda una visible
  g_tabs[i].hidden = true;
  if (i == g_activeTab) ActivateTab(NextVisibleTab(i, 1));
  SaveSession();
  Relayout();
}

static void CloseTab(int i) {
  std::vector<Pane*> ls;
  Leaves(g_tabs[i].root, ls);
  for (Pane* p : ls) ClosePane(p);
}

// Menú de una pestaña (clic derecho): renombrar, ocultar, cerrar.
static void TabMenu(int i, POINT screen) {
  HMENU m = CreatePopupMenu();
  AppendMenuW(m, MF_STRING, 1, T(S_RENAME));
  AppendMenuW(m, MF_STRING | ((int)g_tabs.size() - HiddenCount() <= 1 ? MF_GRAYED : 0), 2, T(S_HIDE));
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, 3, T(S_CLOSE_TAB));
  int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, g_hwnd, nullptr);
  DestroyMenu(m);
  if (cmd == 1) ActivateTab(i), BeginRename(i);
  else if (cmd == 2) HideTab(i);
  else if (cmd == 3) CloseTab(i);
}

// Menú de pestañas ocultas: cada una (la muestra y va a ella) o todas.
static void HiddenMenu(POINT screen) {
  HMENU m = CreatePopupMenu();
  for (int i = 0; i < (int)g_tabs.size(); i++)
    if (g_tabs[i].hidden)
      AppendMenuW(m, MF_STRING, 100 + i,
                  (g_tabs[i].name.empty() ? ShortTitle(g_tabs[i].active) : g_tabs[i].name).c_str());
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, 1, T(S_SHOW_ALL));
  int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, g_hwnd, nullptr);
  DestroyMenu(m);
  if (cmd == 1)
    for (auto& t : g_tabs) t.hidden = false;
  else if (cmd >= 100) ActivateTab(cmd - 100);
  if (cmd) SaveSession(), Relayout();
}

// ---------------------------------------------------------------------------
// Varias ventanas

static std::wstring NewWindowFile() {
  wchar_t id[32];
  swprintf(id, 32, L"%llx%04x", (unsigned long long)GetTickCount64(), (unsigned)(GetCurrentProcessId() & 0xffff));
  return g_windowDir + L"\\window-" + id + L".txt";
}

static void LaunchWindow(const std::wstring& file) {
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --window \"" + file + L"\"";
  STARTUPINFOW si{sizeof si};
  PROCESS_INFORMATION pi{};
  if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
    CloseHandle(pi.hThread), CloseHandle(pi.hProcess);
}

static BOOL CALLBACK CountOthers(HWND h, LPARAM lp) {
  wchar_t cls[32];
  if (h != g_hwnd && GetClassNameW(h, cls, 32) && !wcscmp(cls, L"WarpLite")) ++*(int*)lp;
  return TRUE;
}

static int OtherWindows() {
  int n = 0;
  EnumWindows(CountOthers, (LPARAM)&n);
  return n;
}

static BOOL CALLBACK SendQuitAll(HWND h, LPARAM) {
  wchar_t cls[32];
  if (GetClassNameW(h, cls, 32) && !wcscmp(cls, L"WarpLite")) PostMessageW(h, g_msgQuitAll, 0, 0);
  return TRUE;
}

// Lleva una pestaña (o un solo panel) a una ventana nueva en el punto de la pantalla donde
// se suelta: se escribe su sesión, se abre otro warp-lite con ella y aquí se cierra. Lo que
// estaba en marcha (ssh + tmux) se relanza allí.
static void Detach(int tabIdx, Pane* single, POINT screen) {
  if (tabIdx < 0 || tabIdx >= (int)g_tabs.size()) return;
  Tab& t = g_tabs[tabIdx];
  Node* root = single ? single->node : t.root;
  std::vector<Pane*> moved;
  Leaves(root, moved);
  RECT wr;
  GetWindowRect(g_hwnd, &wr);
  int w = wr.right - wr.left, h = wr.bottom - wr.top;
  RECT win{screen.x - Px(120), screen.y - Px(20), screen.x - Px(120) + w, screen.y - Px(20) + h};
  int ai = single ? 0 : (int)(std::find(moved.begin(), moved.end(), t.active) - moved.begin());
  std::ostringstream o;
  o << SessionHeader(0, &win, false) << "tab " << ai << " " << HexEncode(ToUtf8(single ? L"" : t.name)) << "\n";
  SaveNode(o, root);
  std::wstring file = NewWindowFile();
  if (!WriteSessionFile(file, o.str())) return;
  LaunchWindow(file);
  for (Pane* p : moved) ClosePane(p);
  SaveSession();
}

static bool OutsideWindow(POINT screen) {
  RECT wr;
  GetWindowRect(g_hwnd, &wr);
  return !PtInRect(&wr, screen);
}

static void ToggleVerticalTabs() {
  g_vertTabs = !g_vertTabs;
  SaveSession();
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
      case 'F': OpenFind(act); return true;  // Ctrl+Shift+F: el de Warp
      case 'Q': EnumWindows(SendQuitAll, 0); return true;  // cerrar todas las ventanas guardándolas
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
      case VK_TAB: ActivateTab(NextVisibleTab(g_activeTab, -1)); return true;
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
      case VK_PRIOR: ActivateTab(NextVisibleTab(g_activeTab, -1)); return true;
      case VK_NEXT:
      case VK_TAB: ActivateTab(NextVisibleTab(g_activeTab, 1)); return true;
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
      case 'F': OpenFind(act); return true;
    }
    if (vk >= '1' && vk <= '9') {  // entre las visibles
      std::vector<int> vis;
      for (int i = 0; i < (int)g_tabs.size(); i++)
        if (!g_tabs[i].hidden) vis.push_back(i);
      int k = vk == '9' ? (int)vis.size() - 1 : (int)(vk - '1');
      if (k < (int)vis.size()) ActivateTab(vis[k]);
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

// ---------------------------------------------------------------------------
// Buscar (Ctrl+F)

static std::wstring RowText(Pane* p, int row);

// Texto de una línea absoluta (historial + pantalla), una letra por columna. Con el lock tomado.
static std::wstring AbsLineText(Pane* p, int line) {
  int sbn = (int)p->sb.size();
  if (line >= sbn) {
    int saved = p->scrollOff;
    p->scrollOff = 0;
    std::wstring t = RowText(p, line - sbn);
    p->scrollOff = saved;
    return t;
  }
  std::wstring t(p->cols, L' ');
  auto& v = p->sb[line];
  for (int c = 0; c < p->cols && c < (int)v.size(); c++) {
    uint32_t ch = v[c].chars[0];
    if (ch && ch != (uint32_t)-1) t[c] = ch < 0x10000 ? (wchar_t)ch : L'?';
  }
  return t;
}

static void FindRecompute() {
  Pane* p = g_find.p;
  g_find.matches.clear();
  g_find.cur = -1;
  if (!p || !g_live.count(p) || g_find.query.empty()) return;
  std::wstring q = g_find.query;
  for (auto& c : q) c = towlower(c);
  EnterCriticalSection(&p->cs);
  int total = (int)p->sb.size() + p->rows;
  for (int l = 0; l < total; l++) {
    std::wstring t = AbsLineText(p, l);
    for (auto& c : t) c = towlower(c);
    for (size_t at = 0; (at = t.find(q, at)) != std::wstring::npos; at += q.size())
      g_find.matches.push_back({l, (int)at, (int)q.size()});
  }
  LeaveCriticalSection(&p->cs);
  if (!g_find.matches.empty()) g_find.cur = (int)g_find.matches.size() - 1;  // la más reciente
}

// Desplaza la vista para que se vea la coincidencia actual.
static void FindReveal() {
  Pane* p = g_find.p;
  if (!p || g_find.cur < 0) return;
  EnterCriticalSection(&p->cs);
  int sbn = (int)p->sb.size();
  int line = g_find.matches[g_find.cur].line;
  int viewRow = line - sbn + p->scrollOff;
  if (viewRow < 0 || viewRow >= p->rows) p->scrollOff = std::clamp(sbn - line + p->rows / 2, 0, sbn);
  LeaveCriticalSection(&p->cs);
}

// 0 = nada, 1 = coincidencia, 2 = coincidencia actual. Con el lock tomado.
static int FindMark(Pane* p, int row, int col) {
  if (!g_find.open || g_find.p != p || g_find.matches.empty()) return 0;
  int line = row - p->scrollOff + (int)p->sb.size();
  // Coincidencias ordenadas por línea: búsqueda binaria de la primera de esa línea.
  auto it = std::lower_bound(g_find.matches.begin(), g_find.matches.end(), line,
                             [](const FindMatch& m, int l) { return m.line < l; });
  for (; it != g_find.matches.end() && it->line == line; ++it)
    if (col >= it->col && col < it->col + it->len) return (it - g_find.matches.begin()) == g_find.cur ? 2 : 1;
  return 0;
}

static void OpenFind(Pane* p) {
  g_find.open = true;
  g_find.p = p;
  if (g_hasSel && g_selPane == p) {  // con texto seleccionado, se busca eso
    std::wstring sel = SelectionText();
    if (!sel.empty() && sel.find(L'\n') == std::wstring::npos) g_find.query = sel;
  }
  FindRecompute();
  FindReveal();
  InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void CloseFind() {
  g_find.open = false;
  g_find.matches.clear();
  InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void FindStep(int dir) {
  if (g_find.matches.empty()) return;
  int n = (int)g_find.matches.size();
  g_find.cur = ((g_find.cur < 0 ? 0 : g_find.cur) + dir + n) % n;
  FindReveal();
  InvalidateRect(g_hwnd, nullptr, FALSE);
}

static bool FindKey(WPARAM vk) {
  bool shift = GetKeyState(VK_SHIFT) & 0x8000;
  if (vk == VK_ESCAPE) CloseFind();
  else if (vk == VK_RETURN || vk == VK_F3) FindStep(shift ? -1 : 1);
  else if (vk == VK_UP) FindStep(-1);
  else if (vk == VK_DOWN) FindStep(1);
  else if (vk == VK_BACK) {
    if (!g_find.query.empty()) g_find.query.pop_back();
    FindRecompute();
    FindReveal();
    InvalidateRect(g_hwnd, nullptr, FALSE);
  } else if (vk == 'F' && (GetKeyState(VK_CONTROL) & 0x8000)) CloseFind();
  else return false;
  return true;
}

static void FindChar(wchar_t c) {
  if (c < 0x20 || g_find.query.size() >= 200) return;
  g_find.query += c;
  FindRecompute();
  FindReveal();
  InvalidateRect(g_hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Selector de carpetas (clic en el apartado de la ruta)

static void PickFilter() {
  std::wstring f = g_pick.filter;
  for (auto& c : f) c = towlower(c);
  g_pick.shown.clear();
  for (auto& e : g_pick.all) {
    std::wstring n = e.name;
    for (auto& c : n) c = towlower(c);
    if (e.parent ? f.empty() : n.find(f) != std::wstring::npos) g_pick.shown.push_back(e);
  }
  g_pick.sel = 0;
  g_pick.top = 0;
}

static void OpenPicker(Pane* p) {
  if (p->cwd.empty()) return;
  g_pick = DirPicker{};
  g_pick.open = true;
  g_pick.p = p;
  g_pick.base = p->cwd;
  bool root = p->cwd.size() <= 3;  // C:\ (o C:)
  if (!root) g_pick.all.push_back({L"", true, true});
  std::vector<DirEntry> dirs, files;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((p->cwd + (p->cwd.back() == L'\\' ? L"*" : L"\\*")).c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
      if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
      (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? dirs : files).push_back({fd.cFileName, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, false});
    } while (FindNextFileW(h, &fd) && dirs.size() + files.size() < 3000);
    FindClose(h);
  }
  auto byName = [](const DirEntry& a, const DirEntry& b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; };
  std::sort(dirs.begin(), dirs.end(), byName);
  std::sort(files.begin(), files.end(), byName);
  g_pick.all.insert(g_pick.all.end(), dirs.begin(), dirs.end());
  g_pick.all.insert(g_pick.all.end(), files.begin(), files.end());
  PickFilter();
  InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void ClosePicker() {
  g_pick.open = false;
  InvalidateRect(g_hwnd, nullptr, FALSE);
}

static bool EditorActive(Pane* p);
static void EditorInsert(Pane* p, const std::wstring& t);

// Carpeta: la shell cambia a ella (sin tocar lo que hubiera escrito en la caja).
// Archivo: su ruta entre comillas se escribe en la caja.
static void PickChoose(int i) {
  if (i < 0 || i >= (int)g_pick.shown.size()) return;
  DirEntry e = g_pick.shown[i];
  Pane* p = g_pick.p;
  ClosePicker();
  if (!g_live.count(p) || !EditorActive(p)) return;
  std::wstring base = g_pick.base;
  if (!base.empty() && base.back() == L'\\') base.pop_back();
  std::wstring path;
  if (e.parent) {
    size_t cut = base.find_last_of(L'\\');
    path = cut == std::wstring::npos ? base : base.substr(0, cut);
    if (path.size() == 2 && path[1] == L':') path += L'\\';
  } else {
    path = base + L"\\" + e.name;
  }
  // Cadena literal de PowerShell: toda comilla simple (también las tipográficas ‘ ’ ‚ ‛, que
  // PowerShell trata igual) va duplicada; los controles no pueden darse en nombres de Windows.
  std::wstring quoted = L"'";
  for (wchar_t c : path) {
    if (c < 0x20) return;
    quoted += c;
    if (c == L'\'' || c == 0x2018 || c == 0x2019 || c == 0x201A || c == 0x201B) quoted += c;
  }
  quoted += L"'";
  if (!e.dir) {
    EditorInsert(p, (p->caret && p->edit[p->caret - 1] != L' ' ? L" " : L"") + quoted);
    InvalidateRect(g_hwnd, nullptr, FALSE);
    return;
  }
  std::string u = ToUtf8(L"Set-Location -LiteralPath " + quoted);
  EnterCriticalSection(&p->cs);
  ToBottom(p);
  p->atPrompt = false;
  p->outBuf += u + "\r";
  LeaveCriticalSection(&p->cs);
  FlushOut(p);
}

static bool PickKey(WPARAM vk) {
  int n = (int)g_pick.shown.size();
  switch (vk) {
    case VK_ESCAPE: ClosePicker(); return true;
    case VK_RETURN: PickChoose(g_pick.sel); return true;
    case VK_UP: g_pick.sel = std::max(0, g_pick.sel - 1); break;
    case VK_DOWN: g_pick.sel = std::min(n - 1, g_pick.sel + 1); break;
    case VK_PRIOR: g_pick.sel = std::max(0, g_pick.sel - kPickRows); break;
    case VK_NEXT: g_pick.sel = std::min(n - 1, g_pick.sel + kPickRows); break;
    case VK_BACK:
      if (!g_pick.filter.empty()) g_pick.filter.pop_back();
      PickFilter();
      break;
    default: return false;
  }
  if (g_pick.sel < g_pick.top) g_pick.top = g_pick.sel;
  if (g_pick.sel >= g_pick.top + kPickRows) g_pick.top = g_pick.sel - kPickRows + 1;
  InvalidateRect(g_hwnd, nullptr, FALSE);
  return true;
}

static void PickChar(wchar_t c) {
  if (c < 0x20) return;
  g_pick.filter += c;
  PickFilter();
  InvalidateRect(g_hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Caja de entrada (editor de órdenes)

static bool EditorActive(Pane* p) { return p && p->integrated && p->atPrompt && !p->altScreen; }

static void EditorInsert(Pane* p, const std::wstring& t) {
  p->edit.insert(p->caret, t);
  p->caret += t.size();
  p->histPos = -1;
}

static void SubmitEditor(Pane* p) {
  std::wstring cmd = p->edit;
  std::string u = ToUtf8(cmd);
  if (!cmd.empty()) {
    if (p->history.empty() || p->history.back() != cmd) p->history.push_back(cmd);
    if (p->history.size() > 500) p->history.erase(p->history.begin());
    p->submittedCmd = u;
    if (u != "exit" && u != "logout") p->lastCmd = u;
  }
  p->edit.clear();
  p->caret = 0;
  p->histPos = -1;
  EnterCriticalSection(&p->cs);
  ToBottom(p);
  p->atPrompt = false;  // hasta el siguiente prompt, las teclas van al programa
  p->outBuf += u + "\r";
  LeaveCriticalSection(&p->cs);
  FlushOut(p);
  if (!cmd.empty()) SaveSession();
}

static std::wstring Lower(std::wstring s) {
  for (auto& c : s) c = towlower(c);
  return s;
}

// Lee la lista de órdenes que la shell deja en %TEMP% cuando está libre.
static void LoadShellCommands(Pane* p) {
  if (p->cmdsLoaded || !p->integrated || !p->pid) return;
  wchar_t tmp[MAX_PATH];
  GetTempPathW(MAX_PATH, tmp);
  std::wstring path = std::wstring(tmp) + L"warp-lite-cmds-" + std::to_wstring(p->pid) + L".txt";
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return;
  std::string data(GetFileSize(f, nullptr), 0);
  DWORD r = 0;
  ReadFile(f, data.data(), (DWORD)data.size(), &r, nullptr);
  CloseHandle(f);
  DeleteFileW(path.c_str());
  if (data.rfind("\xEF\xBB\xBF", 0) == 0) data.erase(0, 3);
  std::istringstream in(data);
  std::string line;
  while (std::getline(in, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (!line.empty()) p->cmds.insert(Lower(ToWide(line)));
  }
  p->cmdsLoaded = true;
  p->existsMemo.clear();
  InvalidateRect(g_hwnd, nullptr, FALSE);
}

// ¿Existe la orden? true/false, o -1 si aún no se puede saber (lista sin cargar).
static int CommandExists(Pane* p, const std::wstring& tok) {
  std::wstring low = Lower(tok);
  auto it = p->existsMemo.find(low);
  if (it != p->existsMemo.end()) return it->second;
  static const wchar_t* const kKeywords[] = {
      L"if", L"else", L"elseif", L"for", L"foreach", L"while", L"do", L"until", L"switch", L"function",
      L"filter", L"return", L"break", L"continue", L"try", L"catch", L"finally", L"throw", L"trap",
      L"param", L"begin", L"process", L"end", L"exit", L"class", L"enum", L"using", L"data", L"workflow"};
  bool ok = false;
  for (auto k : kKeywords) ok = ok || low == k;
  ok = ok || p->cmds.count(low);
  wchar_t buf[MAX_PATH];
  if (!ok && tok.find_first_of(L"\\/:") != std::wstring::npos) {  // ruta a un fichero
    std::wstring full = tok.find(L':') == std::wstring::npos && tok[0] != L'\\' ? p->cwd + L"\\" + tok : tok;
    DWORD a = GetFileAttributesW(full.c_str());
    ok = a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
  } else if (!ok) {  // programa en el PATH
    ok = tok.find(L'.') != std::wstring::npos && SearchPathW(nullptr, tok.c_str(), nullptr, MAX_PATH, buf, nullptr);
    wchar_t exts[1024];
    if (!ok && GetEnvironmentVariableW(L"PATHEXT", exts, 1024)) {
      std::wstringstream ss(exts);
      std::wstring ext;
      while (!ok && std::getline(ss, ext, L';'))
        if (!ext.empty()) ok = SearchPathW(nullptr, tok.c_str(), ext.c_str(), MAX_PATH, buf, nullptr) != 0;
    }
  }
  if (!ok && !p->cmdsLoaded) return -1;
  p->existsMemo[low] = ok;
  return ok;
}

static bool IsWordChar(wchar_t c) { return iswalnum(c) || c == L'_' || c == L'-'; }

// Teclas especiales del editor; true si se consume.
static bool EditorKey(Pane* p, WPARAM vk) {
  bool ctrl = GetKeyState(VK_CONTROL) & 0x8000;
  std::wstring& e = p->edit;
  size_t& c = p->caret;
  auto wordLeft = [&] {
    size_t i = c;
    while (i > 0 && !IsWordChar(e[i - 1])) i--;
    while (i > 0 && IsWordChar(e[i - 1])) i--;
    return i;
  };
  auto wordRight = [&] {
    size_t i = c;
    while (i < e.size() && !IsWordChar(e[i])) i++;
    while (i < e.size() && IsWordChar(e[i])) i++;
    return i;
  };
  switch (vk) {
    case VK_RETURN: SubmitEditor(p); break;
    case VK_BACK:
      if (ctrl) {
        size_t i = wordLeft();
        e.erase(i, c - i);
        c = i;
      } else if (c > 0) {
        e.erase(--c, 1);
      }
      break;
    case VK_DELETE:
      if (c < e.size()) e.erase(c, ctrl ? wordRight() - c : 1);
      break;
    case VK_LEFT: c = ctrl ? wordLeft() : (c ? c - 1 : 0); break;
    case VK_RIGHT: c = ctrl ? wordRight() : std::min(c + 1, e.size()); break;
    case VK_HOME: c = 0; break;
    case VK_END: c = e.size(); break;
    case VK_ESCAPE: e.clear(), c = 0, p->histPos = -1; break;
    case VK_TAB: break;  // sin autocompletado por ahora
    case VK_UP:
      if (!p->history.empty()) {
        if (p->histPos < 0) p->histPos = (int)p->history.size();
        if (p->histPos > 0) p->histPos--;
        e = p->history[p->histPos];
        c = e.size();
      }
      break;
    case VK_DOWN:
      if (p->histPos >= 0) {
        if (++p->histPos >= (int)p->history.size()) p->histPos = -1, e.clear();
        else e = p->history[p->histPos];
        c = e.size();
      }
      break;
    case 'C':
      if (!ctrl) return false;
      if (!e.empty()) e.clear(), c = 0;  // Ctrl+C con texto: lo descarta
      else SendChar(p, 3, VTERM_MOD_NONE);
      break;
    case 'A':
      if (!ctrl) return false;
      c = 0;
      break;
    case 'E':
      if (!ctrl) return false;
      c = e.size();
      break;
    default: return false;
  }
  InvalidateRect(g_hwnd, nullptr, FALSE);
  return true;
}

// Devuelve true si la tecla se ha consumido (no se traduce a WM_CHAR).
static bool HandleKeyDown(WPARAM vk, LPARAM) {
  if (g_tabs.empty()) return false;
  if (HandleShortcut(vk)) return true;
  Pane* p = CurTab().active;
  if (EditorActive(p)) {
    if (!(GetKeyState(VK_SHIFT) & 0x8000) || (vk != VK_PRIOR && vk != VK_NEXT)) return EditorKey(p, vk);
  }
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

// Texto de una fila de la vista, una letra por columna (los anchos dejan un hueco).
static std::wstring RowText(Pane* p, int row) {
  std::wstring t(p->cols, L' ');
  for (int c = 0; c < p->cols; c++) {
    VTermScreenCell cell;
    CellAt(p, row, c, &cell);
    uint32_t ch = cell.chars[0];
    if (ch && ch != (uint32_t)-1) t[c] = ch < 0x10000 ? (wchar_t)ch : L'?';
  }
  return t;
}

// URL http(s) en la celda bajo el punto; si llega al final de la fila, sigue en las siguientes.
static LinkHit LinkAt(Pane* p, int x, int y) {
  LinkHit h;
  POINT pt{x, y};
  if (!PtInRect(&p->content, pt)) return h;
  VTermPos pos = CellFromPoint(p, x, y);
  static const std::wstring kStop = L" \t\"'<>`|";
  EnterCriticalSection(&p->cs);
  std::wstring row = RowText(p, pos.row);
  for (size_t at = 0; (at = row.find(L"http", at)) != std::wstring::npos; at++) {
    if (row.compare(at, 7, L"http://") && row.compare(at, 8, L"https://")) continue;
    size_t end = row.find_first_of(kStop, at);
    if (end == std::wstring::npos) end = row.size();
    if ((int)at > pos.col || (int)end <= pos.col) continue;
    h.p = p, h.row = pos.row, h.c0 = (int)at, h.c1 = (int)end;
    h.url = row.substr(at, end - at);
    for (int r = pos.row + 1; end == row.size() && r < p->rows; r++) {  // URL partida en varias filas
      std::wstring next = RowText(p, r);
      end = next.find_first_of(kStop);
      if (end == 0) break;
      if (end == std::wstring::npos) end = next.size();
      h.url += next.substr(0, end);
      row = next;
    }
    while (!h.url.empty() && wcschr(L".,;:!?)]}'\"", h.url.back())) h.url.pop_back();
    break;
  }
  LeaveCriticalSection(&p->cs);
  return h;
}

static int TabWidth() { return Px(200); }
static int TabsX0() { return Px(8); }
static int WinBtnW() { return Px(46); }

// Dónde va cada pestaña visible, el «+», el botón de ocultas y el de alternar, en los dos modos.
struct TabLayout {
  std::vector<std::pair<int, RECT>> tabs;
  RECT plus{}, hiddenBtn{}, toggle{};
  int end = 0;  // x donde acaba lo dibujado en la barra de título
};

static TabLayout ComputeTabs() {
  TabLayout L;
  int th = TabBarH();
  L.toggle = RECT{Px(6), Px(6), Px(6) + Px(32), th - Px(2)};
  int hidden = HiddenCount();
  if (!g_vertTabs) {
    int x = L.toggle.right + Px(6), tw = TabWidth();
    for (int i = 0; i < (int)g_tabs.size(); i++)
      if (!g_tabs[i].hidden) L.tabs.push_back({i, RECT{x, Px(6), x + tw - Px(2), th}}), x += tw;
    L.plus = RECT{x, Px(6), x + Px(34), th};
    x += Px(34);
    if (hidden) L.hiddenBtn = RECT{x, Px(6), x + Px(90), th}, x += Px(90);
    L.end = x;
  } else {
    int y = th + Px(6), rowH = Px(34), sw = SidebarW();
    for (int i = 0; i < (int)g_tabs.size(); i++)
      if (!g_tabs[i].hidden) L.tabs.push_back({i, RECT{Px(6), y, sw - Px(6), y + rowH}}), y += rowH + Px(2);
    L.plus = RECT{Px(6), y, sw - Px(6), y + rowH};
    y += rowH + Px(2);
    if (hidden) L.hiddenBtn = RECT{Px(6), y, sw - Px(6), y + rowH};
    L.end = L.toggle.right;
  }
  return L;
}

static RECT TabCloseRect(RECT r) { return RECT{r.right - Px(28), r.top, r.right - Px(6), r.bottom}; }

// Qué hay bajo el punto en la barra de título o en la barra lateral de pestañas.
static int TitleHit(int x, int y, int* tab) {
  *tab = -1;
  bool inTitle = y < TabBarH(), inSide = !inTitle && x < SidebarW();
  if (!inTitle && !inSide) return HIT_NONE;
  POINT pt{x, y};
  if (inTitle) {
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    int bw = WinBtnW();
    if (x >= rc.right - bw) return HIT_CLOSE;
    if (x >= rc.right - 2 * bw) return HIT_MAX;
    if (x >= rc.right - 3 * bw) return HIT_MIN;
  }
  TabLayout L = ComputeTabs();
  if (PtInRect(&L.toggle, pt)) return HIT_TOGGLE;
  for (auto& [i, r] : L.tabs)
    if (PtInRect(&r, pt)) {
      *tab = i;
      RECT c = TabCloseRect(r);
      return PtInRect(&c, pt) ? HIT_TABCLOSE : HIT_TAB;
    }
  if (PtInRect(&L.plus, pt)) return HIT_PLUS;
  if (PtInRect(&L.hiddenBtn, pt)) return HIT_HIDDEN;
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
  // Altura de línea de Warp: 1,2 × tamaño de letra (DEFAULT_UI_LINE_HEIGHT_RATIO).
  g_cellH = std::max((int)tm.tmHeight, (int)(MulDiv(g_fontPx, ScaledDpi(), 96) * 1.2 + 0.5));
  g_textDy = (g_cellH - (int)tm.tmHeight) / 2;
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

static void Text(HDC dc, const std::wstring& s, RECT r, COLORREF c, UINT flags);

// Caja de entrada: apartado con la ruta y, debajo, la orden que se está escribiendo.
static void DrawInput(HDC dc, Pane* p, bool active, bool focus) {
  RECT r = p->input;
  if (r.bottom <= r.top) return;
  Fill(dc, r, kBg);
  Fill(dc, RECT{r.left, r.top, r.right, r.top + 1}, kDivider);
  int x = r.left + Px(12);
  SetBkMode(dc, TRANSPARENT);
  // Apartado con la ruta
  std::wstring path = p->cwd.empty() ? L"…" : p->cwd;
  SelectObject(dc, g_fonts[1]);
  SIZE sz{};
  GetTextExtentPoint32W(dc, path.c_str(), (int)path.size(), &sz);
  int iconW = Px(24);
  RECT chip{x, r.top + Px(12), std::min<int>(x + iconW + sz.cx + Px(10), r.right - Px(12)), r.top + Px(12) + ChipH()};
  p->chip = chip;
  HBRUSH br = CreateSolidBrush(kChipBg);
  HPEN pen = CreatePen(PS_SOLID, 1, kChipBorder);
  HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pen);
  RoundRect(dc, chip.left, chip.top, chip.right, chip.bottom, Px(6), Px(6));
  SelectObject(dc, ob);
  SelectObject(dc, op);
  DeleteObject(br);
  DeleteObject(pen);
  {  // icono de carpeta (pestaña + cuerpo)
    int fx = chip.left + Px(8), fw = Px(12), fh = Px(9), fy = (chip.top + chip.bottom - fh) / 2;
    Fill(dc, RECT{fx, fy, fx + Px(5), fy + Px(2)}, kTabText);
    Fill(dc, RECT{fx, fy + Px(2), fx + fw, fy + fh}, kTabText);
    Fill(dc, RECT{fx + 1, fy + Px(3), fx + fw - 1, fy + fh - 1}, kChipBg);
  }
  SelectObject(dc, g_fonts[1]);
  Text(dc, path, RECT{chip.left + iconW, chip.top, chip.right - Px(6), chip.bottom}, kFg, DT_LEFT | DT_PATH_ELLIPSIS);
  // Orden
  int ey = chip.bottom + Px(10);
  RECT er{x, ey, r.right - Px(12), ey + g_cellH};
  SelectObject(dc, g_fonts[0]);
  bool ready = EditorActive(p);
  int cx = x;
  if (!ready) {
    Text(dc, T(S_RUNNING), er, kTabText, DT_LEFT | DT_END_ELLIPSIS);
  } else if (p->edit.empty()) {
    Text(dc, T(S_TYPE_COMMAND), er, RGB(0x6a, 0x6a, 0x6a), DT_LEFT);
  } else {
    size_t cols = std::max(1, (int)(er.right - er.left) / g_cellW);
    size_t off = p->caret >= cols ? p->caret - cols + 1 : 0;
    std::wstring vis = p->edit.substr(off, cols);
    std::vector<INT> dx(vis.size(), g_cellW);
    SetTextColor(dc, kFg);
    ExtTextOutW(dc, x, ey, ETO_CLIPPED, &er, vis.c_str(), (UINT)vis.size(), dx.data());
    cx = x + (int)(p->caret - off) * g_cellW;
    // Como Warp: la orden en verde si existe; subrayado rojo discontinuo si no.
    size_t a = p->edit.find_first_not_of(L' ');
    size_t b = a == std::wstring::npos ? a : p->edit.find_first_of(L" \t|;", a);
    if (b == std::wstring::npos) b = p->edit.size();
    static const std::wstring kNeutral = L"$(\"'@&[{0123456789";
    if (a != std::wstring::npos && kNeutral.find(p->edit[a]) == std::wstring::npos && !(p->edit[a] == L'.' && b - a == 1)) {
      int ex = CommandExists(p, p->edit.substr(a, b - a));
      size_t va = std::max(a, off), vb = std::min(b, off + cols);
      if (va < vb) {
        int x0 = x + (int)(va - off) * g_cellW, x1 = x + (int)(vb - off) * g_cellW;
        if (ex == 1) {
          SetTextColor(dc, kCmdOk);
          ExtTextOutW(dc, x0, ey, ETO_CLIPPED, &er, vis.c_str() + (va - off), (UINT)(vb - va), dx.data());
        } else if (ex == 0) {
          int uy = ey + g_cellH - std::max(1, Px(1));
          for (int ux = x0; ux < x1; ux += Px(4)) Fill(dc, RECT{ux, uy, std::min(ux + Px(2), x1), uy + std::max(1, Px(1))}, kCmdBad);
        }
      }
    }
  }
  if (ready && active && focus) Fill(dc, RECT{cx, ey, cx + std::max(2, Px(2)), ey + g_cellH}, kFg);
  SetBkMode(dc, OPAQUE);
}

static void DrawLinkUnderline(HDC dc, Pane* p);

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
        if (c.attrs.dim)  // atenuado (SGR 2): a medio camino entre texto y fondo
          s.fg = RGB((GetRValue(s.fg) + GetRValue(s.bg)) / 2, (GetGValue(s.fg) + GetGValue(s.bg)) / 2,
                     (GetBValue(s.fg) + GetBValue(s.bg)) / 2);
        if (c.attrs.reverse) std::swap(s.fg, s.bg);
        if (InSelection(p, row, col)) s.bg = kSelBg;
        if (int f = FindMark(p, row, col)) s.bg = f == 2 ? kFindCur : kFindBg, s.fg = RGB(0, 0, 0);
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
      ExtTextOutW(dc, r.left, y + g_textDy, ETO_OPAQUE | ETO_CLIPPED, &r, txt.c_str(), (UINT)txt.size(), dx.data());
      if (st.underline || st.strike) {
        int ly = st.strike ? y + g_cellH / 2 : y + g_cellH - 2;
        Fill(dc, RECT{r.left, ly, r.right, ly + 1}, st.fg);
      }
    }
  }
  DrawInput(dc, p, active, showFocus);
  DrawLinkUnderline(dc, p);
  // Cursor (no se dibuja mientras se escribe en la caja de entrada)
  if (active && p->cursorVisible && p->scrollOff == 0 && !EditorActive(p)) {
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
      ExtTextOutW(dc, cr.left, cr.top + g_textDy, ETO_OPAQUE | ETO_CLIPPED, &cr, ch.c_str(), (UINT)ch.size(), nullptr);
    } else {
      HBRUSH b = CreateSolidBrush(kFg);
      FrameRect(dc, &cr, b);
      DeleteObject(b);
    }
  }
  LeaveCriticalSection(&p->cs);
}

static void DrawLinkUnderline(HDC dc, Pane* p) {
  if (g_link.p != p) return;
  int y = p->content.top + kPad + (g_link.row + 1) * g_cellH - std::max(1, Px(1));
  int x = p->content.left + kPad;
  Fill(dc, RECT{x + g_link.c0 * g_cellW, y, x + g_link.c1 * g_cellW, y + std::max(1, Px(1))}, kAccent);
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

static void DrawTabItem(HDC dc, int i, RECT r, bool vertical) {
  bool act = i == g_activeTab, hover = g_hoverTab == i;
  if (act || hover) Fill(dc, r, act ? kTabActiveBg : RGB(0x1a, 0x1a, 0x1a));
  if (vertical && act) Fill(dc, RECT{r.left, r.top, r.left + Px(3), r.bottom}, kAccent);
  RECT tr{r.left + (vertical ? Px(14) : Px(28)), r.top, r.right - Px(28), r.bottom};
  UINT align = vertical ? DT_LEFT : DT_CENTER;
  if (i == g_renameTab) {  // editando el nombre: texto con cursor
    Fill(dc, r, kTabActiveBg);
    Fill(dc, RECT{r.left, r.bottom - Px(2), r.right, r.bottom}, kAccent);
    SIZE sz{};
    GetTextExtentPoint32W(dc, g_renameText.c_str(), (int)g_renameText.size(), &sz);
    int cx = vertical ? std::min<int>(tr.left + sz.cx, tr.right) : std::min((int)(tr.left + tr.right + sz.cx) / 2, (int)tr.right);
    if (g_renameAll && sz.cx > 0)  // selección de todo el nombre
      Fill(dc, RECT{std::max(cx - (int)sz.cx, (int)tr.left), r.top + Px(8), cx + 1, r.bottom - Px(8)}, kSelBg);
    Text(dc, g_renameText, tr, kFg, align);
    Fill(dc, RECT{cx + 1, r.top + Px(9), cx + 1 + std::max(1, Px(1)), r.bottom - Px(9)}, kFg);
    return;
  }
  const std::wstring& label = g_tabs[i].name.empty() ? ShortTitle(g_tabs[i].active) : g_tabs[i].name;
  Text(dc, label, tr, act ? kFg : kTabText, align | DT_END_ELLIPSIS);
  if (act || hover) {
    RECT xr = TabCloseRect(r);
    bool hx = g_hoverHit == HIT_TABCLOSE && hover;
    SelectObject(dc, g_iconFont);
    Text(dc, L"", xr, hx ? kFg : kTabText, DT_CENTER);
    SelectObject(dc, g_uiFont);
  }
}

static void DrawTitleBar(HDC dc, int w, int h) {
  int th = TabBarH();
  Fill(dc, RECT{0, 0, w, th}, kTitleBg);
  if (g_vertTabs) {
    Fill(dc, RECT{0, th, SidebarW(), h}, kTitleBg);
    Fill(dc, RECT{SidebarW() - 1, th, SidebarW(), h}, kDivider);
  }
  SetBkMode(dc, TRANSPARENT);
  SelectObject(dc, g_uiFont);
  TabLayout L = ComputeTabs();
  // Botón para alternar pestañas arriba / en barra lateral (dos rectángulos)
  {
    RECT r = L.toggle;
    bool hov = g_hoverHit == HIT_TOGGLE;
    if (hov || g_vertTabs) Fill(dc, r, hov ? RGB(0x2a, 0x2a, 0x2a) : RGB(0x1e, 0x1e, 0x1e));
    COLORREF c = hov || g_vertTabs ? kFg : kTabText;
    int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, iw = Px(16), ih = Px(12);
    RECT o{cx - iw / 2, cy - ih / 2, cx + iw / 2, cy + ih / 2};
    HBRUSH b = CreateSolidBrush(c);
    FrameRect(dc, &o, b);
    DeleteObject(b);
    Fill(dc, RECT{o.left + Px(5), o.top, o.left + Px(5) + 1, o.bottom}, c);
  }
  for (auto& [i, r] : L.tabs) DrawTabItem(dc, i, r, g_vertTabs);
  if (g_hoverHit == HIT_PLUS) Fill(dc, L.plus, RGB(0x1a, 0x1a, 0x1a));
  Text(dc, L"+", L.plus, g_hoverHit == HIT_PLUS ? kFg : kTabText, DT_CENTER);
  if (L.hiddenBtn.right > L.hiddenBtn.left) {
    if (g_hoverHit == HIT_HIDDEN) Fill(dc, L.hiddenBtn, RGB(0x1a, 0x1a, 0x1a));
    Text(dc, Tf(S_HIDDEN_N, HiddenCount()), L.hiddenBtn, g_hoverHit == HIT_HIDDEN ? kFg : kTabText, DT_CENTER);
  }
  if (!g_toast.empty()) {
    RECT tr{L.end + Px(10), 0, w - 3 * WinBtnW() - Px(12), th};
    Text(dc, g_toast, tr, kFg, DT_RIGHT);
  }
  // Minimizar, maximizar/restaurar, cerrar
  SelectObject(dc, g_iconFont);
  int bw = WinBtnW();
  const wchar_t* glyphs[3] = {L"", IsZoomed(g_hwnd) ? L"" : L"", L""};
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

static void Panel(HDC dc, RECT r) {
  HBRUSH br = CreateSolidBrush(kChipBg);
  HPEN pen = CreatePen(PS_SOLID, 1, kChipBorder);
  HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pen);
  RoundRect(dc, r.left, r.top, r.right, r.bottom, Px(8), Px(8));
  SelectObject(dc, ob);
  SelectObject(dc, op);
  DeleteObject(br);
  DeleteObject(pen);
}

static void DrawFindBar(HDC dc) {
  Pane* p = g_find.p;
  if (!g_find.open || !p || !g_live.count(p) || p != CurTab().active) return;
  RECT c = p->content;
  int w = std::min<int>(Px(340), c.right - c.left - Px(16));
  RECT r{c.right - w - Px(8), c.top + Px(8), c.right - Px(8), c.top + Px(8) + Px(34)};
  Panel(dc, r);
  SetBkMode(dc, TRANSPARENT);
  SelectObject(dc, g_uiFont);
  std::wstring count = g_find.query.empty() ? L""
                       : g_find.matches.empty() ? T(S_NO_RESULTS)
                       : std::to_wstring(g_find.cur + 1) + L"/" + std::to_wstring(g_find.matches.size());
  RECT cr{r.right - Px(110), r.top, r.right - Px(10), r.bottom};
  Text(dc, count, cr, kTabText, DT_RIGHT);
  RECT qr{r.left + Px(10), r.top, cr.left - Px(6), r.bottom};
  SelectObject(dc, g_fonts[0]);
  if (g_find.query.empty()) Text(dc, T(S_FIND), qr, RGB(0x6a, 0x6a, 0x6a), DT_LEFT);
  else Text(dc, g_find.query, qr, kFg, DT_LEFT | DT_END_ELLIPSIS);
  SIZE sz{};
  GetTextExtentPoint32W(dc, g_find.query.c_str(), (int)g_find.query.size(), &sz);
  int cx = std::min<int>(qr.left + sz.cx + 1, qr.right);
  Fill(dc, RECT{cx, r.top + Px(9), cx + std::max(2, Px(2)), r.bottom - Px(9)}, kFg);
  SetBkMode(dc, OPAQUE);
}

static void DrawPicker(HDC dc) {
  Pane* p = g_pick.p;
  if (!g_pick.open || !p || !g_live.count(p)) {
    g_pick.rc = RECT{};
    return;
  }
  int rowH = Px(30), n = std::min<int>(kPickRows, (int)g_pick.shown.size());
  int h = Px(40) + std::max(1, n) * rowH + Px(6);
  int w = std::min<int>(Px(440), p->rc.right - p->rc.left - Px(16));
  int left = p->chip.left, bottom = p->chip.top - Px(6);
  RECT r{left, std::max<int>(TabBarH() + Px(4), bottom - h), left + w, bottom};
  g_pick.rc = r;
  Panel(dc, r);
  SetBkMode(dc, TRANSPARENT);
  SelectObject(dc, g_uiFont);
  RECT fr{r.left + Px(12), r.top, r.right - Px(12), r.top + Px(40)};
  if (g_pick.filter.empty()) Text(dc, T(S_SEARCH_DIRS), fr, RGB(0x8a, 0x8a, 0x8a), DT_LEFT);
  else Text(dc, g_pick.filter, fr, kFg, DT_LEFT | DT_END_ELLIPSIS);
  Fill(dc, RECT{r.left + 1, r.top + Px(40) - 1, r.right - 1, r.top + Px(40)}, kChipBorder);
  for (int i = 0; i < n; i++) {
    int idx = g_pick.top + i;
    if (idx >= (int)g_pick.shown.size()) break;
    const DirEntry& e = g_pick.shown[idx];
    RECT ir{r.left + Px(4), r.top + Px(40) + i * rowH + Px(2), r.right - Px(4), r.top + Px(40) + (i + 1) * rowH};
    bool sel = idx == g_pick.sel;
    if (sel) Fill(dc, ir, kAccent);
    // icono: flecha para la carpeta superior, carpeta o documento
    int ix = ir.left + Px(10), iy = (ir.top + ir.bottom) / 2;
    COLORREF ic = sel ? kFg : kTabText;
    if (e.parent) {
      RECT ar{ix, ir.top, ix + Px(14), ir.bottom};
      Text(dc, L"\u2191", ar, ic, DT_CENTER);
    } else if (e.dir) {
      Fill(dc, RECT{ix, iy - Px(5), ix + Px(5), iy - Px(3)}, ic);
      Fill(dc, RECT{ix, iy - Px(3), ix + Px(13), iy + Px(5)}, ic);
      Fill(dc, RECT{ix + 1, iy - Px(2), ix + Px(13) - 1, iy + Px(5) - 1}, sel ? kAccent : kChipBg);
    } else {
      Fill(dc, RECT{ix + Px(1), iy - Px(6), ix + Px(11), iy + Px(6)}, ic);
      Fill(dc, RECT{ix + Px(1) + 1, iy - Px(6) + 1, ix + Px(11) - 1, iy + Px(6) - 1}, sel ? kAccent : kChipBg);
    }
    RECT tr{ix + Px(24), ir.top, ir.right - Px(8), ir.bottom};
    Text(dc, e.parent ? std::wstring(T(S_PARENT_DIR)) : e.name, tr, sel ? kFg : RGB(0xd0, 0xd0, 0xd0),
         DT_LEFT | DT_END_ELLIPSIS);
  }
  SetBkMode(dc, OPAQUE);
}

static void Render(HDC dc, int w, int h) {
  RECT all{0, 0, w, h};
  Fill(dc, all, kBg);
  DrawTitleBar(dc, w, h);
  if (g_tabs.empty()) return;
  Tab& t = CurTab();
  std::vector<Pane*> ls;
  Leaves(t.root, ls);
  for (Pane* p : ls) {
    DrawPane(dc, p, p == t.active, g_focused);
    DrawPaneHeader(dc, p, p == t.active);
  }
  DrawDividers(dc, t.root);
  DrawFindBar(dc);
  DrawPicker(dc);
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
//   zoom:<%> move:x,y,w,h maximize rename-start rename-type:<texto> shot:<fichero.bmp> wait quit

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
  } else if (s.rfind(L"move:", 0) == 0) {  // move:x,y,ancho,alto
    int x = 0, y = 0, w = 0, h = 0;
    swscanf(s.c_str() + 5, L"%d,%d,%d,%d", &x, &y, &w, &h);
    ShowWindow(g_hwnd, SW_RESTORE);
    SetWindowPos(g_hwnd, nullptr, x, y, w, h, SWP_NOZORDER);
  } else if (s == L"maximize") {
    ShowWindow(g_hwnd, SW_MAXIMIZE);
  } else if (s.rfind(L"edit:", 0) == 0 && p) {  // escribe en la caja de entrada
    for (wchar_t c : s.substr(5)) SendMessageW(g_hwnd, WM_CHAR, c, 0);
    delay = 300;
  } else if (s == L"submit" && p) {
    if (EditorActive(p)) SubmitEditor(p);
    delay = 2000;
  } else if (s.rfind(L"hover:", 0) == 0 && p) {  // hover:col,fila (como Ctrl sobre un enlace)
    int c = 0, r = 0;
    swscanf(s.c_str() + 6, L"%d,%d", &c, &r);
    g_link = LinkAt(p, p->content.left + kPad + c * g_cellW + 1, p->content.top + kPad + r * g_cellH + 1);
    Toast(g_link.url.empty() ? L"(sin enlace)" : g_link.url);
  } else if (s.rfind(L"find:", 0) == 0 && p) {
    OpenFind(p);
    for (wchar_t c : s.substr(5)) FindChar(c);
  } else if (s == L"find-next") {
    FindStep(1);
  } else if (s == L"find-close") {
    CloseFind();
  } else if (s == L"detach-tab") {
    RECT wr;
    GetWindowRect(g_hwnd, &wr);
    Detach(g_activeTab, nullptr, POINT{wr.left + 200, wr.top + 120});
    delay = 3000;
  } else if (s == L"quit-all") {
    EnumWindows(SendQuitAll, 0);
    return;
  } else if (s.rfind(L"hide-tab", 0) == 0) {
    HideTab(g_activeTab);
  } else if (s == L"vtabs") {
    ToggleVerticalTabs();
  } else if (s == L"picker" && p) {
    OpenPicker(p);
  } else if (s.rfind(L"picker-type:", 0) == 0) {
    for (wchar_t c : s.substr(12)) PickChar(c);
  } else if (s == L"picker-enter") {
    PickChoose(g_pick.sel);
    delay = 2000;
  } else if (s.rfind(L"lang:", 0) == 0) {
    for (int i = 0; i < kLangCount; i++)
      if (ToUtf8(s.substr(5)) == kLangCodes[i]) g_lang = i;
  } else if (s == L"rename-start") {
    BeginRename(g_activeTab);
  } else if (s.rfind(L"rename-type:", 0) == 0) {
    for (wchar_t c : s.substr(12)) SendMessageW(g_hwnd, WM_CHAR, c, 0);
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
  InvalidateRect(g_hwnd, nullptr, FALSE);
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
    case WM_NCRBUTTONUP:
      if (wp == HTCAPTION) {  // clic derecho en la barra de título: idioma
        HMENU menu = CreatePopupMenu(), sub = CreatePopupMenu();
        for (int i = 0; i < kLangCount; i++)
          AppendMenuW(sub, MF_STRING | (i == g_lang ? MF_CHECKED : 0), 100 + i, kLangNames[i]);
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)sub, T(S_LANGUAGE));
        int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), 0, hwnd,
                                 nullptr);
        DestroyMenu(menu);
        if (cmd >= 100 && cmd < 100 + kLangCount) {
          g_lang = cmd - 100;
          SaveSession();
          InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
      }
      break;
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
    case WM_APP_LAYOUT: {
      Pane* p = (Pane*)lp;
      if (!g_live.count(p)) return 0;
      if (wp && !p->restoreCmd.empty()) {  // primer prompt: la última orden queda en la caja
        p->edit = ToWide(p->restoreCmd);
        p->caret = p->edit.size();
        p->restoreCmd.clear();
        if (p->resumeRun) {  // estaba en marcha al cerrar: se relanza (tmux new -A reengancha)
          p->resumeRun = false;
          SubmitEditor(p);
        } else {
          p->history.push_back(p->edit);
        }
      }
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
        if (++ticks % 5 == 0) {
          UpdateRunning();
          for (Pane* p : g_live) LoadShellCommands(p);
        }
        // Escribe la última orden en el prompt cuando la shell ha terminado de arrancar.
        DWORD now = GetTickCount();
        for (Pane* p : g_live) {
          // Shells sin integración (WARPLITE_SHELL): se escribe en su prompt tras arrancar.
          if (p->restoreCmd.empty() || !p->proc || p->integrated) continue;
          bool settled = p->firstOut && now - p->lastOut > 400;
          if (now - p->spawnTick < (settled ? 6000u : 10000u)) continue;
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
      if (g_pick.open) {
        if (msg == WM_CHAR) PickChar((wchar_t)wp);
        return 0;
      }
      if (g_find.open && g_renameTab < 0) {
        if (msg == WM_CHAR) FindChar((wchar_t)wp);
        return 0;
      }
      if (g_renameTab >= 0) {
        if (wp >= 0x20 && msg == WM_CHAR) {
          if (g_renameAll) g_renameText.clear(), g_renameAll = false;
          if (g_renameText.size() < 60) g_renameText += (wchar_t)wp;
        }
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
      if (EditorActive(CurTab().active)) {
        if (msg == WM_CHAR && cp >= 0x20 && cp != 0x7f) {
          std::wstring t;
          AppendUtf16(t, cp);
          EditorInsert(CurTab().active, t);
          InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
      }
      SendChar(CurTab().active, cp, msg == WM_SYSCHAR ? VTERM_MOD_ALT : VTERM_MOD_NONE);
      return 0;
    }
    case WM_SYSCOMMAND:
      if ((wp & 0xFFF0) == SC_KEYMENU) return 0;  // Alt no abre el menú de sistema
      break;
    case WM_MOUSEWHEEL: {
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      ScreenToClient(hwnd, &pt);
      if (g_pick.open && PtInRect(&g_pick.rc, pt)) {
        int maxTop = std::max(0, (int)g_pick.shown.size() - kPickRows);
        g_pick.top = std::clamp(g_pick.top + (GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -3 : 3), 0, maxTop);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      if (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) SetZoom(g_zoom + (GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 10 : -10));
      else if (!g_tabs.empty()) OnMouseWheel(pt.x, pt.y, GET_WHEEL_DELTA_WPARAM(wp));
      return 0;
    }
    case WM_LBUTTONDBLCLK: {
      int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp), i;
      if (!g_tabs.empty() && (y < TabBarH() || x < SidebarW()) && TitleHit(x, y, &i) == HIT_TAB) {
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
      if (g_pick.open) {  // selector de carpetas abierto: elegir o cerrar
        POINT pt{x, y};
        int row = (y - g_pick.rc.top - Px(40)) / Px(30);
        if (msg == WM_LBUTTONDOWN && PtInRect(&g_pick.rc, pt) && y >= g_pick.rc.top + Px(40)) PickChoose(g_pick.top + row);
        else if (!PtInRect(&g_pick.rc, pt)) ClosePicker();
        return 0;
      }
      if (y < TabBarH() || x < SidebarW()) {
        int i;
        int hit = TitleHit(x, y, &i);
        POINT sp{x, y};
        ClientToScreen(hwnd, &sp);
        if ((hit == HIT_TAB && msg == WM_MBUTTONDOWN) || (hit == HIT_TABCLOSE && msg == WM_LBUTTONDOWN)) {
          CloseTab(i);
        } else if (msg == WM_RBUTTONDOWN && (hit == HIT_TAB || hit == HIT_TABCLOSE)) {
          TabMenu(i, sp);
        } else if (msg == WM_LBUTTONDOWN) {
          if (hit == HIT_TAB) ActivateTab(i), g_tabDrag = i, g_tabDragStart = POINT{x, y}, SetCapture(hwnd);
          else if (hit == HIT_PLUS) NewTab();
          else if (hit == HIT_TOGGLE) ToggleVerticalTabs();
          else if (hit == HIT_HIDDEN) HiddenMenu(sp);
          else if (hit >= HIT_MIN && hit <= HIT_CLOSE) g_pressedHit = hit;  // se ejecuta al soltar
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
      POINT pt{x, y};
      if (PtInRect(&p->input, pt)) {  // clic en la caja de entrada: enfoca; en la carpeta, selector
        if (msg == WM_LBUTTONDOWN && PtInRect(&p->chip, pt) && EditorActive(p)) OpenPicker(p);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      bool shift = GetKeyState(VK_SHIFT) & 0x8000;
      if (msg == WM_LBUTTONDOWN && (wp & MK_CONTROL)) {  // Ctrl+clic en un enlace: abrirlo
        LinkHit h = LinkAt(p, x, y);
        if (!h.url.empty()) {
          ShellExecuteW(nullptr, L"open", h.url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
          return 0;
        }
      }
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
        int hit = y < TabBarH() || x < SidebarW() ? TitleHit(x, y, &tab) : HIT_NONE;
        SetHover(hit, tab);
        TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
      }
      if (g_tabDrag >= 0) {  // arrastrando una pestaña: fuera de la ventana se abre en otra
        POINT sp;
        GetCursorPos(&sp);
        SetCursor(LoadCursor(nullptr, OutsideWindow(sp) ? IDC_SIZEALL : IDC_ARROW));
        return 0;
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
      // Movimiento del ratón a la aplicación: siempre si lo pide todo (1003, hovers de Claude
      // en tmux); con botón pulsado si pide arrastres (1002).
      bool anyButton = wp & (MK_LBUTTON | MK_MBUTTON | MK_RBUTTON);
      if (Pane* p = PaneAt(x, y); p && (p->mouseMode == VTERM_PROP_MOUSE_MOVE ||
                                       (p->mouseMode == VTERM_PROP_MOUSE_DRAG && anyButton))) {
        MouseToApp(p, x, y, 0, false);
      }
      {  // Ctrl sobre un enlace: subrayado y mano
        LinkHit h;
        if (wp & MK_CONTROL)
          if (Pane* p = PaneAt(x, y)) h = LinkAt(p, x, y);
        if (h.p != g_link.p || h.row != g_link.row || h.c0 != g_link.c0) InvalidateRect(hwnd, nullptr, FALSE);
        g_link = h;
      }
      Node* d = y >= TabBarH() ? DividerAt(CurTab().root, x, y) : nullptr;
      bool onHeader;
      LPCWSTR cur = d ? (d->vertical ? IDC_SIZEWE : IDC_SIZENS)
                    : g_link.p ? IDC_HAND
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
      if (g_tabDrag >= 0) {  // soltar una pestaña fuera de la ventana: a una ventana nueva
        int ti = g_tabDrag;
        g_tabDrag = -1;
        ReleaseCapture();
        POINT sp;
        GetCursorPos(&sp);
        if (msg == WM_LBUTTONUP && OutsideWindow(sp) && g_tabs.size() > 1) Detach(ti, nullptr, sp);
        return 0;
      }
      if (g_dragPane) {  // soltar un panel sobre otro: intercambiarlos; fuera: ventana nueva
        Pane* target = g_dropTarget;
        Pane* dragged = g_dragPane;
        g_dragPane = g_dropTarget = nullptr;
        ReleaseCapture();
        POINT sp;
        GetCursorPos(&sp);
        if (target && g_live.count(target) && g_live.count(dragged)) {
          SwapPanes(dragged, target);
        } else if (g_live.count(dragged) && OutsideWindow(sp) && (dragged->node->parent || g_tabs.size() > 1)) {
          int ti = TabIndexOf(dragged);
          Detach(ti, dragged->node->parent ? dragged : nullptr, sp);
        }
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
      // Con otras ventanas abiertas, cerrar esta la quita de la sesión (como en Warp); la última
      // que se cierra, «salir de todo» (Ctrl+Shift+Q) o apagar Windows la conservan.
      if (!g_keepSession && OtherWindows() > 0 && !g_windowDir.empty()) DeleteFileW(g_sessionPath.c_str()), g_sessionPath.clear();
      else SaveSession();
      DestroyWindow(hwnd);
      return 0;
    case WM_QUERYENDSESSION:
      g_keepSession = true;
      SaveSession();
      return TRUE;
    case WM_ENDSESSION:
      if (wp) SaveSession();
      return 0;
    case WM_DESTROY: {
      if (g_tabs.empty() && !g_keepSession && OtherWindows() > 0 && !g_sessionPath.empty())
        DeleteFileW(g_sessionPath.c_str());  // se cerró su último panel y quedan otras ventanas
      std::vector<Pane*> all(g_live.begin(), g_live.end());
      for (Pane* p : all) DestroyPane(p);
      g_tabs.clear();
      PostQuitMessage(0);
      return 0;
    }
  }
  if (msg == g_msgQuitAll && g_msgQuitAll) {
    g_keepSession = true;
    PostMessageW(hwnd, WM_CLOSE, 0, 0);
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

static HANDLE g_windowMutex;

// Cada ventana «posee» su fichero mientras está abierta (mutex con el nombre del fichero).
static bool ClaimWindowFile(const std::wstring& file) {
  std::wstring name = L"Local\\warp-lite-" + file.substr(file.find_last_of(L'\\') + 1);
  HANDLE m = CreateMutexW(nullptr, TRUE, name.c_str());
  if (!m) return false;
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(m);
    return false;
  }
  g_windowMutex = m;
  return true;
}

// Sesión de esta ventana. Sin --window: restaura todas las ventanas guardadas (esta se queda la
// primera libre y abre las demás); si están todas abiertas, abre una ventana nueva.
static std::wstring SessionPath(const std::wstring& windowArg) {
  wchar_t buf[MAX_PATH];
  if (GetEnvironmentVariableW(L"WARPLITE_SESSION", buf, MAX_PATH)) return buf;  // pruebas: un fichero fijo
  if (GetEnvironmentVariableW(L"WARPLITE_DIR", buf, MAX_PATH)) {
    g_windowDir = buf;
  } else {
    if (!GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH)) return L"warp-lite-session.txt";
    g_windowDir = std::wstring(buf) + L"\\warp-lite";
    // Se llamaba better-warp, y antes había una sola sesión: se convierten en la primera ventana.
    CreateDirectoryW(g_windowDir.c_str(), nullptr);
    std::wstring single = g_windowDir + L"\\session.txt";
    CopyFileW((std::wstring(buf) + L"\\BetterWarp\\session.txt").c_str(), single.c_str(), TRUE);
    if (GetFileAttributesW(single.c_str()) != INVALID_FILE_ATTRIBUTES)
      MoveFileExW(single.c_str(), (g_windowDir + L"\\window-1.txt").c_str(), 0);
  }
  CreateDirectoryW(g_windowDir.c_str(), nullptr);
  if (!windowArg.empty()) {
    ClaimWindowFile(windowArg);
    return windowArg;
  }
  std::vector<std::wstring> files;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((g_windowDir + L"\\window-*.txt").c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do files.push_back(g_windowDir + L"\\" + fd.cFileName);
    while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  std::sort(files.begin(), files.end());
  std::wstring mine;
  for (auto& file : files) {
    if (mine.empty()) {
      if (ClaimWindowFile(file)) mine = file;
      continue;
    }
    std::wstring name = L"Local\\warp-lite-" + file.substr(file.find_last_of(L'\\') + 1);
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, name.c_str());
    if (m) CloseHandle(m);  // ya abierta
    else LaunchWindow(file);
  }
  if (mine.empty()) {
    mine = NewWindowFile();
    ClaimWindowFile(mine);
  }
  return mine;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
  HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
  pCreatePseudoConsole = (PFN_CreatePseudoConsole)GetProcAddress(k32, "CreatePseudoConsole");
  pResizePseudoConsole = (PFN_ResizePseudoConsole)GetProcAddress(k32, "ResizePseudoConsole");
  pClosePseudoConsole = (PFN_ClosePseudoConsole)GetProcAddress(k32, "ClosePseudoConsole");
  if (!pCreatePseudoConsole) {
    MessageBoxW(nullptr, T(S_NEED_WIN10), L"warp-lite", MB_ICONERROR);
    return 1;
  }
  typedef BOOL(WINAPI * PFN_Dpi)(HANDLE);
  if (auto fn = (PFN_Dpi)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"))
    fn((HANDLE)-4);  // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2

  int argc = 0;
  std::wstring windowArg;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  for (int i = 1; i + 1 < argc; i++) {
    if (!wcscmp(argv[i], L"--window")) windowArg = argv[++i];
    else if (!wcscmp(argv[i], L"--script")) {
      std::wstring s = argv[++i];
      size_t pos;
      while ((pos = s.find(L'|')) != std::wstring::npos) g_script.push_back(s.substr(0, pos)), s.erase(0, pos + 1);
      if (!s.empty()) g_script.push_back(s);
    }
  }
  LocalFree(argv);
  g_msgQuitAll = RegisterWindowMessageW(L"warp-lite-quit-all");
  g_sessionPath = SessionPath(windowArg);
  if (g_script.empty() && !windowArg.empty()) {  // pruebas: guion para las ventanas que se abren
    wchar_t sc[2048];
    if (GetEnvironmentVariableW(L"WARPLITE_CHILD_SCRIPT", sc, 2048)) {
      std::wstring s = sc;
      size_t pos;
      while ((pos = s.find(L'|')) != std::wstring::npos) g_script.push_back(s.substr(0, pos)), s.erase(0, pos + 1);
      if (!s.empty()) g_script.push_back(s);
    }
  }
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
  wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));  // assets/warp-lite.ico
  wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                 GetSystemMetrics(SM_CYSMICON), 0);
  wc.lpszClassName = L"WarpLite";
  RegisterClassExW(&wc);
  HWND hwnd = CreateWindowExW(0, L"WarpLite", L"warp-lite", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              1280, 760, nullptr, nullptr, inst, nullptr);
  if (!hwnd) return 1;
  SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
  // Mismo tamaño y estado (maximizada o en ventana) que la última vez, si ese sitio sigue en pantalla.
  if (g_hasWinPos && MonitorFromRect(&g_winRect, MONITOR_DEFAULTTONULL)) {
    WINDOWPLACEMENT wp{sizeof wp};
    wp.rcNormalPosition = g_winRect;
    wp.showCmd = g_winMax ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
    if (show == SW_SHOWMINNOACTIVE || show == SW_SHOWMINIMIZED || show == SW_MINIMIZE) wp.showCmd = show;
    SetWindowPlacement(hwnd, &wp);
  } else {
    ShowWindow(hwnd, show);
  }
  UpdateWindow(hwnd);

  MSG m;
  while (GetMessageW(&m, nullptr, 0, 0)) {
    bool keyDown = m.message == WM_KEYDOWN || m.message == WM_SYSKEYDOWN;
    if (g_pick.open && keyDown) {
      if (!PickKey(m.wParam)) TranslateMessage(&m), DispatchMessageW(&m);
      continue;
    }
    if (g_find.open && keyDown && g_renameTab < 0) {
      if (!FindKey(m.wParam)) TranslateMessage(&m), DispatchMessageW(&m);
      continue;
    }
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
