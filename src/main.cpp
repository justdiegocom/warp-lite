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
#include <objbase.h>
#include <shlobj.h>
#include <wincodec.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <restartmanager.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <unordered_map>
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
        ".global hack_bolditalic_end\nhack_bolditalic_end:\n"
        // Iconos de Nerd Fonts (MIT), para los símbolos que Hack no tiene.
        ".global nerd_symbols\nnerd_symbols:\n.incbin \"third_party/nerd-symbols/SymbolsNerdFontMono-Regular.ttf\"\n"
        ".global nerd_symbols_end\nnerd_symbols_end:\n.text\n");
extern "C" const unsigned char hack_regular[], hack_regular_end[], hack_bold[], hack_bold_end[], hack_italic[],
    hack_italic_end[], hack_bolditalic[], hack_bolditalic_end[], nerd_symbols[], nerd_symbols_end[];

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
static const COLORREF kUrl = RGB(0xc2, 0xd9, 0xf5);     // URLs: blanco azulado
static const COLORREF kCmdOk = RGB(0x4e, 0xd1, 0x8f);   // orden que existe
static const COLORREF kCmdBad = RGB(0xf0, 0x55, 0x55);  // orden desconocida
static const COLORREF kChipBorder = RGB(0x3a, 0x3a, 0x3a);
static const uint32_t kAnsi[16] = {0x616161, 0xFF8272, 0xB4FA72, 0xFEFDC2, 0xA5D5FE, 0xFF8FFD,
                                   0xD0D1FE, 0xF1F1F1, 0x8E8E8E, 0xFFC4BD, 0xD6FCB9, 0xFEFDD5,
                                   0xC1E3FE, 0xFFB1FE, 0xE5E6FE, 0xFEFFFF};
static const int kGap = 1;           // separación entre paneles (px)
static const int kPad = 4;           // margen interior de cada panel (px)
static const size_t kScrollback = 5000;

enum { WM_APP_DIRTY = WM_APP + 1, WM_APP_EXITED, WM_APP_SCRIPT, WM_APP_LAYOUT, WM_APP_CLIP, WM_APP_PARAMS, WM_APP_UPDATE, WM_APP_UPDATED, WM_APP_UPDPROG };
enum { TIMER_RESTORE = 1, TIMER_SCRIPT = 2, TIMER_TOAST = 3, TIMER_FRAME = 4, TIMER_UPDATE = 5, TIMER_UPDANIM = 6 };

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
  std::string clipBuf;  // OSC 52 a medio llegar (tmux copia así al portapapeles)
  char selBuf[4096];    // donde libvterm decodifica el base64 del OSC 52
  std::string token;  // va en el OSC 9;9 del prompt: sin él, un `cat` cualquiera podría imitarlo
  std::wstring cwd, edit;
  size_t caret = 0;
  std::vector<std::wstring> history;
  // Órdenes que conoce la shell (alias, funciones, cmdlets), para colorear la orden escrita.
  std::set<std::wstring> cmds;
  std::vector<std::wstring> cmdNames;  // las mismas, con sus mayúsculas (para el autocompletado)
  bool cmdsLoaded = false;
  std::map<std::wstring, bool> existsMemo;
  int histPos = -1;
  RECT input{};
  // Filas que han cambiado desde el último dibujo (con el lock): solo esas se repintan.
  std::vector<char> dirtyRows;
  bool fullDirty = true;
  RECT chip{};          // apartado de la carpeta (clic: selector de carpetas)
  bool resumeRun = false;  // al reabrir, volver a lanzar la orden (estaba en marcha al cerrar)
  int lineRow = -1, lineCol = -1;
  // Restauración tras reiniciar.
  std::string restoreCmd;
  DWORD spawnTick = 0;
  std::wstring agentName;  // nombre de la conversación de Claude Code que corre aquí (título de la pestaña)
  int agent = 0;           // AGENT_* que corre aquí, también si se lanzó con un atajo (ver UpdateRunning)
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

// Actualizaciones (ver «Actualizaciones» más abajo).
enum { UPD_NONE, UPD_AVAILABLE, UPD_INSTALLING, UPD_READY, UPD_FAILED };
struct UpdateState {
  std::wstring running;          // versión de este proceso (la del .exe al arrancar)
  std::wstring latest, url, sha;  // última release y su instalador
  int stage = UPD_NONE;
  bool hidden = false;            // aviso cerrado con la ×
  bool autoUpdate = true;
  bool checking = false, manual = false;
  bool testUrl = false;           // WARPLITE_UPDATE_URL (pruebas): se admite http
  RECT rc{}, action{}, close{};
};
static UpdateState g_upd;
static std::atomic<int> g_updPct{-1};    // % descargado (-1: aún no se sabe)
static std::atomic<int> g_updPhase{0};   // 0 descargando, 1 instalando
static UINT g_msgUpdated;  // otra ventana ha instalado una versión: todas muestran «Reiniciar»

// Textos de la interfaz: inglés por defecto; se cambia con clic derecho en la barra de título.
enum Str { S_TYPE_COMMAND, S_RUNNING, S_ZOOM, S_FONT, S_NEED_WIN10, S_LANGUAGE, S_FIND, S_NO_RESULTS,
           S_SEARCH_DIRS, S_PARENT_DIR, S_RENAME, S_HIDE, S_CLOSE_TAB, S_SHOW_ALL, S_HIDDEN_N, S_STARTUP,
           S_LOADING, S_KIND_CMD, S_KIND_DIR, S_KIND_FILE, S_KIND_PARAM, S_UPD_AVAIL, S_UPD_INSTALLING, S_UPD_READY,
           S_UPD_FAILED, S_UPD_INSTALL, S_UPD_RESTART, S_UPD_RETRY, S_UPD_CHECK, S_UPD_AUTO, S_UPD_LATEST, S_UPD_CHECKING,
           S_UPD_OFFLINE, S_UPD_DOWNLOADING, S_SWITCH, S_OTHER_WINDOW, S_COUNT };
static const wchar_t* const kStrings[][S_COUNT] = {
    {L"Type a command", L"Running… keys go to the program", L"Zoom %d%%", L"Font %d px",
     L"Windows 10 1809 or later is required (ConPTY).", L"Language", L"Find", L"No results",
     L"Search directories…", L".. (Parent directory)", L"Rename", L"Hide tab", L"Close tab", L"Show all",
     L"%d hidden", L"Start with Windows", L"Loading…", L"Command", L"Folder", L"File", L"Parameter",
     L"Warp %s is available", L"Installing %s…", L"Updated to %s", L"Couldn't update to %s", L"Install", L"Restart",
     L"Retry", L"Check for updates", L"Update automatically", L"Warp is up to date", L"Checking for updates…",
     L"Couldn't check for updates", L"Downloading %s…", L"Search terminals…", L"other window"},
    {L"Escribe una orden", L"Ejecutando… las teclas van al programa", L"Escala %d %%", L"Letra %d px",
     L"Hace falta Windows 10 1809 o posterior (ConPTY).", L"Idioma", L"Buscar", L"Sin resultados",
     L"Buscar carpetas…", L".. (Carpeta superior)", L"Renombrar", L"Ocultar pestaña", L"Cerrar pestaña",
     L"Mostrar todas", L"%d ocultas", L"Iniciar con Windows", L"Cargando…", L"Orden", L"Carpeta", L"Archivo",
     L"Parámetro", L"Warp %s disponible", L"Instalando %s…", L"Actualizado a %s", L"No se pudo actualizar a %s",
     L"Instalar", L"Reiniciar", L"Reintentar", L"Buscar actualizaciones", L"Actualizar automáticamente",
     L"Warp está al día", L"Buscando actualizaciones…", L"No se pudo comprobar si hay actualizaciones",
     L"Descargando %s…", L"Buscar terminales…", L"otra ventana"},
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
static UINT g_msgListPanes, g_msgFocusPane;  // selector de terminales entre ventanas (Ctrl+Shift+K)
static std::wstring g_openDir;      // --cd <carpeta> («Abrir Warp aquí»): pestaña nueva ya en ella
// Las ventanas de un mismo Warp (mismo %APPDATA%\warp-lite, o la misma carpeta en las pruebas) se
// reconocen por esta marca: así las pruebas nunca tocan las ventanas de verdad.
static const ULONG_PTR kCopyOpenDir = 0x57434400, kCopyPaneList = 0x574c5300;
static const wchar_t* kInstanceProp = L"warp-lite-instance";
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
  int row = -1, c0 = 0, c1 = 0;  // trozo bajo el ratón
  std::wstring url;
  std::vector<std::array<int, 3>> segs;  // todos los trozos (fila, c0, c1) de una URL partida
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

static void MarkRow(Pane* p, int r) {
  if (r >= 0 && r < (int)p->dirtyRows.size()) p->dirtyRows[r] = 1;
}

static int CbMoveCursor(VTermPos pos, VTermPos old, int, void* user) {
  Pane* p = (Pane*)user;
  MarkRow(p, old.row);
  MarkRow(p, pos.row);
  p->cursor = pos;
  return 1;
}

static int CbDamage(VTermRect r, void* user) {
  Pane* p = (Pane*)user;
  for (int row = r.start_row; row < r.end_row; row++) MarkRow(p, row);
  return 1;
}

static std::atomic<bool> g_titleDirty{false};  // cambió un título: hay que repintar cabeceras y pestañas

static int CbSetTermProp(VTermProp prop, VTermValue* v, void* user) {
  Pane* p = (Pane*)user;
  switch (prop) {
    case VTERM_PROP_CURSORVISIBLE:
      p->cursorVisible = v->boolean;
      MarkRow(p, p->cursor.row);
      break;
    case VTERM_PROP_ALTSCREEN:
      p->altScreen = v->boolean;
      p->scrollOff = 0;
      p->fullDirty = true;
      if (p->integrated) PostMessage(g_hwnd, WM_APP_LAYOUT, 0, (LPARAM)p);  // la caja se oculta a pantalla completa
      break;
    case VTERM_PROP_MOUSE: p->mouseMode = v->number; break;
    case VTERM_PROP_TITLE:
      if (v->string.initial) p->titleBuf.clear();
      p->titleBuf.append(v->string.str, v->string.len);
      if (v->string.final) {
        std::wstring t = ToWide(p->titleBuf);
        if (t != p->title) p->title = t, g_titleDirty = true;
      }
      break;
    default: break;
  }
  return 1;
}

// Solo carpetas locales (C:\...): nada de \\servidor ni \\?\, que Windows abriría por la red.
static bool IsLocalDir(const std::wstring& d) {
  return d.size() >= 3 && iswalpha(d[0]) && d[1] == L':' && d[2] == L'\\';
}

// OSC 9;9;<token>;<ruta> (lo emite el prompt de warp-lite): la shell espera una orden en <ruta>.
static int CbOsc(int command, VTermStringFragment frag, void* user) {
  Pane* p = (Pane*)user;
  if (command != 9) return 0;
  if (frag.initial) p->oscBuf.clear();
  p->oscBuf.append(frag.str, frag.len);
  std::string head = "9;" + p->token + ";";
  if (frag.final && !p->token.empty() && p->oscBuf.rfind(head, 0) == 0) {
    std::wstring dir = ToWide(p->oscBuf.substr(head.size()));
    if (!IsLocalDir(dir)) return 1;
    p->cwd = dir;
    bool first = !p->integrated;
    p->integrated = p->atPrompt = true;
    PostMessage(g_hwnd, WM_APP_LAYOUT, first, (LPARAM)p);
  }
  return 1;
}

// OSC 52 (lo que se copia en tmux): libvterm ya lo trae decodificado; al acabar, al portapapeles.
// Solo escritura: las consultas (OSC 52;c;?) no se contestan.
static int CbSelSet(VTermSelectionMask mask, VTermStringFragment frag, void* user) {
  Pane* p = (Pane*)user;
  if (!(mask & VTERM_SELECTION_CLIPBOARD)) return 1;
  if (frag.initial) p->clipBuf.clear();
  if (p->clipBuf.size() < (8u << 20)) p->clipBuf.append(frag.str ? frag.str : "", frag.len);
  if (frag.final && !p->clipBuf.empty()) PostMessage(g_hwnd, WM_APP_CLIP, 0, (LPARAM) new std::wstring(ToWide(p->clipBuf)));
  return 1;
}

static int CbPushLine(int cols, const VTermScreenCell* cells, void* user) {
  Pane* p = (Pane*)user;
  p->sb.emplace_back(cells, cells + cols);
  if (p->sb.size() > kScrollback) p->sb.pop_front();
  if (p->scrollOff > 0) p->scrollOff = std::min<int>(p->scrollOff + 1, (int)p->sb.size()), p->fullDirty = true;
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
    c.damage = CbDamage;
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
  static VTermSelectionCallbacks selCbs{CbSelSet, nullptr};
  vterm_state_set_selection_callbacks(vterm_obtain_state(p->vt), &selCbs, p, p->selBuf, sizeof p->selBuf);
  vterm_screen_set_damage_merge(p->scr, VTERM_DAMAGE_ROW);  // un aviso por fila cambiada
  p->dirtyRows.assign(p->rows, 1);
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

// Prompt de PowerShell que avisa a warp-lite (OSC 9;9;<token>;<ruta>) y deja solo un "> " gris.
static std::wstring IntegrationArgs(const std::string& token) {
  std::wstring prompt = L"function global:prompt { $e = [char]27; $b = [char]7; "
                        L"$p = $executionContext.SessionState.Path.CurrentLocation.ProviderPath; "
                        L"[Console]::Write(\"$e]9;9;" + ToWide(token) + L";$p$b\"); \"$e[90m> $e[0m\" }; ";
  // El aviso va aparte: PSReadLine redibuja el texto del prompt y no entiende OSC.
  std::wstring script = prompt +
      // Lista de órdenes para colorear la caja de entrada; se escribe en segundo plano.
      L"$null = Register-EngineEvent -SourceIdentifier PowerShell.OnIdle -MaxTriggerCount 1 -Action { "
      L"try { $f = Join-Path $env:TEMP ('warp-lite-cmds-' + $PID + '.txt'); "
      L"Get-Command -CommandType Alias,Function,Filter,Cmdlet | ForEach-Object Name | "
      L"Set-Content -Encoding UTF8 ($f + '.tmp'); Move-Item -Force ($f + '.tmp') $f } catch {} }";
  std::string bytes((const char*)script.data(), script.size() * sizeof(wchar_t));  // UTF-16LE
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

static std::wstring ShellCommand(const std::string& token) {
  wchar_t buf[MAX_PATH * 2];
  DWORD n = GetEnvironmentVariableW(L"WARPLITE_SHELL", buf, MAX_PATH * 2);
  if (n > 0 && n < MAX_PATH * 2) return buf;  // otra shell: sin caja de entrada
  if (SearchPathW(nullptr, L"pwsh.exe", nullptr, MAX_PATH, buf, nullptr)) return L"pwsh.exe -NoLogo" + IntegrationArgs(token);
  return L"powershell.exe -NoLogo" + IntegrationArgs(token);
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
    vterm_screen_flush_damage(p->scr);  // la fila en curso también (spinners que reescriben una línea)
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
  GUID g;  // token nuevo por cada shell
  CoCreateGuid(&g);
  char tok[33];
  snprintf(tok, sizeof tok, "%08lx%04x%04x%02x%02x%02x%02x%02x%02x%02x%02x", g.Data1, g.Data2, g.Data3, g.Data4[0],
           g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
  p->token = tok;
  std::wstring cmd = ShellCommand(p->token);
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
  p->dirtyRows.assign(rows, 1);
  p->fullDirty = true;
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

static Pane* NewTab(int at = -1, const std::wstring& dir = L"") {
  Pane* p = NewPane();
  p->cwd = dir;
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
      // Último panel cerrado: la ventana se cierra y se descarta su sesión (WM_DESTROY).
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
// Claude Code y Codex: si estaban abiertos al cerrar, al reabrir no se relanza la orden tal cual
// (empezaría una conversación nueva) sino que se reanuda la conversación de ese panel.

static std::string JsonStr(const std::string& j, const std::string& key, size_t from);

enum { AGENT_NONE, AGENT_CLAUDE, AGENT_CODEX };

// Trocea una orden por espacios respetando las comillas; cada trozo queda tal como se escribió.
static std::vector<std::string> SplitArgs(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  char q = 0;
  for (char c : s) {
    if (q) {
      if (c == q) q = 0;
    } else if (c == '"' || c == '\'') {
      q = c;
    } else if (c == ' ' || c == '\t') {
      if (!cur.empty()) out.push_back(cur), cur.clear();
      continue;
    }
    cur += c;
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

// El programa que lanza la orden, sin comillas (vacío si no hay).
static std::string ProgramOf(const std::string& cmd) {
  std::vector<std::string> a = SplitArgs(cmd);
  size_t i = !a.empty() && a[0] == "&" ? 1 : 0;
  if (i >= a.size()) return "";
  std::string exe = a[i];
  exe.erase(std::remove(exe.begin(), exe.end(), '"'), exe.end());
  exe.erase(std::remove(exe.begin(), exe.end(), '\''), exe.end());
  return exe;
}

// ¿La orden lanza un .exe? Una shell (cmd, pwsh, wsl…) en la que después se abre Claude no es un
// atajo suyo: `pwsh --resume <id>` no reanudaría nada.
static bool LaunchesExe(const std::string& cmd) {
  static std::map<std::string, bool> memo;
  std::string exe = ProgramOf(cmd);
  auto it = memo.find(exe);
  if (it != memo.end()) return it->second;
  wchar_t buf[MAX_PATH];
  DWORD n = exe.empty() ? 0 : SearchPathW(nullptr, ToWide(exe).c_str(), L".exe", MAX_PATH, buf, nullptr);
  return memo[exe] = n > 4 && n < MAX_PATH && _wcsicmp(buf + n - 4, L".exe") == 0;
}

static int AgentKind(const std::string& cmd) {
  std::string exe = ProgramOf(cmd);
  if (exe.empty()) return AGENT_NONE;
  size_t slash = exe.find_last_of("\\/");
  if (slash != std::string::npos) exe = exe.substr(slash + 1);
  std::transform(exe.begin(), exe.end(), exe.begin(), [](char c) { return (char)tolower((unsigned char)c); });
  for (const char* ext : {".exe", ".cmd", ".ps1", ".bat"})
    if (exe.size() > strlen(ext) && exe.compare(exe.size() - strlen(ext), strlen(ext), ext) == 0) exe.resize(exe.size() - strlen(ext));
  return exe == "claude" ? AGENT_CLAUDE : exe == "codex" ? AGENT_CODEX : AGENT_NONE;
}

// La orden para reanudar: el mismo programa con sus opciones (--model, --yolo…), sin el texto
// inicial ni lo que ya elegía sesión, y con la sesión dada (vacía: la última de esa carpeta).
// Vacía si no es una conversación interactiva (claude -p, codex exec, subórdenes…).
// kind: lo que corre de verdad, que puede venir de un atajo (clauded.cmd con
// «claude --dangerously-skip-permissions %*», una función de PowerShell…).
static std::string ResumeCommand(const std::string& cmd, const std::string& session, int kind) {
  if (kind == AGENT_NONE) return "";
  bool shortcut = AgentKind(cmd) == AGENT_NONE;
  std::vector<std::string> a = SplitArgs(cmd);
  size_t i = !a.empty() && a[0] == "&" ? 1 : 0;
  if (i >= a.size()) return "";
  std::string head = (i ? "& " : "") + a[i];
  // Opciones sin valor (lo que va detrás es texto, no su valor) y las que se quitan.
  static const std::set<std::string> flagsClaude = {"--dangerously-skip-permissions", "--verbose", "--debug", "-d", "--ide",
                                                    "--chrome", "--no-chrome", "--strict-mcp-config", "--fork-session"};
  static const std::set<std::string> flagsCodex = {"--yolo", "--dangerously-bypass-approvals-and-sandbox", "--full-auto",
                                                   "--search", "--oss", "--no-alt-screen", "--last", "--all"};
  static const std::set<std::string> dropClaude = {"-c", "--continue", "-r", "--resume", "--session-id", "--fork-session"};
  static const std::set<std::string> dropCodex = {"--last", "--all"};
  static const std::set<std::string> otherClaude = {"mcp", "config", "update", "doctor", "install", "migrate-installer",
                                                    "setup-token", "plugin", "plugins", "agents", "auth", "login", "logout"};
  static const std::set<std::string> otherCodex = {"exec", "e", "review", "login", "logout", "mcp", "mcp-server", "app-server",
                                                   "apply", "a", "sandbox", "debug", "cloud", "completion", "features", "fork", "help"};
  const auto& flags = kind == AGENT_CLAUDE ? flagsClaude : flagsCodex;
  const auto& drop = kind == AGENT_CLAUDE ? dropClaude : dropCodex;
  std::string opts;
  bool sawWord = false;
  for (size_t k = i + 1; k < a.size(); k++) {
    const std::string& t = a[k];
    if (t.size() > 1 && t[0] == '-') {
      std::string name = t.substr(0, t.find('='));
      if (kind == AGENT_CLAUDE && (name == "-p" || name == "--print")) return "";
      bool takesValue = t.find('=') == std::string::npos && !flags.count(name) &&
                        !(kind == AGENT_CLAUDE && (name == "-c" || name == "--continue")) && k + 1 < a.size() &&
                        a[k + 1][0] != '-';
      // claude -r sin id abre el selector: solo se come lo siguiente si parece un id.
      if (kind == AGENT_CLAUDE && (name == "-r" || name == "--resume") && takesValue)
        takesValue = a[k + 1].find(' ') == std::string::npos && a[k + 1].size() >= 8;
      if (!drop.count(name)) opts += " " + t + (takesValue ? " " + a[k + 1] : "");
      if (takesValue) k++;
      continue;
    }
    // Con un atajo no se sabe si las palabras son el texto inicial o suyas: se relanza tal cual.
    if (shortcut) return "";
    if (!sawWord) {  // primera palabra: ¿una suborden?
      sawWord = true;
      if (kind == AGENT_CODEX && t == "resume") continue;
      if (kind == AGENT_CODEX && otherCodex.count(t)) return "";
      if (kind == AGENT_CLAUDE && otherClaude.count(t)) return "";
    }
    // Texto inicial o id de sesión: fuera.
  }
  if (kind == AGENT_CLAUDE) return head + opts + (session.empty() ? " --continue" : " --resume " + session);
  return head + opts + (session.empty() ? " resume --last" : " resume " + session);
}

static std::string ResumeCommand(const std::string& cmd, const std::string& session) {
  return ResumeCommand(cmd, session, AgentKind(cmd));
}

// ¿Ya elige conversación? (claude --resume/--continue, codex resume)
static bool PicksSession(const std::string& cmd) {
  bool claude = AgentKind(cmd) == AGENT_CLAUDE;
  for (const std::string& t : SplitArgs(cmd)) {
    std::string name = t.substr(0, t.find('='));
    if (claude ? name == "-c" || name == "--continue" || name == "-r" || name == "--resume" || name == "--session-id"
               : name == "resume")
      return true;
  }
  return false;
}

struct AgentSession {
  std::string id;
  FILETIME mtime;
  std::wstring path;
};

static bool NewerFirst(const AgentSession& x, const AgentSession& y) { return CompareFileTime(&x.mtime, &y.mtime) > 0; }

static std::wstring EnvDir(const wchar_t* var, const wchar_t* fallback) {
  wchar_t buf[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(var, buf, MAX_PATH);
  if (n > 0 && n < MAX_PATH) return buf;
  n = GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
  return n > 0 && n < MAX_PATH ? std::wstring(buf) + fallback : L"";
}

// Conversaciones de Claude Code en esa carpeta, de la más reciente a la más antigua:
// %USERPROFILE%\.claude\projects\<carpeta con todo lo que no es letra o cifra cambiado por «-»>\<id>.jsonl
static std::vector<AgentSession> ClaudeSessions(const std::wstring& cwd) {
  std::vector<AgentSession> out;
  std::wstring root = EnvDir(L"CLAUDE_CONFIG_DIR", L"\\.claude");
  if (root.empty() || cwd.empty()) return out;
  std::wstring name = cwd;
  for (wchar_t& c : name)
    if (!iswascii(c) || !iswalnum(c)) c = L'-';
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((root + L"\\projects\\" + name + L"\\*.jsonl").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return out;
  do {
    std::wstring f = fd.cFileName;
    if (f.size() == 36 + 6 && f[8] == L'-' && f[13] == L'-')  // <uuid>.jsonl
      out.push_back({ToUtf8(f.substr(0, 36)), fd.ftLastWriteTime, L""});
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  std::sort(out.begin(), out.end(), NewerFirst);
  return out;
}

// Conversaciones de Codex en esa carpeta (las de la última semana): %USERPROFILE%\.codex\sessions\
// AAAA\MM\DD\rollout-…-<id>.jsonl, cuya primera línea dice su carpeta («cwd»).
static std::vector<AgentSession> CodexSessions(const std::wstring& cwd) {
  std::vector<AgentSession> out;
  std::wstring root = EnvDir(L"CODEX_HOME", L"\\.codex");
  if (root.empty() || cwd.empty()) return out;
  FILETIME now;
  GetSystemTimeAsFileTime(&now);
  std::set<std::wstring> days;
  for (int d = 0; d < 8; d++) {
    ULARGE_INTEGER u{{now.dwLowDateTime, now.dwHighDateTime}};
    u.QuadPart -= (ULONGLONG)d * 24 * 3600 * 10000000ULL;
    FILETIME ft{u.LowPart, u.HighPart}, lt;
    SYSTEMTIME st;
    for (int local = 0; local < 2; local++) {  // las carpetas van por fecha local o UTC según la versión
      if (local ? !FileTimeToLocalFileTime(&ft, &lt) || !FileTimeToSystemTime(&lt, &st) : !FileTimeToSystemTime(&ft, &st)) continue;
      wchar_t b[32];
      swprintf(b, 32, L"\\%04d\\%02d\\%02d\\", st.wYear, st.wMonth, st.wDay);
      days.insert(b);
    }
  }
  for (const std::wstring& day : days) {
    std::wstring dir = root + L"\\sessions" + day;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"rollout-*.jsonl").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) continue;
    do {
      FILE* f = _wfopen((dir + fd.cFileName).c_str(), L"rb");
      if (!f) continue;
      std::string head(4096, '\0');
      head.resize(fread(head.data(), 1, head.size(), f));
      fclose(f);
      head = head.substr(0, head.find('\n'));
      if (JsonStr(head, "source", 0) == "exec") continue;  // codex exec: no es interactiva
      std::string dir8 = JsonStr(head, "cwd", 0), id = JsonStr(head, "id", 0);
      for (size_t k = 0; (k = dir8.find("\\\\", k)) != std::string::npos; k++) dir8.erase(k, 1);
      if (id.empty() || _wcsicmp(ToWide(dir8).c_str(), cwd.c_str()) != 0) continue;
      out.push_back({id, fd.ftLastWriteTime, dir + fd.cFileName});
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  std::sort(out.begin(), out.end(), NewerFirst);
  return out;
}

// Procesos que cuelgan (a cualquier profundidad) de la shell de cada panel.
static std::map<DWORD, std::vector<DWORD>> ChildrenByParent() {
  std::map<DWORD, std::vector<DWORD>> kids;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return kids;
  PROCESSENTRY32W pe{sizeof pe};
  for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
    if (pe.th32ProcessID != pe.th32ParentProcessID) kids[pe.th32ParentProcessID].push_back(pe.th32ProcessID);
  CloseHandle(snap);
  return kids;
}

static std::vector<DWORD> Descendants(const std::map<DWORD, std::vector<DWORD>>& kids, DWORD root) {
  std::vector<DWORD> out, todo{root};
  std::set<DWORD> seen{root};
  while (!todo.empty()) {
    auto it = kids.find(todo.back());
    todo.pop_back();
    if (it == kids.end()) continue;
    for (DWORD c : it->second)
      if (seen.insert(c).second) out.push_back(c), todo.push_back(c);
  }
  return out;
}

// Claude Code apunta en %USERPROFILE%\.claude\sessions\<pid>.json la conversación de cada proceso
// (también tras /clear o /resume): con eso cada panel sabe exactamente cuál es la suya.
static std::string ClaudeSessionJson(DWORD pid) {
  std::wstring root = EnvDir(L"CLAUDE_CONFIG_DIR", L"\\.claude");
  FILE* f = root.empty() ? nullptr : _wfopen((root + L"\\sessions\\" + std::to_wstring(pid) + L".json").c_str(), L"rb");
  if (!f) return "";
  std::string j(8192, '\0');
  j.resize(fread(j.data(), 1, j.size(), f));
  fclose(f);
  return j;
}

static std::string ClaudeSessionOfPid(DWORD pid) { return JsonStr(ClaudeSessionJson(pid), "sessionId", 0); }

// Codex tiene abierto el archivo de su conversación mientras corre: el Restart Manager de Windows
// dice qué procesos lo tienen abierto.
static std::vector<DWORD> PidsWithFileOpen(const std::wstring& path) {
  std::vector<DWORD> out;
  DWORD h;
  WCHAR key[CCH_RM_SESSION_KEY + 1] = {};
  if (RmStartSession(&h, 0, key) != ERROR_SUCCESS) return out;
  LPCWSTR files[] = {path.c_str()};
  if (RmRegisterResources(h, 1, files, 0, nullptr, 0, nullptr) == ERROR_SUCCESS) {
    UINT need = 0, count = 0;
    DWORD reasons = 0;
    std::vector<RM_PROCESS_INFO> info;
    DWORD r = RmGetList(h, &need, &count, nullptr, &reasons);
    if (r == ERROR_MORE_DATA && need) {
      info.resize(need);
      count = need;
      if (RmGetList(h, &need, &count, info.data(), &reasons) == ERROR_SUCCESS)
        for (UINT i = 0; i < count; i++) out.push_back(info[i].Process.dwProcessId);
    }
  }
  RmEndSession(h);
  return out;
}

// Para cada panel con Claude Code o Codex en marcha, la orden que lo reanuda. Primero lo exacto
// (la conversación del proceso que corre en ese panel); los que queden, por carpeta: el que
// escribió hace menos se queda con la conversación tocada hace menos.
static std::map<Pane*, std::string> g_resumeCmd;

static void ComputeResumes() {
  g_resumeCmd.clear();
  std::map<std::pair<int, std::wstring>, std::vector<Pane*>> groups;
  std::map<DWORD, std::vector<DWORD>> kids;
  bool haveKids = false;
  for (auto& t : g_tabs) {
    std::vector<Pane*> ls;
    Leaves(t.root, ls);
    for (Pane* p : ls)
      if (!p->runningCmd.empty() && !ResumeCommand(p->lastCmd, "", p->agent).empty()) {
        std::wstring key = p->cwd;
        std::transform(key.begin(), key.end(), key.begin(), towlower);
        groups[{p->agent, key}].push_back(p);
        if (!haveKids) kids = ChildrenByParent(), haveKids = true;
      }
  }
  for (auto& [key, panes] : groups) {
    std::sort(panes.begin(), panes.end(), [](Pane* x, Pane* y) { return (int)(x->lastOut - y->lastOut) > 0; });
    std::wstring cwd = panes[0]->cwd;
    std::vector<AgentSession> s = key.first == AGENT_CLAUDE ? ClaudeSessions(cwd) : CodexSessions(cwd);
    std::map<DWORD, std::string> codexByPid;
    if (key.first == AGENT_CODEX)
      for (size_t k = 0; k < s.size() && k < 30; k++)
        for (DWORD pid : PidsWithFileOpen(s[k].path)) codexByPid[pid] = s[k].id;
    std::set<std::string> taken;
    std::vector<Pane*> rest;
    for (Pane* p : panes) {
      std::string id;
      for (DWORD pid : Descendants(kids, p->pid)) {
        if (key.first == AGENT_CLAUDE) id = ClaudeSessionOfPid(pid);
        else if (codexByPid.count(pid)) id = codexByPid[pid];
        if (!id.empty()) break;
      }
      if (id.empty() || !taken.insert(id).second) {
        rest.push_back(p);
        continue;
      }
      std::string cmd = ResumeCommand(p->lastCmd, id, key.first);
      // Claude abierto sin escribirle nada: esa conversación no existe todavía y --resume fallaría.
      bool exists = key.first != AGENT_CLAUDE ||
                    std::any_of(s.begin(), s.end(), [&](const AgentSession& a) { return a.id == id; });
      if (!exists) cmd = cmd.substr(0, cmd.size() - id.size() - 10) + " --session-id " + id;
      g_resumeCmd[p] = cmd;
    }
    size_t k = 0;
    for (Pane* p : rest) {
      while (k < s.size() && taken.count(s[k].id)) k++;
      std::string id = k < s.size() ? s[k++].id : "";
      if (!id.empty()) taken.insert(id);
      g_resumeCmd[p] = ResumeCommand(p->lastCmd, id, key.first);
    }
  }
}

// ---------------------------------------------------------------------------
// Sesión: pestañas, divisiones y última orden de cada panel

static void SaveNode(std::ostringstream& o, Node* n) {
  if (n->leaf()) {
    // «run»: tenía algo en marcha (p. ej. ssh + tmux) y se vuelve a lanzar al reabrir; Claude Code
    // y Codex, con la orden que reanuda su conversación.
    auto r = g_resumeCmd.find(n->pane);
    const std::string& cmd = r != g_resumeCmd.end() && !n->pane->runningCmd.empty() ? r->second : n->pane->lastCmd;
    o << "L " << HexEncode(cmd) << " " << HexEncode(ToUtf8(n->pane->cwd))
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
  ComputeResumes();
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
    if (!IsLocalDir(p->cwd)) p->cwd.clear();
    auto pos2 = in.tellg();
    std::string flag;
    if (in >> flag && flag == "run") {
      p->resumeRun = true;
    } else {
      in.clear();
      in.seekg(pos2);
    }
    // Guardada sin conversación (por una versión anterior o sin encontrar su id): `claude` a secas
    // también reanuda, la última de esa carpeta.
    if (p->resumeRun && AgentKind(p->lastCmd) != AGENT_NONE && !PicksSession(p->lastCmd)) {
      std::string r = ResumeCommand(p->lastCmd, "");
      if (!r.empty()) p->lastCmd = r;
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

static void SetClipboardText(const std::wstring& s) {
  if (s.empty() || !OpenClipboard(g_hwnd)) return;
  EmptyClipboard();
  HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t));
  memcpy(GlobalLock(h), s.c_str(), (s.size() + 1) * sizeof(wchar_t));
  GlobalUnlock(h);
  SetClipboardData(CF_UNICODETEXT, h);
  CloseClipboard();
}

static void CopySelection() { SetClipboardText(SelectionText()); }

static bool EditorActive(Pane* p);
static void EditorInsert(Pane* p, const std::wstring& t);

static std::wstring QuotePath(const std::wstring& path) {
  return path.find(L' ') == std::wstring::npos ? path : L"\"" + path + L"\"";
}

// Guarda la imagen del portapapeles (abierto) como PNG con WIC.
static bool SaveClipboardPng(HBITMAP hb, const std::wstring& path) {
  IWICImagingFactory* f = nullptr;
  IWICBitmap* bmp = nullptr;
  IWICStream* st = nullptr;
  IWICBitmapEncoder* enc = nullptr;
  IWICBitmapFrameEncode* fr = nullptr;
  bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, (void**)&f)) &&
            SUCCEEDED(f->CreateBitmapFromHBITMAP(hb, nullptr, WICBitmapIgnoreAlpha, &bmp)) &&
            SUCCEEDED(f->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
            SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
            SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) &&
            SUCCEEDED(fr->Initialize(nullptr));
  if (ok) {
    UINT w = 0, h = 0;
    bmp->GetSize(&w, &h);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    ok = SUCCEEDED(fr->SetSize(w, h)) && SUCCEEDED(fr->SetPixelFormat(&fmt)) && SUCCEEDED(fr->WriteSource(bmp, nullptr)) &&
         SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
  }
  if (fr) fr->Release();
  if (enc) enc->Release();
  if (st) st->Release();
  if (bmp) bmp->Release();
  if (f) f->Release();
  return ok;
}

// Portapapeles sin texto: archivos copiados → sus rutas; imagen → se guarda en %TEMP%\warp-lite
// como PNG y se devuelve su ruta. Vacío si no hay nada que pegar.
static std::wstring ClipboardAsPaths() {
  if (!OpenClipboard(g_hwnd)) return {};
  std::wstring out;
  if (HDROP drop = (HDROP)GetClipboardData(CF_HDROP)) {
    UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < n; i++) {
      wchar_t f[MAX_PATH * 2];
      if (DragQueryFileW(drop, i, f, MAX_PATH * 2)) out += (out.empty() ? L"" : L" ") + QuotePath(f);
    }
  } else if (HBITMAP hb = (HBITMAP)GetClipboardData(CF_BITMAP)) {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"warp-lite";
    CreateDirectoryW(dir.c_str(), nullptr);
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[64];
    swprintf(name, 64, L"\\paste-%04d%02d%02d-%02d%02d%02d-%03d.png", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
             t.wSecond, t.wMilliseconds);
    std::wstring path = dir + name;
    if (SaveClipboardPng(hb, path)) out = QuotePath(path);
  }
  CloseClipboard();
  return out;
}

static void PasteText(Pane* p, std::wstring w);

static void Paste(Pane* p) {
  if (!OpenClipboard(g_hwnd)) return;
  std::wstring w;
  if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
    w = (const wchar_t*)GlobalLock(h);
    GlobalUnlock(h);
  }
  CloseClipboard();
  if (w.empty()) w = ClipboardAsPaths();  // imagen o archivos: se pega su ruta de Windows
  PasteText(p, w);
}

// Escribe texto como un pegado: en la caja de entrada o, si hay un programa en marcha, a él.
static void PasteText(Pane* p, std::wstring w) {
  if (w.empty()) return;
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

static void Toast(const std::wstring& t, UINT ms = 1500) {
  g_toast = t;
  SetTimer(g_hwnd, TIMER_TOAST, ms, nullptr);
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
static void OpenSwitcher();

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
  ComputeResumes();
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
      case 'K': OpenSwitcher(); return true;  // Ctrl+Shift+K: saltar a cualquier terminal
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

static std::wstring Lower(std::wstring s);

// ---------------------------------------------------------------------------
// Ventanas del mismo Warp

static HANDLE InstanceKey() {
  wchar_t buf[MAX_PATH];
  std::wstring k;
  if (GetEnvironmentVariableW(L"WARPLITE_SESSION", buf, MAX_PATH)) {
    k = buf;
    k = k.substr(0, k.find_last_of(L"\\/") == std::wstring::npos ? 0 : k.find_last_of(L"\\/"));
  } else if (GetEnvironmentVariableW(L"WARPLITE_DIR", buf, MAX_PATH)) {
    k = buf;
  } else if (GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH)) {
    k = std::wstring(buf) + L"\\warp-lite";
  }
  uint32_t h = 2166136261u;  // FNV-1a
  for (wchar_t c : k) h = (h ^ (uint32_t)towlower(c)) * 16777619u;
  return (HANDLE)(ULONG_PTR)(h | 1);
}

// Las otras ventanas de este Warp, de la de delante a la de detrás.
static std::vector<HWND> SiblingWindows() {
  struct Ctx {
    HANDLE key;
    std::vector<HWND> out;
  } ctx{InstanceKey(), {}};
  EnumWindows(
      [](HWND h, LPARAM lp) -> BOOL {
        Ctx* c = (Ctx*)lp;
        wchar_t cls[32];
        if (h != g_hwnd && GetClassNameW(h, cls, 32) && !wcscmp(cls, L"WarpLite") && GetPropW(h, kInstanceProp) == c->key)
          c->out.push_back(h);
        return TRUE;
      },
      (LPARAM)&ctx);
  return ctx.out;
}

static void BringToFront(HWND h) {
  if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
  SetForegroundWindow(h);
}

// «Abrir Warp aquí»: pestaña nueva en esa carpeta, detrás de la activa.
static void OpenDirTab(const std::wstring& dir) {
  NewTab(g_tabs.empty() ? -1 : g_activeTab + 1, dir);
  SaveSession();
  BringToFront(g_hwnd);
  InvalidateRect(g_hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Selector de terminales (Ctrl+Shift+K): todas las de todas las ventanas, con su conversación de
// Claude, su carpeta y lo que tienen en marcha; se escribe para filtrar y Enter salta a ella.

struct SwitchItem {
  HWND hwnd;  // ventana (la propia o la de otro proceso)
  int tab, pane;
  std::wstring title, cwd, running;
};
struct Switcher {
  bool open = false;
  std::wstring filter;
  std::vector<SwitchItem> all, shown;
  int sel = 0, top = 0;
  RECT rc{};
};
static Switcher g_sw;

// Una línea por panel: pestaña, panel, título, carpeta y orden en marcha, separados por tabuladores.
static std::wstring PaneListText() {
  auto clean = [](std::wstring t) {
    for (wchar_t& c : t)
      if (c == L'\t' || c == L'\n' || c == L'\r') c = L' ';
    return t;
  };
  std::wstring out;
  for (int i = 0; i < (int)g_tabs.size(); i++) {
    std::vector<Pane*> ls;
    Leaves(g_tabs[i].root, ls);
    for (int j = 0; j < (int)ls.size(); j++) {
      Pane* p = ls[j];
      std::wstring title = !p->agentName.empty() ? p->agentName
                           : ls.size() == 1 && !g_tabs[i].name.empty() ? g_tabs[i].name : ShortTitle(p);
      out += std::to_wstring(i) + L"\t" + std::to_wstring(j) + L"\t" + clean(title) + L"\t" + clean(p->cwd) + L"\t" +
             clean(p->runningCmd) + L"\n";
    }
  }
  return out;
}

static void AddPaneList(HWND from, const std::wstring& text) {
  size_t a = 0;
  while (a < text.size()) {
    size_t e = text.find(L'\n', a);
    if (e == std::wstring::npos) e = text.size();
    std::vector<std::wstring> f;
    for (size_t b = a; b <= e;) {
      size_t t = text.find(L'\t', b);
      if (t == std::wstring::npos || t > e) t = e;
      f.push_back(text.substr(b, t - b));
      b = t + 1;
    }
    if (f.size() >= 5) g_sw.all.push_back({from, _wtoi(f[0].c_str()), _wtoi(f[1].c_str()), f[2], f[3], f[4]});
    a = e + 1;
  }
}

static void SwFilter() {
  std::wstring f = Lower(g_sw.filter);
  g_sw.shown.clear();
  for (auto& it : g_sw.all)
    if (f.empty() || Lower(it.title + L" " + it.cwd + L" " + it.running).find(f) != std::wstring::npos) g_sw.shown.push_back(it);
  g_sw.sel = 0;
  g_sw.top = 0;
  // Sin filtro, la seleccionada es la terminal en la que se está.
  if (f.empty() && !g_tabs.empty()) {
    std::vector<Pane*> ls;
    Leaves(CurTab().root, ls);
    int pj = (int)(std::find(ls.begin(), ls.end(), CurTab().active) - ls.begin());
    for (int i = 0; i < (int)g_sw.shown.size(); i++)
      if (g_sw.shown[i].hwnd == g_hwnd && g_sw.shown[i].tab == g_activeTab && g_sw.shown[i].pane == pj) g_sw.sel = i;
  }
}

static void OpenSwitcher() {
  g_sw = Switcher{};
  g_sw.open = true;
  AddPaneList(g_hwnd, PaneListText());
  // Las otras ventanas contestan con WM_COPYDATA mientras se espera aquí.
  for (HWND h : SiblingWindows()) {
    DWORD_PTR r;
    SendMessageTimeoutW(h, g_msgListPanes, (WPARAM)g_hwnd, 0, SMTO_ABORTIFHUNG | SMTO_NORMAL, 500, &r);
  }
  SwFilter();
  if (g_sw.sel >= kPickRows) g_sw.top = g_sw.sel - kPickRows + 1;
  InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void CloseSwitcher() {
  g_sw.open = false;
  InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void FocusPane(int tab, int pane) {
  if (tab < 0 || tab >= (int)g_tabs.size()) return;
  ActivateTab(tab);
  std::vector<Pane*> ls;
  Leaves(g_tabs[tab].root, ls);
  if (pane >= 0 && pane < (int)ls.size()) g_tabs[tab].active = ls[pane];
  InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void SwChoose(int i) {
  if (i < 0 || i >= (int)g_sw.shown.size()) return;
  SwitchItem it = g_sw.shown[i];
  CloseSwitcher();
  if (it.hwnd == g_hwnd) {
    FocusPane(it.tab, it.pane);
  } else if (IsWindow(it.hwnd)) {
    PostMessageW(it.hwnd, g_msgFocusPane, it.tab, it.pane);
    BringToFront(it.hwnd);
  }
}

static bool SwKey(WPARAM vk) {
  int n = (int)g_sw.shown.size();
  switch (vk) {
    case VK_ESCAPE: CloseSwitcher(); return true;
    case VK_RETURN: SwChoose(g_sw.sel); return true;
    case VK_UP: g_sw.sel = std::max(0, g_sw.sel - 1); break;
    case VK_DOWN: g_sw.sel = std::min(n - 1, g_sw.sel + 1); break;
    case VK_PRIOR: g_sw.sel = std::max(0, g_sw.sel - kPickRows); break;
    case VK_NEXT: g_sw.sel = std::min(n - 1, g_sw.sel + kPickRows); break;
    case VK_BACK:
      if (!g_sw.filter.empty()) g_sw.filter.pop_back();
      SwFilter();
      break;
    case 'K':
      if ((GetKeyState(VK_CONTROL) & 0x8000) && (GetKeyState(VK_SHIFT) & 0x8000)) {
        CloseSwitcher();  // el mismo atajo lo cierra
        return true;
      }
      return false;
    default: return false;
  }
  g_sw.sel = std::max(0, g_sw.sel);
  if (g_sw.sel < g_sw.top) g_sw.top = g_sw.sel;
  if (g_sw.sel >= g_sw.top + kPickRows) g_sw.top = g_sw.sel - kPickRows + 1;
  InvalidateRect(g_hwnd, nullptr, FALSE);
  return true;
}

static void SwChar(wchar_t c) {
  if (c < 0x20) return;
  g_sw.filter += c;
  SwFilter();
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
    if (line.empty()) continue;
    std::wstring w = ToWide(line);
    if (p->cmds.insert(Lower(w)).second) p->cmdNames.push_back(w);
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

// ---------------------------------------------------------------------------
// Autocompletado con Tab, como el menú de Warp: órdenes en la primera palabra, parámetros tras
// un «-» y rutas en el resto. Una sola opción se escribe directamente; varias abren el menú.

enum { COMP_CMD, COMP_DIR, COMP_FILE, COMP_PARAM };
struct CompItem {
  std::wstring label, insert;
  int kind;
};
struct CompState {
  bool open = false;
  Pane* p = nullptr;
  size_t start = 0;            // dónde empieza la palabra que se completa
  std::vector<CompItem> items;
  int sel = 0, top = 0;
  std::wstring loading;        // orden cuyos parámetros se están pidiendo a PowerShell
  bool autoAccept = false;     // abierto con Tab y sin escribir nada más: si queda una, se escribe
  RECT rc{};
};
static CompState g_comp;
static const int kCompRows = 9;
// Parámetros por orden (en minúsculas), pedidos una vez a un PowerShell aparte.
static std::map<std::wstring, std::vector<std::wstring>> g_params;
static std::set<std::wstring> g_paramsPending;

struct ParamsReply {
  std::wstring cmd;
  std::vector<std::wstring> names;
};

static DWORD WINAPI ParamsThread(LPVOID arg) {
  std::unique_ptr<std::wstring> cmd((std::wstring*)arg);
  auto* reply = new ParamsReply{*cmd, {}};
  wchar_t buf[MAX_PATH];
  std::wstring exe = SearchPathW(nullptr, L"pwsh.exe", nullptr, MAX_PATH, buf, nullptr) ? L"pwsh.exe" : L"powershell.exe";
  // El nombre solo lleva letras, cifras, «-», «_» y «.» (lo comprueba quien llama): no hay inyección.
  std::wstring line = exe + L" -NoLogo -NoProfile -NonInteractive -Command \"$c = Get-Command -Name '" + *cmd +
                      L"' -ErrorAction SilentlyContinue | Select-Object -First 1; "
                      L"if ($c -and $c.Parameters) { $c.Parameters.Keys }\"";
  SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
  HANDLE rd = nullptr, wr = nullptr;
  if (CreatePipe(&rd, &wr, &sa, 0)) {
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
      CloseHandle(wr);
      wr = nullptr;
      std::string out;
      char chunk[4096];
      DWORD n;
      while (ReadFile(rd, chunk, sizeof chunk, &n, nullptr) && n) out.append(chunk, n);
      WaitForSingleObject(pi.hProcess, 15000);
      CloseHandle(pi.hProcess);
      CloseHandle(pi.hThread);
      std::istringstream in(out);
      std::string l;
      while (std::getline(in, l)) {
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
        if (!l.empty()) reply->names.push_back(L"-" + ToWide(l));
      }
    }
    if (wr) CloseHandle(wr);
    CloseHandle(rd);
  }
  if (!PostMessage(g_hwnd, WM_APP_PARAMS, 0, (LPARAM)reply)) delete reply;
  return 0;
}

static void RequestParams(const std::wstring& cmd) {
  std::wstring low = Lower(cmd);
  if (g_params.count(low) || g_paramsPending.count(low)) return;
  g_paramsPending.insert(low);
  HANDLE t = CreateThread(nullptr, 0, ParamsThread, new std::wstring(cmd), 0, nullptr);
  if (t) CloseHandle(t);
  else g_paramsPending.erase(low);
}

static bool StartsWithI(const std::wstring& s, const std::wstring& pre) {
  return s.size() >= pre.size() && !_wcsnicmp(s.c_str(), pre.c_str(), pre.size());
}

// Ruta como cadena literal de PowerShell si hace falta (espacios, comillas, etc.).
static std::wstring QuoteIfNeeded(const std::wstring& s, bool force) {
  if (!force && s.find_first_of(L" '\"`$&()[]{};,@#") == std::wstring::npos) return s;
  std::wstring q = L"'";
  for (wchar_t c : s) {
    q += c;
    if (c == L'\'' || c == 0x2018 || c == 0x2019 || c == 0x201A || c == 0x201B) q += c;
  }
  return q + L"'";
}

static void PathCandidates(Pane* p, std::wstring tok, std::vector<CompItem>& out) {
  bool quoted = !tok.empty() && (tok[0] == L'\'' || tok[0] == L'"');
  if (quoted) {
    wchar_t q = tok[0];
    tok.erase(0, 1);
    if (!tok.empty() && tok.back() == q) tok.pop_back();
  }
  size_t k = tok.find_last_of(L"\\/");
  std::wstring dirPart = k == std::wstring::npos ? L"" : tok.substr(0, k + 1), pre = tok.substr(dirPart.size());
  std::wstring base;
  if (dirPart.rfind(L"\\\\", 0) == 0 || dirPart.rfind(L"//", 0) == 0) return;  // red: nunca
  if (dirPart.empty()) base = p->cwd;
  else if (dirPart[0] == L'~') {
    wchar_t home[MAX_PATH] = L"";
    GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
    base = home + dirPart.substr(1);
  } else if (dirPart.size() >= 2 && dirPart[1] == L':') base = dirPart;
  else if (dirPart[0] == L'\\' || dirPart[0] == L'/') base = p->cwd.substr(0, 2) + dirPart;
  else base = p->cwd + L"\\" + dirPart;
  if (!IsLocalDir(base) || base.size() > 1000) return;
  if (base.back() != L'\\' && base.back() != L'/') base += L'\\';
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((base + L"*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  std::vector<CompItem> dirs, files;
  do {
    std::wstring n = fd.cFileName;
    if (n == L"." || n == L"..") continue;
    if ((fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) && pre.empty()) continue;
    if (!StartsWithI(n, pre)) continue;
    bool dir = fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
    std::wstring ins = dirPart + n + (dir ? L"\\" : L"");
    (dir ? dirs : files).push_back({n + (dir ? L"\\" : L""), QuoteIfNeeded(ins, quoted), dir ? COMP_DIR : COMP_FILE});
  } while (FindNextFileW(h, &fd) && dirs.size() + files.size() < 2000);
  FindClose(h);
  auto byName = [](const CompItem& a, const CompItem& b) { return _wcsicmp(a.label.c_str(), b.label.c_str()) < 0; };
  std::sort(dirs.begin(), dirs.end(), byName);
  std::sort(files.begin(), files.end(), byName);
  out.insert(out.end(), dirs.begin(), dirs.end());
  out.insert(out.end(), files.begin(), files.end());
}

// Calcula las opciones para la palabra que acaba en el cursor.
static void CompCompute(Pane* p) {
  const std::wstring& e = p->edit;
  size_t c = std::min(p->caret, e.size());
  size_t start = 0, segStart = 0;
  wchar_t q = 0;
  for (size_t i = 0; i < c; i++) {
    wchar_t ch = e[i];
    if (q) {
      if (ch == q) q = 0;
    } else if (ch == L'\'' || ch == L'"') {
      q = ch;
    } else if (ch == L' ' || ch == L'\t') {
      start = i + 1;
    } else if (ch == L'|' || ch == L';' || ch == L'(' || ch == L'{') {
      start = segStart = i + 1;
    }
  }
  std::wstring tok = e.substr(start, c - start);
  size_t first = e.find_first_not_of(L" \t", segStart);
  bool cmdPos = first == std::wstring::npos || first >= start;
  g_comp.start = start;
  g_comp.items.clear();
  g_comp.loading.clear();
  if (!cmdPos && !tok.empty() && tok[0] == L'-') {  // parámetro de la orden del tramo
    size_t ce = e.find_first_of(L" \t", first);
    std::wstring cmd = e.substr(first, ce - first);
    if (cmd.empty() || cmd.size() > 100 ||
        cmd.find_first_not_of(L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.") != std::wstring::npos)
      return;
    auto it = g_params.find(Lower(cmd));
    if (it == g_params.end()) {
      RequestParams(cmd);
      g_comp.loading = Lower(cmd);
      return;
    }
    for (auto& n : it->second)
      if (StartsWithI(n, tok)) g_comp.items.push_back({n, n, COMP_PARAM});
    return;
  }
  if (cmdPos && !tok.empty() && tok.find_first_of(L"\\/:'\"~") == std::wstring::npos) {
    for (auto& n : p->cmdNames)
      if (StartsWithI(n, tok) && g_comp.items.size() < 500) g_comp.items.push_back({n, n, COMP_CMD});
    std::sort(g_comp.items.begin(), g_comp.items.end(),
              [](const CompItem& a, const CompItem& b) { return _wcsicmp(a.label.c_str(), b.label.c_str()) < 0; });
  }
  PathCandidates(p, tok, g_comp.items);
}

static void CompClose() {
  if (!g_comp.open) return;
  g_comp.open = false;
  g_comp.autoAccept = false;
  InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void CompAccept(int i) {
  Pane* p = g_comp.p;
  if (i < 0 || i >= (int)g_comp.items.size() || !g_live.count(p)) return CompClose();
  const CompItem& it = g_comp.items[i];
  size_t c = std::min(p->caret, p->edit.size()), s = std::min(g_comp.start, c);
  // Órdenes y parámetros llevan un espacio detrás (si no lo hay ya); las rutas no.
  bool space = (it.kind == COMP_CMD || it.kind == COMP_PARAM) && (c >= p->edit.size() || p->edit[c] != L' ');
  std::wstring ins = it.insert + (space ? L" " : L"");
  p->edit.replace(s, c - s, ins);
  p->caret = s + ins.size();
  p->histPos = -1;
  CompClose();
}

// Tras escribir o mover el cursor con el menú abierto: se vuelve a filtrar.
static void CompRefresh() {
  Pane* p = g_comp.p;
  if (!g_comp.open || !g_live.count(p) || !EditorActive(p)) return CompClose();
  CompCompute(p);
  g_comp.sel = g_comp.top = 0;
  if (g_comp.autoAccept && g_comp.loading.empty() && g_comp.items.size() == 1) return CompAccept(0);
  if (g_comp.items.empty() && g_comp.loading.empty()) return CompClose();
  InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void CompStart(Pane* p) {
  LoadShellCommands(p);
  g_comp = CompState{};
  g_comp.p = p;
  g_comp.open = true;
  g_comp.autoAccept = true;
  CompRefresh();
}

static bool CompKey(WPARAM vk) {
  int n = (int)g_comp.items.size();
  bool shift = GetKeyState(VK_SHIFT) & 0x8000;
  switch (vk) {
    case VK_ESCAPE: CompClose(); return true;
    case VK_RETURN:
      if (n) CompAccept(g_comp.sel);
      else CompClose();
      return true;
    case VK_TAB:
      if (n) g_comp.sel = (g_comp.sel + (shift ? n - 1 : 1)) % n;
      break;
    case VK_UP: g_comp.sel = std::max(0, g_comp.sel - 1); break;
    case VK_DOWN: g_comp.sel = std::min(n - 1, g_comp.sel + 1); break;
    case VK_PRIOR: g_comp.sel = std::max(0, g_comp.sel - kCompRows); break;
    case VK_NEXT: g_comp.sel = std::min(n - 1, g_comp.sel + kCompRows); break;
    default: g_comp.autoAccept = false; return false;  // al editor; luego se vuelve a filtrar
  }
  g_comp.autoAccept = false;
  g_comp.sel = std::max(0, g_comp.sel);
  if (g_comp.sel < g_comp.top) g_comp.top = g_comp.sel;
  if (g_comp.sel >= g_comp.top + kCompRows) g_comp.top = g_comp.sel - kCompRows + 1;
  InvalidateRect(g_hwnd, nullptr, FALSE);
  return true;
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
    case VK_TAB: CompStart(p); return true;
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
  if (g_comp.open && g_comp.p == p) CompRefresh();
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
// URLs http(s) que empiezan en la fila r0, con sus trozos en filas siguientes si vienen partidas.
// Partida = acaba a ≤3 columnas del borde (solo espacios detrás) y la fila siguiente sigue con una
// sangría entre la de la fila donde empieza y la propia URL (Claude Code parte así: «…/warp-li» +
// «  te», y en listas «- https://…/z» + «  as/…»). No sigue si la fila de abajo empieza por «(» o
// es una palabra normal con más texto detrás («…:8680/» + «(sirve la…»). El borde es el del panel
// de tmux si lo hay (columnas separadas por «│»), y también vale partir el esquema: «https» + «://…».
struct UrlSeg {
  int row, c0, c1;
};
struct UrlSpan {
  std::wstring url;
  std::vector<UrlSeg> segs;
};

static bool IsColumnBorder(wchar_t c) { return c == L'\u2502' || c == L'|'; }

static bool IsUrlStart(const std::wstring& s) { return !s.compare(0, 7, L"http://") || !s.compare(0, 8, L"https://"); }

static std::vector<UrlSpan> UrlsFrom(Pane* p, int r0, const std::function<const std::wstring&(int)>& text) {
  static const std::wstring kStop = L" \t\"'<>`|\u2502";
  static const wchar_t* const kHeads[] = {L"http", L"https", L"http:", L"https:", L"http:/", L"https:/"};
  std::vector<UrlSpan> out;
  const std::wstring& t = text(r0);
  size_t skip = 0;  // no buscar otra URL dentro de la anterior
  for (size_t at = 0; (at = t.find(L"http", at)) != std::wstring::npos; at += 4) {
    if (at < skip) continue;
    size_t L = at, R = at;  // tramo de la columna (panel de tmux) donde está
    while (L > 0 && !IsColumnBorder(t[L - 1])) L--;
    while (R < t.size() && !IsColumnBorder(t[R])) R++;
    size_t end = std::min(t.find_first_of(kStop, at), R);
    UrlSpan u;
    u.url = t.substr(at, end - at);
    bool full = IsUrlStart(u.url);
    if (!full) {  // solo el principio del esquema, al final del tramo: puede seguir abajo
      bool head = false;
      for (auto h : kHeads) head = head || u.url == h;
      if (!head || t.find_first_not_of(L' ', end) < R) continue;
    }
    u.segs.push_back({r0, (int)at, (int)end});
    size_t indent = t.find_first_not_of(L' ', L);
    int r = r0;
    size_t curEnd = end;
    const std::wstring* cur = &t;
    while (r + 1 < p->rows && curEnd + 3 >= R && cur->find_first_not_of(L' ', curEnd) >= R) {
      const std::wstring& next = text(r + 1);
      size_t s0 = next.find_first_not_of(L' ', L);
      if (s0 >= R || s0 < (indent == std::wstring::npos ? L : indent) || s0 > at) break;
      if (wcschr(L"([{<", next[s0])) break;
      size_t e = std::min(next.find_first_of(kStop, s0), R);
      if (e == s0) break;
      bool urlish = next.find_first_of(L"/.?=&#%:_~+0123456789", s0) < e;
      if (!urlish && next.find_first_not_of(L' ', e) < R) break;
      u.url += next.substr(s0, e - s0);
      u.segs.push_back({r + 1, (int)s0, (int)e});
      r++, cur = &next, curEnd = e;
    }
    if (!full && !IsUrlStart(u.url)) continue;
    while (!u.url.empty() && wcschr(L".,;:!?)]}'\"", u.url.back())) {  // puntuación final fuera
      u.url.pop_back();
      if (u.segs.back().c1 > u.segs.back().c0) u.segs.back().c1--;
    }
    out.push_back(u);
    skip = end;
  }
  return out;
}

// URL bajo el punto (también si se pulsa en el trozo de una URL partida).
static LinkHit LinkAt(Pane* p, int x, int y) {
  LinkHit h;
  POINT pt{x, y};
  if (!PtInRect(&p->content, pt)) return h;
  VTermPos pos = CellFromPoint(p, x, y);
  EnterCriticalSection(&p->cs);
  std::map<int, std::wstring> cache;
  auto text = [&](int r) -> const std::wstring& {
    auto it = cache.find(r);
    return it != cache.end() ? it->second : (cache[r] = RowText(p, r));
  };
  for (int r0 = std::max(0, pos.row - 4); r0 <= pos.row && h.url.empty(); r0++)
    for (auto& u : UrlsFrom(p, r0, text))
      for (auto& sg : u.segs)
        if (sg.row == pos.row && pos.col >= sg.c0 && pos.col < sg.c1) {
          h.p = p, h.row = pos.row, h.c0 = sg.c0, h.c1 = sg.c1, h.url = u.url;
          for (auto& s2 : u.segs) h.segs.push_back({s2.row, s2.c0, s2.c1});
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

// Rueda: tantas líneas por golpe como diga Windows (3 por defecto), sumando los medios golpes de
// touchpads y ruedas finas. En tmux/apps con ratón, un evento de rueda por línea (como Warp).
static void OnMouseWheel(int x, int y, int delta) {
  static int acc = 0;
  Pane* p = PaneAt(x, y);
  if (!p) return;
  UINT perNotch = 3;
  SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &perNotch, 0);
  if (perNotch == 0 || perNotch > 100) perNotch = 3;
  acc += delta;
  int units = acc * (int)perNotch / WHEEL_DELTA;
  if (!units) return;
  acc -= units * WHEEL_DELTA / (int)perNotch;
  int n = abs(units);
  if (p->mouseMode) {
    for (int i = 0; i < n; i++) MouseToApp(p, x, y, units > 0 ? 4 : 5, true);
  } else if (p->altScreen) {
    for (int i = 0; i < n; i++) SendKey(p, units > 0 ? VTERM_KEY_UP : VTERM_KEY_DOWN, VTERM_MOD_NONE);
  } else {
    EnterCriticalSection(&p->cs);
    p->scrollOff = std::clamp(p->scrollOff + units, 0, (int)p->sb.size());
    LeaveCriticalSection(&p->cs);
  }
  InvalidateRect(g_hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Dibujo

// Respaldo por carácter, como Warp: lo que Hack no tiene (⎿ de Claude, iconos de Nerd Fonts,
// emoji, CJK…) se dibuja con la primera de estas fuentes que lo tenga.
static const wchar_t* const kFallbackFaces[] = {L"Symbols Nerd Font Mono", L"Segoe UI Symbol", L"Cascadia Mono",
                                                L"Consolas", L"Segoe UI Emoji", L"MS Gothic", L"Malgun Gothic",
                                                L"Microsoft YaHei", L"Nirmala UI", L"Segoe UI"};
static const int kFallbackCount = sizeof kFallbackFaces / sizeof kFallbackFaces[0];
static HFONT g_fallback[kFallbackCount];
static HDC g_measureDC;
static std::unordered_map<uint32_t, uint8_t> g_fbCache;  // 0 = Hack; i = g_fallback[i-1]

static bool FontHas(HFONT f, uint32_t cp) {
  if (cp >= 0x10000) return false;  // GDI solo informa del plano básico
  SelectObject(g_measureDC, f);
  wchar_t ch = (wchar_t)cp;
  WORD gi = 0xFFFF;
  return GetGlyphIndicesW(g_measureDC, &ch, 1, &gi, GGI_MARK_NONEXISTING_GLYPHS) != GDI_ERROR && gi != 0xFFFF;
}

static int FallbackFor(uint32_t cp) {
  if (cp < 0x2000 && cp != 0) {  // latín, griego, cirílico…: Hack los tiene casi todos
    if (cp < 0x370) return 0;
  }
  auto it = g_fbCache.find(cp);
  if (it != g_fbCache.end()) return it->second;
  int r = 0;
  if (cp >= 0xF0000) r = 1;                         // iconos de Nerd Fonts en el plano privado suplementario
  else if (cp >= 0x10000) r = 5;                    // emoji y demás fuera del plano básico
  else if (!FontHas(g_fonts[0], cp))
    for (int i = 0; i < kFallbackCount; i++)
      if (FontHas(g_fallback[i], cp)) {
        r = i + 1;
        break;
      }
  g_fbCache[cp] = (uint8_t)r;
  return r;
}

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
  if (!g_measureDC) g_measureDC = CreateCompatibleDC(nullptr);
  for (int i = 0; i < kFallbackCount; i++) {
    if (g_fallback[i]) DeleteObject(g_fallback[i]);
    g_fallback[i] = CreateFontW(h, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, 0, kFallbackFaces[i]);
  }
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

static std::wstring RowText(Pane* p, int row);

// Celdas de la fila que forman parte de una URL http(s) (también la continuación de una URL
// partida desde la fila anterior). Con el lock tomado.
static std::vector<char> UrlMask(Pane* p, int row, const std::function<const std::wstring&(int)>& text) {
  std::vector<char> m(p->cols, 0);
  for (int r0 = std::max(0, row - 4); r0 <= row; r0++) {
    if (text(r0).find(L"http") == std::wstring::npos) continue;
    for (auto& u : UrlsFrom(p, r0, text))
      for (auto& sg : u.segs)
        if (sg.row == row)
          for (int i = sg.c0; i < sg.c1 && i < p->cols; i++) m[i] = 1;
  }
  return m;
}

struct Style {
  COLORREF fg, bg;
  int font;
  int fb;  // fuente de respaldo (0 = Hack)
  int underline;
  bool strike;
  bool operator!=(const Style& o) const {
    return fg != o.fg || bg != o.bg || font != o.font || fb != o.fb || underline != o.underline || strike != o.strike;
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

// Dibuja el panel. Con onlyDirty, solo las filas que han cambiado (salida de la shell); devuelve
// el rectángulo repintado para copiarlo a la ventana.
static RECT DrawPane(HDC dc, Pane* p, bool active, bool showFocus, bool onlyDirty = false) {
  EnterCriticalSection(&p->cs);
  bool full = !onlyDirty || p->fullDirty || p->scrollOff > 0 || (int)p->dirtyRows.size() != p->rows;
  RECT drawn{};
  if (full) Fill(dc, p->content, kBg), drawn = p->content;
  int ox = p->content.left + kPad, oy = p->content.top + kPad;
  std::wstring txt;
  std::vector<INT> dx;
  std::map<int, std::wstring> rowCache;  // textos de fila para detectar URLs
  auto rowText = [&](int r) -> const std::wstring& {
    auto it = rowCache.find(r);
    return it != rowCache.end() ? it->second : (rowCache[r] = RowText(p, r));
  };
  for (int row = 0; row < p->rows; row++) {
    int y = oy + row * g_cellH;
    if (!full) {
      if (!p->dirtyRows[row]) continue;
      RECT rr{p->content.left, y, p->content.right, y + g_cellH};
      Fill(dc, rr, kBg);
      if (drawn.right == drawn.left) drawn = rr;
      else UnionRect(&drawn, &drawn, &rr);
    }
    std::vector<char> url = UrlMask(p, row, rowText);
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
        s.fg = url[col] && VTERM_COLOR_IS_DEFAULT_FG(&c.fg) ? kUrl : ResolveColor(p, c.fg, true);
        s.bg = ResolveColor(p, c.bg, false);
        if (c.attrs.dim)  // atenuado (SGR 2): a medio camino entre texto y fondo
          s.fg = RGB((GetRValue(s.fg) + GetRValue(s.bg)) / 2, (GetGValue(s.fg) + GetGValue(s.bg)) / 2,
                     (GetBValue(s.fg) + GetBValue(s.bg)) / 2);
        if (c.attrs.reverse) std::swap(s.fg, s.bg);
        if (InSelection(p, row, col)) s.bg = kSelBg;
        if (int f = FindMark(p, row, col)) s.bg = f == 2 ? kFindCur : kFindBg, s.fg = RGB(0, 0, 0);
        s.font = (c.attrs.bold ? 1 : 0) | (c.attrs.italic ? 2 : 0);
        s.fb = c.chars[0] ? FallbackFor(c.chars[0]) : 0;
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
      SelectObject(dc, st.fb ? g_fallback[st.fb - 1] : g_fonts[st.font]);
      SetTextColor(dc, st.fg);
      SetBkColor(dc, st.bg);
      ExtTextOutW(dc, r.left, y + g_textDy, ETO_OPAQUE | ETO_CLIPPED, &r, txt.c_str(), (UINT)txt.size(), dx.data());
      if (st.underline || st.strike) {
        int ly = st.strike ? y + g_cellH / 2 : y + g_cellH - 2;
        Fill(dc, RECT{r.left, ly, r.right, ly + 1}, st.fg);
      }
    }
  }
  if (full) {
    DrawInput(dc, p, active, showFocus);
    DrawLinkUnderline(dc, p);
  }
  bool cursorRowDrawn = full || (p->cursor.row < (int)p->dirtyRows.size() && p->dirtyRows[p->cursor.row]);
  std::fill(p->dirtyRows.begin(), p->dirtyRows.end(), 0);
  p->fullDirty = false;
  // Cursor (no se dibuja mientras se escribe en la caja de entrada)
  if (cursorRowDrawn && active && p->cursorVisible && p->scrollOff == 0 && !EditorActive(p)) {
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
  return drawn;
}

static void DrawLinkUnderline(HDC dc, Pane* p) {
  if (g_link.p != p) return;
  int x = p->content.left + kPad, t = std::max(1, Px(1));
  for (auto& [row, c0, c1] : g_link.segs) {  // todos los trozos, también los de otras filas
    int y = p->content.top + kPad + (row + 1) * g_cellH - t;
    Fill(dc, RECT{x + c0 * g_cellW, y, x + c1 * g_cellW, y + t}, kAccent);
  }
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
  if (!p->agentName.empty()) return p->agentName;
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
  std::map<DWORD, std::vector<DWORD>> kids;
  std::map<DWORD, std::wstring> exes;
  PROCESSENTRY32W pe{sizeof pe};
  for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
    if (_wcsicmp(pe.szExeFile, L"conhost.exe") != 0) children.push_back({pe.th32ParentProcessID, pe.szExeFile});
    if (pe.th32ProcessID != pe.th32ParentProcessID) kids[pe.th32ParentProcessID].push_back(pe.th32ProcessID);
    exes[pe.th32ProcessID] = pe.szExeFile;
  }
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
  // Claude Code o Codex, por el nombre de la orden o, si es un atajo (clauded.cmd, una función de
  // PowerShell…), por el proceso: Claude apunta el suyo en sessions\<pid>.json y Codex es codex.exe.
  // Claude Code: la pestaña toma el nombre de la conversación (el que le pone Claude o /rename),
  // no el derivado de la carpeta, que no dice nada nuevo.
  for (Pane* p : g_live) {
    std::wstring name;
    int agent = AGENT_NONE;
    if (!p->runningCmd.empty()) {
      int named = agent = AgentKind(p->lastCmd);
      if (named != AGENT_CODEX)
        for (DWORD pid : Descendants(kids, p->pid)) {
          std::string j = ClaudeSessionJson(pid);
          if (!j.empty()) {
            agent = AGENT_CLAUDE;
            if (JsonStr(j, "nameSource", 0) != "derived") name = ToWide(JsonStr(j, "name", 0));
            break;
          }
          if (named == AGENT_NONE && _wcsicmp(exes[pid].c_str(), L"codex.exe") == 0) {
            agent = AGENT_CODEX;
            break;
          }
        }
      if (named == AGENT_NONE && agent != AGENT_NONE && LaunchesExe(p->lastCmd)) agent = AGENT_NONE, name.clear();
    }
    p->agent = agent;
    if (name != p->agentName) p->agentName = name, changed = true;
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

static void DrawSwitcher(HDC dc, int cw) {
  if (!g_sw.open) {
    g_sw.rc = RECT{};
    return;
  }
  int rowH = Px(30), n = std::min<int>(kPickRows, (int)g_sw.shown.size());
  int h = Px(40) + std::max(1, n) * rowH + Px(6);
  int w = std::min<int>(Px(620), cw - Px(32));
  RECT r{(cw - w) / 2, TabBarH() + Px(12), (cw - w) / 2 + w, TabBarH() + Px(12) + h};
  g_sw.rc = r;
  Panel(dc, r);
  SetBkMode(dc, TRANSPARENT);
  SelectObject(dc, g_uiFont);
  RECT fr{r.left + Px(12), r.top, r.right - Px(12), r.top + Px(40)};
  if (g_sw.filter.empty()) Text(dc, T(S_SWITCH), fr, RGB(0x8a, 0x8a, 0x8a), DT_LEFT);
  else Text(dc, g_sw.filter, fr, kFg, DT_LEFT | DT_END_ELLIPSIS);
  Fill(dc, RECT{r.left + 1, r.top + Px(40) - 1, r.right - 1, r.top + Px(40)}, kChipBorder);
  if (g_sw.shown.empty()) {
    RECT ir{r.left + Px(16), r.top + Px(40), r.right - Px(8), r.top + Px(40) + rowH};
    Text(dc, T(S_NO_RESULTS), ir, kTabText, DT_LEFT);
  }
  for (int i = 0; i < n; i++) {
    int idx = g_sw.top + i;
    if (idx >= (int)g_sw.shown.size()) break;
    const SwitchItem& it = g_sw.shown[idx];
    RECT ir{r.left + Px(4), r.top + Px(40) + i * rowH + Px(2), r.right - Px(4), r.top + Px(40) + (i + 1) * rowH};
    bool sel = idx == g_sw.sel;
    if (sel) Fill(dc, ir, kAccent);
    // Punto verde: tiene algo en marcha (Claude, ssh…).
    int cx = ir.left + Px(12), cy = (ir.top + ir.bottom) / 2;
    if (!it.running.empty()) Fill(dc, RECT{cx - Px(3), cy - Px(3), cx + Px(3), cy + Px(3)}, RGB(0x4e, 0xc9, 0x6b));
    // A la derecha la carpeta (solo su último trozo) y si es de otra ventana.
    std::wstring right = it.cwd;
    size_t cut = right.find_last_of(L'\\');
    if (cut != std::wstring::npos && cut + 1 < right.size()) right = right.substr(cut + 1);
    if (it.hwnd != g_hwnd) right += L"  \u00b7  " + std::wstring(T(S_OTHER_WINDOW));
    SIZE sz{};
    GetTextExtentPoint32W(dc, right.c_str(), (int)right.size(), &sz);
    int rw = std::min<int>(sz.cx, (ir.right - ir.left) / 2);
    RECT rr{ir.right - Px(10) - rw, ir.top, ir.right - Px(10), ir.bottom};
    Text(dc, right, rr, sel ? kFg : RGB(0x8a, 0x8a, 0x8a), DT_RIGHT | DT_END_ELLIPSIS);
    RECT tr{ir.left + Px(26), ir.top, rr.left - Px(12), ir.bottom};
    Text(dc, it.title, tr, sel ? kFg : RGB(0xd0, 0xd0, 0xd0), DT_LEFT | DT_END_ELLIPSIS);
  }
  SetBkMode(dc, OPAQUE);
}

// Aviso de actualización: una tarjeta pequeña abajo a la derecha, con su acción y una ×.
static void DrawUpdateNotice(HDC dc, int w, int h) {
  g_upd.rc = g_upd.action = g_upd.close = RECT{};
  if (g_upd.stage == UPD_NONE || g_upd.hidden) return;
  static const Str kMsg[] = {S_UPD_AVAIL, S_UPD_AVAIL, S_UPD_INSTALLING, S_UPD_READY, S_UPD_FAILED};
  wchar_t msg[160];
  bool busy = g_upd.stage == UPD_INSTALLING;
  int pct = g_updPct;
  if (busy && g_updPhase == 0) {
    swprintf(msg, 160, T(S_UPD_DOWNLOADING), g_upd.latest.c_str());
    if (pct >= 0) swprintf(msg + wcslen(msg), 160 - wcslen(msg), L" %d %%", pct);
  } else {
    swprintf(msg, 160, T(kMsg[g_upd.stage]), g_upd.latest.c_str());
  }
  const wchar_t* act = g_upd.stage == UPD_AVAILABLE ? T(S_UPD_INSTALL)
                       : g_upd.stage == UPD_READY   ? T(S_UPD_RESTART)
                       : g_upd.stage == UPD_FAILED  ? T(S_UPD_RETRY)
                                                    : nullptr;
  SelectObject(dc, g_uiFont);
  SIZE ms{}, as{};
  GetTextExtentPoint32W(dc, msg, (int)wcslen(msg), &ms);
  if (act) GetTextExtentPoint32W(dc, act, (int)wcslen(act), &as);
  int bh = Px(40), aw = act ? as.cx + Px(20) : 0, cw = Px(28);
  int cardW = Px(14) + ms.cx + Px(12) + aw + Px(4) + cw + Px(4);
  if (busy) cardW = std::max<int>(cardW, Px(300));  // que no baile al cambiar el % 
  RECT r{w - cardW - Px(14), h - bh - Px(14), w - Px(14), h - Px(14)};
  g_upd.rc = r;
  Panel(dc, r);
  SetBkMode(dc, TRANSPARENT);
  RECT tr{r.left + Px(14), r.top, r.left + Px(14) + ms.cx + 2, r.bottom};
  Text(dc, msg, tr, RGB(0xd0, 0xd0, 0xd0), DT_LEFT);
  if (act) {
    g_upd.action = RECT{tr.right + Px(10), r.top + Px(7), tr.right + Px(10) + aw, r.bottom - Px(7)};
    Fill(dc, g_upd.action, kAccent);
    Text(dc, act, g_upd.action, kFg, DT_CENTER);
  }
  g_upd.close = RECT{r.right - cw - Px(4), r.top + Px(6), r.right - Px(4), r.bottom - Px(6)};
  Text(dc, L"\u00d7", g_upd.close, kTabText, DT_CENTER);
  if (busy) {  // barra de progreso fina abajo: % al descargar, en movimiento al instalar
    RECT bar{r.left + Px(10), r.bottom - Px(5), r.right - Px(10), r.bottom - Px(3)};
    Fill(dc, bar, kChipBorder);
    int bw = bar.right - bar.left;
    if (g_updPhase == 0 && pct >= 0) {
      Fill(dc, RECT{bar.left, bar.top, bar.left + bw * pct / 100, bar.bottom}, kAccent);
    } else {
      int seg = bw / 4, x = (int)(GetTickCount64() / 4 % (DWORD)(bw + seg)) - seg;
      Fill(dc, RECT{bar.left + std::max(0, x), bar.top, bar.left + std::min(bw, x + seg), bar.bottom}, kAccent);
    }
  }
  SetBkMode(dc, OPAQUE);
}

static void DrawCompletion(HDC dc) {
  Pane* p = g_comp.p;
  if (!g_comp.open || !p || !g_live.count(p) || g_tabs.empty() || p != CurTab().active || !EditorActive(p)) {
    g_comp.rc = RECT{};
    return;
  }
  int rowH = Px(30), n = std::min<int>(kCompRows, (int)g_comp.items.size());
  int h = std::max(1, n) * rowH + Px(8);
  int w = std::min<int>(Px(480), p->rc.right - p->rc.left - Px(16));
  int left = p->chip.left, bottom = p->chip.top - Px(6);
  RECT r{left, std::max<int>(TabBarH() + Px(4), bottom - h), left + w, bottom};
  g_comp.rc = r;
  Panel(dc, r);
  SetBkMode(dc, TRANSPARENT);
  SelectObject(dc, g_uiFont);
  if (!n) {
    RECT tr{r.left + Px(14), r.top + Px(4), r.right - Px(14), r.top + Px(4) + rowH};
    Text(dc, T(S_LOADING), tr, RGB(0x8a, 0x8a, 0x8a), DT_LEFT);
  }
  static const Str kKind[] = {S_KIND_CMD, S_KIND_DIR, S_KIND_FILE, S_KIND_PARAM};
  for (int i = 0; i < n; i++) {
    int idx = g_comp.top + i;
    if (idx >= (int)g_comp.items.size()) break;
    const CompItem& it = g_comp.items[idx];
    RECT ir{r.left + Px(4), r.top + Px(4) + i * rowH, r.right - Px(4), r.top + Px(4) + (i + 1) * rowH};
    bool sel = idx == g_comp.sel;
    if (sel) Fill(dc, ir, kAccent);
    RECT kr{ir.right - Px(110), ir.top, ir.right - Px(10), ir.bottom};
    Text(dc, T(kKind[it.kind]), kr, sel ? kFg : RGB(0x8a, 0x8a, 0x8a), DT_RIGHT);
    RECT tr{ir.left + Px(10), ir.top, kr.left - Px(8), ir.bottom};
    Text(dc, it.label, tr, sel ? kFg : RGB(0xd0, 0xd0, 0xd0), DT_LEFT | DT_END_ELLIPSIS);
  }
  if ((int)g_comp.items.size() > kCompRows) {  // barra de desplazamiento fina
    int total = (int)g_comp.items.size(), ih = r.bottom - r.top - Px(8);
    int th = std::max(Px(12), ih * kCompRows / total), ty = r.top + Px(4) + (ih - th) * g_comp.top / std::max(1, total - kCompRows);
    Fill(dc, RECT{r.right - Px(4), ty, r.right - Px(2), ty + th}, kTabText);
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
  DrawCompletion(dc);
  DrawSwitcher(dc, w);
  DrawUpdateNotice(dc, w, h);
  if (g_dropTarget) {  // destino al arrastrar un panel por su cabecera
    HBRUSH b = CreateSolidBrush(kAccent);
    RECT r = g_dropTarget->rc;
    for (int i = 0; i < Px(2); i++, InflateRect(&r, -1, -1)) FrameRect(dc, &r, b);
    DeleteObject(b);
  }
}

// ---------------------------------------------------------------------------
// Pintado: búfer fijo; la salida de la shell solo repinta sus filas, como mucho a 60 fps.

static HDC g_backDC;
static HBITMAP g_backBmp;
static int g_backW, g_backH;
static DWORD g_lastFrame, g_lastFull;
static bool g_frameTimer;

static void EnsureBack(HDC ref, int w, int h) {
  if (g_backDC && w == g_backW && h == g_backH) return;
  if (!g_backDC) g_backDC = CreateCompatibleDC(ref);
  HBITMAP bmp = CreateCompatibleBitmap(ref, std::max(1, w), std::max(1, h));
  HGDIOBJ old = SelectObject(g_backDC, bmp);
  if (g_backBmp) DeleteObject(g_backBmp);
  else (void)old;
  g_backBmp = bmp;
  g_backW = w, g_backH = h;
}

static void PaintDirty() {
  DWORD now = GetTickCount();
  g_lastFrame = now;
  if (!g_backDC || g_tabs.empty() || IsIconic(g_hwnd)) return;
  bool overlay = g_find.open || g_pick.open || g_sw.open || g_comp.open || (g_upd.stage != UPD_NONE && !g_upd.hidden) || g_dragPane || g_link.p || g_renameTab >= 0;
  if (overlay || (g_titleDirty && now - g_lastFull >= 250)) {  // títulos: como mucho 4 veces por segundo
    g_titleDirty = false;
    InvalidateRect(g_hwnd, nullptr, FALSE);
    return;
  }
  if (g_titleDirty && !g_frameTimer) SetTimer(g_hwnd, TIMER_FRAME, 250, nullptr), g_frameTimer = true;
  std::vector<Pane*> ls;
  Leaves(CurTab().root, ls);
  HDC wdc = GetDC(g_hwnd);
  for (Pane* p : ls) {
    RECT u = DrawPane(g_backDC, p, p == CurTab().active, g_focused, true);
    if (u.right > u.left) BitBlt(wdc, u.left, u.top, u.right - u.left, u.bottom - u.top, g_backDC, u.left, u.top, SRCCOPY);
  }
  ReleaseDC(g_hwnd, wdc);
}

// Pruebas: prepara el búfer como tras un WM_PAINT (en la sesión de pruebas Windows no pinta).
static void PrimeBack() {
  RECT rc;
  GetClientRect(g_hwnd, &rc);
  HDC wdc = GetDC(g_hwnd);
  EnsureBack(wdc, rc.right, rc.bottom);
  ReleaseDC(g_hwnd, wdc);
  Render(g_backDC, rc.right, rc.bottom);
  g_lastFull = GetTickCount();
}

// Pruebas: ¿lo pintado fila a fila coincide con un pintado completo? Devuelve píxeles distintos.
static long VerifyPartial() {
  // Con los paneles bloqueados no entra salida nueva entre las dos imágenes (el lock es reentrante).
  std::vector<Pane*> locked(g_live.begin(), g_live.end());
  for (Pane* p : locked) EnterCriticalSection(&p->cs);
  PaintDirty();
  long r = [&] {
  RECT rc;
  GetClientRect(g_hwnd, &rc);
  int w = rc.right, h = rc.bottom;
  BITMAPINFO bi{};
  bi.bmiHeader = {sizeof bi.bmiHeader, w, -h, 1, 32, BI_RGB};
  std::vector<uint32_t> a((size_t)w * h), b((size_t)w * h);
  GetDIBits(g_backDC, g_backBmp, 0, h, a.data(), &bi, DIB_RGB_COLORS);
  HDC tmp = CreateCompatibleDC(g_backDC);
  HBITMAP bmp = CreateCompatibleBitmap(g_backDC, w, h);
  HGDIOBJ old = SelectObject(tmp, bmp);
  Render(tmp, w, h);
  SelectObject(tmp, old);
  GetDIBits(tmp, bmp, 0, h, b.data(), &bi, DIB_RGB_COLORS);
  DeleteObject(bmp);
  DeleteDC(tmp);
  // Solo la zona de texto de los paneles: lo demás (cabeceras, caja) lo pinta WM_PAINT, que en la
  // sesión de pruebas no llega.
  long diff = 0;
  std::vector<Pane*> ls;
  Leaves(CurTab().root, ls);
  for (Pane* p : ls)
    for (int y = std::max(0, (int)p->content.top); y < std::min(h, (int)p->content.bottom); y++)
      for (int x = std::max(0, (int)p->content.left); x < std::min(w, (int)p->content.right); x++)
        diff += (a[(size_t)y * w + x] & 0xffffff) != (b[(size_t)y * w + x] & 0xffffff);
  return diff;
  }();
  for (Pane* p : locked) LeaveCriticalSection(&p->cs);
  return r;
}

// Mide el tiempo de dibujo (paso «bench» de las pruebas): pintado completo y solo filas cambiadas.
static std::wstring Bench() {
  RECT rc;
  GetClientRect(g_hwnd, &rc);
  HDC wdc = GetDC(g_hwnd);
  EnsureBack(wdc, rc.right, rc.bottom);
  ReleaseDC(g_hwnd, wdc);
  LARGE_INTEGER f, a, b, c;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&a);
  for (int i = 0; i < 20; i++) Render(g_backDC, rc.right, rc.bottom);
  QueryPerformanceCounter(&b);
  std::vector<Pane*> ls;
  Leaves(CurTab().root, ls);
  for (int i = 0; i < 20; i++)
    for (Pane* p : ls) {
      EnterCriticalSection(&p->cs);
      MarkRow(p, p->cursor.row);
      LeaveCriticalSection(&p->cs);
      DrawPane(g_backDC, p, p == CurTab().active, true, true);
    }
  QueryPerformanceCounter(&c);
  wchar_t out[200];
  swprintf(out, 200, L"%dx%d, %d paneles: completo %.2f ms, solo filas %.3f ms", (int)rc.right, (int)rc.bottom,
           (int)ls.size(), (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart / 20,
           (c.QuadPart - b.QuadPart) * 1000.0 / f.QuadPart / 20);
  return out;
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
//   auto-update:on|off update-check update-click update-state key:<tab|down|up|enter|esc> edit-text comp drag-tab:<de>,<a> tabs clipboard (estos y hover/hover-text dejan el resultado en <sesión>.bench)

static void SetStartWithWindows(bool on);

// ---------------------------------------------------------------------------
// Actualizaciones: mira la última release de GitHub al arrancar y cada 6 h. Con «Actualizar
// automáticamente» se descarga, se comprueba su SHA-256 y se instala sola (efecto al reiniciar);
// si no, un aviso pequeño abajo a la derecha ofrece instalarla. Nunca se reinicia sin pulsar.

static const wchar_t* kUpdateApi = L"https://api.github.com/repos/justdiegocom/warp-lite/releases/latest";

static std::array<int, 3> ParseVersion(std::wstring v) {
  if (!v.empty() && (v[0] == L'v' || v[0] == L'V')) v.erase(0, 1);
  std::array<int, 3> r{0, 0, 0};
  swscanf(v.c_str(), L"%d.%d.%d", &r[0], &r[1], &r[2]);
  return r;
}

// Versión (FILEVERSION) leída de los bytes del archivo. No vale GetFileVersionInfo: si este
// proceso se arrancó desde esa misma ruta, Windows devuelve la del módulo ya cargado (la vieja),
// aunque el instalador haya puesto otro archivo en su sitio.
static std::wstring ExeVersion(const std::wstring& path) {
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return L"";
  LARGE_INTEGER size{};
  std::string data;
  if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (64 << 20)) {
    data.resize((size_t)size.QuadPart);
    DWORD got = 0;
    if (!ReadFile(f, data.data(), (DWORD)data.size(), &got, nullptr)) got = 0;
    data.resize(got);
  }
  CloseHandle(f);
  // VS_FIXEDFILEINFO: firma 0xFEEF04BD, versión de la estructura (1.0), FileVersionMS y LS. Se busca
  // desde el final (los recursos van al final; la constante de aquí también está en el código).
  for (size_t i = data.size() >= 16 ? (data.size() - 16) & ~(size_t)3 : 0; i + 16 <= data.size(); i -= 4) {
    uint32_t sig, ver, ms, ls;
    memcpy(&sig, data.data() + i, 4);
    memcpy(&ver, data.data() + i + 4, 4);
    if (sig != 0xFEEF04BD || ver != 0x00010000) {
      if (i == 0) break;
      continue;
    }
    memcpy(&ms, data.data() + i + 8, 4);
    memcpy(&ls, data.data() + i + 12, 4);
    return std::to_wstring(HIWORD(ms)) + L"." + std::to_wstring(LOWORD(ms)) + L"." + std::to_wstring(HIWORD(ls));
  }
  return L"";
}

static std::wstring InstalledExe() {
  wchar_t buf[MAX_PATH];
  if (!GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH)) return L"";
  return std::wstring(buf) + L"\\Programs\\warp-lite\\Warp.exe";
}


static std::wstring UpdateSettingsPath() {
  return g_windowDir.empty() ? g_sessionPath + L".update" : g_windowDir + L"\\update.txt";
}

// update.log: una línea por intento (para saber por qué falló, si falla).
static void UpdateLog(const std::wstring& line) {
  std::wstring path = g_windowDir.empty() ? g_sessionPath + L".updlog" : g_windowDir + L"\\update.log";
  if (FILE* f = _wfopen(path.c_str(), L"a")) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d %s\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
            ToUtf8(line).c_str());
    fclose(f);
  }
}

static void LoadUpdateSettings() {
  FILE* f = _wfopen(UpdateSettingsPath().c_str(), L"r");
  if (!f) return;
  char k[32];
  int v;
  while (fscanf(f, "%31s %d", k, &v) == 2)
    if (!strcmp(k, "auto")) g_upd.autoUpdate = v != 0;
  fclose(f);
}

static void SaveUpdateSettings() {
  if (FILE* f = _wfopen(UpdateSettingsPath().c_str(), L"w")) {
    fprintf(f, "auto %d\n", g_upd.autoUpdate ? 1 : 0);
    fclose(f);
  }
}

static bool HttpGet(const std::wstring& url, std::string& out, size_t maxBytes, bool allowHttp, std::wstring* err = nullptr,
                    std::atomic<int>* pct = nullptr) {
  auto fail = [&](const wchar_t* what) {
    if (err && err->empty()) *err = std::wstring(what) + L" (" + std::to_wstring(GetLastError()) + L")";
    return false;
  };
  wchar_t host[256], path[2048], extra[2048];
  URL_COMPONENTS uc{sizeof uc};
  uc.lpszHostName = host, uc.dwHostNameLength = 256;
  uc.lpszUrlPath = path, uc.dwUrlPathLength = 2048;
  uc.lpszExtraInfo = extra, uc.dwExtraInfoLength = 2048;
  if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return fail(L"url");
  bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;
  if (!https && !allowHttp) return fail(L"http");
  std::wstring ua = L"warp-lite/" + g_upd.running;
  HINTERNET s = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!s) return fail(L"open");
  WinHttpSetTimeouts(s, 10000, 10000, 15000, 60000);
  bool ok = false;
  if (HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0)) {
    std::wstring obj = std::wstring(path) + extra;
    if (HINTERNET r = WinHttpOpenRequest(c, L"GET", obj.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         https ? WINHTTP_FLAG_SECURE : 0)) {
      if (WinHttpSendRequest(r, L"Accept: application/vnd.github+json\r\n", (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
          WinHttpReceiveResponse(r, nullptr)) {
        DWORD status = 0, sz = sizeof status;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
                            &sz, WINHTTP_NO_HEADER_INDEX);
        ok = status == 200;
        if (!ok && err) *err = L"HTTP " + std::to_wstring(status);
        DWORD total = 0;
        sz = sizeof total;
        if (pct && !WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                        &total, &sz, WINHTTP_NO_HEADER_INDEX))
          total = 0;
        DWORD avail = 0;
        while (ok && WinHttpQueryDataAvailable(r, &avail) && avail) {
          size_t at = out.size();
          if (at + avail > maxBytes) {
            ok = fail(L"too big");
            break;
          }
          out.resize(at + avail);
          DWORD got = 0;
          if (!WinHttpReadData(r, out.data() + at, avail, &got)) ok = fail(L"read");
          out.resize(at + got);
          if (pct && total) {
            int v = (int)std::min<uint64_t>(100, out.size() * 100ull / total);
            if (pct->exchange(v) != v) PostMessage(g_hwnd, WM_APP_UPDPROG, 0, 0);
          }
        }
      }
      else fail(L"send");
      WinHttpCloseHandle(r);
    }
    WinHttpCloseHandle(c);
  }
  WinHttpCloseHandle(s);
  return ok || fail(L"connect");
}

// Valor de texto de "clave": "..." en el JSON a partir de from (las releases no llevan escapes ahí).
static std::string JsonStr(const std::string& j, const std::string& key, size_t from = 0) {
  size_t k = j.find("\"" + key + "\"", from);
  if (k == std::string::npos) return "";
  size_t colon = j.find(':', k), q = colon == std::string::npos ? colon : j.find_first_not_of(" \t\r\n", colon + 1);
  if (q == std::string::npos || j[q] != '"') return "";
  size_t e = j.find('"', q + 1);
  if (e == std::string::npos) return "";
  return j.substr(q + 1, e - q - 1);
}

static std::string Sha256Hex(const std::string& data) {
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE h = nullptr;
  unsigned char d[32];
  std::string hex;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) {
    if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
      if (BCryptHashData(h, (PUCHAR)data.data(), (ULONG)data.size(), 0) == 0 && BCryptFinishHash(h, d, 32, 0) == 0) {
        static const char* x = "0123456789abcdef";
        for (unsigned char b : d) hex += x[b >> 4], hex += x[b & 15];
      }
      BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
  }
  return hex;
}

struct UpdateReply {
  bool ok = false, manual = false;
  std::wstring latest, url, sha;
};

static DWORD WINAPI UpdateCheckThread(LPVOID arg) {
  auto* r = (UpdateReply*)arg;
  wchar_t buf[2048];
  std::wstring api = GetEnvironmentVariableW(L"WARPLITE_UPDATE_URL", buf, 2048) ? buf : kUpdateApi;
  std::string j;
  if (HttpGet(api, j, 2 << 20, g_upd.testUrl)) {
    std::string tag = JsonStr(j, "tag_name");
    size_t a = j.find("\"name\": \"warp-lite-setup.exe\"");
    if (a == std::string::npos) a = j.find("\"name\":\"warp-lite-setup.exe\"");
    if (!tag.empty() && a != std::string::npos) {
      std::string url = JsonStr(j, "browser_download_url", a), dig = JsonStr(j, "digest", a);
      if (dig.rfind("sha256:", 0) == 0 && !url.empty()) {
        r->latest = ToWide(tag[0] == 'v' ? tag.substr(1) : tag);
        r->url = ToWide(url);
        r->sha = ToWide(dig.substr(7));
        r->ok = true;
      }
    }
  }
  if (!PostMessage(g_hwnd, WM_APP_UPDATE, 0, (LPARAM)r)) delete r;
  return 0;
}

static void CheckForUpdates(bool manual) {
  if (g_upd.checking || g_upd.stage == UPD_INSTALLING) return;
  g_upd.checking = true;
  auto* r = new UpdateReply;
  r->manual = manual;
  HANDLE t = CreateThread(nullptr, 0, UpdateCheckThread, r, 0, nullptr);
  if (t) CloseHandle(t);
  else delete r, g_upd.checking = false;
  if (manual) Toast(T(S_UPD_CHECKING), 3000);
}

struct InstallJob {
  std::wstring latest, url, sha;
  bool ok = false;
  std::wstring error;
};

// Descarga, comprueba el SHA-256 y ejecuta el instalador en silencio. Un solo proceso a la vez.
static DWORD WINAPI InstallThread(LPVOID arg) {
  auto* job = (InstallJob*)arg;
  HANDLE mx = CreateMutexW(nullptr, FALSE, L"Local\\warp-lite-update");
  bool mine = mx && WaitForSingleObject(mx, 0) == WAIT_OBJECT_0;
  if (!mine) {
    job->error = L"busy";
  } else {
    std::string data;
    std::wstring why;
    g_updPhase = 0, g_updPct = -1;
    if (!HttpGet(job->url, data, 64 << 20, g_upd.testUrl, &why, &g_updPct)) {
      job->error = L"download: " + why;
    } else if (Sha256Hex(data) != ToUtf8(job->sha)) {
      job->error = L"checksum";
    } else {
      wchar_t tmp[MAX_PATH];
      GetTempPathW(MAX_PATH, tmp);
      std::wstring dir = std::wstring(tmp) + L"warp-lite-update";
      CreateDirectoryW(dir.c_str(), nullptr);
      std::wstring exe = dir + L"\\warp-lite-setup-" + job->latest + L".exe";
      HANDLE f = CreateFileW(exe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
      DWORD w = 0;
      bool written = f != INVALID_HANDLE_VALUE && WriteFile(f, data.data(), (DWORD)data.size(), &w, nullptr) && w == data.size();
      if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
      g_updPhase = 1;
      PostMessage(g_hwnd, WM_APP_UPDPROG, 0, 0);
      std::wstring cmd = L"\"" + exe + L"\" --silent";
      STARTUPINFOW si{sizeof si};
      PROCESS_INFORMATION pi{};
      if (!written) job->error = L"write";
      else if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi)) {
        job->error = L"run (" + std::to_wstring(GetLastError()) + L")";
      } else {
        WaitForSingleObject(pi.hProcess, 120000);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        job->ok = ParseVersion(ExeVersion(InstalledExe())) == ParseVersion(job->latest);
        if (!job->ok) job->error = L"version " + ExeVersion(InstalledExe()) + L" tras instalar (salida " + std::to_wstring(code) + L")";
      }
      DeleteFileW(exe.c_str());
    }
    ReleaseMutex(mx);
  }
  if (mx) CloseHandle(mx);
  if (!PostMessage(g_hwnd, WM_APP_UPDATED, 0, (LPARAM)job)) delete job;
  return 0;
}

static void InstallUpdate() {
  if (g_upd.stage == UPD_INSTALLING || g_upd.url.empty()) return;
  g_upd.stage = UPD_INSTALLING;
  g_upd.hidden = false;
  g_updPhase = 0, g_updPct = -1;
  SetTimer(g_hwnd, TIMER_UPDANIM, 50, nullptr);
  UpdateLog(L"instalando " + g_upd.latest + L" desde " + g_upd.running);
  auto* job = new InstallJob{g_upd.latest, g_upd.url, g_upd.sha};
  HANDLE t = CreateThread(nullptr, 0, InstallThread, job, 0, nullptr);
  if (t) CloseHandle(t);
  else delete job, g_upd.stage = UPD_FAILED;
  InvalidateRect(g_hwnd, nullptr, FALSE);
}

static BOOL CALLBACK SendUpdated(HWND h, LPARAM) {
  wchar_t cls[32];
  if (h != g_hwnd && GetClassNameW(h, cls, 32) && !wcscmp(cls, L"WarpLite")) PostMessageW(h, g_msgUpdated, 0, 0);
  return TRUE;
}

// ¿Ya está instalada (en disco) una versión más nueva que la que corre? Entonces toca reiniciar.
static bool UpdateReadyOnDisk() {
  std::wstring disk = ExeVersion(InstalledExe());
  return !disk.empty() && ParseVersion(disk) > ParseVersion(g_upd.running);
}

static void OnUpdateReply(UpdateReply* r) {
  g_upd.checking = false;
  if (!r->ok) {
    if (r->manual) Toast(T(S_UPD_OFFLINE), 3000);
    UpdateLog(L"no se pudo comprobar" + std::wstring(r->manual ? L" (a mano)" : L""));
    return;
  }
  if (ParseVersion(r->latest) <= ParseVersion(g_upd.running)) {
    if (r->manual) Toast(std::wstring(T(S_UPD_LATEST)) + L" (" + g_upd.running + L")", 3000);
    return;
  }
  bool newer = r->latest != g_upd.latest;
  g_upd.latest = r->latest, g_upd.url = r->url, g_upd.sha = r->sha;
  if (newer || r->manual) g_upd.hidden = false;
  if (ParseVersion(ExeVersion(InstalledExe())) >= ParseVersion(r->latest)) g_upd.stage = UPD_READY;
  else if (g_upd.autoUpdate || (r->manual && g_upd.stage == UPD_AVAILABLE)) InstallUpdate();
  else g_upd.stage = UPD_AVAILABLE;
  InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void OnUpdateInstalled(InstallJob* job) {
  KillTimer(g_hwnd, TIMER_UPDANIM);
  UpdateLog(job->ok ? L"instalada " + job->latest : L"error al instalar " + job->latest + L": " + job->error);
  if (job->ok) {
    g_upd.stage = UPD_READY;
    EnumWindows(SendUpdated, 0);
  } else if (job->error == L"busy") {
    g_upd.stage = UPD_AVAILABLE;  // la instala otra ventana; avisará al acabar
  } else {
    g_upd.stage = ParseVersion(ExeVersion(InstalledExe())) >= ParseVersion(job->latest) ? UPD_READY : UPD_FAILED;
  }
  g_upd.hidden = false;
  InvalidateRect(g_hwnd, nullptr, FALSE);
}

// Reiniciar: se abre el Warp instalado (espera a que se cierren todas) y se cierran todas
// guardándose, como Ctrl+Shift+Q; al abrir, vuelven todas las ventanas.
static void RestartForUpdate() {
  std::wstring exe = InstalledExe();
  std::wstring cmd = L"\"" + exe + L"\" --relaunch";
  STARTUPINFOW si{sizeof si};
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return;
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  EnumWindows(SendQuitAll, 0);
}

static void UpdateAction() {
  if (g_upd.stage == UPD_AVAILABLE || g_upd.stage == UPD_FAILED) InstallUpdate();
  else if (g_upd.stage == UPD_READY) RestartForUpdate();
}

// Resultado de un paso, una línea en <sesión>.bench (lo leen los wintest-*.ps1).
static void ScriptLog(const std::wstring& r) {
  std::string u = ToUtf8(r) + "\n";
  HANDLE fh = CreateFileW((g_sessionPath + L".bench").c_str(), FILE_APPEND_DATA, 0, nullptr, OPEN_ALWAYS, 0, nullptr);
  if (fh == INVALID_HANDLE_VALUE) return;
  DWORD w;
  WriteFile(fh, u.data(), (DWORD)u.size(), &w, nullptr);
  CloseHandle(fh);
}

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
  } else if (s.rfind(L"drop:", 0) == 0) {  // drop:ruta1;ruta2 — como soltar archivos en el centro
    std::wstring list = s.substr(5), files;
    for (auto& c : list) files += c == L';' ? L'\0' : c;
    files += L'\0';
    files += L'\0';
    HGLOBAL hg = GlobalAlloc(GHND, sizeof(DROPFILES) + files.size() * sizeof(wchar_t));
    DROPFILES* df = (DROPFILES*)GlobalLock(hg);
    df->pFiles = sizeof(DROPFILES);
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    df->pt = POINT{rc.right / 2, rc.bottom / 2};
    df->fWide = TRUE;
    memcpy((char*)df + sizeof(DROPFILES), files.data(), files.size() * sizeof(wchar_t));
    GlobalUnlock(hg);
    SendMessageW(g_hwnd, WM_DROPFILES, (WPARAM)hg, 0);
  } else if (s == L"clear" && p) {
    CompClose();
    p->edit.clear();
    p->caret = 0;
  } else if (s == L"submit" && p) {
    if (EditorActive(p)) SubmitEditor(p);
    delay = 2000;
  } else if (s.rfind(L"hover:", 0) == 0 && p) {  // hover:col,fila (como Ctrl sobre un enlace)
    int c = 0, r = 0;
    swscanf(s.c_str() + 6, L"%d,%d", &c, &r);
    g_link = LinkAt(p, p->content.left + kPad + c * g_cellW + 1, p->content.top + kPad + r * g_cellH + 1);
    Toast(g_link.url.empty() ? L"(sin enlace)" : g_link.url);
    ScriptLog(L"hover " + std::to_wstring(c) + L"," + std::to_wstring(r) + L": " + (g_link.url.empty() ? L"(sin enlace)" : g_link.url));
  } else if (s.rfind(L"find:", 0) == 0 && p) {
    OpenFind(p);
    for (wchar_t c : s.substr(5)) FindChar(c);
  } else if (s == L"find-next") {
    FindStep(1);
  } else if (s == L"find-close") {
    CloseFind();
  } else if (s == L"prime") {
    PrimeBack();
  } else if (s == L"bench" || s == L"verify") {
    ScriptLog(s == L"bench" ? Bench() : L"píxeles distintos entre pintado por filas y completo: " + std::to_wstring(VerifyPartial()));
  } else if (s.rfind(L"drag-tab:", 0) == 0) {  // drag-tab:de,a — arrastra la pestaña «de» sobre «a» con el ratón
    int from = 0, to = 0;
    swscanf(s.c_str() + 9, L"%d,%d", &from, &to);
    TabLayout L = ComputeTabs();
    auto center = [&](int ti) {
      for (auto& [i, r] : L.tabs)
        if (i == ti) return POINT{(r.left + r.right) / 2 - Px(30), (r.top + r.bottom) / 2};
      return POINT{-1, -1};
    };
    POINT a = center(from), b = center(to);
    auto at = [](POINT q) { return MAKELPARAM(q.x, q.y); };
    auto cursor = [](POINT q) {
      ClientToScreen(g_hwnd, &q);
      SetCursorPos(q.x, q.y);
    };
    cursor(a);
    SendMessageW(g_hwnd, WM_LBUTTONDOWN, MK_LBUTTON, at(a));
    for (int k = 1; k <= 8; k++) {
      POINT q{a.x + (b.x - a.x) * k / 8, a.y + (b.y - a.y) * k / 8};
      cursor(q);
      SendMessageW(g_hwnd, WM_MOUSEMOVE, MK_LBUTTON, at(q));
    }
    SendMessageW(g_hwnd, WM_LBUTTONUP, 0, at(b));
  } else if (s.rfind(L"key:", 0) == 0) {  // key:tab|down|up|enter|esc — por el bucle de mensajes, como una tecla
    std::wstring k = s.substr(4);
    WPARAM vk = k == L"tab" ? VK_TAB : k == L"down" ? VK_DOWN : k == L"up" ? VK_UP : k == L"enter" ? VK_RETURN : VK_ESCAPE;
    PostMessageW(g_hwnd, WM_KEYDOWN, vk, 0);
    delay = 600;
  } else if (s == L"edit-text" && p) {
    ScriptLog(L"edit: [" + p->edit + L"]");
  } else if (s == L"comp") {
    std::wstring r = L"comp:";
    if (!g_comp.open) r += L" (cerrado)";
    else if (!g_comp.loading.empty()) r += L" (cargando)";
    for (size_t i = 0; g_comp.open && i < g_comp.items.size() && i < 6; i++) r += L" " + g_comp.items[i].label;
    ScriptLog(r);
  } else if (s.rfind(L"auto-update:", 0) == 0) {
    g_upd.autoUpdate = s.substr(12) == L"on";
  } else if (s == L"update-check") {
    CheckForUpdates(true);
    delay = 4000;
  } else if (s == L"update-click") {  // la acción del aviso (Instalar / Reiniciar / Reintentar)
    UpdateAction();
    delay = 3000;
  } else if (s == L"update-state") {
    static const wchar_t* kSt[] = {L"none", L"available", L"installing", L"ready", L"failed"};
    ScriptLog(std::wstring(L"update: ") + kSt[g_upd.stage] + L" " + g_upd.latest + L" (corre " + g_upd.running + L")" +
              (g_upd.stage == UPD_INSTALLING ? (g_updPhase == 0 ? L" descargando " + std::to_wstring(g_updPct) + L"%" : L" instalando") : L""));
  } else if (s.rfind(L"http-get:", 0) == 0) {  // descarga como el actualizador y dice qué ha pasado
    std::string data;
    std::wstring why;
    bool ok = HttpGet(s.substr(9), data, 64 << 20, true, &why);
    ScriptLog(L"http-get: " + std::wstring(ok ? L"ok " : L"error ") + std::to_wstring(data.size()) + L" " +
              ToWide(Sha256Hex(data).substr(0, 12)) + L" " + why);
    delay = 3000;
  } else if (s.rfind(L"resume-cmd:", 0) == 0) {  // cómo se reanudaría esa orden (Claude Code / Codex)
    std::string r = ResumeCommand(ToUtf8(s.substr(11)), "ID");
    ScriptLog(L"resume-cmd: " + (r.empty() ? L"(no)" : ToWide(r)));
    delay = 50;
  } else if (s == L"resume") {  // lo que guardaría la sesión para cada panel con Claude Code o Codex
    UpdateRunning();
    ComputeResumes();
    std::vector<std::wstring> v;
    for (auto& [q, r] : g_resumeCmd) v.push_back(ToWide(r));
    std::sort(v.begin(), v.end());
    for (auto& r : v) ScriptLog(L"resume: " + r);
    if (v.empty()) ScriptLog(L"resume: (nada)");
  } else if (s == L"switcher") {  // abre el selector de terminales y apunta lo que lista
    OpenSwitcher();
    std::wstring r = L"switcher:";
    for (auto& it : g_sw.shown) {
      std::wstring dir = it.cwd.substr(it.cwd.find_last_of(L'\\') == std::wstring::npos ? 0 : it.cwd.find_last_of(L'\\') + 1);
      r += L" [" + it.title + L" | " + dir + (it.hwnd == g_hwnd ? L"" : L" | otra") + L"]";
    }
    ScriptLog(r + L" sel=" + std::to_wstring(g_sw.sel));
    delay = 300;
  } else if (s.rfind(L"sw-type:", 0) == 0) {
    for (wchar_t c : s.substr(8)) SwChar(c);
    delay = 200;
  } else if (s == L"active") {  // terminal activa: pestaña, título y carpeta (y si la ventana está delante)
    Pane* a = CurTab().active;
    ScriptLog(L"active: tab " + std::to_wstring(g_activeTab) + L" [" + ShortTitle(a) + L"] " + a->cwd +
              (GetForegroundWindow() == g_hwnd ? L" (delante)" : L""));
  } else if (s == L"titles") {  // título de cada pestaña
    std::wstring r = L"titles:";
    for (auto& t : g_tabs) r += L" [" + (t.name.empty() ? ShortTitle(t.active) : t.name) + L"]";
    ScriptLog(r);
  } else if (s == L"tabs") {  // orden de las pestañas (por su nombre) y cuál está activa
    std::wstring r = L"tabs:";
    for (int i = 0; i < (int)g_tabs.size(); i++) r += L" " + (i == g_activeTab ? L"*" + g_tabs[i].name : g_tabs[i].name);
    ScriptLog(r);
  } else if (s == L"clipboard") {
    std::wstring w = L"(vacío)";
    if (OpenClipboard(g_hwnd)) {
      if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) w = (const wchar_t*)GlobalLock(h), GlobalUnlock(h);
      CloseClipboard();
    }
    for (size_t i; (i = w.find(L"\r\n")) != std::wstring::npos;) w.replace(i, 2, L"\\n");
    ScriptLog(L"clipboard: " + w);
  } else if (s == L"paste" && p) {
    Paste(p);
  } else if (s == L"detach-tab") {
    RECT wr;
    GetWindowRect(g_hwnd, &wr);
    Detach(g_activeTab, nullptr, POINT{wr.left + 200, wr.top + 120});
    delay = 3000;
  } else if (s.rfind(L"startup:", 0) == 0) {
    SetStartWithWindows(s.substr(8) == L"on");
  } else if (s.rfind(L"goto:", 0) == 0) {
    ActivateTab(_wtoi(s.c_str() + 5));
  } else if (s == L"close") {  // como pulsar la X de esta ventana
    PostMessage(g_hwnd, WM_CLOSE, 0, 0);
    return;
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
  } else if (s.rfind(L"hover-text:", 0) == 0 && p) {  // Ctrl sobre la primera fila cuyo texto es ese
    std::wstring want = s.substr(11);
    for (int r = 0; r < p->rows; r++) {
      EnterCriticalSection(&p->cs);
      std::wstring t = RowText(p, r);
      LeaveCriticalSection(&p->cs);
      size_t a = t.find_first_not_of(L' '), b = t.find_last_not_of(L' ');
      if (a != std::wstring::npos && t.substr(a, b - a + 1) == want) {
        g_link = LinkAt(p, p->content.left + kPad + (int)a * g_cellW + 1, p->content.top + kPad + r * g_cellH + 1);
        Toast(g_link.url.empty() ? L"(sin enlace)" : g_link.url);
        ScriptLog(L"hover-text " + want + L": " + (g_link.url.empty() ? L"(sin enlace)" : g_link.url));
        break;
      }
    }
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

// Iniciar con Windows: valor «Warp» en HKCU\...\Run (por usuario, sin administrador).
static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

static bool StartsWithWindows() {
  HKEY k;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
  bool on = RegQueryValueExW(k, L"Warp", nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
  RegCloseKey(k);
  return on;
}

static void SetStartWithWindows(bool on) {
  HKEY k;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
  if (on) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring v = L"\"" + std::wstring(exe) + L"\"";
    RegSetValueExW(k, L"Warp", 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
  } else {
    RegDeleteValueW(k, L"Warp");
  }
  RegCloseKey(k);
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
      if (wp == HTCAPTION) {  // clic derecho en la barra de título: idioma e iniciar con Windows
        HMENU menu = CreatePopupMenu(), sub = CreatePopupMenu();
        for (int i = 0; i < kLangCount; i++)
          AppendMenuW(sub, MF_STRING | (i == g_lang ? MF_CHECKED : 0), 100 + i, kLangNames[i]);
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)sub, T(S_LANGUAGE));
        AppendMenuW(menu, MF_STRING | (StartsWithWindows() ? MF_CHECKED : 0), 1, T(S_STARTUP));
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        wchar_t upd[160] = L"";
        if (g_upd.stage == UPD_AVAILABLE || g_upd.stage == UPD_FAILED) swprintf(upd, 160, L"%ls: %ls", T(S_UPD_INSTALL), g_upd.latest.c_str());
        else if (g_upd.stage == UPD_READY) swprintf(upd, 160, L"%ls (%ls)", T(S_UPD_RESTART), g_upd.latest.c_str());
        if (upd[0]) AppendMenuW(menu, MF_STRING, 4, upd);
        AppendMenuW(menu, MF_STRING | (g_upd.checking || g_upd.stage == UPD_INSTALLING ? MF_GRAYED : 0), 2, T(S_UPD_CHECK));
        AppendMenuW(menu, MF_STRING | (g_upd.autoUpdate ? MF_CHECKED : 0), 3, T(S_UPD_AUTO));
        int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), 0, hwnd,
                                 nullptr);
        DestroyMenu(menu);
        if (cmd == 1) SetStartWithWindows(!StartsWithWindows());
        if (cmd == 2) CheckForUpdates(true);
        if (cmd == 3) {
          g_upd.autoUpdate = !g_upd.autoUpdate;
          SaveUpdateSettings();
          if (g_upd.autoUpdate && g_upd.stage == UPD_AVAILABLE) InstallUpdate();
        }
        if (cmd == 4) UpdateAction();
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
      SetPropW(hwnd, kInstanceProp, InstanceKey());
      if (!LoadSession() && g_openDir.empty()) NewTab();
      if (!g_openDir.empty()) NewTab(g_tabs.empty() ? -1 : g_activeTab + 1, g_openDir);
      SetTimer(hwnd, TIMER_RESTORE, 250, nullptr);
      if (!g_script.empty()) SetTimer(hwnd, TIMER_SCRIPT, 3000, nullptr);
      if (!GetEnvironmentVariableW(L"WARPLITE_NO_UPDATE", nullptr, 0)) SetTimer(hwnd, TIMER_UPDATE, 20000, nullptr);
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
    case WM_APP_UPDPROG: InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_APP_UPDATE: OnUpdateReply(std::unique_ptr<UpdateReply>((UpdateReply*)lp).get()); return 0;
    case WM_APP_UPDATED: OnUpdateInstalled(std::unique_ptr<InstallJob>((InstallJob*)lp).get()); return 0;
    case WM_APP_PARAMS: {  // parámetros de una orden, de ParamsThread
      std::unique_ptr<ParamsReply> r((ParamsReply*)lp);
      std::wstring low = Lower(r->cmd);
      g_paramsPending.erase(low);
      g_params[low] = r->names;
      if (g_comp.open && g_comp.loading == low) CompRefresh();
      return 0;
    }
    case WM_APP_CLIP: {  // OSC 52: texto copiado en tmux → portapapeles de Windows (con \r\n)
      std::unique_ptr<std::wstring> t((std::wstring*)lp);
      std::wstring w;
      for (wchar_t c : *t) {
        if (c == L'\n' && (w.empty() || w.back() != L'\r')) w += L'\r';
        w += c;
      }
      SetClipboardText(w);
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
      if (GetTickCount() - g_lastFrame < 16) {  // como mucho 60 fotogramas por segundo
        if (!g_frameTimer) SetTimer(hwnd, TIMER_FRAME, 16, nullptr), g_frameTimer = true;
      } else {
        PaintDirty();
      }
      return 0;
    case WM_APP_EXITED: {
      Pane* p = (Pane*)lp;
      if (g_live.count(p)) ClosePane(p);
      return 0;
    }
    case WM_TIMER:
      if (wp == TIMER_FRAME) {
        KillTimer(hwnd, TIMER_FRAME);
        g_frameTimer = false;
        PaintDirty();
      } else if (wp == TIMER_UPDANIM) {  // barra de «Instalando…» en movimiento
        if (g_upd.stage == UPD_INSTALLING && !g_upd.hidden) InvalidateRect(hwnd, &g_upd.rc, FALSE);
      } else if (wp == TIMER_UPDATE) {
        SetTimer(hwnd, TIMER_UPDATE, 6 * 3600 * 1000, nullptr);
        CheckForUpdates(false);
      } else if (wp == TIMER_TOAST) {
        KillTimer(hwnd, TIMER_TOAST);
        g_toast.clear();
        InvalidateRect(hwnd, nullptr, FALSE);
      } else if (wp == TIMER_SCRIPT) {
        KillTimer(hwnd, TIMER_SCRIPT);
        RunScriptStep();
      } else if (wp == TIMER_RESTORE) {
        static int ticks = 0;
        if (++ticks % 4 == 0) {  // cada segundo
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
      EnsureBack(dc, rc.right, rc.bottom);
      Render(g_backDC, rc.right, rc.bottom);
      BitBlt(dc, 0, 0, rc.right, rc.bottom, g_backDC, 0, 0, SRCCOPY);
      EndPaint(hwnd, &ps);
      g_lastFrame = g_lastFull = GetTickCount();
      g_titleDirty = false;
      return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SETFOCUS: g_focused = true; InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_KILLFOCUS: g_focused = false; InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_CHAR:
    case WM_SYSCHAR: {
      if (g_tabs.empty()) return 0;
      if (g_sw.open) {
        if (msg == WM_CHAR) SwChar((wchar_t)wp);
        return 0;
      }
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
          if (g_comp.open && g_comp.p == CurTab().active) g_comp.autoAccept = false, CompRefresh();
          InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
      }
      SendChar(CurTab().active, cp, msg == WM_SYSCHAR ? VTERM_MOD_ALT : VTERM_MOD_NONE);
      return 0;
    }
    case WM_DROPFILES: {  // archivos o carpetas soltados: su ruta en el panel de debajo
      HDROP drop = (HDROP)wp;
      POINT pt;
      DragQueryPoint(drop, &pt);
      std::wstring paths;
      UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
      for (UINT i = 0; i < n; i++) {
        wchar_t f[MAX_PATH * 2];
        if (DragQueryFileW(drop, i, f, MAX_PATH * 2)) paths += (paths.empty() ? L"" : L" ") + QuotePath(f);
      }
      DragFinish(drop);
      if (g_tabs.empty()) return 0;
      Pane* p = PaneAt(pt.x, pt.y);
      if (!p) p = CurTab().active;
      CurTab().active = p;
      SetForegroundWindow(hwnd);
      if (EditorActive(p) && p->caret && p->edit[p->caret - 1] != L' ') paths = L" " + paths;
      PasteText(p, paths);
      InvalidateRect(hwnd, nullptr, FALSE);
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
      if (POINT pt{x, y}; PtInRect(&g_upd.rc, pt)) {  // aviso de actualización
        if (msg == WM_LBUTTONDOWN && PtInRect(&g_upd.close, pt)) g_upd.hidden = true;
        else if (msg == WM_LBUTTONDOWN && PtInRect(&g_upd.action, pt)) UpdateAction();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      if (g_comp.open) {  // menú de autocompletado: clic en una opción la elige; fuera, se cierra
        POINT pt{x, y};
        if (PtInRect(&g_comp.rc, pt)) {
          if (msg == WM_LBUTTONDOWN) CompAccept(g_comp.top + (y - g_comp.rc.top - Px(4)) / Px(30));
          return 0;
        }
        CompClose();
      }
      if (g_sw.open) {  // selector de terminales: elegir o cerrar
        POINT pt{x, y};
        if (msg == WM_LBUTTONDOWN && PtInRect(&g_sw.rc, pt) && y >= g_sw.rc.top + Px(40))
          SwChoose(g_sw.top + (y - g_sw.rc.top - Px(40)) / Px(30));
        else if (!PtInRect(&g_sw.rc, pt)) CloseSwitcher();
        return 0;
      }
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
        int tab = -1;
        int hit = y < TabBarH() || x < SidebarW() ? TitleHit(x, y, &tab) : HIT_NONE;
        SetHover(hit, tab);
        TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
      }
      if (g_tabDrag >= 0) {  // arrastrando una pestaña: sobre otra se cambian de sitio; fuera, a otra ventana
        POINT sp{x, y};
        ClientToScreen(hwnd, &sp);
        int j;
        bool moved = abs(x - g_tabDragStart.x) > Px(6) || abs(y - g_tabDragStart.y) > Px(6);
        int hit = moved && !OutsideWindow(sp) ? TitleHit(x, y, &j) : HIT_NONE;
        if ((hit == HIT_TAB || hit == HIT_TABCLOSE) && j != g_tabDrag) {
          if (j > g_tabDrag) std::rotate(g_tabs.begin() + g_tabDrag, g_tabs.begin() + g_tabDrag + 1, g_tabs.begin() + j + 1);
          else std::rotate(g_tabs.begin() + j, g_tabs.begin() + g_tabDrag, g_tabs.begin() + g_tabDrag + 1);
          g_activeTab = g_tabDrag = j;
          InvalidateRect(hwnd, nullptr, FALSE);
        }
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
      // Flecha en todas partes, como Warp; cursor de texto solo en la línea de la caja de entrada
      // y mano en lo que se puede pulsar (carpeta, enlaces con Ctrl).
      LPCWSTR cur = IDC_ARROW;
      if (d) cur = d->vertical ? IDC_SIZEWE : IDC_SIZENS;
      else if (g_link.p) cur = IDC_HAND;
      else if (Pane* p = y >= TabBarH() && !HeaderAt(x, y, &onHeader) ? PaneAt(x, y) : nullptr) {
        POINT pt{x, y};
        if (PtInRect(&p->chip, pt) && EditorActive(p)) cur = IDC_HAND;
        else if (PtInRect(&p->input, pt) && y >= p->chip.bottom && EditorActive(p)) cur = IDC_IBEAM;
      }
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
        POINT sp{x, y};
        ClientToScreen(hwnd, &sp);
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
      // Cerrar con la X guarda la ventana: al volver a abrir Warp vuelven todas. Para descartarla
      // se cierran sus pestañas (al quedarse vacía se borra su sesión, ver WM_DESTROY).
      SaveSession();
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
      if (g_tabs.empty() && !g_keepSession && !g_windowDir.empty() && !g_sessionPath.empty())
        DeleteFileW(g_sessionPath.c_str());  // se cerraron todas sus pestañas: la ventana se descarta
      std::vector<Pane*> all(g_live.begin(), g_live.end());
      for (Pane* p : all) DestroyPane(p);
      g_tabs.clear();
      PostQuitMessage(0);
      return 0;
    }
  }
  if (msg == g_msgUpdated && g_msgUpdated) {  // otra ventana instaló la actualización
    if (UpdateReadyOnDisk()) {
      g_upd.stage = UPD_READY;
      g_upd.latest = ExeVersion(InstalledExe());
      g_upd.hidden = false;
      InvalidateRect(hwnd, nullptr, FALSE);
    }
    return 0;
  }
  if (msg == g_msgQuitAll && g_msgQuitAll) {
    g_keepSession = true;
    PostMessageW(hwnd, WM_CLOSE, 0, 0);
    return 0;
  }
  if (msg == g_msgListPanes && g_msgListPanes) {  // otra ventana abre el selector: se le mandan los paneles
    HWND to = (HWND)wp;
    if (!IsWindow(to) || GetPropW(to, kInstanceProp) != InstanceKey()) return 0;
    std::wstring t = PaneListText();
    COPYDATASTRUCT cd{kCopyPaneList, (DWORD)(t.size() * sizeof(wchar_t)), (void*)t.data()};
    SendMessageW(to, WM_COPYDATA, (WPARAM)hwnd, (LPARAM)&cd);
    return 0;
  }
  if (msg == g_msgFocusPane && g_msgFocusPane) {  // elegida en el selector de otra ventana
    FocusPane((int)wp, (int)lp);
    return 0;
  }
  if (msg == WM_COPYDATA) {
    auto* cd = (COPYDATASTRUCT*)lp;
    std::wstring t(cd->lpData ? (const wchar_t*)cd->lpData : L"", cd->cbData / sizeof(wchar_t));
    if (cd->dwData == kCopyOpenDir) {  // «Abrir Warp aquí» desde el Explorador, con Warp ya abierto
      DWORD a = GetFileAttributesW(t.c_str());
      if (!IsLocalDir(t) || a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return FALSE;
      OpenDirTab(t);
      return TRUE;
    }
    if (cd->dwData == kCopyPaneList && g_sw.open) {
      AddPaneList((HWND)wp, t);
      return TRUE;
    }
    return FALSE;
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
    MessageBoxW(nullptr, T(S_NEED_WIN10), L"Warp", MB_ICONERROR);
    return 1;
  }
  typedef BOOL(WINAPI * PFN_Dpi)(HANDLE);
  if (auto fn = (PFN_Dpi)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"))
    fn((HANDLE)-4);  // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2

  int argc = 0;
  std::wstring windowArg;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  for (int i = 1; i < argc; i++)
    if (!wcscmp(argv[i], L"--relaunch"))  // tras actualizar: esperar a que se cierren las ventanas viejas
      for (int t = 0; t < 200 && FindWindowW(L"WarpLite", nullptr); t++) Sleep(100);
  for (int i = 1; i + 1 < argc; i++) {
    if (!wcscmp(argv[i], L"--window")) windowArg = argv[++i];
    else if (!wcscmp(argv[i], L"--cd")) g_openDir = argv[++i];
    else if (!wcscmp(argv[i], L"--script")) {
      std::wstring s = argv[++i];
      size_t pos;
      while ((pos = s.find(L'|')) != std::wstring::npos) g_script.push_back(s.substr(0, pos)), s.erase(0, pos + 1);
      if (!s.empty()) g_script.push_back(s);
    }
  }
  LocalFree(argv);
  g_msgQuitAll = RegisterWindowMessageW(L"warp-lite-quit-all");
  g_msgUpdated = RegisterWindowMessageW(L"warp-lite-updated");
  g_msgListPanes = RegisterWindowMessageW(L"warp-lite-list-panes");
  g_msgFocusPane = RegisterWindowMessageW(L"warp-lite-focus-pane");
  if (!g_openDir.empty()) {
    // El Explorador pasa "C:\" y Windows lo lee como «C:"»: se arregla la barra del final.
    while (!g_openDir.empty() && (g_openDir.back() == L' ' || g_openDir.back() == L'\t')) g_openDir.pop_back();
    if (!g_openDir.empty() && g_openDir.back() == L'"') g_openDir.back() = L'\\';
    if (g_openDir.size() > 3 && g_openDir.back() == L'\\') g_openDir.pop_back();
    if (g_openDir.size() == 2 && g_openDir[1] == L':') g_openDir += L'\\';
    DWORD a = GetFileAttributesW(g_openDir.c_str());
    if (!IsLocalDir(g_openDir) || a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) g_openDir.clear();
  }
  if (!g_openDir.empty() && windowArg.empty()) {
    // Warp ya abierto: la pestaña se abre en su ventana de delante y este proceso termina.
    std::vector<HWND> open = SiblingWindows();
    if (!open.empty()) {
      DWORD pid = 0;
      GetWindowThreadProcessId(open[0], &pid);
      AllowSetForegroundWindow(pid);
      COPYDATASTRUCT cd{kCopyOpenDir, (DWORD)(g_openDir.size() * sizeof(wchar_t)), (void*)g_openDir.data()};
      DWORD_PTR r = 0;
      if (SendMessageTimeoutW(open[0], WM_COPYDATA, 0, (LPARAM)&cd, SMTO_ABORTIFHUNG, 5000, &r) && r) return 0;
    }
  }
  {
    wchar_t self[MAX_PATH], buf[64];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    g_upd.running = GetEnvironmentVariableW(L"WARPLITE_FAKE_VERSION", buf, 64) ? buf : ExeVersion(self);
    g_upd.testUrl = GetEnvironmentVariableW(L"WARPLITE_UPDATE_URL", nullptr, 0) > 0;
  }
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // WIC (pegar imágenes)
  g_sessionPath = SessionPath(windowArg);
  LoadUpdateSettings();
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
                                                     {hack_bolditalic, hack_bolditalic_end},
                                                     {nerd_symbols, nerd_symbols_end}};
  for (auto& f : fonts) {
    DWORD n = 0;
    AddFontMemResourceEx((void*)f.a, (DWORD)(f.b - f.a), nullptr, &n);
  }

  WNDCLASSEXW wc{sizeof wc};
  wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));  // assets/warp-lite.ico
  wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                 GetSystemMetrics(SM_CYSMICON), 0);
  wc.lpszClassName = L"WarpLite";
  RegisterClassExW(&wc);
  HWND hwnd = CreateWindowExW(0, L"WarpLite", L"Warp", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              1280, 760, nullptr, nullptr, inst, nullptr);
  if (!hwnd) return 1;
  SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
  DragAcceptFiles(hwnd, TRUE);  // soltar archivos: se escribe su ruta
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
    if (g_sw.open && keyDown) {
      if (!SwKey(m.wParam)) TranslateMessage(&m), DispatchMessageW(&m);
      continue;
    }
    if (g_pick.open && keyDown) {
      if (!PickKey(m.wParam)) TranslateMessage(&m), DispatchMessageW(&m);
      continue;
    }
    if (g_comp.open && keyDown) {
      if (g_tabs.empty() || g_comp.p != CurTab().active || !EditorActive(g_comp.p)) CompClose();
      else if (CompKey(m.wParam)) continue;
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
