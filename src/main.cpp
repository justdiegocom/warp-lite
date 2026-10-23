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
#include <shobjidl.h>
#include <inspectable.h>
#include <wincodec.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <restartmanager.h>
#include <dwrite_3.h>
#include <dbghelp.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <csignal>
#include <exception>
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
#include "qrcodegen.hpp"

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
// Default terminal text (title-bar menu → Text brightness): #CCCCCC is Windows Terminal's and cmd's; the default is a
// step brighter, between it and the white of the versions before DirectWrite.
static const COLORREF kTextLevels[] = {RGB(0xaa, 0xaa, 0xaa), RGB(0xcc, 0xcc, 0xcc), RGB(0xdd, 0xdd, 0xdd),
                                       RGB(0xee, 0xee, 0xee), RGB(0xff, 0xff, 0xff)};
static const int kTextLevelCount = 5, kTextDefault = 2;
static COLORREF g_termFg = kTextLevels[kTextDefault];
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
static const COLORREF kPathFg = kCmdOk;                 // files and folders that exist, in the output: the same green
static const COLORREF kChipBorder = RGB(0x3a, 0x3a, 0x3a);
static const uint32_t kAnsi[16] = {0x616161, 0xFF8272, 0xB4FA72, 0xFEFDC2, 0xA5D5FE, 0xFF8FFD,
                                   0xD0D1FE, 0xF1F1F1, 0x8E8E8E, 0xFFC4BD, 0xD6FCB9, 0xFEFDD5,
                                   0xC1E3FE, 0xFFB1FE, 0xE5E6FE, 0xFEFFFF};
static const int kGap = 1;           // separación entre paneles (px)
static const int kPad = 4;           // margen interior de cada panel (px)
static const size_t kScrollback = 5000;

enum { WM_APP_DIRTY = WM_APP + 1, WM_APP_EXITED, WM_APP_SCRIPT, WM_APP_LAYOUT, WM_APP_CLIP, WM_APP_PARAMS, WM_APP_UPDATE, WM_APP_UPDATED, WM_APP_UPDPROG, WM_APP_FONT, WM_APP_PATHS, WM_APP_PROMPT, WM_APP_NETCHANGE, WM_APP_NOTE, WM_APP_OPEN };
enum { TIMER_RESTORE = 1, TIMER_SCRIPT = 2, TIMER_TOAST = 3, TIMER_FRAME = 4, TIMER_UPDATE = 5, TIMER_UPDANIM = 6, TIMER_PATHS = 7,
       TIMER_REMOTE = 8, TIMER_PAIR = 9, TIMER_PULSE = 10, TIMER_PHONE = 11, TIMER_JOIN = 12, TIMER_SAVE = 13 };

// ---------------------------------------------------------------------------
// Modelo: pestañas -> árbol de paneles

struct Pane;
struct Window;

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

// Auto-reconnect of an ssh whose connection dropped (see «Reconnect» below).
enum { RC_OFF, RC_WAITING, RC_TRYING };
static const int kNoExit = INT_MIN;  // the prompt did not say how the command exited
struct Reconnect {
  std::string cmd;        // command line of the run in flight, as sent from the box ("" for an empty Enter)
  bool byWarp = false;    // that run was launched by Warp (session restore or a reconnect attempt)
  int state = RC_OFF;     // RC_WAITING: next attempt at `due`; RC_TRYING: attempt `attempt` is running
  int attempt = 0;
  DWORD due = 0, launched = 0, outSeen = 0;
  bool ended = false;     // the prompt came back: decided a moment later, when ssh's last lines are drawn
  int exit = kNoExit;
  DWORD endedAt = 0;
  std::wstring label;     // status card text last drawn
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
  Reconnect recon;
  // Paths in their own color (see «Path highlighting»): for each view row, the hash of the text they were found in and
  // their cells [c0, c1). outSeq counts the shell's output, so new text is looked at again.
  std::vector<size_t> pathHash;
  std::vector<std::vector<std::pair<int, int>>> pathSpans;
  std::atomic<unsigned> outSeq{0};
  unsigned pathSeq = 0;
  bool pathDue = false;  // look again: text drawn that no pass has seen, or new answers from the disk
  int pathRounds = 0;    // passes redone for answers since the text last changed (capped)
  // Unread badge (see «Unread badges»). Reader thread, with the lock: what the title and OSC 7501 last said
  // (NOTE_IDLE/NOTE_BUSY, 0 = no agent) and the last kitty notification id. UI thread: the count and its dedupe.
  int titleState = 0, statusState = 0;
  std::string kittyId;
  int unread = 0;
  DWORD noteTick = 0;      // last event (counted or not), to merge an OSC notification with its BEL
  bool noteQuiet = false;  // a finish was seen through the title or OSC 7501: its own bell/notification is not new
  // Reader thread, with the lock (OnNote takes the first two): the title and body the last OSC 9/777/99 notification
  // carried, and the kitty notification being gathered from its chunks (its id, title and body).
  std::string noteTitle, noteBody, kittyPending, kittyTitle, kittyBody;
  // UI thread. attention: a counted event the user hasn't clicked or typed in yet (see «Attention»); toasted: its
  // Windows notification may still be in the action center (see «Windows notifications»).
  unsigned id = 0;  // this process's serial number: a notification's click and a phone find the pane by it
  bool attention = false, toasted = false;
  Window* win = nullptr;  // the window it is in now (UI thread only: the reader thread posts to g_host)
  std::wstring savedCwd;  // its folder when its window's session was last told (SessionChanged)
  // Reader thread → UI thread: rows to draw again, and its title changed (headers and tabs, at most 4 times a second).
  std::atomic<bool> repaint{false}, titleChanged{false};
};

static std::set<Pane*> g_live;
static std::atomic<bool> g_dirtyPosted{false};
static std::wstring g_fontFace = L"Hack";  // terminal font family (title-bar menu → Font)
static std::wstring g_fontLoading;          // font being downloaded (its progress replaces the toast)
static std::atomic<int> g_fontPct{-1}, g_fontFile{0};  // download progress: % of file number g_fontFile
static int g_fontFiles = 0;                 // files in that download
static const int kDefaultFontPx = 13;  // DEFAULT_MONOSPACE_FONT_SIZE de Warp

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
           S_UPD_OFFLINE, S_UPD_DOWNLOADING, S_SWITCH, S_OTHER_WINDOW, S_FONT_MENU, S_OTHER_FONTS, S_FONT_FAILED,
           S_FONT_SIZE, S_TEXT_BRIGHTNESS, S_DEFAULT, S_RECONNECTING, S_RECONNECT_WAIT, S_REOPEN, S_OPEN, S_COPY_PATH, S_COPY_LINK,
           S_OPEN_LOCATION, S_UNREAD, S_PAIR_PHONE, S_PAIRED_DEVICES, S_NO_DEVICES, S_REMOVE_DEVICE, S_PAIR_TITLE, S_PAIR_HINT,
           S_PAIR_STARTING, S_PAIR_EXPIRES, S_PAIR_EXPIRED, S_PAIR_NEW, S_COPY, S_COPIED, S_PAIR_FAILED, S_PHONE_PAIRED,
           S_PHONE_CONNECTED, S_WIN_NOTIFY, S_ALLOW_TYPING, S_PHONE_TYPING, S_COUNT };
static const wchar_t* const kStrings[][S_COUNT] = {
    {L"Type a command", L"Running… keys go to the program", L"Zoom %d%%", L"Font %d px",
     L"Windows 10 1809 or later is required (ConPTY).", L"Language", L"Find", L"No results",
     L"Search directories…", L".. (Parent directory)", L"Rename", L"Hide tab", L"Close tab", L"Show all",
     L"%d hidden", L"Start with Windows", L"Loading…", L"Command", L"Folder", L"File", L"Parameter",
     L"Warp %s is available", L"Installing %s…", L"Updated to %s", L"Couldn't update to %s", L"Install", L"Restart",
     L"Retry", L"Check for updates", L"Update automatically", L"Warp is up to date", L"Checking for updates…",
     L"Couldn't check for updates", L"Downloading %s…", L"Search terminals…", L"other window", L"Font",
     L"Other installed", L"Couldn't download %s", L"Font size", L"Text brightness", L"default",
     L"Reconnecting… (attempt %d) — Esc to cancel", L"Reconnecting in %d s (attempt %d) — Esc to cancel",
     L"Reopen on restart", L"Open", L"Copy path", L"Copy link", L"Open file location", L"%d unread", L"Pair phone…",
     L"Paired devices", L"No paired devices", L"Remove \u201c%ls\u201d", L"Pair phone",
     L"Scan it with the Warp app on your phone, or paste the text there.", L"Getting a pairing code…",
     L"Expires in %d:%02d", L"This code has expired.", L"New code", L"Copy", L"Copied",
     L"Couldn't start pairing: %ls", L"Phone paired", L"Phone connected",
     L"Windows notifications", L"Allow typing", L"Phone typing"},
    {L"Escribe una orden", L"Ejecutando… las teclas van al programa", L"Escala %d %%", L"Letra %d px",
     L"Hace falta Windows 10 1809 o posterior (ConPTY).", L"Idioma", L"Buscar", L"Sin resultados",
     L"Buscar carpetas…", L".. (Carpeta superior)", L"Renombrar", L"Ocultar pestaña", L"Cerrar pestaña",
     L"Mostrar todas", L"%d ocultas", L"Iniciar con Windows", L"Cargando…", L"Orden", L"Carpeta", L"Archivo",
     L"Parámetro", L"Warp %s disponible", L"Instalando %s…", L"Actualizado a %s", L"No se pudo actualizar a %s",
     L"Instalar", L"Reiniciar", L"Reintentar", L"Buscar actualizaciones", L"Actualizar automáticamente",
     L"Warp está al día", L"Buscando actualizaciones…", L"No se pudo comprobar si hay actualizaciones",
     L"Descargando %s…", L"Buscar terminales…", L"otra ventana", L"Fuente", L"Otras instaladas",
     L"No se pudo descargar %s", L"Tamaño de letra", L"Brillo del texto", L"predeterminado",
     L"Reconectando… (intento %d) — Esc para cancelar", L"Reconectando en %d s (intento %d) — Esc para cancelar",
     L"Abrir al reiniciar", L"Abrir", L"Copiar ruta", L"Copiar enlace", L"Abrir ubicación del archivo", L"%d sin leer",
     L"Vincular móvil…", L"Dispositivos vinculados", L"Ningún dispositivo vinculado", L"Quitar \u00ab%ls\u00bb",
     L"Vincular móvil", L"Escanéalo con la app Warp del móvil, o pega allí el texto.", L"Preparando el código…",
     L"Caduca en %d:%02d", L"Este código ha caducado.", L"Código nuevo", L"Copiar", L"Copiado",
     L"No se pudo empezar a vincular: %ls", L"Móvil vinculado", L"Móvil conectado",
     L"Notificaciones de Windows", L"Permitir escribir", L"Móvil escribiendo"},
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
// Several windows, all in one process (see «One Warp, many windows»), each with its file window-<id>.txt in g_windowDir.
// The main window (window-main.txt, the first one Warp opens) always comes back at the next start;
// the others only with «Reopen on restart» (g_reopen) or after an update restart (g_restartAll).
static std::wstring g_windowDir;
static std::wstring g_sessionFile;  // WARPLITE_SESSION (tests): the one session file; this Warp's settings go next to it
static UINT g_msgListPanes;                  // warp-remote asks for the panes (see «Phone»)
static UINT g_msgRemote, g_msgRemoteState;   // warp-remote → this Warp: phones / events; this Warp → it: how many phones
static int g_remotePhones = 0;               // phones connected now (title-bar indicator, see «Phone»)
// Las ventanas de un mismo Warp (mismo %APPDATA%\warp-lite, o la misma carpeta en las pruebas) se
// reconocen por esta marca: así las pruebas nunca tocan las ventanas de verdad.
static const ULONG_PTR kCopyPaneList = 0x574c5300;
static const wchar_t* kInstanceProp = L"warp-lite-instance";

// Enlace bajo el ratón con Ctrl pulsado (Ctrl+clic lo abre).
struct LinkHit {
  Pane* p = nullptr;
  int row = -1, c0 = 0, c1 = 0;  // trozo bajo el ratón
  std::wstring url;
  std::wstring path;  // or a file/folder that exists (full path)
  bool dir = false;   // path is a folder
  std::vector<std::array<int, 3>> segs;  // todos los trozos (fila, c0, c1) de una URL partida
};

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
  RECT rc{}, prev{}, next{}, close{};  // bar and its buttons, from the last paint
};

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
static const int kPickRows = 9;
static bool g_layoutHeaders = false;
// Barra de título propia.
enum { HIT_NONE, HIT_TAB, HIT_TABCLOSE, HIT_PLUS, HIT_MIN, HIT_MAX, HIT_CLOSE, HIT_TOGGLE, HIT_HIDDEN };

struct SwitchItem {
  HWND hwnd;  // its window (this one or another process's)
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
static const int kCompRows = 9;

struct PairCard {
  bool open = false;
  std::wstring ticket, error;
  DWORD shown = 0, expires = 0;          // GetTickCount: card opened (or code asked for), code stops working
  std::vector<std::vector<bool>> qr;     // modules of the QR code, [y][x]
  bool copied = false;
  RECT rc{}, action{}, close{};
};

// The hint on this window while a tab or pane of another one is over it (JoinHover): that window, the point of the
// screen and the mark drawn. TIMER_JOIN runs only while it is shown, in case that window is gone without a word.
struct JoinHint {
  HWND from = nullptr;
  POINT at{};
  RECT mark{};
};

// Respaldo por carácter, como Warp: lo que Hack no tiene (⎿ de Claude, iconos de Nerd Fonts,
// emoji, CJK…) se dibuja con la primera de estas fuentes que lo tenga.
static const wchar_t* const kFallbackFaces[] = {L"Symbols Nerd Font Mono", L"Segoe UI Symbol", L"Cascadia Mono",
                                                L"Consolas", L"Segoe UI Emoji", L"MS Gothic", L"Malgun Gothic",
                                                L"Microsoft YaHei", L"Nirmala UI", L"Segoe UI"};
static const int kFallbackCount = sizeof kFallbackFaces / sizeof kFallbackFaces[0];
static const int kEmojiFace = 4;  // Segoe UI Emoji

// A window and everything that is its own: its tabs, its session file, its scale and fonts, what is drawn over its
// panes, its selection and drags, its back buffer. g_win is the window being handled: WndProc sets it (InWindow) and
// whatever acts on another window sets it the same way.
struct Window {
  HWND hwnd = nullptr;
  std::vector<Tab> tabs;
  int activeTab = 0;
  // Its session file (window-<id>.txt) and whether it comes back at the next start (see «Several windows»).
  std::wstring sessionPath;
  HANDLE fileMutex = nullptr;  // held while it is open (see ClaimWindowFile)
  bool isMain = false, reopen = false, restartAll = false;
  bool keepSession = false;  // closing for quit-all / Windows shutdown: its last pane closing doesn't discard it
  bool hasWinPos = false, winMax = false;  // its place and size last time (from its session)
  RECT winRect{};
  std::wstring openDir;  // --cd <folder> («Open Warp here»): a new tab already in it
  Tab adopt{};           // a tab moved out of another window to this new one (Detach): its first tab
  // Scale and fonts: each window has its own DPI, zoom and font size.
  int dpi = 96, zoom = 100, fontPx = kDefaultFontPx;
  HFONT fonts[4] = {};  // regular, bold, italic, bold italic
  HFONT uiFont = nullptr, badgeFont = nullptr, iconFont = nullptr;  // badges: bold; icons: Segoe MDL2 Assets
  HFONT fallback[kFallbackCount] = {};
  int cellW = 8, cellH = 16;
  int textDy = 0;  // vertical margin of the text in its cell
  float dwEm = 0;  // DirectWrite: the terminal text's size and baseline (the faces are shared)
  int dwAscent = 0;
  std::vector<uint8_t> builtin[176];       // coverage of each builtin glyph at builtinW x builtinH
  std::vector<uint16_t> builtinSpan[176];  // and the covered columns [first, last) of each of its rows
  int builtinW = 0, builtinH = 0;
  bool vertTabs = false;  // tabs in a sidebar
  bool focused = true;
  // Title bar: hover, a pressed button, a tab pressed (to drag it), a tab being renamed, a short notice.
  int hoverHit = HIT_NONE, hoverTab = -1, pressedHit = HIT_NONE;
  int tabDrag = -1;
  POINT tabDragStart{};
  int renameTab = -1;
  std::wstring renameText;
  bool renameAll = false;  // the whole name selected: what is typed replaces it
  std::wstring toast;
  // Selection (view coordinates in a pane), drags of dividers and panes, the link under the mouse with Ctrl.
  Pane* selPane = nullptr;
  VTermPos selA{}, selB{};
  bool selecting = false, hasSel = false;
  Node* dragNode = nullptr;
  Pane* dragPane = nullptr;
  Pane* dropTarget = nullptr;
  LinkHit link;
  // What is drawn over the panes.
  FindState find;
  DirPicker pick;
  Switcher sw;
  CompState comp;
  PairCard pair;
  RECT updRc{}, updAction{}, updClose{};  // the update notice and its buttons, from the last paint
  bool phoneTyping = false;               // a phone typed into one of its panes in the last seconds (title bar)
  JoinHint joinHint;                      // a tab or pane of another window over this one (see «Join»)
  HWND joinOver = nullptr;                // the window showing where what is dragged from this one would go
  // Painting: back buffer, frame pacing, timers running.
  HDC backDC = nullptr;
  HBITMAP backBmp = nullptr;
  int backW = 0, backH = 0;
  DWORD lastFrame = 0, lastFull = 0;
  bool frameTimer = false, pulseTimer = false, pathTimer = false;
  bool titleDirty = false;  // a pane's title changed: its headers and tabs are drawn again (at most 4 times a second)
  bool saveDue = false;     // its session changed: saved soon (SessionChanged)
  int unreadShown = -1;     // total on its taskbar button (-1: set it again)
  bool dead = false;        // destroyed: freed once the message that destroyed it is over (see FreeDeadWindows)
  // Tests: its name (label: step), its script (--script; childScript: the one for the windows it opens), the waits of
  // its steps, a drag, and what its events and toasts did.
  std::wstring label;
  std::vector<std::wstring> script;
  std::wstring childScript;
  size_t scriptPos = 0;
  int waited = 0;
  POINT scriptDrag{};
  std::wstring noteLog;
  std::vector<std::wstring> toastLog;
  std::wstring toastLaunch;
  HANDLE toastProc = nullptr;
};
static Window* g_win;                  // the window being handled
static std::vector<Window*> g_windows;  // every window of this process
// This process's hidden message window (see «One Warp, many windows»): the reader threads and the workers post what
// they have to it, and it hands each thing to the window it is for on the UI thread, so they never touch a window.
// It never goes away while they run.
static HWND g_host;
static std::vector<Window*> g_dead;  // destroyed windows, freed by the message loop (FreeDeadWindows)
static volatile LONG g_crashArmed;   // windows are open: a crash relaunches Warp (not while it starts or ends; «Crashes»)

// Handles window w until the end of the scope (WndProc, and whatever acts on another window).
struct InWindow {
  Window* prev;
  explicit InWindow(Window* w) : prev(g_win) { g_win = w; }
  ~InWindow() { g_win = prev; }
  InWindow(const InWindow&) = delete;
  InWindow& operator=(const InWindow&) = delete;
};
static Window* FrontWindow();
static void InvalidateAll();
static void CloseWindowState();

// A window to open: its session file (empty: a new one), a folder for a new tab in it (--cd), and in a test's Warp its
// script and the one for the windows it opens. A tab moved out of another window (Detach): that tab, instead of a
// session, with that window's scale and tab bar, at that place.
struct WindowInit {
  std::wstring file, openDir, script, childScript;
  int show = SW_SHOWNORMAL;
  Tab adopt{};
  const Window* like = nullptr;
  RECT place{};
};
static Window* OpenWindow(const WindowInit& in);
// A window's session changed (a tab or pane opened, closed or moved, a folder, a name, what runs, its place): it is
// saved a moment later, at most once a second for all of them, so a crash loses nothing that matters (see «Crashes»).
// The file is replaced whole (a temporary file renamed over it).
static bool g_saveTimer = false;
static void SessionChanged(Window* w = nullptr) {
  if (!w) w = g_win;
  if (!w || w->dead) return;
  w->saveDue = true;
  if (!g_saveTimer && g_host) SetTimer(g_host, TIMER_SAVE, 1000, nullptr), g_saveTimer = true;
}

// The window with that handle, if it is one of this process's and still open.
static Window* LiveWindow(HWND h) {
  for (Window* w : g_windows)
    if (w->hwnd == h && h) return w;
  return nullptr;
}


static int ScaledDpi() { return MulDiv(g_win->dpi, g_win->zoom, 100); }
static int Px(int v) { return MulDiv(v, ScaledDpi(), 96); }
static int TabBarH() { return Px(38); }
static int PaneHdrH() { return std::max(Px(26), g_win->cellH + Px(8)); }
static Tab& CurTab() { return g_win->tabs[g_win->activeTab]; }

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

// Rows of p changed (any thread): its window draws them soon (OnDirty).
static void RequestRepaint(Pane* p) {
  p->repaint = true;
  if (!g_dirtyPosted.exchange(true)) PostMessage(g_host, WM_APP_DIRTY, 0, 0);
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


// Unread badges, reader thread side: what a program says when it finishes or needs input. It is only posted to the
// window (WM_APP_NOTE); counting and dedupe happen on the UI thread (OnNote).
//   NOTE_ALERT  a desktop notification: OSC 9 (iTerm2), OSC 777;notify (Ghostty), OSC 99 (kitty)
//   NOTE_BELL   BEL (Claude Code's terminal_bell, Codex's default, a bell tmux passes on)
//   NOTE_BUSY / NOTE_IDLE  an agent starts / stops working: Claude Code's title (◐ ◑ while working, ✳ otherwise;
//               always ✳ under tmux) or OSC 7501 (Codex: state=working, then done, idle, blocked...)
enum { NOTE_ALERT, NOTE_BELL, NOTE_BUSY, NOTE_IDLE };

static void PostNote(Pane* p, int kind) { PostMessage(g_host, WM_APP_NOTE, kind, (LPARAM)p); }

// Working → idle posts NOTE_IDLE (it has finished, or waits for an answer); anything else → working, NOTE_BUSY.
static void AgentState(Pane* p, int& state, int now) {
  if (now == NOTE_BUSY && state != NOTE_BUSY) PostNote(p, NOTE_BUSY);
  if (now == NOTE_IDLE && state == NOTE_BUSY) PostNote(p, NOTE_IDLE);
  state = now;
}

static int TitleState(const std::wstring& t) {
  if (t.size() < 2 || t[1] != L' ') return 0;
  if (t[0] == 0x25D0 || t[0] == 0x25D1 || (t[0] > 0x2800 && t[0] <= 0x28FF)) return NOTE_BUSY;  // or a braille spinner
  return t[0] == 0x2733 ? NOTE_IDLE : 0;
}

static int CbBell(void* user) {
  PostNote((Pane*)user, NOTE_BELL);
  return 1;
}

// Value of <key>= in a list of key=value separated by sep ("" if it isn't there).
static std::string KeyValue(const std::string& s, const std::string& key, char sep) {
  for (size_t a = 0; a < s.size();) {
    size_t b = s.find(sep, a);
    if (b == std::string::npos) b = s.size();
    if (b - a > key.size() && s.compare(a, key.size() + 1, key + "=") == 0) return s.substr(a + key.size() + 1, b - a - key.size() - 1);
    a = b + 1;
  }
  return "";
}

// A whole OSC 9 / 99 / 777 / 7501 from a program (not the prompt's 9;9;<token>). A notification's title and body
// go with it, for its Windows notification.
static void OscNote(Pane* p, int command, const std::string& s) {
  auto alert = [p](const std::string& title, const std::string& body) {
    p->noteTitle = title.substr(0, 1024);
    p->noteBody = body.substr(0, 1024);
    PostNote(p, NOTE_ALERT);
  };
  if (command == 9) {  // 9;<text>; ConEmu's 9;<number>;... (9;9 folder, 9;4 progress) are not notifications
    size_t n = 0;
    while (n < s.size() && s[n] >= '0' && s[n] <= '9') n++;
    if (!n || (n < s.size() && s[n] != ';')) alert(s, "");
  } else if (command == 777) {  // 777;notify;<title>;<body>
    if (s == "notify" || s.rfind("notify;", 0) == 0) {
      std::string rest = s.substr(std::min<size_t>(s.size(), 7));
      size_t semi = rest.find(';');
      alert(rest.substr(0, semi), semi == std::string::npos ? "" : rest.substr(semi + 1));
    }
  } else if (command == 99) {  // 99;<key=value:...>;<text>, in chunks: d=0 = more to come; one per id; p=title|body
    size_t semi = s.find(';');
    std::string meta = s.substr(0, semi), id = KeyValue(meta, "i", ':'), kind = KeyValue(meta, "p", ':');
    if (kind == "?" || kind == "close" || kind == "alive") return;
    if (!id.empty() && id == p->kittyId) return;
    if (id.empty() || id != p->kittyPending) p->kittyPending = id, p->kittyTitle.clear(), p->kittyBody.clear();
    std::string& part = kind == "body" ? p->kittyBody : p->kittyTitle;
    if ((kind.empty() || kind == "title" || kind == "body") && KeyValue(meta, "e", ':') != "1" && semi != std::string::npos &&
        part.size() < 1024)
      part += s.substr(semi + 1);  // base64 (e=1) is left out
    if (KeyValue(meta, "d", ':') == "0") return;
    p->kittyId = id;
    alert(p->kittyTitle, p->kittyBody);
    p->kittyPending.clear(), p->kittyTitle.clear(), p->kittyBody.clear();
  } else if (command == 7501) {  // 7501;state=working|blocked|idle|done|error|clear:app=...
    std::string st = KeyValue(s, "state", ':');
    if (st == "working") AgentState(p, p->statusState, NOTE_BUSY);
    else if (st == "idle" || st == "done" || st == "blocked" || st == "error") AgentState(p, p->statusState, NOTE_IDLE);
    else if (st == "clear") p->statusState = 0;
  }
}

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
      if (p->integrated) PostMessage(g_host, WM_APP_LAYOUT, 0, (LPARAM)p);  // la caja se oculta a pantalla completa
      break;
    case VTERM_PROP_MOUSE: p->mouseMode = v->number; break;
    case VTERM_PROP_TITLE:
      if (v->string.initial) p->titleBuf.clear();
      p->titleBuf.append(v->string.str, v->string.len);
      if (v->string.final) {
        std::wstring t = ToWide(p->titleBuf);
        if (t != p->title) p->title = t, p->titleChanged = true;
        AgentState(p, p->titleState, TitleState(t));
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

// OSC 9;9;<token>;<código de salida>;<ruta> (lo emite el prompt de warp-lite): la shell espera una
// orden en <ruta>. The exit code is $LASTEXITCODE as an int; empty or invalid means unknown. Other OSC 9, and
// OSC 99 / 777 / 7501, may be a program saying it has finished (OscNote).
static int CbOsc(int command, VTermStringFragment frag, void* user) {
  Pane* p = (Pane*)user;
  if (command != 9 && command != 99 && command != 777 && command != 7501) return 0;
  if (frag.initial) p->oscBuf.clear();
  if (p->oscBuf.size() < 65536) p->oscBuf.append(frag.str ? frag.str : "", frag.len);
  if (!frag.final) return 1;
  std::string head = "9;" + p->token + ";";
  if (command == 9 && !p->token.empty() && p->oscBuf.rfind(head, 0) == 0) {
    size_t semi = p->oscBuf.find(';', head.size());
    if (semi == std::string::npos) return 1;
    std::string code = p->oscBuf.substr(head.size(), semi - head.size());
    if (code.size() > 11 || code.find_first_not_of("-0123456789") != std::string::npos) code.clear();
    std::wstring dir = ToWide(p->oscBuf.substr(semi + 1));
    if (!IsLocalDir(dir)) return 1;
    p->cwd = dir;
    bool first = !p->integrated;
    bool ended = p->integrated && !p->atPrompt;  // a command sent from the box has just finished
    p->integrated = p->atPrompt = true;
    PostMessage(g_host, WM_APP_LAYOUT, first, (LPARAM)p);
    if (ended) PostMessage(g_host, WM_APP_PROMPT, (WPARAM)(INT_PTR)(code.empty() ? kNoExit : atoi(code.c_str())), (LPARAM)p);
    return 1;
  }
  OscNote(p, command, p->oscBuf);
  return 1;
}

// OSC 52 (lo que se copia en tmux): libvterm ya lo trae decodificado; al acabar, al portapapeles.
// Solo escritura: las consultas (OSC 52;c;?) no se contestan.
static int CbSelSet(VTermSelectionMask mask, VTermStringFragment frag, void* user) {
  Pane* p = (Pane*)user;
  if (!(mask & VTERM_SELECTION_CLIPBOARD)) return 1;
  if (frag.initial) p->clipBuf.clear();
  if (p->clipBuf.size() < (8u << 20)) p->clipBuf.append(frag.str ? frag.str : "", frag.len);
  if (frag.final && !p->clipBuf.empty()) PostMessage(g_host, WM_APP_CLIP, 0, (LPARAM) new std::wstring(ToWide(p->clipBuf)));
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
  static unsigned serial = 0;
  Pane* p = new Pane();
  p->id = ++serial;
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
    c.bell = CbBell;
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
  vterm_color_rgb(&fg, GetRValue(g_termFg), GetGValue(g_termFg), GetBValue(g_termFg));
  vterm_color_rgb(&bg, GetRValue(kBg), GetGValue(kBg), GetBValue(kBg));
  vterm_state_set_default_colors(st, &fg, &bg);
  for (int i = 0; i < 16; i++) {
    VTermColor c;
    vterm_color_rgb(&c, kAnsi[i] >> 16, (kAnsi[i] >> 8) & 0xff, kAnsi[i] & 0xff);
    vterm_state_set_palette_color(st, i, &c);
  }
  vterm_screen_reset(p->scr, 1);
  p->win = g_win;
  g_live.insert(p);
  return p;
}

// Prompt de PowerShell que avisa a warp-lite (OSC 9;9;<token>;<código de salida>;<ruta>) y deja solo
// un "> " gris.
static std::wstring IntegrationArgs(const std::string& token) {
  std::wstring prompt = L"function global:prompt { $c = $global:LASTEXITCODE -as [int]; $e = [char]27; $b = [char]7; "
                        L"$p = $executionContext.SessionState.Path.CurrentLocation.ProviderPath; "
                        L"[Console]::Write(\"$e]9;9;" + ToWide(token) + L";$c;$p$b\"); \"$e[90m> $e[0m\" }; ";
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
  PostMessage(g_host, WM_APP_EXITED, 0, (LPARAM)ctx);
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
    p->outSeq++;
    RequestRepaint(p);
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

// A window lets go of a pane that closes or moves to another window: nothing of the window points to it any more.
static void ForgetPane(Window* w, Pane* p) {
  if (w->selPane == p) w->selPane = nullptr, w->hasSel = w->selecting = false;
  if (w->dragPane == p || w->dropTarget == p) w->dragPane = w->dropTarget = nullptr;
  if (w->link.p == p) w->link = LinkHit{};
  if (w->find.p == p) w->find = FindState{};
  if (w->pick.p == p) w->pick = DirPicker{};
  if (w->comp.p == p) w->comp = CompState{};
}

// Ends a pane's console and shell: its reader thread then sees the end of its pipe. A window closing does this to all
// its panes first, so their consoles close together.
static void StopPane(Pane* p) {
  if (p->wait) UnregisterWaitEx(p->wait, INVALID_HANDLE_VALUE), p->wait = nullptr;
  if (p->hpc) pClosePseudoConsole(p->hpc), p->hpc = nullptr;
  if (p->proc) {
    if (WaitForSingleObject(p->proc, 0) == WAIT_TIMEOUT) TerminateProcess(p->proc, 0);
    CloseHandle(p->proc), p->proc = nullptr;
  }
}

static void DestroyPane(Pane* p) {
  g_live.erase(p);
  if (p->win) ForgetPane(p->win, p);
  StopPane(p);
  if (p->reader) {
    // A reader thread that doesn't end keeps its pane: freeing it under it would bring down the whole process, and
    // every window with it.
    bool ended = WaitForSingleObject(p->reader, 2000) == WAIT_OBJECT_0;
    CloseHandle(p->reader);
    if (!ended) return;
  }
  if (p->inW) CloseHandle(p->inW);
  if (p->outR) CloseHandle(p->outR);
  vterm_free(p->vt);
  DeleteCriticalSection(&p->cs);
  DeleteCriticalSection(&p->writeCs);
  delete p;
}

static int ChipH() { return g_win->cellH + Px(8); }
static int InputH() { return Px(12) + ChipH() + Px(10) + g_win->cellH + Px(12); }

static void ResizePane(Pane* p, RECT r) {
  p->rc = p->content = r;
  if (g_layoutHeaders) p->content.top += PaneHdrH();
  p->input = RECT{};
  if (p->integrated && !p->altScreen && p->content.bottom - p->content.top > InputH() + 2 * g_win->cellH) {
    p->input = RECT{p->content.left, p->content.bottom - InputH(), p->content.right, p->content.bottom};
    p->content.bottom = p->input.top;
  }
  RECT& c = p->content;
  int cols = std::max(2, (int)(c.right - c.left - 2 * kPad) / g_win->cellW);
  int rows = std::max(1, (int)(c.bottom - c.top - 2 * kPad) / g_win->cellH);
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

static int SidebarW() { return g_win->vertTabs ? Px(220) : 0; }

static void Relayout() {
  if (!g_win->hwnd || g_win->tabs.empty()) return;
  RECT rc;
  GetClientRect(g_win->hwnd, &rc);
  rc.top += TabBarH();
  rc.left += SidebarW();
  for (auto& t : g_win->tabs) {
    g_layoutHeaders = !t.root->leaf();  // cabeceras solo con el panel dividido
    LayoutNode(t.root, rc);
  }
  InvalidateRect(g_win->hwnd, nullptr, FALSE);
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

// Every pane of the window being handled.
static std::vector<Pane*> WindowPanes() {
  std::vector<Pane*> ls;
  for (auto& t : g_win->tabs) Leaves(t.root, ls);
  return ls;
}

static Node* RootOf(Node* n) {
  while (n->parent) n = n->parent;
  return n;
}

static int TabIndexOf(Pane* p) {
  Node* root = RootOf(p->node);
  for (size_t i = 0; i < g_win->tabs.size(); i++)
    if (g_win->tabs[i].root == root) return (int)i;
  return -1;
}

static void ClearSelection() {
  g_win->hasSel = false;
  g_win->selecting = false;
  g_win->selPane = nullptr;
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
  SessionChanged();
  return np;
}

static Pane* NewTab(int at = -1, const std::wstring& dir = L"") {
  Pane* p = NewPane();
  p->cwd = dir;
  Node* n = new Node();
  n->pane = p;
  p->node = n;
  Tab t{n, p};
  if (at < 0 || at > (int)g_win->tabs.size()) at = (int)g_win->tabs.size();
  g_win->tabs.insert(g_win->tabs.begin() + at, t);
  g_win->activeTab = at;
  Relayout();
  SpawnPane(p);
  SessionChanged();
  return p;
}

static void SaveEmptyMain();

// Takes leaf n out of its split in tab t: its sibling takes the split's place, and the tab's active pane, if it was
// n's, becomes one of the panes left. n itself (with its pane) is left on its own.
static void UnlinkLeaf(Tab& t, Node* n) {
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
  n->parent = nullptr;
  if (t.active == n->pane) {
    std::vector<Pane*> ls;
    Leaves(par, ls);
    t.active = ls.front();
  }
  Equalize(t.root);
}

// Tab ti leaves this window (closed, or moved to another one): the active one stays in place or moves back.
static void EraseTab(int ti) {
  g_win->tabs.erase(g_win->tabs.begin() + ti);
  if (g_win->activeTab >= (int)g_win->tabs.size()) g_win->activeTab = std::max(0, (int)g_win->tabs.size() - 1);
  else if (g_win->activeTab > ti) g_win->activeTab--;
}

// A window left without tabs closes, and its session is discarded (WM_DESTROY); the main one keeps its place and
// settings and comes back with a new tab. true if it closed.
static bool CloseIfEmpty() {
  if (!g_win->tabs.empty()) return false;
  if (g_win->isMain && !g_win->keepSession) SaveEmptyMain();
  DestroyWindow(g_win->hwnd);
  return true;
}

static void ClosePane(Pane* p) {
  int ti = TabIndexOf(p);
  if (ti < 0) return;
  Tab& t = g_win->tabs[ti];
  Node* n = p->node;
  if (!n->parent) {
    EraseTab(ti);
    delete n;
    DestroyPane(p);
    if (CloseIfEmpty()) return;  // Último panel cerrado
  } else {
    UnlinkLeaf(t, n);
    delete n;
    DestroyPane(p);
  }
  Relayout();
  SessionChanged();
}

static void EndRename(bool commit);
static void CloseSwitcher();

// Takes a tab, or one pane of a split tab (single), out of this window without ending anything: what runs in it,
// its scrollback and its state go wherever it is put (PutIn), and this window forgets its panes. The tab is erased
// (a window left without tabs is the caller's to close: CloseIfEmpty); a pane taken from a split leaves the rest of its
// tab closing up as when a pane closes.
static Tab TakeOut(int ti, Pane* single) {
  Tab& t = g_win->tabs[ti];
  Tab out;
  if (single && single->node->parent) {
    UnlinkLeaf(t, single->node);
    out = Tab{single->node, single, L"", false};
  } else {
    out = t;
    out.hidden = false;
    EraseTab(ti);
  }
  if (g_win->renameTab >= 0) EndRename(true);  // the tab indexes change
  if (g_win->sw.open) CloseSwitcher();
  std::vector<Pane*> moved;
  Leaves(out.root, moved);
  for (Pane* p : moved) ForgetPane(g_win, p);
  Relayout();
  return out;
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
  SessionChanged();
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

// The program's name in lower case, without its folder or extension (.exe, .cmd, .ps1, .bat).
static std::string ProgramName(const std::string& cmd) {
  std::string exe = ProgramOf(cmd);
  size_t slash = exe.find_last_of("\\/");
  if (slash != std::string::npos) exe = exe.substr(slash + 1);
  std::transform(exe.begin(), exe.end(), exe.begin(), [](char c) { return (char)tolower((unsigned char)c); });
  for (const char* ext : {".exe", ".cmd", ".ps1", ".bat"})
    if (exe.size() > strlen(ext) && exe.compare(exe.size() - strlen(ext), strlen(ext), ext) == 0) exe.resize(exe.size() - strlen(ext));
  return exe;
}

static int AgentKind(const std::string& cmd) {
  std::string exe = ProgramName(cmd);
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
  for (auto& t : g_win->tabs) {
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
    // y Codex, con la orden que reanuda su conversación. An ssh waiting to reconnect counts as running.
    auto r = g_resumeCmd.find(n->pane);
    const std::string& cmd = r != g_resumeCmd.end() && !n->pane->runningCmd.empty() ? r->second : n->pane->lastCmd;
    bool running = !n->pane->runningCmd.empty() || n->pane->recon.state == RC_WAITING;
    o << "L " << HexEncode(cmd) << " " << HexEncode(ToUtf8(n->pane->cwd))
      << (!running || n->pane->lastCmd.empty() ? "" : " run") << "\n";
  } else {
    o << "S " << (n->vertical ? 1 : 0) << " " << n->ratio << "\n";
    SaveNode(o, n->a);
    SaveNode(o, n->b);
  }
}

static void SaveNode(std::ostringstream& o, Node* n);

static std::string SessionHeader(int active, const RECT* win, bool max) {
  std::ostringstream o;
  o << "BW1\nactive " << active << "\nui " << g_win->zoom << " " << g_win->fontPx << "\nlang " << kLangCodes[g_lang]
    << "\nvtabs " << (g_win->vertTabs ? 1 : 0) << "\n";
  if (win) o << "win " << win->left << " " << win->top << " " << win->right << " " << win->bottom << " " << (max ? 1 : 0) << "\n";
  return o.str();
}

// Last lines of a window file (after the tabs, so older versions still read those): «reopen 0/1»
// («Reopen on restart»; versions before it wrote no such line) and «restart 1» while it must come
// back at the next start whatever it says (an update restart, or a tab just moved out to it).
static std::string SessionFlags(bool reopen, bool restart) {
  return std::string("reopen ") + (reopen ? "1" : "0") + "\n" + (restart ? "restart 1\n" : "");
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
  g_win->saveDue = false;
  if (g_win->tabs.empty() || g_win->sessionPath.empty()) return;
  std::ostringstream o;
  o << "BW1\nactive " << g_win->activeTab << "\nui " << g_win->zoom << " " << g_win->fontPx << "\nlang " << kLangCodes[g_lang]
    << "\nvtabs " << (g_win->vertTabs ? 1 : 0) << "\n";
  WINDOWPLACEMENT wp{sizeof wp};
  if (g_win->hwnd && GetWindowPlacement(g_win->hwnd, &wp)) {
    RECT& r = wp.rcNormalPosition;
    bool max = wp.showCmd == SW_SHOWMAXIMIZED ||
               (wp.showCmd == SW_SHOWMINIMIZED && (wp.flags & WPF_RESTORETOMAXIMIZED));
    o << "win " << r.left << " " << r.top << " " << r.right << " " << r.bottom << " " << (max ? 1 : 0) << "\n";
  }
  ComputeResumes();
  for (auto& t : g_win->tabs) {
    std::vector<Pane*> ls;
    Leaves(t.root, ls);
    int ai = (int)(std::find(ls.begin(), ls.end(), t.active) - ls.begin());
    o << "tab " << ai << " " << HexEncode(ToUtf8(t.name)) << (t.hidden ? " hidden" : "") << "\n";
    SaveNode(o, t.root);
  }
  o << SessionFlags(g_win->reopen, g_win->restartAll);
  for (Pane* p : WindowPanes()) p->savedCwd = p->cwd;
  WriteSessionFile(g_win->sessionPath, o.str());
}

// The main window with its last tab closed: only its place, size and settings are kept, and it
// comes back with a new tab (LoadSession finds no tabs).
static void SaveEmptyMain() {
  WINDOWPLACEMENT wp{sizeof wp};
  if (g_windowDir.empty() || g_win->sessionPath.empty() || !GetWindowPlacement(g_win->hwnd, &wp)) return;
  bool max = wp.showCmd == SW_SHOWMAXIMIZED || (wp.showCmd == SW_SHOWMINIMIZED && (wp.flags & WPF_RESTORETOMAXIMIZED));
  WriteSessionFile(g_win->sessionPath, SessionHeader(0, &wp.rcNormalPosition, max) + SessionFlags(g_win->reopen, false));
}

// Does this window come back at the next start? The main one always; the others with «Reopen on
// restart» or after an update restart.
static bool ReopensAtStart() { return g_win->isMain || g_win->reopen || g_win->restartAll; }

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

// A tab as a session file writes it, after its «tab» word: the index of its active pane, its name (hex; old sessions
// have none), «hidden» and its panes (created, not started). Also what a tab or pane brings from another window (Join).
static bool LoadTab(std::istringstream& in, std::vector<Pane*>& created, Tab* out) {
  int ai = 0;
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
  if (!root) return false;
  *out = Tab{root, created[std::min(first + std::max(ai, 0), created.size() - 1)], ToWide(name), hidden};
  return true;
}

static bool LoadSession() {
  HANDLE f = CreateFileW(g_win->sessionPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0,
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
      g_win->vertTabs = v != 0;
      continue;
    }
    if (word == "lang") {  // one language for every window: the main one's
      std::string code;
      in >> code;
      for (int i = 0; i < kLangCount && g_win->isMain; i++)
        if (code == kLangCodes[i]) g_lang = i;
      continue;
    }
    if (word == "ui") {
      in >> g_win->zoom >> g_win->fontPx;
      g_win->zoom = std::clamp(g_win->zoom, 50, 300);
      g_win->fontPx = std::clamp(g_win->fontPx, 6, 48);
      continue;
    }
    if (word == "win") {
      int m = 0;
      in >> g_win->winRect.left >> g_win->winRect.top >> g_win->winRect.right >> g_win->winRect.bottom >> m;
      g_win->winMax = m != 0;
      g_win->hasWinPos = g_win->winRect.right - g_win->winRect.left > 100 && g_win->winRect.bottom - g_win->winRect.top > 100;
      continue;
    }
    if (word == "reopen" || word == "restart") {  // «restart» only matters at start (SessionPath)
      int v = 0;
      in >> v;
      if (word == "reopen") g_win->reopen = v != 0;
      continue;
    }
    if (word != "tab") break;
    Tab t;
    if (!LoadTab(in, created, &t)) break;
    g_win->tabs.push_back(t);
  }
  if (g_win->tabs.empty()) return false;
  g_win->activeTab = std::clamp(active, 0, (int)g_win->tabs.size() - 1);
  g_win->tabs[g_win->activeTab].hidden = false;
  Relayout();
  for (auto& t : g_win->tabs) {
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
  if (g_win->selPane == p) ClearSelection();
}

static void SendKey(Pane* p, VTermKey key, VTermModifier mod) {
  p->noteQuiet = false;  // typed to: its next bell or notification is news again
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
  p->noteQuiet = false;
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
  if (s.empty() || !OpenClipboard(g_win ? g_win->hwnd : g_host)) return;
  EmptyClipboard();
  HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t));
  memcpy(GlobalLock(h), s.c_str(), (s.size() + 1) * sizeof(wchar_t));
  GlobalUnlock(h);
  SetClipboardData(CF_UNICODETEXT, h);
  CloseClipboard();
}

static void CopySelection() { SetClipboardText(SelectionText()); }
// OSC 52 that arrives right after a mouse release in an app (tmux copies on drag end) waits here
// until Ctrl+C; copies from the keyboard (tmux y/Enter, nvim) still go straight to the clipboard.
static std::wstring g_clipPending;
static DWORD g_appMouseUp = 0;  // GetTickCount() of the last button release sent to an app (0: none)
static bool CopyPending() {
  if (g_clipPending.empty()) return false;
  SetClipboardText(g_clipPending);
  g_clipPending.clear();
  return true;
}

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
  if (!OpenClipboard(g_win->hwnd)) return {};
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
static void StopReconnect(Pane* p);

static void Paste(Pane* p) {
  if (!OpenClipboard(g_win->hwnd)) return;
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
  StopReconnect(p);  // pasting is typing: the user takes over
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
    InvalidateRect(g_win->hwnd, nullptr, FALSE);
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
  p->noteQuiet = false;
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
  g_win->toast = t;
  SetTimer(g_win->hwnd, TIMER_TOAST, ms, nullptr);
  InvalidateRect(g_win->hwnd, nullptr, FALSE);
}

// Como Warp: Ctrl +/-/0 escala toda la interfaz; Ctrl+Shift +/- solo el tamaño de letra.
static void SetZoom(int z) {
  g_win->zoom = std::clamp(z, 50, 300);
  MakeFonts();
  Relayout();
  Toast(Tf(S_ZOOM, g_win->zoom));
  SaveSession();
}

static void SetFontSize(int px) {
  g_win->fontPx = std::clamp(px, 6, 48);
  MakeFonts();
  Relayout();
  Toast(Tf(S_FONT, g_win->fontPx));
  SaveSession();
}

static std::wstring ShortTitle(Pane* p);

// Renombrar pestaña (doble clic): Enter guarda, Esc cancela; vacío vuelve al título de la shell.
static void BeginRename(int i) {
  if (i < 0 || i >= (int)g_win->tabs.size()) return;
  g_win->renameTab = i;
  g_win->renameText = g_win->tabs[i].name.empty() ? ShortTitle(g_win->tabs[i].active) : g_win->tabs[i].name;
  g_win->renameAll = true;
  InvalidateRect(g_win->hwnd, nullptr, FALSE);
}

static void EndRename(bool commit) {
  if (g_win->renameTab < 0) return;
  if (commit && g_win->renameTab < (int)g_win->tabs.size()) {
    std::wstring t = g_win->renameText;
    size_t a = t.find_first_not_of(L" \t"), b = t.find_last_not_of(L" \t");
    g_win->tabs[g_win->renameTab].name = a == std::wstring::npos ? L"" : t.substr(a, b - a + 1);
    SaveSession();
  }
  g_win->renameTab = -1;
  InvalidateRect(g_win->hwnd, nullptr, FALSE);
}

// Teclas durante el renombrado; true si se consume.
static bool RenameKey(WPARAM vk) {
  if (vk == VK_RETURN) EndRename(true);
  else if (vk == VK_ESCAPE) EndRename(false);
  else if (vk == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
    g_win->renameAll = true;
    InvalidateRect(g_win->hwnd, nullptr, FALSE);
  } else if (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_HOME || vk == VK_END) {
    g_win->renameAll = false;  // sin selección: se sigue escribiendo al final
    InvalidateRect(g_win->hwnd, nullptr, FALSE);
  } else if (vk == VK_BACK && g_win->renameAll) {
    g_win->renameText.clear();
    g_win->renameAll = false;
    InvalidateRect(g_win->hwnd, nullptr, FALSE);
  } else if (vk == VK_BACK) {
    if (!g_win->renameText.empty()) g_win->renameText.pop_back();
    if (!g_win->renameText.empty() && g_win->renameText.back() >= 0xD800 && g_win->renameText.back() < 0xDC00)
      g_win->renameText.pop_back();
    InvalidateRect(g_win->hwnd, nullptr, FALSE);
  } else return false;
  return true;
}

// ---------------------------------------------------------------------------
// Unread badges: a program in a pane the user isn't looking at has finished or wants an answer (NOTE_* above). That
// pane counts one more, and its header, its tab (or the hidden-tabs button) and the taskbar button show how many,
// like a chat app. Looking at it (the active pane of the active tab, with the window in front) clears it.

static const DWORD kNoteMergeMs = 1500;  // events closer than this are one (OSC 9 + BEL, title + bell)
static const wchar_t* const kNoteNames[] = {L"alert", L"bell", L"busy", L"idle"};

static int TabUnread(const Tab& t) {
  std::vector<Pane*> ls;
  Leaves(t.root, ls);
  int n = 0;
  for (Pane* p : ls) n += p->unread;
  return n;
}

static int HiddenUnread() {
  int n = 0;
  for (auto& t : g_win->tabs)
    if (t.hidden) n += TabUnread(t);
  return n;
}

static bool Foreground() { return g_win->focused && !IsIconic(g_win->hwnd); }
static bool PaneSeen(Pane* p) { return Foreground() && !g_win->tabs.empty() && CurTab().active == p; }

static UINT g_msgTaskbar;         // TaskbarButtonCreated
static ITaskbarList3* g_taskbar;  // this window's taskbar button (SetOverlay, TaskbarReady)
static HRESULT g_taskbarHr = S_FALSE, g_overlayHr = S_FALSE;  // tests (step «taskbar»)
static void SetOverlay(int total);
static void TaskbarReady();
static std::vector<uint32_t> BadgePixels(int n, int size);
static void SyncPulse();
static void NotifyWindows(Pane* p, const std::wstring& title, const std::wstring& body);
static void ToastRemove(Pane* p);

// Clears what the user is looking at and keeps the taskbar button and the attention pulse in step. Cheap: it runs
// before every message.
static void SyncUnread() {
  if (g_win->tabs.empty()) return;
  bool changed = false;
  Pane* a = CurTab().active;
  if (a && a->unread && PaneSeen(a)) {
    a->unread = 0, changed = true;
    if (a->toasted) ToastRemove(a);  // read: its Windows notification leaves the action center too
  }
  int total = 0;
  for (Pane* p : WindowPanes()) total += p->unread;
  if (total != g_win->unreadShown) g_win->unreadShown = total, SetOverlay(total), changed = true;
  if (changed) InvalidateRect(g_win->hwnd, nullptr, FALSE);
  SyncPulse();
}

static void OnNote(Pane* p, int kind) {
  if (kind == NOTE_BUSY) {
    p->noteQuiet = false;
    return;
  }
  std::string title, body;  // what an OSC notification said, for the Windows one
  if (kind == NOTE_ALERT) {
    EnterCriticalSection(&p->cs);
    title.swap(p->noteTitle), body.swap(p->noteBody);
    LeaveCriticalSection(&p->cs);
  }
  // The finish was already seen through the title or OSC 7501: the program's own bell or notification for it
  // (Claude Code rings about 60 s later) is the same news, until it works again or is typed to.
  const wchar_t* what = L"quiet";
  if (kind == NOTE_IDLE || !p->noteQuiet) {
    if (kind == NOTE_IDLE) p->noteQuiet = true;
    DWORD now = std::max<DWORD>(GetTickCount(), 1);
    bool merged = p->noteTick && now - p->noteTick < kNoteMergeMs;
    p->noteTick = now;
    what = merged ? L"merged" : PaneSeen(p) ? L"seen" : L"counted";
    if (!merged && !PaneSeen(p)) {
      p->unread = std::min(p->unread + 1, 999);
      p->attention = true;  // it pulses until clicked or typed in, also once it is in view (see «Attention»)
      // The taskbar button flashes and stays lit until the window is in front (focus isn't taken), but not over a
      // full-screen game or a presentation, where it would bring the taskbar up.
      QUERY_USER_NOTIFICATION_STATE qs;
      if (!Foreground() && SUCCEEDED(SHQueryUserNotificationState(&qs)) && qs == QUNS_ACCEPTS_NOTIFICATIONS) {
        FLASHWINFO fi{sizeof fi, g_win->hwnd, FLASHW_TRAY, 3, 0};
        FlashWindowEx(&fi);
      }
      NotifyWindows(p, ToWide(title), ToWide(body));
      SyncUnread();
    }
  }
  if (!g_win->script.empty()) g_win->noteLog += std::wstring(g_win->noteLog.empty() ? L"" : L" ") + kNoteNames[kind] + L":" + what;
}

// Attention: a pane with a counted event keeps a soft accent outline pulsing (DrawAttention, SyncPulse) after its
// badge is gone, so the user arriving from the taskbar, the tab or a Windows notification sees which terminal it was.
// Only a click in it or a key typed into it ends it; going to its tab or bringing Warp to front doesn't.
static void Attend(Pane* p) {
  if (!p || !p->attention) return;
  p->attention = false;
  InvalidateRect(g_win->hwnd, nullptr, FALSE);
}

static bool IsModifierKey(WPARAM vk) {
  return vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN || vk == VK_CAPITAL ||
         (vk >= VK_LSHIFT && vk <= VK_RMENU);
}

static void ActivateTab(int i) {
  if (i < 0 || i >= (int)g_win->tabs.size()) return;
  g_win->tabs[i].hidden = false;  // ir a una pestaña oculta la vuelve a mostrar
  g_win->activeTab = i;
  ClearSelection();
  Relayout();
}

static void OpenFind(Pane* p);
static void OpenSwitcher();

static int HiddenCount() {
  int n = 0;
  for (auto& t : g_win->tabs) n += t.hidden;
  return n;
}

// Siguiente pestaña visible en la dirección dir (las ocultas se saltan).
static int NextVisibleTab(int from, int dir) {
  int n = (int)g_win->tabs.size();
  for (int k = 1; k <= n; k++) {
    int j = ((from + dir * k) % n + n) % n;
    if (!g_win->tabs[j].hidden) return j;
  }
  return from;
}

static void HideTab(int i) {
  if ((int)g_win->tabs.size() - HiddenCount() <= 1) return;  // siempre queda una visible
  g_win->tabs[i].hidden = true;
  if (i == g_win->activeTab) ActivateTab(NextVisibleTab(i, 1));
  SaveSession();
  Relayout();
}

static void CloseTab(int i) {
  std::vector<Pane*> ls;
  Leaves(g_win->tabs[i].root, ls);
  for (Pane* p : ls) ClosePane(p);
}

// Menú de una pestaña (clic derecho): renombrar, ocultar, cerrar.
static void TabMenu(int i, POINT screen) {
  HMENU m = CreatePopupMenu();
  AppendMenuW(m, MF_STRING, 1, T(S_RENAME));
  AppendMenuW(m, MF_STRING | ((int)g_win->tabs.size() - HiddenCount() <= 1 ? MF_GRAYED : 0), 2, T(S_HIDE));
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, 3, T(S_CLOSE_TAB));
  int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, g_win->hwnd, nullptr);
  DestroyMenu(m);
  if (cmd == 1) ActivateTab(i), BeginRename(i);
  else if (cmd == 2) HideTab(i);
  else if (cmd == 3) CloseTab(i);
}

// Menú de pestañas ocultas: cada una (la muestra y va a ella) o todas.
static void HiddenMenu(POINT screen) {
  HMENU m = CreatePopupMenu();
  for (int i = 0; i < (int)g_win->tabs.size(); i++)
    if (g_win->tabs[i].hidden) {
      std::wstring t = g_win->tabs[i].name.empty() ? ShortTitle(g_win->tabs[i].active) : g_win->tabs[i].name;
      if (int n = TabUnread(g_win->tabs[i])) t += L"  (" + std::to_wstring(n) + L")";  // unread
      AppendMenuW(m, MF_STRING, 100 + i, t.c_str());
    }
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, 1, T(S_SHOW_ALL));
  int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, g_win->hwnd, nullptr);
  DestroyMenu(m);
  if (cmd == 1)
    for (auto& t : g_win->tabs) t.hidden = false;
  else if (cmd >= 100) ActivateTab(cmd - 100);
  if (cmd) SaveSession(), Relayout();
}

// ---------------------------------------------------------------------------
// Varias ventanas

// A new window's file: its id is the time, the process and a serial number (two windows can open in the same tick).
// With a single session file (tests), next to it.
static std::wstring NewWindowFile() {
  static unsigned serial = 0;
  wchar_t id[48];
  swprintf(id, 48, L"%llx%04x%02x", (unsigned long long)GetTickCount64(), (unsigned)(GetCurrentProcessId() & 0xffff), ++serial & 0xff);
  return (g_windowDir.empty() ? g_sessionFile + L"." : g_windowDir + L"\\") + L"window-" + id + L".txt";
}


static BOOL CALLBACK CountOthers(HWND h, LPARAM lp) {
  wchar_t cls[32];
  if (h != g_win->hwnd && GetClassNameW(h, cls, 32) && !wcscmp(cls, L"WarpLite")) ++*(int*)lp;
  return TRUE;
}

static int OtherWindows() {
  int n = 0;
  EnumWindows(CountOthers, (LPARAM)&n);
  return n;
}

static std::vector<HWND> SiblingWindows();
static bool RemoteCommand(const std::wstring& cmd);

// Closes every window of this Warp (Ctrl+Shift+Q, the update restart), never those of another one
// (a test's, or the real one from a test). update: they all come back at the next start. Its warp-remote
// goes too (after an update the new one starts with the windows).
static void QuitAll(bool update) {
  for (Window* w : g_windows) {
    w->keepSession = true;
    if (update) w->restartAll = true;  // update restart: this window comes back too, once
    PostMessageW(w->hwnd, WM_CLOSE, 0, 0);
  }
  RemoteCommand(L"quit");
}

// Takes a tab (or one pane of a split) to a new window where it is dropped on the screen, of this window's size and
// scale: it goes on running there as it was (the same shell and what runs in it, its scrollback, what is typed in its
// box). The new window doesn't reopen at restart.
static void Detach(int tabIdx, Pane* single, POINT screen) {
  if (tabIdx < 0 || tabIdx >= (int)g_win->tabs.size()) return;
  RECT wr;
  GetWindowRect(g_win->hwnd, &wr);
  WindowInit in;
  in.like = g_win;
  in.place = RECT{screen.x - Px(120), screen.y - Px(20), screen.x - Px(120) + (wr.right - wr.left), screen.y - Px(20) + (wr.bottom - wr.top)};
  in.script = in.childScript = g_win->childScript;
  in.adopt = TakeOut(tabIdx, single);
  if (Window* to = OpenWindow(in)) {
    InWindow iw(to);
    SaveSession();
  } else {  // no new window: it stays here, as a tab
    g_win->tabs.push_back(in.adopt);
    ActivateTab((int)g_win->tabs.size() - 1);
  }
  SaveSession();
}

static bool OutsideWindow(POINT screen) {
  RECT wr;
  GetWindowRect(g_win->hwnd, &wr);
  return !PtInRect(&wr, screen);
}

static void ToggleVerticalTabs() {
  g_win->vertTabs = !g_win->vertTabs;
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
      case VK_ADD: SetFontSize(g_win->fontPx + 1); return true;
      case VK_OEM_MINUS:
      case VK_SUBTRACT: SetFontSize(g_win->fontPx - 1); return true;
      case 'D': SplitActive(true); return true;    // dividir a la derecha
      case 'E': SplitActive(false); return true;   // dividir hacia abajo
      case 'T': NewTab(g_win->activeTab + 1); return true;
      case 'W': ClosePane(act); return true;
      case 'C':
        if (g_win->hasSel) CopySelection();
        else CopyPending();
        return true;
      case 'V': Paste(act); return true;
      case 'F': OpenFind(act); return true;  // Ctrl+Shift+F: el de Warp
      case 'K': OpenSwitcher(); return true;  // Ctrl+Shift+K: saltar a cualquier terminal
      case 'Q': QuitAll(false); return true;  // close every window (the main one and «Reopen on restart» ones come back)
      case VK_OEM_4: CyclePane(-1); InvalidateRect(g_win->hwnd, nullptr, FALSE); return true;  // Ctrl+Shift+{
      case VK_OEM_6: CyclePane(+1); InvalidateRect(g_win->hwnd, nullptr, FALSE); return true;  // Ctrl+Shift+}
      case VK_PRIOR:
      case VK_NEXT: {  // mover pestaña
        int j = g_win->activeTab + (vk == VK_PRIOR ? -1 : 1);
        if (j < 0 || j >= (int)g_win->tabs.size()) return true;
        std::swap(g_win->tabs[g_win->activeTab], g_win->tabs[j]);
        g_win->activeTab = j;
        SessionChanged();
        InvalidateRect(g_win->hwnd, nullptr, FALSE);
        return true;
      }
      case VK_TAB: ActivateTab(NextVisibleTab(g_win->activeTab, -1)); return true;
    }
  }
  if (ctrl && alt && (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN)) {
    int dx = vk == VK_LEFT ? -1 : vk == VK_RIGHT ? 1 : 0;
    int dy = vk == VK_UP ? -1 : vk == VK_DOWN ? 1 : 0;
    if (Pane* n = Neighbor(act, dx, dy)) {
      if (shift) SwapPanes(act, n);  // Ctrl+Alt+Shift+flecha: mover el panel
      else CurTab().active = n;
      ClearSelection();
      InvalidateRect(g_win->hwnd, nullptr, FALSE);
    }
    return true;
  }
  if (ctrl && !shift && !alt) {
    switch (vk) {
      case VK_PRIOR: ActivateTab(NextVisibleTab(g_win->activeTab, -1)); return true;
      case VK_NEXT:
      case VK_TAB: ActivateTab(NextVisibleTab(g_win->activeTab, 1)); return true;
      case VK_OEM_PLUS:
      case VK_ADD: SetZoom(g_win->zoom + 10); return true;
      case VK_OEM_MINUS:
      case VK_SUBTRACT: SetZoom(g_win->zoom - 10); return true;
      case '0':
      case VK_NUMPAD0: SetZoom(100); return true;
      case 'C':
        if (g_win->hasSel) {
          CopySelection();
          ClearSelection();
          InvalidateRect(g_win->hwnd, nullptr, FALSE);
          return true;
        }
        return CopyPending();
      case 'V': Paste(act); return true;
      case 'F': OpenFind(act); return true;
    }
    if (vk >= '1' && vk <= '9') {  // entre las visibles
      std::vector<int> vis;
      for (int i = 0; i < (int)g_win->tabs.size(); i++)
        if (!g_win->tabs[i].hidden) vis.push_back(i);
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
    InvalidateRect(g_win->hwnd, nullptr, FALSE);
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
  Pane* p = g_win->find.p;
  g_win->find.matches.clear();
  g_win->find.cur = -1;
  if (!p || !g_live.count(p) || g_win->find.query.empty()) return;
  std::wstring q = g_win->find.query;
  for (auto& c : q) c = towlower(c);
  EnterCriticalSection(&p->cs);
  int total = (int)p->sb.size() + p->rows;
  for (int l = 0; l < total; l++) {
    std::wstring t = AbsLineText(p, l);
    for (auto& c : t) c = towlower(c);
    for (size_t at = 0; (at = t.find(q, at)) != std::wstring::npos; at += q.size())
      g_win->find.matches.push_back({l, (int)at, (int)q.size()});
  }
  LeaveCriticalSection(&p->cs);
  if (!g_win->find.matches.empty()) g_win->find.cur = (int)g_win->find.matches.size() - 1;  // la más reciente
}

// Desplaza la vista para que se vea la coincidencia actual.
static void FindReveal() {
  Pane* p = g_win->find.p;
  if (!p || g_win->find.cur < 0) return;
  EnterCriticalSection(&p->cs);
  int sbn = (int)p->sb.size();
  int line = g_win->find.matches[g_win->find.cur].line;
  int viewRow = line - sbn + p->scrollOff;
  if (viewRow < 0 || viewRow >= p->rows) p->scrollOff = std::clamp(sbn - line + p->rows / 2, 0, sbn);
  LeaveCriticalSection(&p->cs);
}

// 0 = nada, 1 = coincidencia, 2 = coincidencia actual. Con el lock tomado.
static int FindMark(Pane* p, int row, int col) {
  if (!g_win->find.open || g_win->find.p != p || g_win->find.matches.empty()) return 0;
  int line = row - p->scrollOff + (int)p->sb.size();
  // Coincidencias ordenadas por línea: búsqueda binaria de la primera de esa línea.
  auto it = std::lower_bound(g_win->find.matches.begin(), g_win->find.matches.end(), line,
                             [](const FindMatch& m, int l) { return m.line < l; });
  for (; it != g_win->find.matches.end() && it->line == line; ++it)
    if (col >= it->col && col < it->col + it->len) return (it - g_win->find.matches.begin()) == g_win->find.cur ? 2 : 1;
  return 0;
}

static void OpenFind(Pane* p) {
  g_win->find.open = true;
  g_win->find.p = p;
  if (g_win->hasSel && g_win->selPane == p) {  // con texto seleccionado, se busca eso
    std::wstring sel = SelectionText();
    if (!sel.empty() && sel.find(L'\n') == std::wstring::npos) g_win->find.query = sel;
  }
  FindRecompute();
  FindReveal();
  InvalidateRect(g_win->hwnd, nullptr, FALSE);
}

static void CloseFind() {
  g_win->find.open = false;
  g_win->find.matches.clear();
  g_win->find.cur = -1;
  InvalidateRect(g_win->hwnd, nullptr, FALSE);
}

static void FindStep(int dir) {
  if (g_win->find.matches.empty()) return;
  int n = (int)g_win->find.matches.size();
  g_win->find.cur = ((g_win->find.cur < 0 ? 0 : g_win->find.cur) + dir + n) % n;
  FindReveal();
  InvalidateRect(g_win->hwnd, nullptr, FALSE);
}

static bool FindKey(WPARAM vk) {
  bool shift = GetKeyState(VK_SHIFT) & 0x8000;
  if (vk == VK_ESCAPE) CloseFind();
  else if (vk == VK_RETURN || vk == VK_F3) FindStep(shift ? -1 : 1);
  else if (vk == VK_UP) FindStep(-1);
  else if (vk == VK_DOWN) FindStep(1);
  else if (vk == VK_BACK) {
    if (!g_win->find.query.empty()) g_win->find.query.pop_back();
    FindRecompute();
    FindReveal();
    InvalidateRect(g_win->hwnd, nullptr, FALSE);
  } else if (vk == 'F' && (GetKeyState(VK_CONTROL) & 0x8000)) CloseFind();
  else return false;
  return true;
}

static void FindChar(wchar_t c) {
  if (c < 0x20 || g_win->find.query.size() >= 200) return;
  g_win->find.query += c;
  FindRecompute();
  FindReveal();
  InvalidateRect(g_win->hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Selector de carpetas (clic en el apartado de la ruta)

static void PickFilter() {
  std::wstring f = g_win->pick.filter;
  for (auto& c : f) c = towlower(c);
  g_win->pick.shown.clear();
  for (auto& e : g_win->pick.all) {
    std::wstring n = e.name;
    for (auto& c : n) c = towlower(c);
    if (e.parent ? f.empty() : n.find(f) != std::wstring::npos) g_win->pick.shown.push_back(e);
  }
  g_win->pick.sel = 0;
  g_win->pick.top = 0;
}

static void OpenPicker(Pane* p) {
  if (p->cwd.empty()) return;
  g_win->pick = DirPicker{};
  g_win->pick.open = true;
  g_win->pick.p = p;
  g_win->pick.base = p->cwd;
  bool root = p->cwd.size() <= 3;  // C:\ (o C:)
  if (!root) g_win->pick.all.push_back({L"", true, true});
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
  g_win->pick.all.insert(g_win->pick.all.end(), dirs.begin(), dirs.end());
  g_win->pick.all.insert(g_win->pick.all.end(), files.begin(), files.end());
  PickFilter();
  InvalidateRect(g_win->hwnd, nullptr, FALSE);
}

static void ClosePicker() {
  g_win->pick.open = false;
  InvalidateRect(g_win->hwnd, nullptr, FALSE);
}

static bool EditorActive(Pane* p);
static void EditorInsert(Pane* p, const std::wstring& t);

// Carpeta: la shell cambia a ella (sin tocar lo que hubiera escrito en la caja).
// Archivo: su ruta entre comillas se escribe en la caja.
static void PickChoose(int i) {
  if (i < 0 || i >= (int)g_win->pick.shown.size()) return;
  DirEntry e = g_win->pick.shown[i];
  Pane* p = g_win->pick.p;
  ClosePicker();
  if (!g_live.count(p) || !EditorActive(p)) return;
  std::wstring base = g_win->pick.base;
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
    InvalidateRect(g_win->hwnd, nullptr, FALSE);
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
  int n = (int)g_win->pick.shown.size();
  switch (vk) {
    case VK_ESCAPE: ClosePicker(); return true;
    case VK_RETURN: PickChoose(g_win->pick.sel); return true;
    case VK_UP: g_win->pick.sel = std::max(0, g_win->pick.sel - 1); break;
    case VK_DOWN: g_win->pick.sel = std::min(n - 1, g_win->pick.sel + 1); break;
    case VK_PRIOR: g_win->pick.sel = std::max(0, g_win->pick.sel - kPickRows); break;
    case VK_NEXT: g_win->pick.sel = std::min(n - 1, g_win->pick.sel + kPickRows); break;
    case VK_BACK:
      if (!g_win->pick.filter.empty()) g_win->pick.filter.pop_back();
      PickFilter();
      break;
    default: return false;
  }
  if (g_win->pick.sel < g_win->pick.top) g_win->pick.top = g_win->pick.sel;
  if (g_win->pick.sel >= g_win->pick.top + kPickRows) g_win->pick.top = g_win->pick.sel - kPickRows + 1;
  InvalidateRect(g_win->hwnd, nullptr, FALSE);
  return true;
}

static void PickChar(wchar_t c) {
  if (c < 0x20) return;
  g_win->pick.filter += c;
  PickFilter();
  InvalidateRect(g_win->hwnd, nullptr, FALSE);
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
        if ((!g_win || h != g_win->hwnd) && GetClassNameW(h, cls, 32) && !wcscmp(cls, L"WarpLite") &&
            GetPropW(h, kInstanceProp) == c->key)
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
  NewTab(g_win->tabs.empty() ? -1 : g_win->activeTab + 1, dir);
  SaveSession();
  BringToFront(g_win->hwnd);
  InvalidateRect(g_win->hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Selector de terminales (Ctrl+Shift+K): todas las de todas las ventanas, con su conversación de
// Claude, su carpeta y lo que tienen en marcha; se escribe para filtrar y Enter salta a ella.


// A pane's name: its Claude Code conversation, its tab's name when alone in it, or its title.
static std::wstring PaneLabel(Pane* p) {
  int ti = TabIndexOf(p);
  std::vector<Pane*> ls;
  if (ti >= 0) Leaves(g_win->tabs[ti].root, ls);
  return !p->agentName.empty() ? p->agentName : ls.size() == 1 && !g_win->tabs[ti].name.empty() ? g_win->tabs[ti].name : ShortTitle(p);
}

// Una línea por panel: pestaña, panel, título, carpeta y orden en marcha, separados por tabuladores.
static std::wstring PaneListText() {
  auto clean = [](std::wstring t) {
    for (wchar_t& c : t)
      if (c == L'\t' || c == L'\n' || c == L'\r') c = L' ';
    return t;
  };
  std::wstring out;
  for (int i = 0; i < (int)g_win->tabs.size(); i++) {
    std::vector<Pane*> ls;
    Leaves(g_win->tabs[i].root, ls);
    for (int j = 0; j < (int)ls.size(); j++) {
      Pane* p = ls[j];
      // Then the tab's name, the agent (Claude Code / Codex), the pane's id and its window, for warp-remote.
      std::wstring tab = g_win->tabs[i].name.empty() ? ShortTitle(g_win->tabs[i].active) : g_win->tabs[i].name;
      const wchar_t* agent = p->agent == AGENT_CLAUDE ? L"claude" : p->agent == AGENT_CODEX ? L"codex" : L"";
      out += std::to_wstring(i) + L"\t" + std::to_wstring(j) + L"\t" + clean(PaneLabel(p)) + L"\t" + clean(p->cwd) + L"\t" +
             clean(p->runningCmd) + L"\t" + clean(tab) + L"\t" + agent + L"\t" + std::to_wstring(p->id) + L"\t" +
             std::to_wstring((unsigned long long)(ULONG_PTR)g_win->hwnd) + L"\n";
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
    if (f.size() >= 5) g_win->sw.all.push_back({from, _wtoi(f[0].c_str()), _wtoi(f[1].c_str()), f[2], f[3], f[4]});
    a = e + 1;
  }
}

static void SwFilter() {
  std::wstring f = Lower(g_win->sw.filter);
  g_win->sw.shown.clear();
  for (auto& it : g_win->sw.all)
    if (f.empty() || Lower(it.title + L" " + it.cwd + L" " + it.running).find(f) != std::wstring::npos) g_win->sw.shown.push_back(it);
  g_win->sw.sel = 0;
  g_win->sw.top = 0;
  // Sin filtro, la seleccionada es la terminal en la que se está.
  if (f.empty() && !g_win->tabs.empty()) {
    std::vector<Pane*> ls;
    Leaves(CurTab().root, ls);
    int pj = (int)(std::find(ls.begin(), ls.end(), CurTab().active) - ls.begin());
    for (int i = 0; i < (int)g_win->sw.shown.size(); i++)
      if (g_win->sw.shown[i].hwnd == g_win->hwnd && g_win->sw.shown[i].tab == g_win->activeTab && g_win->sw.shown[i].pane == pj) g_win->sw.sel = i;
  }
}

static void OpenSwitcher() {
  g_win->sw = Switcher{};
  g_win->sw.open = true;
  AddPaneList(g_win->hwnd, PaneListText());
  for (Window* w : g_windows)  // then the other windows of this Warp
    if (w != g_win) {
      std::wstring t;
      {
        InWindow in(w);
        t = PaneListText();
      }
      AddPaneList(w->hwnd, t);
    }
  SwFilter();
  if (g_win->sw.sel >= kPickRows) g_win->sw.top = g_win->sw.sel - kPickRows + 1;
  InvalidateRect(g_win->hwnd, nullptr, FALSE);
}

static void CloseSwitcher() {
  g_win->sw.open = false;
  InvalidateRect(g_win->hwnd, nullptr, FALSE);
}

static void FocusPane(int tab, int pane) {
  if (tab < 0 || tab >= (int)g_win->tabs.size()) return;
  ActivateTab(tab);
  std::vector<Pane*> ls;
  Leaves(g_win->tabs[tab].root, ls);
  if (pane >= 0 && pane < (int)ls.size()) g_win->tabs[tab].active = ls[pane];
  InvalidateRect(g_win->hwnd, nullptr, FALSE);
}

static void SwChoose(int i) {
  if (i < 0 || i >= (int)g_win->sw.shown.size()) return;
  SwitchItem it = g_win->sw.shown[i];
  CloseSwitcher();
  if (it.hwnd == g_win->hwnd) {
    FocusPane(it.tab, it.pane);
  } else if (Window* w = LiveWindow(it.hwnd)) {
    InWindow in(w);
    FocusPane(it.tab, it.pane);
    BringToFront(w->hwnd);
  }
}

static bool SwKey(WPARAM vk) {
  int n = (int)g_win->sw.shown.size();
  switch (vk) {
    case VK_ESCAPE: CloseSwitcher(); return true;
    case VK_RETURN: SwChoose(g_win->sw.sel); return true;
    case VK_UP: g_win->sw.sel = std::max(0, g_win->sw.sel - 1); break;
    case VK_DOWN: g_win->sw.sel = std::min(n - 1, g_win->sw.sel + 1); break;
    case VK_PRIOR: g_win->sw.sel = std::max(0, g_win->sw.sel - kPickRows); break;
    case VK_NEXT: g_win->sw.sel = std::min(n - 1, g_win->sw.sel + kPickRows); break;
    case VK_BACK:
      if (!g_win->sw.filter.empty()) g_win->sw.filter.pop_back();
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
  g_win->sw.sel = std::max(0, g_win->sw.sel);
  if (g_win->sw.sel < g_win->sw.top) g_win->sw.top = g_win->sw.sel;
  if (g_win->sw.sel >= g_win->sw.top + kPickRows) g_win->sw.top = g_win->sw.sel - kPickRows + 1;
  InvalidateRect(g_win->hwnd, nullptr, FALSE);
  return true;
}

static void SwChar(wchar_t c) {
  if (c < 0x20) return;
  g_win->sw.filter += c;
  SwFilter();
  InvalidateRect(g_win->hwnd, nullptr, FALSE);
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
  p->recon = Reconnect{};  // a new run (it also stops a reconnect loop: the user typed something)
  p->recon.cmd = u;
  p->noteQuiet = false;
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
  if (p->win) InvalidateRect(p->win->hwnd, nullptr, FALSE);
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
// Reconnect: when the home internet blips, ssh dies with «client_loop: send disconnect: Connection
// reset» (or «closed by remote host», «Broken pipe»…) and exit code 255, while tmux lives on in the
// server. Warp launches the same command again, 2, 4, 8, 16 and then every 30 s, until it stays
// connected. Only ssh to a login shell (or a -N tunnel) and to tmux/screen commands that reattach: any
// other remote command could repeat what it already did. Never on a normal exit (exit, Ctrl+D, tmux
// detach: code 0), on «~.», or on errors that retrying won't fix (permission denied, host key). Esc or
// typing in the pane stops it; the state lives in the pane, so closing it or quitting leaves nothing.

static void ScriptLog(const std::wstring& r);

// Is that command ssh to a login shell or tunnel, or to tmux or screen reattaching a session?
static bool Reconnectable(const std::string& cmd) {
  if (ProgramName(cmd) != "ssh") return false;
  std::vector<std::string> a = SplitArgs(cmd);
  bool dest = false, opts = true;
  std::string remote;
  for (size_t i = !a.empty() && a[0] == "&" ? 2 : 1; i < a.size(); i++) {
    const std::string& t = a[i];
    if (opts && t == "--") {
      opts = false;
    } else if (opts && t.size() > 1 && t[0] == '-') {  // -t, -tt, -p 22, -p22, -o Opt=x… (also after the host)
      for (size_t k = 1; k < t.size(); k++)
        if (strchr("BbcDEeFIiJLlmOoPpRSWw", t[k])) {  // takes a value: the rest of the word or the next one
          if (k + 1 == t.size()) i++;
          break;
        }
    } else if (!dest) {
      dest = true;
    } else {
      for (; i < a.size(); i++) remote += a[i] + " ";
    }
  }
  if (!dest) return false;
  for (char& c : remote)
    if (c == '"' || c == '\'') c = ' ';
  std::vector<std::string> w = SplitArgs(remote);
  if (w.empty()) return true;
  std::string prog = w[0].substr(w[0].find_last_of('/') + 1);
  if (prog == "tmux") {
    size_t k = 1;
    while (k < w.size() && w[k][0] == '-') k += w[k] == "-L" || w[k] == "-S" || w[k] == "-f" || w[k] == "-c" || w[k] == "-T" ? 2 : 1;
    if (k >= w.size()) return false;  // bare tmux: a new session every time
    const std::string& sub = w[k];
    if (sub == "a" || sub == "at" || sub == "att" || sub == "attach" || sub == "attach-session") return true;
    if (sub != "new" && sub != "new-session") return false;
    for (size_t j = k + 1; j < w.size(); j++) {  // new -A (also -dA…): attach if the session exists
      size_t at = w[j].find('A');
      if (w[j][0] == '-' && at != std::string::npos && w[j].find_first_not_of("dDEPX", 1) == at) return true;
    }
    return false;
  }
  if (prog == "screen")  // -r, -R, -x, -dR, -DR, -RR…
    for (size_t j = 1; j < w.size(); j++)
      if (w[j].size() > 1 && w[j][0] == '-' && w[j].find_first_of("rRx", 1) != std::string::npos) return true;
  return false;
}

enum { SSH_OTHER, SSH_FATAL, SSH_LOST, SSH_UNREACHABLE };
static const wchar_t* const kSshKind[] = {L"other", L"fatal", L"lost", L"unreachable"};

// What ssh's last lines say: an error that retrying won't fix, an established connection that dropped,
// or a connection that couldn't be made (network or server down).
static int SshFailure(const std::wstring& text) {
  std::wstring t = Lower(text);
  auto has = [&](std::initializer_list<const wchar_t*> l) {
    return std::any_of(l.begin(), l.end(), [&](const wchar_t* s) { return t.find(s) != std::wstring::npos; });
  };
  if (has({L"permission denied", L"host key verification failed", L"remote host identification has changed",
           L"too many authentication failures", L"authentication failed", L"unable to negotiate",
           L"bad configuration option", L"usage: ssh"}))
    return SSH_FATAL;
  // Before the «lost» ones: «ssh: connect to host fr port 22: Connection timed out».
  if (has({L"connect to host", L"could not resolve hostname", L"kex_exchange_identification", L"banner exchange",
           L"connection closed by"}))
    return SSH_UNREACHABLE;
  if (has({L"client_loop:", L"closed by remote host", L"read from remote host", L"packet_write_wait", L"write failed",
           L"broken pipe", L"connection reset", L"connection timed out", L"connection abort", L"not responding"}))
    return SSH_LOST;
  return SSH_OTHER;
}

// ssh's last lines: up to three rows above the prompt that came back, never past the prompt row where
// the command was typed (with the lock taken).
static std::wstring SshTail(Pane* p) {
  int cur = (int)p->sb.size() + p->cursor.row;
  std::wstring t;
  for (int line = cur; line >= 0 && line > cur - 4; line--) {
    std::wstring row = AbsLineText(p, line);
    if (row.rfind(L"> ", 0) == 0) {
      if (line == cur) continue;
      break;
    }
    t = row + t;
  }
  return t;
}

// Pruebas (--script): each step of the loop goes to <sesión>.bench.
static void ReconnectLog(const std::wstring& s) {
  if (!g_win->script.empty()) ScriptLog(L"reconnect: " + s);
}

static DWORD WINAPI NetWatchThread(LPVOID) {
  typedef DWORD(WINAPI * PFN_NotifyAddrChange)(PHANDLE, LPOVERLAPPED);
  HMODULE m = LoadLibraryExW(L"iphlpapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  auto notify = m ? (PFN_NotifyAddrChange)GetProcAddress(m, "NotifyAddrChange") : nullptr;
  while (notify && notify(nullptr, nullptr) == NO_ERROR) {  // blocks until an address changes
    PostMessage(g_host, WM_APP_NETCHANGE, 0, 0);
    Sleep(1000);
  }
  return 0;
}

// Network changes (Wi-Fi back, a new address): panes waiting to reconnect try soon instead of waiting
// out their backoff. Watched from the first reconnect on.
static void StartNetWatch() {
  static bool started = false;
  if (started) return;
  started = true;
  if (HANDLE t = CreateThread(nullptr, 0, NetWatchThread, nullptr, 0, nullptr)) CloseHandle(t);
}

static void OnNetworkChange() {
  static DWORD last = 0;
  DWORD now = GetTickCount();
  if (last && now - last < 10000) return;  // a flapping network doesn't make them hammer
  int k = 0;
  for (Pane* p : g_live) {
    Reconnect& r = p->recon;
    DWORD due = now + 1500 + 700 * k;  // a moment for DHCP and DNS, and not all panes at once
    if (r.state != RC_WAITING || (int)(r.due - due) <= 0) continue;
    r.due = due, k++, last = now;
    InWindow in(p->win);
    ReconnectLog(L"network changed -> attempt " + std::to_wstring(r.attempt) + L" sooner");
  }
}

// Esc or typing in the pane: the loop stops (a later drop starts a new one).
static void StopReconnect(Pane* p) {
  Reconnect& r = p->recon;
  if (r.state == RC_OFF) return;
  r.state = RC_OFF;
  r.attempt = 0;
  r.byWarp = false;  // an attempt still connecting that fails doesn't start the loop again
  ReconnectLog(L"cancelled");
  InvalidateRect(g_win->hwnd, nullptr, FALSE);
}

// The prompt is back after a command sent from the box (code: its exit code). For an ssh that can
// reconnect, what happened is decided a moment later, when its last lines are surely on the screen.
static void CommandEnded(Pane* p, int code) {
  Reconnect& r = p->recon;
  if (!Reconnectable(r.cmd)) return;
  r.ended = true;
  r.exit = code;
  r.endedAt = GetTickCount();
}

static void ScheduleReconnect(Pane* p, const std::wstring& why) {
  Reconnect& r = p->recon;
  r.attempt++;
  DWORD wait = std::min<DWORD>(30000, 1000u << std::min(r.attempt, 5));  // 2, 4, 8, 16, 30, 30… s
  r.state = RC_WAITING;
  r.due = r.endedAt + wait + (DWORD)(((uintptr_t)p >> 4) ^ GetTickCount()) % (wait / 10);  // + up to 10 %: panes apart
  StartNetWatch();
  ReconnectLog(why + L" -> attempt " + std::to_wstring(r.attempt) + L" in " + std::to_wstring(wait / 1000) + L"s");
}

static void ClassifyEnd(Pane* p) {
  Reconnect& r = p->recon;
  r.ended = false;
  EnterCriticalSection(&p->cs);
  std::wstring tail = SshTail(p);
  LeaveCriticalSection(&p->cs);
  // 255 is ssh's own error; any other code is the remote command's (0: exit, Ctrl+D, tmux detach).
  int kind = r.exit != kNoExit && r.exit != 255 ? SSH_OTHER : SshFailure(tail);
  bool trying = r.state == RC_TRYING;
  // Couldn't connect at all: again only inside the loop or for Warp's own relaunch (Warp started
  // before the network), so that a mistyped host isn't retried forever.
  bool again = kind == SSH_LOST || (kind == SSH_UNREACHABLE && (trying || r.byWarp));
  std::wstring why = L"exit " + (r.exit == kNoExit ? std::wstring(L"?") : std::to_wstring(r.exit)) + L" " + kSshKind[kind];
  if (again) return ScheduleReconnect(p, why);
  r.state = RC_OFF;
  r.attempt = 0;
  ReconnectLog(why + (trying ? L" -> stop" : L" -> no"));
}

// An attempt: the command goes to the shell as if typed in the box and Enter pressed.
static void LaunchReconnect(Pane* p) {
  Reconnect r = p->recon;
  p->edit = ToWide(r.cmd);
  p->caret = p->edit.size();
  SubmitEditor(p);  // a new run: p->recon starts over
  p->recon.byWarp = true;
  p->recon.state = RC_TRYING;
  p->recon.attempt = r.attempt;
  p->recon.launched = GetTickCount();
  ReconnectLog(L"attempt " + std::to_wstring(r.attempt) + L" launched");
}

static std::wstring ReconnectLabel(Pane* p) {
  const Reconnect& r = p->recon;
  wchar_t b[160] = L"";
  if (r.state == RC_WAITING)
    swprintf(b, 160, T(S_RECONNECT_WAIT), (std::max(0, (int)(r.due - GetTickCount())) + 999) / 1000, r.attempt);
  else if (r.state == RC_TRYING)
    swprintf(b, 160, T(S_RECONNECTING), r.attempt);
  return b;
}

// For the tests: "waiting N" / "trying N" (attempt N), "idle" at the prompt, "running" otherwise.
static std::wstring ReconnectState(Pane* p) {
  const Reconnect& r = p->recon;
  if (r.state != RC_OFF) return (r.state == RC_WAITING ? L"waiting " : L"trying ") + std::to_wstring(r.attempt);
  return p->atPrompt && !r.ended ? L"idle" : L"running";
}

// Every 250 ms: decide on the commands that ended, launch the attempts that are due and notice the
// ones that connected (still running, and talking to us for a few seconds).
static void ReconnectTick() {
  DWORD now = GetTickCount();
  for (Pane* p : std::vector<Pane*>(g_live.begin(), g_live.end())) {
    if (!g_live.count(p) || !p->win) continue;  // a pane closed by the one before (a reconnect sends a command)
    InWindow in(p->win);
    Reconnect& r = p->recon;
    if (r.ended && now - r.endedAt >= 400) ClassifyEnd(p);
    if (r.state == RC_WAITING && (int)(now - r.due) >= 0) {
      if (!p->edit.empty()) StopReconnect(p);  // something is written in the box: the user took over
      else if (p->atPrompt) LaunchReconnect(p);
    } else if (r.state == RC_TRYING && !p->atPrompt) {
      if (!r.outSeen && (int)(p->lastOut - r.launched) > 1500) r.outSeen = now;  // not the echo of the command
      if ((r.outSeen && now - r.outSeen >= 2500) || now - r.launched >= 45000) {
        r.state = RC_OFF;
        r.attempt = 0;
        ReconnectLog(L"connected");
      }
    }
    std::wstring label = ReconnectLabel(p);
    if (label != r.label) r.label = label, InvalidateRect(g_win->hwnd, nullptr, FALSE);
  }
}

// ---------------------------------------------------------------------------
// Autocompletado con Tab, como el menú de Warp: órdenes en la primera palabra, parámetros tras
// un «-» y rutas en el resto. Una sola opción se escribe directamente; varias abren el menú.

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
  if (!PostMessage(g_host, WM_APP_PARAMS, 0, (LPARAM)reply)) delete reply;
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
  g_win->comp.start = start;
  g_win->comp.items.clear();
  g_win->comp.loading.clear();
  if (!cmdPos && !tok.empty() && tok[0] == L'-') {  // parámetro de la orden del tramo
    size_t ce = e.find_first_of(L" \t", first);
    std::wstring cmd = e.substr(first, ce - first);
    if (cmd.empty() || cmd.size() > 100 ||
        cmd.find_first_not_of(L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.") != std::wstring::npos)
      return;
    auto it = g_params.find(Lower(cmd));
    if (it == g_params.end()) {
      RequestParams(cmd);
      g_win->comp.loading = Lower(cmd);
      return;
    }
    for (auto& n : it->second)
      if (StartsWithI(n, tok)) g_win->comp.items.push_back({n, n, COMP_PARAM});
    return;
  }
  if (cmdPos && !tok.empty() && tok.find_first_of(L"\\/:'\"~") == std::wstring::npos) {
    for (auto& n : p->cmdNames)
      if (StartsWithI(n, tok) && g_win->comp.items.size() < 500) g_win->comp.items.push_back({n, n, COMP_CMD});
    std::sort(g_win->comp.items.begin(), g_win->comp.items.end(),
              [](const CompItem& a, const CompItem& b) { return _wcsicmp(a.label.c_str(), b.label.c_str()) < 0; });
  }
  PathCandidates(p, tok, g_win->comp.items);
}

static void CompClose() {
  if (!g_win->comp.open) return;
  g_win->comp.open = false;
  g_win->comp.autoAccept = false;
  InvalidateRect(g_win->hwnd, nullptr, FALSE);
}

static void CompAccept(int i) {
  Pane* p = g_win->comp.p;
  if (i < 0 || i >= (int)g_win->comp.items.size() || !g_live.count(p)) return CompClose();
  const CompItem& it = g_win->comp.items[i];
  size_t c = std::min(p->caret, p->edit.size()), s = std::min(g_win->comp.start, c);
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
  Pane* p = g_win->comp.p;
  if (!g_win->comp.open || !g_live.count(p) || !EditorActive(p)) return CompClose();
  CompCompute(p);
  g_win->comp.sel = g_win->comp.top = 0;
  if (g_win->comp.autoAccept && g_win->comp.loading.empty() && g_win->comp.items.size() == 1) return CompAccept(0);
  if (g_win->comp.items.empty() && g_win->comp.loading.empty()) return CompClose();
  InvalidateRect(g_win->hwnd, nullptr, FALSE);
}

static void CompStart(Pane* p) {
  LoadShellCommands(p);
  g_win->comp = CompState{};
  g_win->comp.p = p;
  g_win->comp.open = true;
  g_win->comp.autoAccept = true;
  CompRefresh();
}

static bool CompKey(WPARAM vk) {
  int n = (int)g_win->comp.items.size();
  bool shift = GetKeyState(VK_SHIFT) & 0x8000;
  switch (vk) {
    case VK_ESCAPE: CompClose(); return true;
    case VK_RETURN:
      if (n) CompAccept(g_win->comp.sel);
      else CompClose();
      return true;
    case VK_TAB:
      if (n) g_win->comp.sel = (g_win->comp.sel + (shift ? n - 1 : 1)) % n;
      break;
    case VK_UP: g_win->comp.sel = std::max(0, g_win->comp.sel - 1); break;
    case VK_DOWN: g_win->comp.sel = std::min(n - 1, g_win->comp.sel + 1); break;
    case VK_PRIOR: g_win->comp.sel = std::max(0, g_win->comp.sel - kCompRows); break;
    case VK_NEXT: g_win->comp.sel = std::min(n - 1, g_win->comp.sel + kCompRows); break;
    default: g_win->comp.autoAccept = false; return false;  // al editor; luego se vuelve a filtrar
  }
  g_win->comp.autoAccept = false;
  g_win->comp.sel = std::max(0, g_win->comp.sel);
  if (g_win->comp.sel < g_win->comp.top) g_win->comp.top = g_win->comp.sel;
  if (g_win->comp.sel >= g_win->comp.top + kCompRows) g_win->comp.top = g_win->comp.sel - kCompRows + 1;
  InvalidateRect(g_win->hwnd, nullptr, FALSE);
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
  if (g_win->comp.open && g_win->comp.p == p) CompRefresh();
  InvalidateRect(g_win->hwnd, nullptr, FALSE);
  return true;
}

// Devuelve true si la tecla se ha consumido (no se traduce a WM_CHAR).
static bool HandleKeyDown(WPARAM vk, LPARAM) {
  if (g_win->tabs.empty()) return false;
  // Any other key drops a pending tmux copy, so a later Ctrl+C still interrupts instead of copying.
  if (vk != VK_SHIFT && vk != VK_CONTROL && vk != VK_MENU && !(vk == 'C' && (GetKeyState(VK_CONTROL) & 0x8000)))
    g_clipPending.clear();
  if (HandleShortcut(vk)) return true;
  Pane* p = CurTab().active;
  if (!IsModifierKey(vk)) Attend(p);  // a key for the pane (not one of Warp's shortcuts): the user is at it
  // Any key in a pane that is reconnecting stops it (bare modifiers don't); Esc while waiting does
  // only that.
  if (p->recon.state != RC_OFF && vk != VK_SHIFT && vk != VK_CONTROL && vk != VK_MENU && vk != VK_LWIN &&
      vk != VK_RWIN && vk != VK_CAPITAL) {
    bool waiting = p->recon.state == RC_WAITING;
    StopReconnect(p);
    if (vk == VK_ESCAPE && waiting) return true;
  }
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
  int col = std::clamp((x - (int)p->content.left - kPad) / g_win->cellW, 0, p->cols - 1);
  int row = std::clamp((y - (int)p->content.top - kPad) / g_win->cellH, 0, p->rows - 1);
  return VTermPos{row, col};
}

static bool InSelection(Pane* p, int row, int col) {
  if (!g_win->hasSel || g_win->selPane != p) return false;
  VTermPos a = g_win->selA, b = g_win->selB;
  if (b.row < a.row || (b.row == a.row && b.col < a.col)) std::swap(a, b);
  if (row < a.row || row > b.row) return false;
  if (row == a.row && col < a.col) return false;
  if (row == b.row && col > b.col) return false;
  return true;
}

// Claude Code draws block quotes with a bar in front of each line («▎ text»): copied text leaves it out,
// keeping the indent before it.
static void StripQuoteBar(std::wstring& line) {
  size_t i = line.find_first_not_of(L' ');
  if (i == std::wstring::npos || line[i] < L'\u258C' || line[i] > L'\u258F') return;
  line.erase(i, i + 1 < line.size() && line[i + 1] == L' ' ? 2 : 1);
}

static std::wstring SelectionText() {
  Pane* p = g_win->selPane;
  if (!g_win->hasSel || !p || !g_live.count(p)) return {};
  VTermPos a = g_win->selA, b = g_win->selB;
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
    StripQuoteBar(line);
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

// ---------------------------------------------------------------------------
// Paths as links, like URLs (Ctrl+hover underlines, Ctrl+click opens): C:\…, C:/…, \\server\share\…
// and paths relative to the pane's folder, quoted or not, with :line[:col] or (line) behind, also
// wrapped over several rows. Claude Code wraps at spaces and drops them, indenting the next row, so a
// row break may have been a space or nothing: both are tried. Only paths that exist count, which
// settles that (the longest one that exists wins) and keeps ordinary words from being links. Answers
// are cached for a few seconds; local disks are asked right away (a bounded number of calls per
// hover), removable or optical drives on a worker thread, and the link shows up when it answers.
// Shares (\\host\share) and network drives are never links: just asking about one would connect to
// that host and hand it the user's NTLM hash whenever the mouse passes over text a program printed.

static const DWORD kPathCacheMs = 3000;
static const wchar_t kBreak = 1;               // row break in the joined text: a space or nothing
static const DWORD kAttrPending = 0xFFFFFFFE;  // not known yet (slow drive, or out of budget)

struct PathRow {  // a row's text for paths: every character once (wide ones too) and its cells
  std::wstring s;
  std::vector<int> c0, c1;
};
struct TimedAttrs {
  DWORD attrs, tick;
};
struct DirList {
  DWORD tick;
  bool ok;                                        // false: too big or unreadable
  std::unordered_map<std::wstring, DWORD> names;  // lower case → attributes
};
struct PathCache {
  std::unordered_map<std::wstring, TimedAttrs> attrs;  // lower-case full path → attributes
  std::unordered_map<std::wstring, DirList> dirs;      // folder listings (for relative paths)
  std::map<wchar_t, TimedAttrs> drives;                // drive letter → GetDriveTypeW
  std::set<std::wstring> pending, dirsPending;         // handed to the worker (paths, folders to list)
  std::vector<std::wstring> queue, dirQueue;           // waiting for it: it takes one batch at a time
  bool busy = false;                                   // the worker is checking a batch
  DWORD memoTick = 0;                                  // last answer (same rows, folder and cell: reused)
  std::wstring memoKey;
  LinkHit memo;
};
static PathCache g_paths;

struct PathChecks {  // a batch for the worker: paths to look up and folders to list
  std::vector<std::wstring> paths, dirs;
  std::vector<std::pair<std::wstring, DWORD>> attrs;  // answers: those paths and the folders above them it asked about
  std::vector<DirList> lists;                         // one per folder in dirs
};

// A pass of path highlighting (see «Path highlighting»): PathAt never waits for the disk then.
struct PathScan {
  bool pending = false;     // some answers aren't known yet (asked on the worker)
  bool revalidate = false;  // the text changed: old answers are used and asked again
};

static bool IsPathSep(wchar_t c) { return c == L'\\' || c == L'/'; }

// Disk lookups for path links, and how many went to the network (must stay 0; the path-probes step
// reports both). Every lookup goes through here.
static std::atomic<int> g_pathProbes{0}, g_remoteProbes{0};

static void CountProbe(const std::wstring& full) {
  g_pathProbes++;
  wchar_t root[] = {full.empty() ? L'?' : full[0], L':', L'\\', 0};
  if (full.size() < 2 || full[1] != L':' || GetDriveTypeW(root) == DRIVE_REMOTE) g_remoteProbes++;
}

// A folder's names, lower case (ok false if it has more than 20000 or can't be read).
static void ListDir(const std::wstring& dir, DirList& l) {
  WIN32_FIND_DATAW fd;
  HANDLE f = FindFirstFileExW((dir + (IsPathSep(dir.back()) ? L"*" : L"\\*")).c_str(), FindExInfoBasic, &fd,
                              FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
  if (f == INVALID_HANDLE_VALUE) {
    l.ok = false;
    return;
  }
  do {
    if (l.names.size() >= 20000) {
      l.ok = false;
      break;
    }
    l.names[Lower(fd.cFileName)] = fd.dwFileAttributes;
  } while (FindNextFileW(f, &fd));
  FindClose(f);
}

// On the worker: a path's attributes, its folders first (every one it asks about goes back in the answer, so the
// other candidates under a missing folder cost nothing).
static DWORD WorkerAttrs(const std::wstring& full, std::map<std::wstring, DWORD>& seen) {
  auto it = seen.find(full);
  if (it != seen.end()) return it->second;
  size_t cut = full.find_last_of(L'\\');
  DWORD up = cut != std::wstring::npos && cut > 2 && cut + 1 < full.size() ? WorkerAttrs(full.substr(0, cut), seen)
                                                                           : FILE_ATTRIBUTE_DIRECTORY;
  DWORD a = INVALID_FILE_ATTRIBUTES;
  if (up != INVALID_FILE_ATTRIBUTES && (up & FILE_ATTRIBUTE_DIRECTORY)) CountProbe(full), a = GetFileAttributesW(full.c_str());
  return seen[full] = a;
}

static DWORD WINAPI PathCheckThread(LPVOID arg) {
  auto* job = (PathChecks*)arg;
  SetThreadErrorMode(SEM_FAILCRITICALERRORS, nullptr);  // no "insert a disk" box
  std::map<std::wstring, DWORD> seen;
  for (auto& f : job->paths) WorkerAttrs(f, seen);
  job->attrs.assign(seen.begin(), seen.end());
  for (auto& d : job->dirs) {
    job->lists.push_back(DirList{0, true, {}});
    CountProbe(d);
    ListDir(d, job->lists.back());
  }
  if (!PostMessage(g_host, WM_APP_PATHS, 0, (LPARAM)job)) delete job;
  return 0;
}

// Hands a path, or a folder to list, to the worker (only local drives get here, see DriveSpeed). They go in batches,
// one at a time: PathSend sends the next one when it's free, and OnPathChecks when it answers.
static void PathAsk(const std::wstring& full) {
  if (g_paths.pending.insert(Lower(full)).second) g_paths.queue.push_back(full);
}

static void PathAskDir(const std::wstring& dir) {
  if (g_paths.dirsPending.insert(Lower(dir)).second) g_paths.dirQueue.push_back(dir);
}

static void PathSend() {
  if (g_paths.busy || (g_paths.queue.empty() && g_paths.dirQueue.empty())) return;
  auto* job = new PathChecks{};
  size_t n = std::min<size_t>(g_paths.queue.size(), 256), m = std::min<size_t>(g_paths.dirQueue.size(), 4);
  job->paths.assign(g_paths.queue.begin(), g_paths.queue.begin() + n);
  job->dirs.assign(g_paths.dirQueue.begin(), g_paths.dirQueue.begin() + m);
  g_paths.queue.erase(g_paths.queue.begin(), g_paths.queue.begin() + n);
  g_paths.dirQueue.erase(g_paths.dirQueue.begin(), g_paths.dirQueue.begin() + m);
  HANDLE t = CreateThread(nullptr, 0, PathCheckThread, job, 0, nullptr);
  if (!t) {  // asked again next time
    for (auto& f : job->paths) g_paths.pending.erase(Lower(f));
    for (auto& d : job->dirs) g_paths.dirsPending.erase(Lower(d));
    delete job;
    return;
  }
  CloseHandle(t);
  g_paths.busy = true;
}

// 1: local fixed disk (answers at once); 2: removable or optical drive (may take seconds or wake a
// disc: asked on the worker); 0: no such drive, or a share or network drive (never asked, see above;
// GetDriveTypeW itself stays local).
static int DriveSpeed(const std::wstring& full) {
  if (full.size() < 2 || full[1] != L':') return 0;
  wchar_t d = towupper(full[0]);
  DWORD now = GetTickCount();
  auto it = g_paths.drives.find(d);
  if (it == g_paths.drives.end() || now - it->second.tick >= kPathCacheMs) {
    wchar_t root[] = {d, L':', L'\\', 0};
    it = g_paths.drives.insert_or_assign(d, TimedAttrs{GetDriveTypeW(root), now}).first;
  }
  UINT t = it->second.attrs;
  return t == DRIVE_FIXED || t == DRIVE_RAMDISK ? 1 : t == DRIVE_REMOVABLE || t == DRIVE_CDROM ? 2 : 0;
}

// Attributes of a full path, cached or from the disk: INVALID_FILE_ATTRIBUTES if it doesn't exist,
// kAttrPending if not known yet (slow ones go into `slow` for the worker; `budget` caps the calls to
// the disk in one hover). Its folder is checked first, so candidates that only differ after a
// missing folder cost nothing more.
static DWORD PathAttrs(const std::wstring& full, int& budget, std::vector<std::wstring>& slow) {
  std::wstring key = Lower(full);
  DWORD now = GetTickCount();
  auto it = g_paths.attrs.find(key);
  if (it != g_paths.attrs.end() && now - it->second.tick < kPathCacheMs) return it->second.attrs;
  int speed = DriveSpeed(full);
  if (!speed) return INVALID_FILE_ATTRIBUTES;
  if (speed == 2) {
    if (!g_paths.pending.count(key) && slow.size() < 16 && std::find(slow.begin(), slow.end(), full) == slow.end())
      slow.push_back(full);
    return kAttrPending;
  }
  size_t cut = full.find_last_of(L'\\');
  if (cut != std::wstring::npos && cut > 2 && cut + 1 < full.size()) {
    DWORD up = PathAttrs(full.substr(0, cut), budget, slow);
    if (up == kAttrPending) return kAttrPending;
    if (up == INVALID_FILE_ATTRIBUTES || !(up & FILE_ATTRIBUTE_DIRECTORY)) {
      g_paths.attrs[key] = TimedAttrs{INVALID_FILE_ATTRIBUTES, now};
      return INVALID_FILE_ATTRIBUTES;
    }
  }
  if (budget <= 0) return kAttrPending;
  budget--;
  CountProbe(full);
  DWORD a = GetFileAttributesW(full.c_str());
  g_paths.attrs[key] = TimedAttrs{a, now};
  return a;
}

// The same for highlighting, which never asks the disk on this thread: an answer in the cache is used even if old (and
// asked again when the text changed, so nothing flickers while it's checked), and the rest is asked on the worker
// (kAttrPending, scan.pending).
static DWORD CachedPathAttrs(const std::wstring& full, PathScan& scan) {
  std::wstring key = Lower(full);
  DWORD now = GetTickCount();
  auto it = g_paths.attrs.find(key);
  if (it != g_paths.attrs.end()) {
    if (scan.revalidate && now - it->second.tick >= kPathCacheMs && DriveSpeed(full)) PathAsk(full);
    return it->second.attrs;
  }
  if (!DriveSpeed(full)) return INVALID_FILE_ATTRIBUTES;
  size_t cut = full.find_last_of(L'\\');
  if (cut != std::wstring::npos && cut > 2 && cut + 1 < full.size()) {  // in a folder known to be missing: missing
    auto up = g_paths.attrs.find(key.substr(0, cut));
    if (up != g_paths.attrs.end() && now - up->second.tick < kPathCacheMs &&
        (up->second.attrs == INVALID_FILE_ATTRIBUTES || !(up->second.attrs & FILE_ATTRIBUTE_DIRECTORY)))
      return INVALID_FILE_ATTRIBUTES;
  }
  PathAsk(full);
  scan.pending = true;
  return kAttrPending;
}

// Keeps the cache small: old answers go first, and everything if that isn't enough (highlighting asks again).
static void TrimPathCache() {
  if (g_paths.attrs.size() > 4096) {
    DWORD now = GetTickCount();
    for (auto it = g_paths.attrs.begin(); it != g_paths.attrs.end();)
      it = now - it->second.tick >= kPathCacheMs ? g_paths.attrs.erase(it) : std::next(it);
    if (g_paths.attrs.size() > 4096) g_paths.attrs.clear();
  }
  if (g_paths.dirs.size() > 64) g_paths.dirs.clear();
}

// Names in a local folder, so relative candidates whose first part isn't there (most words of a
// sentence) are ruled out without asking the disk for each. nullptr if it can't be used.
static const DirList* DirNames(const std::wstring& dir, int& budget) {
  std::wstring key = Lower(dir);
  DWORD now = GetTickCount();
  auto it = g_paths.dirs.find(key);
  if (it != g_paths.dirs.end() && now - it->second.tick < kPathCacheMs) return it->second.ok ? &it->second : nullptr;
  if (dir.empty() || DriveSpeed(dir) != 1 || budget <= 0) return nullptr;
  budget--;
  DirList& l = g_paths.dirs[key];
  l = DirList{now, true, {}};
  CountProbe(dir);
  ListDir(dir, l);
  return l.ok ? &l : nullptr;
}

// The same for highlighting: the listing in the cache (an old one too, asked again with scan.revalidate), or nullptr:
// it can't be used, or it isn't known yet (asked on the worker: *asked).
static const DirList* CachedDirNames(const std::wstring& dir, PathScan& scan, bool& asked) {
  auto it = g_paths.dirs.find(Lower(dir));
  if (it != g_paths.dirs.end()) {
    if (scan.revalidate && GetTickCount() - it->second.tick >= kPathCacheMs && DriveSpeed(dir) == 1) PathAskDir(dir);
    return it->second.ok ? &it->second : nullptr;
  }
  if (dir.empty() || DriveSpeed(dir) != 1) return nullptr;
  PathAskDir(dir);
  asked = scan.pending = true;
  return nullptr;
}

// Length of a :line[:col] or (line[,col]) starting at s[i] (0 if there's none).
static int LineSuffixAt(const std::wstring& s, int i) {
  auto digits = [&](int k) {
    int n = 0;
    while (k + n < (int)s.size() && s[k + n] >= L'0' && s[k + n] <= L'9') n++;
    return n;
  };
  if (i >= (int)s.size() || (s[i] != L':' && s[i] != L'(')) return 0;
  int n = digits(i + 1);
  if (!n) return 0;
  int len = 1 + n;
  wchar_t sep = s[i] == L':' ? L':' : L',';
  if (i + len < (int)s.size() && s[i + len] == sep)
    if (int m = digits(i + len + 1)) len += 1 + m;
  if (s[i] == L':') return len;
  return i + len < (int)s.size() && s[i + len] == L')' ? len + 1 : 0;
}

// Start of a :line or (line[,col]) that ends at e (e if there's none), leaving something before it.
static int LineSuffixStart(const std::wstring& s, int a, int e) {
  int k = e;
  while (k > a && s[k - 1] >= L'0' && s[k - 1] <= L'9') k--;
  if (k < e && k > a + 1 && s[k - 1] == L':') return k - 1;
  if (e > a + 3 && s[e - 1] == L')') {
    size_t o = s.rfind(L'(', e - 1);
    if (o != std::wstring::npos && (int)o > a && LineSuffixAt(s, (int)o) == e - (int)o) return (int)o;
  }
  return e;
}

// Could this be a local path at all? Nothing Windows forbids in a name, a colon only after the drive
// letter (C:\ or C:/), nothing starting with \ or / (\\server\share and //server/share would reach the
// network, \\?\ and \\.\ are device paths, \foo is rooted on whatever drive: usually a Unix path over
// ssh), not ending in . or a space (Windows would drop them) and not a device name (nul, con, com1…).
static bool PathShaped(const std::wstring& s) {
  if (s.empty() || s.size() > 1024 || IsPathSep(s[0])) return false;
  for (size_t i = 0; i < s.size(); i++)
    if (s[i] < 32 || wcschr(L"\"<>|?*\u2502", s[i]) || (s[i] == L':' && i != 1)) return false;
  if (s.size() >= 2 && s[1] == L':') {
    wchar_t d = s[0] | 0x20;
    if (s.size() < 3 || !IsPathSep(s[2]) || d < L'a' || d > L'z') return false;
  }
  if (s.back() == L'.' || s.back() == L' ') return false;
  size_t end = s.size();
  while (end > 0 && IsPathSep(s[end - 1])) end--;
  size_t k = end ? s.find_last_of(L"\\/", end - 1) : std::wstring::npos;
  size_t from = k == std::wstring::npos ? 0 : k + 1;
  std::wstring base = Lower(s.substr(from, end - from));
  base = base.substr(0, base.find(L'.'));
  while (!base.empty() && base.back() == L' ') base.pop_back();
  static const wchar_t* const kDevices[] = {L"con", L"prn", L"aux", L"nul", L"conin$", L"conout$", L"clock$"};
  for (auto d : kDevices)
    if (base == d) return false;
  if (base.size() == 4 && (!base.compare(0, 3, L"com") || !base.compare(0, 3, L"lpt")) && iswdigit(base[3])) return false;
  return true;
}

// Full form of a path seen in the pane (relative ones against its folder); empty if there's none
// (never \\server\share, see PathShaped).
static std::wstring FullPath(const std::wstring& s, const std::wstring& cwd) {
  if (s.empty() || IsPathSep(s[0])) return {};
  std::wstring f = s;
  if (!(s.size() >= 2 && s[1] == L':')) {
    if (cwd.empty()) return {};
    f = cwd + (IsPathSep(cwd.back()) ? L"" : L"\\") + s;
  }
  wchar_t buf[2048];
  DWORD n = GetFullPathNameW(f.c_str(), 2048, buf, nullptr);
  return n && n < 2048 ? std::wstring(buf, n) : std::wstring();
}

// file:///C:/a%20b → C:/a b
static std::wstring PercentDecode(const std::wstring& s) {
  std::string u = ToUtf8(s), o;
  for (size_t i = 0; i < u.size(); i++) {
    if (u[i] == '%' && i + 2 < u.size() && isxdigit((unsigned char)u[i + 1]) && isxdigit((unsigned char)u[i + 2])) {
      o += (char)strtol(u.substr(i + 1, 2).c_str(), nullptr, 16);
      i += 2;
    } else {
      o += u[i];
    }
  }
  return ToWide(o);
}

// A row for paths, with the lock held.
static PathRow PathRowAt(Pane* p, int row) {
  PathRow r;
  for (int c = 0; c < p->cols; c++) {
    VTermScreenCell cell;
    CellAt(p, row, c, &cell);
    if (cell.chars[0] == (uint32_t)-1) continue;  // second half of a wide character
    if (!cell.chars[0]) r.s += L' ';
    for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i]; i++) AppendUtf16(r.s, cell.chars[i]);
    r.c0.resize(r.s.size(), c);
    r.c1.resize(r.s.size(), c + std::max(1, (int)cell.width));
  }
  return r;
}

struct PathCand {
  int a, link, b;  // path start, underline start (file:/// included), end (a whole token's)
  int cls;         // 0: absolute or quoted, 1: relative
  bool fileUrl;
};

// Path that exists under the cell pos; rows[i] is screen row first + i (a few above and below, or the whole view).
// With scan (highlighting) the hover's memo isn't used and the disk is never asked here: answers come from the cache
// and the rest is asked on the worker (scan->pending).
static bool PathAt(const std::vector<PathRow>& rows, int first, VTermPos pos, int ncols, const std::wstring& cwd,
                   LinkHit& h, PathScan* scan = nullptr) {
  int hr = pos.row - first;
  if (hr < 0 || hr >= (int)rows.size()) return false;
  TrimPathCache();
  std::wstring key;
  DWORD now = GetTickCount();
  if (!scan) {
    key = cwd + L"\n" + std::to_wstring(first) + L"," + std::to_wstring(pos.row) + L"," + std::to_wstring(pos.col) + L"\n";
    for (auto& r : rows) key += r.s + L"\n";
    if (g_paths.memoTick && now - g_paths.memoTick < kPathCacheMs && key == g_paths.memoKey) {
      h = g_paths.memo;
      return !h.path.empty();
    }
  }
  auto done = [&](bool hit) {
    if (!scan) g_paths.memoKey = key, g_paths.memo = h, g_paths.memoTick = now ? now : 1;
    return hit;
  };
  // The column of the mouse (tmux panes: between │ borders) and the character under it.
  const PathRow& cur = rows[hr];
  int L = 0, R = ncols, hu = -1;
  for (int u = 0; u < (int)cur.s.size(); u++) {
    if (cur.c0[u] <= pos.col) hu = u;
    if (IsColumnBorder(cur.s[u])) {
      if (cur.c1[u] <= pos.col) L = cur.c1[u];
      else if (cur.c0[u] > pos.col) R = std::min(R, cur.c0[u]);
      else return done(false);
    }
  }
  // Each row's text in that column without the spaces around it: [u0, u1), last column + 1, indent (only the rows
  // a path through the mouse's row can reach).
  struct Piece {
    int u0 = 0, u1 = 0, end = 0, indent = 0;
  };
  std::vector<Piece> pc(rows.size());
  for (int i = std::max(0, hr - 4); i < (int)rows.size() && i <= hr + 4; i++) {
    const PathRow& r = rows[i];
    int u0 = -1, u1 = -1;
    for (int u = 0; u < (int)r.s.size(); u++)
      if (r.c0[u] >= L && r.c1[u] <= R && r.s[u] != L' ') {
        if (u0 < 0) u0 = u;
        u1 = u + 1;
      }
    if (u0 >= 0) pc[i] = Piece{u0, u1, r.c1[u1 - 1], r.c0[u0] - L};
  }
  // Rows that go on in the next one: it fills the column (wrapped by the terminal, the next one goes
  // on from the left), or the next one is indented (wrapped by Claude Code and the like).
  auto empty = [&](int i) { return pc[i].u1 <= pc[i].u0; };
  auto full = [&](int i) { return pc[i].end + 3 >= R; };
  auto joins = [&](int i) { return !empty(i) && !empty(i + 1) && (full(i) || pc[i + 1].indent > 0); };
  if (hu < 0 || empty(hr)) return done(false);
  int ra = hr, rb = hr;
  while (ra > 0 && hr - ra < 4 && joins(ra - 1)) ra--;
  while (rb + 1 < (int)rows.size() && rb - hr < 4 && joins(rb)) rb++;
  // Those rows joined, with a kBreak between them (tried first as nothing after a full row, as a space otherwise).
  std::wstring s;
  std::vector<int> srow, sc0, sc1;
  std::vector<char> space;  // at each kBreak: a space is tried first
  int hpos = -1;
  for (int i = ra; i <= rb; i++) {
    if (i > ra) s += kBreak, srow.push_back(-1), sc0.push_back(0), sc1.push_back(0), space.push_back(!full(i - 1));
    for (int u = pc[i].u0; u < pc[i].u1; u++) {
      if (i == hr && u == hu) hpos = (int)s.size();
      s += rows[i].s[u];
      srow.push_back(first + i), sc0.push_back(rows[i].c0[u]), sc1.push_back(rows[i].c1[u]), space.push_back(0);
    }
  }
  if (hpos < 0) return done(false);
  // Runs without spaces or breaks; kL..kR are the ones at the mouse (two if it's on a space).
  std::vector<std::pair<int, int>> tk;
  for (int i = 0; i < (int)s.size();) {
    if (s[i] == L' ' || s[i] == kBreak) {
      i++;
      continue;
    }
    int j = i;
    while (j < (int)s.size() && s[j] != L' ' && s[j] != kBreak) j++;
    tk.push_back({i, j});
    i = j;
  }
  int kL = -1, kR = -1;
  for (int k = 0; k < (int)tk.size(); k++) {
    if (tk[k].first <= hpos) kL = k;
    if (kR < 0 && tk[k].second > hpos) kR = k;
  }
  if (kL < 0 || kR < 0) return done(false);
  // Candidates: from a token at or before the mouse to one at or after it, starting after the quotes and
  // brackets in front, after file:///, or inside the token after ( " ' = or at a drive letter
  // (Read(src\x.cpp), --out=C:\x). Absolute and quoted ones first, then the longest.
  std::vector<PathCand> cands;
  for (int i = kL; i >= 0 && i > kR - 10; i--) {
    int ts = tk[i].first, te = tk[i].second, a = ts;
    while (a < te && wcschr(L"\"'`([{<", s[a])) a++;
    std::vector<std::pair<int, bool>> starts;
    if (a < te) starts.push_back({a, false});
    if (te - a > 8 && !_wcsnicmp(s.c_str() + a, L"file:///", 8)) starts.push_back({a + 8, true});
    for (int k = a + 1; k < te && starts.size() < 4; k++) {
      bool drive = k + 2 < te && iswalpha(s[k]) && s[k + 1] == L':' && IsPathSep(s[k + 2]) && !iswalnum(s[k - 1]) &&
                   !IsPathSep(s[k - 1]);  // not inside \\?\C:\… or \\.\C:\…
      bool dup = false;
      for (auto& st : starts) dup = dup || st.first == k;
      if (!dup && (drive || wcschr(L"(\"'=`", s[k - 1]))) starts.push_back({k, false});
    }
    for (auto [st, fu] : starts) {
      int link = fu ? a : st;
      if (link > hpos) continue;
      int n = (int)s.size();
      bool abs = fu || (st + 2 < n && iswalpha(s[st]) && s[st + 1] == L':' && IsPathSep(s[st + 2]));
      bool quoted = st > ts && wcschr(L"\"'`", s[st - 1]);
      int cls = abs || quoted ? 0 : 1;
      for (int j = std::max(i, kR); j < (int)tk.size() && j - i < (cls ? 6 : 10); j++)
        cands.push_back({st, link, tk[j].second, cls, fu});
    }
  }
  std::stable_sort(cands.begin(), cands.end(), [](const PathCand& x, const PathCand& y) {
    return x.cls != y.cls ? x.cls < y.cls : x.b - x.a > y.b - y.a;
  });
  int budget = 64;
  std::vector<std::wstring> slow;
  const DirList* names = nullptr;
  bool listed = false, listing = false;  // listing: the folder's names were asked on the worker (scan)
  // Attributes of a candidate (relative ones whose first part isn't in the pane's folder: missing).
  auto attrsOf = [&](const std::wstring& t, std::wstring& full) -> DWORD {
    full = FullPath(t, cwd);
    if (full.empty()) return INVALID_FILE_ATTRIBUTES;
    if (!(t.size() >= 2 && t[1] == L':')) {
      size_t sep = t.find_first_of(L"\\/");
      std::wstring head = t.substr(0, sep);
      if (head != L"." && head != L"..") {
        if (!listed) names = scan ? CachedDirNames(cwd, *scan, listing) : DirNames(cwd, budget), listed = true;
        if (names) {
          auto it = names->names.find(Lower(head));
          if (it == names->names.end()) return INVALID_FILE_ATTRIBUTES;
          if (sep == std::wstring::npos) return it->second;
        } else if (listing) {
          return kAttrPending;  // not one by one: the listing is on its way
        }
      }
    }
    return scan ? CachedPathAttrs(full, *scan) : PathAttrs(full, budget, slow);
  };
  for (auto& c : cands) {
    std::vector<int> breaks;
    for (int k = c.a; k < c.b; k++)
      if (s[k] == kBreak) breaks.push_back(k);
    int masks = 1 << std::min((int)breaks.size(), 4);
    for (int m = 0; m < masks; m++) {
      // Shorter and shorter: without a :line or (line) or a punctuation mark at the end each time.
      for (int e = c.b, step = 0; e > c.a && step < 6; step++) {
        wchar_t last = s[e - 1];
        if (last == kBreak) break;
        int le = e + LineSuffixAt(s, e);  // the underline takes a :line behind too
        if (last != L'.' && hpos < le) {
          std::wstring t;
          for (int k = c.a, bi = 0; k < e; k++) {
            if (s[k] != kBreak) {
              t += s[k];
              continue;
            }
            if ((space[k] != 0) != (bi < 4 && (m >> bi & 1))) t += L' ';
            bi++;
          }
          if (c.fileUrl) t = PercentDecode(t);
          std::wstring full;
          DWORD at = PathShaped(t) ? attrsOf(t, full) : INVALID_FILE_ATTRIBUTES;
          if (at != INVALID_FILE_ATTRIBUTES && at != kAttrPending) {
            h.path = full, h.dir = at & FILE_ATTRIBUTE_DIRECTORY, h.row = pos.row;
            for (int k = c.link; k < le; k++) {
              if (s[k] == kBreak) continue;
              if (h.segs.empty() || h.segs.back()[0] != srow[k]) h.segs.push_back({srow[k], sc0[k], sc1[k]});
              else h.segs.back()[2] = sc1[k];
            }
            for (auto& [row, c0, c1] : h.segs)
              if (row == pos.row) h.c0 = c0, h.c1 = c1;
            return done(true);
          }
        }
        int k = LineSuffixStart(s, c.a, e);
        if (k < e) e = k;
        else if (wcschr(L".,;:!?)]}>'\"`", last)) e--;
        else break;
      }
    }
  }
  // Nothing (yet): ask the worker about the slow ones; when it answers, the hover is looked up again.
  if (!slow.empty()) {
    for (auto& f : slow) PathAsk(f);
    PathSend();
    return false;  // not final: no memo
  }
  return done(false);
}

// Programs and scripts: Ctrl+click never runs them (Explorer opens with them selected instead).
static bool IsRunnable(const std::wstring& path) {
  size_t slash = path.find_last_of(L"\\/"), dot = path.rfind(L'.');
  if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return false;
  std::wstring ext = Lower(path.substr(dot));
  static const wchar_t* const kRun[] = {
      L".exe", L".com", L".bat", L".cmd", L".ps1", L".psm1", L".psd1", L".ps1xml", L".psc1", L".vbs", L".vbe",
      L".vb", L".js", L".jse", L".wsf", L".wsh", L".ws", L".msi", L".msp", L".mst", L".scr", L".pif", L".lnk",
      L".url", L".website", L".hta", L".cpl", L".msc", L".jar", L".reg", L".inf", L".scf", L".chm",
      L".application", L".appref-ms", L".gadget", L".settingcontent-ms", L".appx", L".appxbundle", L".msix",
      L".msixbundle", L".diagcab", L".py", L".pyw", L".pyz", L".sh", L".bash", L".pl", L".rb", L".ahk", L".au3"};
  for (auto e : kRun)
    if (ext == e) return true;
  wchar_t buf[1024];
  DWORD n = GetEnvironmentVariableW(L"PATHEXT", buf, 1024);
  if (n && n < 1024) {
    std::wstringstream ss(Lower(buf));
    std::wstring x;
    while (std::getline(ss, x, L';'))
      if (x == ext) return true;
  }
  return false;
}

// What Ctrl+click does: URLs and files open with their app, folders in Explorer, and programs and
// scripts are never run (Explorer opens with them selected).
enum { LINK_OPEN, LINK_EXPLORER, LINK_SELECT };
static const wchar_t* const kLinkActions[] = {L"open", L"explorer", L"select"};

static int LinkAction(const LinkHit& h) {
  if (!h.url.empty()) return LINK_OPEN;
  return h.dir ? LINK_EXPLORER : IsRunnable(h.path) ? LINK_SELECT : LINK_OPEN;
}

struct PathOpen {
  std::wstring path;
  int action;
};

// On its own thread: the file's app, Explorer or the network may take a while to answer.
static DWORD WINAPI PathOpenThread(LPVOID arg) {
  std::unique_ptr<PathOpen> job((PathOpen*)arg);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  if (job->action == LINK_SELECT) {
    if (auto id = ILCreateFromPathW(job->path.c_str())) {
      SHOpenFolderAndSelectItems(id, 0, nullptr, 0);
      ILFree(id);
    }
  } else if (job->action == LINK_EXPLORER) {
    if ((INT_PTR)ShellExecuteW(nullptr, L"explore", job->path.c_str(), nullptr, nullptr, SW_SHOWNORMAL) <= 32)
      ShellExecuteW(nullptr, L"open", job->path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  } else {
    ShellExecuteW(nullptr, L"open", job->path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  }
  CoUninitialize();
  return 0;
}

static void OpenPathLink(const LinkHit& h, int action = -1) {
  if (!DriveSpeed(h.path)) return;  // never a share or network drive (PathAt doesn't return those either)
  auto* job = new PathOpen{h.path, action < 0 ? LinkAction(h) : action};
  HANDLE t = CreateThread(nullptr, 0, PathOpenThread, job, 0, nullptr);
  if (t) CloseHandle(t);
  else delete job;
}

// Right-click menu on a link: open it (like Ctrl+click), copy it, or show the file or folder selected
// in Explorer.
static std::vector<Str> LinkMenuItems(const LinkHit& h) {
  if (!h.url.empty()) return {S_OPEN, S_COPY_LINK};
  return {S_OPEN, S_COPY_PATH, S_OPEN_LOCATION};
}

static void LinkMenuRun(const LinkHit& h, Str item) {
  if (item == S_OPEN && !h.url.empty()) ShellExecuteW(nullptr, L"open", h.url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  else if (item == S_OPEN) OpenPathLink(h);
  else if (item == S_COPY_LINK || item == S_COPY_PATH) SetClipboardText(h.url.empty() ? h.path : h.url);
  else if (item == S_OPEN_LOCATION) OpenPathLink(h, LINK_SELECT);  // file or folder selected in Explorer
}

static void LinkMenu(const LinkHit& h, POINT screen) {
  HMENU m = CreatePopupMenu();
  auto items = LinkMenuItems(h);
  for (size_t i = 0; i < items.size(); i++) AppendMenuW(m, MF_STRING, i + 1, T(items[i]));
  int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, g_win->hwnd, nullptr);
  DestroyMenu(m);
  if (cmd > 0) LinkMenuRun(h, items[cmd - 1]);
}

// URL bajo el punto (también si se pulsa en el trozo de una URL partida).
// If there's none, a path that exists (PathAt): the rows are copied with the lock held and the disk
// is asked after letting it go.
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
  std::vector<PathRow> rows;
  int first = std::max(0, pos.row - 4), cols = p->cols;
  std::wstring cwd;
  if (h.url.empty()) {
    for (int r = first; r < p->rows && r <= pos.row + 4; r++) rows.push_back(PathRowAt(p, r));
    cwd = p->cwd;
  }
  LeaveCriticalSection(&p->cs);
  if (h.url.empty() && PathAt(rows, first, pos, cols, cwd, h)) h.p = p;
  return h;
}

// ---------------------------------------------------------------------------
// Path highlighting: files and folders that exist are drawn in their own color (kPathFg), always, not only under
// Ctrl. A pass over the view of each pane on screen finds them with PathAt at every seed (a \ or /, or the dot of a
// name.ext, so plain words and bare folder names are never colored), in its scan mode: only the cache, the rest asked
// on the worker and the pass redone when it answers. Passes run on a one-shot timer, at most every kPathScanMs while
// the text changes (output, or DrawPane drawing text no pass has seen: scrolling, resizing), and never when idle.
// DrawPane colors a row only with the paths found in that same text, so they never land on other cells.

static const UINT kPathScanMs = 150;
static const int kPathRounds = 6;  // passes redone for answers without the text changing

static void ArmPathScan() {
  if (!g_win->pathTimer && g_win->hwnd) SetTimer(g_win->hwnd, TIMER_PATHS, kPathScanMs, nullptr), g_win->pathTimer = true;
}

// Does more of a path come right after cell c1 of a row: after one space (or at the start of the next row, when that
// ends its row and the next one goes on from it, as PathAt joins them) a run with the separator sep in it that doesn't
// start a path of its own (drive, quote, bracket, file:///) nor a switch or another path (/s, \\host)? run: its row
// and cells.
static bool PathGoesOn(const std::vector<PathRow>& rows, int cols, int row, int c1, wchar_t sep, std::array<int, 3>& run) {
  const PathRow* r = &rows[row];
  int u = 0, spaces = 0;
  while (u < (int)r->s.size() && r->c0[u] < c1) u++;
  while (u < (int)r->s.size() && r->s[u] == L' ') u++, spaces++;
  if (u == (int)r->s.size()) {
    if (row + 1 >= (int)rows.size()) return false;
    r = &rows[++row], u = 0;
    while (u < (int)r->s.size() && r->s[u] == L' ') u++;
    if (u == 0 && c1 + 3 < cols) return false;  // neither full nor an indented row below
  } else if (spaces != 1) {
    return false;
  }
  int e = u;
  while (e < (int)r->s.size() && r->s[e] != L' ') e++;
  if (u == e || IsPathSep(r->s[u]) || wcschr(L"\"'`([{<", r->s[u]) || (e - u > 2 && r->s[u + 1] == L':') ||
      (e - u > 8 && !_wcsnicmp(r->s.c_str() + u, L"file:///", 8)))
    return false;
  run = {row, r->c0[u], r->c1[e - 1]};
  for (int k = u; k < e; k++)
    if (r->s[k] == sep) return true;
  return false;
}

// Paths that exist in the rows of a pane's view: for each row, its cells [c0, c1).
static std::vector<std::vector<std::pair<int, int>>> FindPaths(const std::vector<PathRow>& rows, int cols,
                                                               const std::wstring& cwd, PathScan& scan) {
  std::vector<std::vector<std::pair<int, int>>> out(rows.size());
  std::vector<std::vector<std::array<int, 3>>> folders;  // segments of the folders found
  auto covered = [&](int r, int c) {
    for (auto& [c0, c1] : out[r])
      if (c >= c0 && c < c1) return true;
    return false;
  };
  for (int r = 0; r < (int)rows.size(); r++) {
    const std::wstring& s = rows[r].s;
    for (int u = 0; u < (int)s.size();) {
      if (s[u] == L' ') {
        u++;
        continue;
      }
      int e = u, seed = -1;
      while (e < (int)s.size() && s[e] != L' ') e++;
      for (int k = u; k < e && seed < 0; k++)
        if (IsPathSep(s[k])) seed = k;
      for (int k = e - 2; k >= u && seed < 0; k--)
        if (s[k] == L'.' && iswalnum(s[k + 1])) seed = k;
      u = e;
      if (seed < 0 || covered(r, rows[r].c0[seed])) continue;
      LinkHit h;
      if (!PathAt(rows, 0, VTermPos{r, rows[r].c0[seed]}, cols, cwd, h, &scan) || h.segs.empty()) continue;
      for (auto& [sr, c0, c1] : h.segs)
        if (sr >= 0 && sr < (int)out.size()) out[sr].push_back({c0, c1});
      if (h.dir) folders.push_back(h.segs);
    }
  }
  // A folder followed by more of a path that isn't one is the start of a longer path that doesn't exist (C:\Users\Ana in
  // «C:\Users\Ana Lopez\new.txt» when there's a C:\Users\Ana): not colored.
  for (auto& segs : folders) {
    std::array<int, 3> run;
    int lr = segs.back()[0];
    if (lr < 0 || lr >= (int)rows.size()) continue;
    wchar_t sep = 0;  // the separator the folder is written with (its last one)
    for (auto& [sr, c0, c1] : segs)
      for (int u = 0; sr >= 0 && sr < (int)rows.size() && u < (int)rows[sr].s.size(); u++)
        if (rows[sr].c0[u] >= c0 && rows[sr].c1[u] <= c1 && IsPathSep(rows[sr].s[u])) sep = rows[sr].s[u];
    if (!sep || !PathGoesOn(rows, cols, lr, segs.back()[2], sep, run)) continue;
    bool own = false;  // a path was found in it after all
    for (int c = run[1]; c < run[2] && !own; c++) own = covered(run[0], c);
    if (own) continue;
    for (auto& [sr, c0, c1] : segs) {
      auto& v = out[sr];
      auto it = std::find(v.begin(), v.end(), std::pair<int, int>{c0, c1});
      if (it != v.end()) v.erase(it);
    }
  }
  for (auto& v : out) std::sort(v.begin(), v.end());
  return out;
}

// The paths found for a row with this text: the row's own, or those of a row that had the same text (the view has
// scrolled since); nullptr if no pass has seen it.
static const std::vector<std::pair<int, int>>* PathSpansFor(Pane* p, int row, size_t hash) {
  if (row >= 0 && row < (int)p->pathHash.size() && p->pathHash[row] == hash) return &p->pathSpans[row];
  for (size_t r = 0; r < p->pathHash.size(); r++)
    if (p->pathHash[r] == hash) return &p->pathSpans[r];
  return nullptr;
}

// A pass over a pane's view (rows copied with the lock held, looked up without it); rows whose paths changed are
// drawn again.
static void PathScanPane(Pane* p, bool revalidate) {
  EnterCriticalSection(&p->cs);
  int n = p->rows, cols = p->cols;
  std::vector<PathRow> rows;
  std::vector<size_t> hash;
  for (int r = 0; r < n; r++) rows.push_back(PathRowAt(p, r)), hash.push_back(std::hash<std::wstring>{}(RowText(p, r)));
  std::wstring cwd = p->cwd;
  LeaveCriticalSection(&p->cs);
  PathScan scan;
  scan.revalidate = revalidate;
  auto spans = FindPaths(rows, cols, cwd, scan);
  static const std::vector<std::pair<int, int>> kNone;
  bool redraw = false;
  EnterCriticalSection(&p->cs);  // dirtyRows is the reader's too
  for (int r = 0; r < n; r++) {
    // While answers are on their way, a row with the same text keeps its paths (redone when they arrive).
    if (scan.pending && r < (int)p->pathHash.size() && p->pathHash[r] == hash[r]) spans[r] = p->pathSpans[r];
    const auto* was = PathSpansFor(p, r, hash[r]);  // what DrawPane would draw now
    if ((was ? *was : kNone) != spans[r]) MarkRow(p, r), redraw = true;
  }
  p->pathHash = std::move(hash);
  p->pathSpans = std::move(spans);
  LeaveCriticalSection(&p->cs);
  if (redraw) RequestRepaint(p);
}

// TIMER_PATHS: a pass over the panes on screen whose text changed or that got answers.
static void ScanPaths() {
  if (g_win->tabs.empty() || IsIconic(g_win->hwnd)) return;  // looked at again when drawn
  std::vector<Pane*> ls;
  Leaves(CurTab().root, ls);
  for (Pane* p : ls) {
    unsigned seq = p->outSeq;
    if (seq != p->pathSeq) p->pathSeq = seq, p->pathDue = true, p->pathRounds = 0;
    if (!p->pathDue) continue;
    p->pathDue = false;
    PathScanPane(p, p->pathRounds == 0);
  }
  PathSend();
}

// New output in a pane on screen: a pass soon.
static void PathsMaybeChanged() {
  if (g_win->tabs.empty()) return;
  std::vector<Pane*> ls;
  Leaves(CurTab().root, ls);
  for (Pane* p : ls)
    if (p->outSeq != p->pathSeq) return ArmPathScan();
}

// Cells of a row to draw in kPathFg. Text no pass has seen gets none for now, and a pass soon.
static std::vector<char> PathMask(Pane* p, int row, const std::wstring& text) {
  std::vector<char> m(p->cols, 0);
  const auto* spans = PathSpansFor(p, row, std::hash<std::wstring>{}(text));
  if (!spans) p->pathDue = true, p->pathRounds = 0;
  if (p->pathDue) ArmPathScan();
  if (spans)
    for (auto [c0, c1] : *spans)
      for (int c = std::max(0, c0); c < c1 && c < p->cols; c++) m[c] = 1;
  return m;
}

// Answers from PathCheckThread: cached, and the link under the mouse is looked up again with them; if any of them is
// new or different, the panes' paths too (a few rounds at most until their text changes).
static void OnPathChecks(PathChecks* job) {
  g_paths.busy = false;
  DWORD now = GetTickCount();
  bool changed = false;
  for (auto& f : job->paths) g_paths.pending.erase(Lower(f));
  for (auto& [f, a] : job->attrs) {
    std::wstring key = Lower(f);
    auto it = g_paths.attrs.find(key);
    changed = changed || it == g_paths.attrs.end() || it->second.attrs != a;
    g_paths.attrs[key] = TimedAttrs{a, now};
  }
  for (size_t i = 0; i < job->dirs.size() && i < job->lists.size(); i++) {
    std::wstring key = Lower(job->dirs[i]);
    g_paths.dirsPending.erase(key);
    auto it = g_paths.dirs.find(key);
    changed = changed || it == g_paths.dirs.end() || it->second.ok != job->lists[i].ok ||
              it->second.names != job->lists[i].names;
    job->lists[i].tick = now;
    g_paths.dirs[key] = std::move(job->lists[i]);
  }
  PathSend();
  g_paths.memoTick = 0;
  for (Window* w : g_windows) {
    InWindow in(w);
    if (changed) {
      for (Pane* p : WindowPanes())
        if (p->pathRounds < kPathRounds) p->pathRounds++, p->pathDue = true;
      ArmPathScan();
    }
    POINT pt;
    if (w->tabs.empty() || !(GetKeyState(VK_CONTROL) & 0x8000) || !GetCursorPos(&pt) || WindowFromPoint(pt) != w->hwnd ||
        !ScreenToClient(w->hwnd, &pt))
      continue;
    Pane* p = PaneAt(pt.x, pt.y);
    LinkHit h = p ? LinkAt(p, pt.x, pt.y) : LinkHit{};
    if (h.p != w->link.p || h.row != w->link.row || h.c0 != w->link.c0 || h.path != w->link.path)
      InvalidateRect(w->hwnd, nullptr, FALSE);
    w->link = h;
    if (w->link.p) SetCursor(LoadCursor(nullptr, IDC_HAND));
  }
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
  if (!g_win->vertTabs) {
    int x = L.toggle.right + Px(6), tw = TabWidth();
    for (int i = 0; i < (int)g_win->tabs.size(); i++)
      if (!g_win->tabs[i].hidden) L.tabs.push_back({i, RECT{x, Px(6), x + tw - Px(2), th}}), x += tw;
    L.plus = RECT{x, Px(6), x + Px(34), th};
    x += Px(34);
    if (hidden) L.hiddenBtn = RECT{x, Px(6), x + Px(90), th}, x += Px(90);
    L.end = x;
  } else {
    int y = th + Px(6), rowH = Px(34), sw = SidebarW();
    for (int i = 0; i < (int)g_win->tabs.size(); i++)
      if (!g_win->tabs[i].hidden) L.tabs.push_back({i, RECT{Px(6), y, sw - Px(6), y + rowH}}), y += rowH + Px(2);
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
    GetClientRect(g_win->hwnd, &rc);
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

// ---------------------------------------------------------------------------
// Join: a tab or pane dragged out of this window and dropped on another window of this Warp moves there as it is, the
// way Detach moves one to a new window: the same shell and what runs in it (ssh, tmux, Claude Code), its scrollback and
// what is typed in its box; only its size changes, to its place there. Dropped on that window's tabs (or its tab
// sidebar) it becomes a new tab there; on one of its panes, a split of that pane on the side nearest the point. While
// dragging, the window under the cursor draws where it would go (it doesn't take the focus before the drop). A window of
// another Warp (a test's, or the real one from a test), a minimized one or one covered there by another program is no
// target: the drop is outside, as before.

enum { SIDE_LEFT, SIDE_RIGHT, SIDE_TOP, SIDE_BOTTOM };
static const wchar_t* const kSideNames[] = {L"left", L"right", L"top", L"bottom"};

// Where a tab or pane dropped at that point of this window's client area goes: a new tab at index tab, or a split of
// pane (of the tab in view) with the dropped one on side. mark: what the hint draws (the insertion bar on the tabs, or
// the part of the pane it would take).
struct JoinSpot {
  int tab = 0;
  Pane* pane = nullptr;
  int side = SIDE_RIGHT;
  RECT mark{};
};

static JoinSpot JoinSpotAt(POINT pt) {
  JoinSpot s;
  RECT rc;
  GetClientRect(g_win->hwnd, &rc);
  pt.x = std::clamp(pt.x, rc.left, std::max(rc.left, rc.right - 1));  // the invisible resize border counts as the edge
  pt.y = std::clamp(pt.y, rc.top, std::max(rc.top, rc.bottom - 1));
  if (Pane* p = pt.y >= TabBarH() && pt.x >= SidebarW() ? PaneAt(pt.x, pt.y) : nullptr) {
    // The side nearest the point, relative to the pane's size (its diagonals split it in four).
    RECT r = p->node->rc;
    double w = std::max(1L, r.right - r.left), h = std::max(1L, r.bottom - r.top);
    double fx = (pt.x - r.left) / w, fy = (pt.y - r.top) / h, d[4] = {fx, 1 - fx, fy, 1 - fy};
    s.side = (int)(std::min_element(d, d + 4) - d);
    s.pane = p;
    s.mark = r;
    if (s.side == SIDE_LEFT) s.mark.right = r.left + (r.right - r.left) / 2;
    else if (s.side == SIDE_RIGHT) s.mark.left = r.left + (r.right - r.left) / 2;
    else if (s.side == SIDE_TOP) s.mark.bottom = r.top + (r.bottom - r.top) / 2;
    else s.mark.top = r.top + (r.bottom - r.top) / 2;
    return s;
  }
  // On the tabs (or anywhere else): before the first visible tab whose middle is past the point, else after the last.
  TabLayout L = ComputeTabs();
  RECT at = L.plus;
  s.tab = L.tabs.empty() ? (int)g_win->tabs.size() : L.tabs.back().first + 1;
  for (auto& [i, r] : L.tabs)
    if (g_win->vertTabs ? pt.y < (r.top + r.bottom) / 2 : pt.x < (r.left + r.right) / 2) {
      s.tab = i, at = r;
      break;
    }
  s.mark = g_win->vertTabs ? RECT{at.left, at.top - Px(2), at.right, at.top + Px(1)} : RECT{at.left - Px(2), at.top, at.left + Px(1), at.bottom};
  return s;
}


static void JoinHintClear() {
  KillTimer(g_win->hwnd, TIMER_JOIN);
  if (!g_win->joinHint.from) return;
  g_win->joinHint = JoinHint{};
  InvalidateRect(g_win->hwnd, nullptr, FALSE);
}

static void JoinHintAt(HWND from, POINT screen) {
  POINT pt = screen;
  ScreenToClient(g_win->hwnd, &pt);
  RECT mark = JoinSpotAt(pt).mark;
  if (!g_win->joinHint.from) SetTimer(g_win->hwnd, TIMER_JOIN, 500, nullptr);
  bool same = g_win->joinHint.from == from && EqualRect(&mark, &g_win->joinHint.mark);
  g_win->joinHint = JoinHint{from, screen, mark};
  if (!same) InvalidateRect(g_win->hwnd, nullptr, FALSE);
}

// Another window of this Warp (of this process).
static Window* OtherWindow(HWND h) {
  Window* w = LiveWindow(h);
  return w != g_win ? w : nullptr;
}

// The top-level window in view at that point of the screen. A desktop that takes no input (a service session, where the
// tests run) answers nothing to WindowFromPoint: there, the visible windows by position and z-order.
static HWND TopWindowAt(POINT screen) {
  HWND h = WindowFromPoint(screen);
  if (!h) h = ChildWindowFromPointEx(GetDesktopWindow(), screen, CWP_SKIPINVISIBLE | CWP_SKIPTRANSPARENT);
  return h ? GetAncestor(h, GA_ROOT) : nullptr;
}

// The window of this Warp that a tab or pane dropped at that point of the screen would join: the one in view there (not
// covered by another program there, not minimized or hidden). Its own WS_VISIBLE: IsWindowVisible also asks the desktop,
// which that service session's isn't.
static Window* JoinTargetAt(POINT screen) {
  Window* w = OtherWindow(TopWindowAt(screen));
  return w && (GetWindowLongW(w->hwnd, GWL_STYLE) & WS_VISIBLE) && !IsIconic(w->hwnd) && !w->tabs.empty() ? w : nullptr;
}

// The window that was showing where what is dragged from this one would go stops showing it.
static void JoinLeave() {
  if (Window* o = LiveWindow(g_win->joinOver)) {
    HWND from = g_win->hwnd;
    InWindow in(o);
    if (o->joinHint.from == from) JoinHintClear();
  }
  g_win->joinOver = nullptr;
}

// While a tab or pane is dragged: the other window of this Warp in view under the cursor (also over this one's area, if
// it is in front there) shows where it would go, and the one before stops; returns it. Over this one, or over none of
// this Warp, none.
static Window* JoinHover(POINT screen) {
  Window* to = JoinTargetAt(screen);
  if (LiveWindow(g_win->joinOver) != to) JoinLeave();
  if (to) {
    HWND from = g_win->hwnd;
    g_win->joinOver = to->hwnd;
    InWindow in(to);
    JoinHintAt(from, screen);
  }
  return to;
}

// Puts a tab or pane taken out of another window (TakeOut) where JoinSpotAt says for that point of this window's
// client area: a new tab, or a split of one of its panes. Its panes are this window's from now on, at their size here.
static void PutIn(Tab t, POINT at) {
  JoinHintClear();
  if (g_win->renameTab >= 0) EndRename(true);  // the tab indexes are about to change
  if (g_win->sw.open) CloseSwitcher();
  std::vector<Pane*> moved;
  Leaves(t.root, moved);
  for (Pane* p : moved) p->win = g_win;
  JoinSpot s = JoinSpotAt(at);
  int ti = s.tab;
  if (s.pane) {  // its place now holds both: the one that was there and, on that side, the one dropped
    ti = TabIndexOf(s.pane);
    Node* leaf = s.pane->node;
    Node* old = new Node();
    old->pane = s.pane;
    old->parent = leaf;
    s.pane->node = old;
    bool first = s.side == SIDE_LEFT || s.side == SIDE_TOP;
    leaf->pane = nullptr;
    leaf->a = first ? t.root : old;
    leaf->b = first ? old : t.root;
    leaf->vertical = s.side == SIDE_LEFT || s.side == SIDE_RIGHT;
    t.root->parent = leaf;
    g_win->tabs[ti].active = t.active;
    Equalize(g_win->tabs[ti].root);
  } else {
    g_win->tabs.insert(g_win->tabs.begin() + ti, Tab{t.root, t.active, t.name, false});
  }
  ActivateTab(ti);
}

// Moves a tab (or a single pane) to that window, dropped at that point of the screen; it comes to front. A window left
// without tabs closes as when its last tab is closed (the main one keeps its place; another one is discarded).
static bool JoinWindow(Window* to, int tabIdx, Pane* single, POINT screen) {
  if (tabIdx < 0 || tabIdx >= (int)g_win->tabs.size()) return false;
  Tab t = TakeOut(tabIdx, single);
  {
    InWindow in(to);
    POINT at = screen;
    ScreenToClient(to->hwnd, &at);
    PutIn(t, at);
    SaveSession();
  }
  BringToFront(to->hwnd);  // this window is in front: it may hand that over
  if (!CloseIfEmpty()) SaveSession();
  return true;
}

// A tab or pane let go of at that point of the screen: over another window of this Warp in view there, it joins it;
// outside this window, anywhere else, it opens in a new window there (never the only tab: that would just be this
// window again); over this window, nothing (moving tabs and swapping panes within it is done while dragging).
static void DropAway(int tabIdx, Pane* single, POINT screen) {
  if (Window* to = JoinTargetAt(screen)) JoinWindow(to, tabIdx, single, screen);
  else if (OutsideWindow(screen) && (single || g_win->tabs.size() > 1)) Detach(tabIdx, single, screen);
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
  InvalidateRect(g_win->hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Dibujo

static HDC g_measureDC;
static std::unordered_map<uint32_t, uint8_t> g_fbCache;  // 0 = Hack; i = g_win->fallback[i-1]

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
  else if (cp >= 0x10000) r = kEmojiFace + 1;       // emoji y demás fuera del plano básico
  else if (!FontHas(g_win->fonts[0], cp))
    for (int i = 0; i < kFallbackCount; i++)
      if (FontHas(g_win->fallback[i], cp)) {
        r = i + 1;
        break;
      }
  g_fbCache[cp] = (uint8_t)r;
  return r;
}

// Emoji presentation in the basic plane: a wide symbol (✅ ⭐ ⚡…) or one followed by VS16 (❤️ ⚠️…), if Segoe UI Emoji
// has it. The rest of the emoji are past U+FFFF and go to it anyway.
static bool EmojiPresentation(const VTermScreenCell& c) {
  uint32_t cp = c.chars[0];
  if (c.chars[1] != 0xFE0F && (c.width != 2 || cp < 0x2190 || cp >= 0x2C00 || cp == 0x2329 || cp == 0x232A)) return false;
  if (cp < 0xA9 || cp >= 0x10000) return false;
  static std::unordered_map<uint32_t, bool> has;
  auto it = has.find(cp);
  return it != has.end() ? it->second : (has[cp] = FontHas(g_win->fallback[kEmojiFace], cp));
}

// Box drawing, block elements and the powerline separators are drawn by the app over the whole cell, like Windows
// Terminal's builtin glyphs: lines join across rows and columns whatever the font and the line height.
static bool IsBuiltin(uint32_t c) { return (c >= 0x2500 && c < 0x25A0) || (c >= 0xE0B0 && c < 0xE0C0); }

// Arms of U+2500-U+257F, one hex digit each: up, right, down, left; 0 none, 1 light, 2 heavy, 3 double.
// 0: dashes, arcs and diagonals, drawn apart.
static const uint16_t kBoxArms[128] = {
    0x0101, 0x0202, 0x1010, 0x2020, 0,      0,      0,      0,       // ─━│┃┄┅┆┇
    0,      0,      0,      0,      0x0110, 0x0210, 0x0120, 0x0220,  // ┈┉┊┋┌┍┎┏
    0x0011, 0x0012, 0x0021, 0x0022, 0x1100, 0x1200, 0x2100, 0x2200,  // ┐┑┒┓└┕┖┗
    0x1001, 0x1002, 0x2001, 0x2002, 0x1110, 0x1210, 0x2110, 0x1120,  // ┘┙┚┛├┝┞┟
    0x2120, 0x2210, 0x1220, 0x2220, 0x1011, 0x1012, 0x2011, 0x1021,  // ┠┡┢┣┤┥┦┧
    0x2021, 0x2012, 0x1022, 0x2022, 0x0111, 0x0112, 0x0211, 0x0212,  // ┨┩┪┫┬┭┮┯
    0x0121, 0x0122, 0x0221, 0x0222, 0x1101, 0x1102, 0x1201, 0x1202,  // ┰┱┲┳┴┵┶┷
    0x2101, 0x2102, 0x2201, 0x2202, 0x1111, 0x1112, 0x1211, 0x1212,  // ┸┹┺┻┼┽┾┿
    0x2111, 0x1121, 0x2121, 0x2112, 0x2211, 0x1122, 0x1221, 0x2212,  // ╀╁╂╃╄╅╆╇
    0x1222, 0x2122, 0x2221, 0x2222, 0,      0,      0,      0,       // ╈╉╊╋╌╍╎╏
    0x0303, 0x3030, 0x0310, 0x0130, 0x0330, 0x0013, 0x0031, 0x0033,  // ═║╒╓╔╕╖╗
    0x1300, 0x3100, 0x3300, 0x1003, 0x3001, 0x3003, 0x1310, 0x3130,  // ╘╙╚╛╜╝╞╟
    0x3330, 0x1013, 0x3031, 0x3033, 0x0313, 0x0131, 0x0333, 0x1303,  // ╠╡╢╣╤╥╦╧
    0x3101, 0x3303, 0x1313, 0x3131, 0x3333, 0,      0,      0,       // ╨╩╪╫╬╭╮╯
    0,      0,      0,      0,      0x0001, 0x1000, 0x0100, 0x0010,  // ╰╱╲╳╴╵╶╷
    0x0002, 0x2000, 0x0200, 0x0020, 0x0201, 0x1020, 0x0102, 0x2010,  // ╸╹╺╻╼╽╾╿
};

// Coverage (0-255) of builtin glyph c in a w x h cell. As in Windows Terminal, a light line is a sixth of the cell width
// (at least a pixel), a heavy one twice that and a double one two light lines a light line apart; stroke edges fall on
// whole pixels, the same in every cell, so lines join across rows and columns. Arcs, diagonals and the powerline shapes
// are antialiased with 4x4 samples per pixel. The shades are patterns, drawn by BuiltinPaint.
static void BuiltinRaster(uint32_t c, int w, int h, uint8_t* m) {
  const int lw = std::max(1, (int)std::lround(w / 6.0));
  const float cx = w / 2.0f, cy = h / 2.0f, t = lw / 2.0f;
  auto rect = [&](int x0, int y0, int x1, int y1) {
    x0 = std::max(x0, 0), y0 = std::max(y0, 0), x1 = std::min(x1, w), y1 = std::min(y1, h);
    for (int y = y0; y < y1 && x0 < x1; y++) std::fill(m + (size_t)y * w + x0, m + (size_t)y * w + x1, (uint8_t)255);
  };
  auto shape = [&](auto inside) {
    for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++) {
        int k = 0;
        for (int i = 0; i < 16; i++) k += inside(x + (i % 4 + 0.5f) / 4, y + (i / 4 + 0.5f) / 4);
        uint8_t& v = m[(size_t)y * w + x];
        v = std::max(v, (uint8_t)(k * 255 / 16));
      }
  };
  // [first, second) of a stroke n light lines wide centred on mid
  auto stroke = [&](float mid, int n) {
    int a = (int)std::lround(mid - n * t);
    return std::make_pair(a, a + n * lw);
  };
  auto seg = [](float x, float y, float ax, float ay, float bx, float by) {
    float dx = bx - ax, dy = by - ay, u = std::clamp(((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy), 0.0f, 1.0f);
    return std::hypot(x - ax - u * dx, y - ay - u * dy);
  };
  float W = (float)w, H = (float)h, diag = std::hypot(W, H);
  if (c < 0x2580) {
    int k = (int)c - 0x2500, a = kBoxArms[k];
    int mx = (int)std::lround(cx), my = (int)std::lround(cy);
    auto v1 = stroke(cx, 1), h1 = stroke(cy, 1);  // light strokes through the centre
    if (a) {
      int U = a >> 12, R = a >> 8 & 15, D = a >> 4 & 15, L = a & 15;
      if (std::max({U, R, D, L}) < 3) {  // light and heavy: each arm reaches the far side of the widest crossing stroke
        auto vs = stroke(cx, std::max(U, D)), hs = stroke(cy, std::max(L, R));
        bool vert = U || D, horz = L || R;
        if (U) rect(stroke(cx, U).first, 0, stroke(cx, U).second, horz ? hs.second : my);
        if (D) rect(stroke(cx, D).first, horz ? hs.first : my, stroke(cx, D).second, h);
        if (L) rect(0, stroke(cy, L).first, vert ? vs.second : mx, stroke(cy, L).second);
        if (R) rect(vert ? vs.first : mx, stroke(cy, R).first, w, stroke(cy, R).second);
      } else {
        // Double lines: a and b are the left/top and right/bottom ones. A single arm stops at the near double line if
        // they go straight through (╤), at the far one in a corner (╓) and crosses them if it goes on (╪); a double arm
        // stops at a single line, and its lines meet those of a perpendicular double arm in corners (╔ ╬).
        auto va = stroke(cx - lw, 1), vb = stroke(cx + lw, 1), ha = stroke(cy - lw, 1), hb = stroke(cy + lw, 1);
        bool vd = U == 3 || D == 3, hd = L == 3 || R == 3, vthru = U == 3 && D == 3, hthru = L == 3 && R == 3;
        int vl = U || D ? v1.first : mx, vr = U || D ? v1.second : mx, ht = L || R ? h1.first : my, hbt = L || R ? h1.second : my;
        if (R == 1) rect(vd ? (L ? v1.first : vthru ? vb.first : va.first) : vl, h1.first, w, h1.second);
        if (L == 1) rect(0, h1.first, vd ? (R ? v1.second : vthru ? va.second : vb.second) : vr, h1.second);
        if (D == 1) rect(v1.first, hd ? (U ? h1.first : hthru ? hb.first : ha.first) : ht, v1.second, h);
        if (U == 1) rect(v1.first, 0, v1.second, hd ? (D ? h1.second : hthru ? ha.second : hb.second) : hbt);
        if (R == 3) {
          rect(vd ? (U == 3 ? vb.first : va.first) : vl, ha.first, w, ha.second);
          rect(vd ? (D == 3 ? vb.first : va.first) : vl, hb.first, w, hb.second);
        }
        if (L == 3) {
          rect(0, ha.first, vd ? (U == 3 ? va.second : vb.second) : vr, ha.second);
          rect(0, hb.first, vd ? (D == 3 ? va.second : vb.second) : vr, hb.second);
        }
        if (D == 3) {
          rect(va.first, hd ? (L == 3 ? hb.first : ha.first) : ht, va.second, h);
          rect(vb.first, hd ? (R == 3 ? hb.first : ha.first) : ht, vb.second, h);
        }
        if (U == 3) {
          rect(va.first, 0, va.second, hd ? (L == 3 ? ha.second : hb.second) : hbt);
          rect(vb.first, 0, vb.second, hd ? (R == 3 ? ha.second : hb.second) : hbt);
        }
      }
    } else if (k < 12 || (k >= 0x4C && k < 0x50)) {  // dashes ┄┅┆┇┈┉┊┋╌╍╎╏: 3, 4 or 2 per cell, as in Windows Terminal
      int n = k >= 0x4C ? 2 : k < 8 ? 3 : 4, len = k & 2 ? h : w;
      auto s = stroke(k & 2 ? cx : cy, k & 1 ? 2 : 1);
      for (int i = 0; i < n; i++) {
        int a0 = (int)std::lround(len * i / (double)n), a1 = (int)std::lround(len * (3 * i + 2) / (3.0 * n));
        if (k & 2) rect(s.first, a0, s.second, a1);
        else rect(a0, s.first, a1, s.second);
      }
    } else if (k < 0x71) {  // ╭╮╯╰: a quarter circle between the light lines
      float X = (v1.first + v1.second) / 2.0f, Y = (h1.first + h1.second) / 2.0f;
      float r = std::min({5.0f * lw, X, W - X, Y, H - Y});
      bool right = k == 0x6D || k == 0x70, down = k <= 0x6E;
      shape([&](float x, float y) {
        float px = right ? x - X : X - x, py = down ? y - Y : Y - y;  // from the corner towards the arms
        float d = px < r && py < r ? std::fabs(std::hypot(px - r, py - r) - r)
                                   : std::min(px >= r ? std::fabs(py) : 1e9f, py >= r ? std::fabs(px) : 1e9f);
        return d <= t;
      });
    } else {  // ╱╲╳
      shape([&](float x, float y) {
        return (k != 0x72 && std::fabs(H * x + W * y - W * H) / diag <= t) || (k != 0x71 && std::fabs(H * x - W * y) / diag <= t);
      });
    }
  } else if (c < 0x25A0) {  // block elements; the shades ░▒▓ are left to BuiltinPaint
    int k = (int)c - 0x2580;
    auto fx = [&](int n) { return (int)std::lround(w * n / 8.0); };
    auto fy = [&](int n) { return (int)std::lround(h * n / 8.0); };
    if (k == 0) rect(0, 0, w, fy(4));               // ▀
    else if (k <= 8) rect(0, fy(8 - k), w, h);      // ▁▂▃▄▅▆▇█
    else if (k <= 0xF) rect(0, 0, fx(16 - k), h);   // ▉▊▋▌▍▎▏
    else if (k == 0x10) rect(fx(4), 0, w, h);       // ▐
    else if (k == 0x14) rect(0, 0, w, fy(1));       // ▔
    else if (k == 0x15) rect(fx(7), 0, w, h);       // ▕
    else if (k >= 0x16) {                           // quadrants ▖▗▘▙▚▛▜▝▞▟: 1 upper left, 2 upper right, 4 lower left, 8 lower right
      static const uint8_t kQuad[] = {4, 8, 1, 13, 9, 7, 11, 2, 6, 14};
      int q = kQuad[k - 0x16], qx = fx(4), qy = fy(4);
      if (q & 1) rect(0, 0, qx, qy);
      if (q & 2) rect(qx, 0, w, qy);
      if (q & 4) rect(0, qy, qx, h);
      if (q & 8) rect(qx, qy, w, h);
    }
  } else {  // powerline U+E0B0-U+E0BF
    switch (c - 0xE0B0) {
      case 0x0: shape([&](float x, float y) { return x <= W * (1 - std::fabs(2 * y / H - 1)); }); break;
      case 0x1: shape([&](float x, float y) { return std::min(seg(x, y, 0, 0, W - t, H / 2), seg(x, y, W - t, H / 2, 0, H)) <= t; }); break;
      case 0x2: shape([&](float x, float y) { return x >= W * std::fabs(2 * y / H - 1); }); break;
      case 0x3: shape([&](float x, float y) { return std::min(seg(x, y, W, 0, t, H / 2), seg(x, y, t, H / 2, W, H)) <= t; }); break;
      case 0x4: case 0x6:  // half discs
        shape([&](float x, float y) {
          float dx = (c == 0xE0B4 ? x : W - x) / W, dy = 2 * y / H - 1;
          return dx * dx + dy * dy <= 1;
        });
        break;
      case 0x5: case 0x7:  // half circles: distance to the ellipse, to first order
        shape([&](float x, float y) {
          float dx = c == 0xE0B5 ? x : W - x, dy = y - H / 2, a = W - t, b = H / 2 - t;
          float f = dx * dx / (a * a) + dy * dy / (b * b) - 1, g = 2 * std::hypot(dx / (a * a), dy / (b * b));
          return std::fabs(f) <= t * std::max(g, 1e-6f);
        });
        break;
      case 0x8: shape([&](float x, float y) { return y * W >= x * H; }); break;            // lower left triangle
      case 0xA: shape([&](float x, float y) { return y * W >= (W - x) * H; }); break;      // lower right
      case 0xC: shape([&](float x, float y) { return y * W <= (W - x) * H; }); break;      // upper left
      case 0xE: shape([&](float x, float y) { return y * W <= x * H; }); break;            // upper right
      case 0x9: case 0xF: shape([&](float x, float y) { return std::fabs(H * x - W * y) / diag <= t; }); break;
      default: shape([&](float x, float y) { return std::fabs(H * x + W * y - W * H) / diag <= t; }); break;
    }
  }
}

static uint32_t DibColor(COLORREF c) { return (GetRValue(c) << 16) | (GetGValue(c) << 8) | GetBValue(c); }


// Paints builtin glyph c in fg over the cell whose top left corner is (x, y) in the 32-bit pixels px (stride in pixels),
// inside clip; (ox, oy) is where px's origin is in the window, so the shade patterns line up across cells.
static void BuiltinPaint(uint32_t* px, int stride, RECT clip, int x, int y, int ox, int oy, uint32_t c, COLORREF fg) {
  int w = g_win->cellW, h = g_win->cellH;
  if (w != g_win->builtinW || h != g_win->builtinH) {
    for (auto& m : g_win->builtin) m.clear();
    for (auto& s : g_win->builtinSpan) s.clear();
    g_win->builtinW = w, g_win->builtinH = h;
  }
  uint32_t f = DibColor(fg);
  int x0 = std::max(x, (int)clip.left), x1 = std::min(x + w, (int)clip.right);
  int y0 = std::max(y, (int)clip.top), y1 = std::min(y + h, (int)clip.bottom);
  if (c >= 0x2591 && c <= 0x2593) {  // ░▒▓: a quarter, half or three quarters of a grid of dots, as in Windows Terminal
    int dot = std::max(1, (int)std::lround(std::max(w / 12.0, h / 24.0)));
    for (int yy = y0; yy < y1; yy++)
      for (int xx = x0; xx < x1; xx++) {
        int a = (xx + ox) / dot, b = (yy + oy) / dot;
        if (c == 0x2592 ? (a + b) % 2 == 0 : ((a + 2 * b) % 4 == 0) == (c == 0x2591)) px[(size_t)yy * stride + xx] = f;
      }
    return;
  }
  int i = c < 0xE000 ? c - 0x2500 : 160 + c - 0xE0B0;
  std::vector<uint8_t>& m = g_win->builtin[i];
  std::vector<uint16_t>& sp = g_win->builtinSpan[i];
  if (m.empty()) {
    m.assign((size_t)w * h, 0), sp.assign((size_t)2 * h, 0);
    BuiltinRaster(c, w, h, m.data());
    for (int r = 0; r < h; r++) {
      int a = 0, b = w;
      while (a < b && !m[(size_t)r * w + a]) a++;
      while (b > a && !m[(size_t)r * w + b - 1]) b--;
      sp[2 * r] = (uint16_t)a, sp[2 * r + 1] = (uint16_t)b;
    }
  }
  for (int yy = y0; yy < y1; yy++) {
    uint32_t* row = px + (size_t)yy * stride;
    const uint8_t* mr = m.data() + (size_t)(yy - y) * w - x;
    int s0 = std::max(x0, x + sp[2 * (yy - y)]), s1 = std::min(x1, x + sp[2 * (yy - y) + 1]);
    for (int xx = s0; xx < s1; xx++) {
      unsigned a = mr[xx], d = row[xx];
      if (a == 255) row[xx] = f;
      else if (a) {
        auto ch = [&](int s) { return ((f >> s & 255) * a + (d >> s & 255) * (255 - a) + 127) / 255 << s; };
        row[xx] = ch(16) | ch(8) | ch(0);
      }
    }
  }
}

// A builtin glyph with GDI text (WARPLITE_GDI_TEXT or no DirectWrite): the cell is composed over bg and copied.
static void GdiBuiltin(HDC dc, int x, int y, uint32_t c, COLORREF fg, COLORREF bg) {
  static std::vector<uint32_t> px;
  int w = g_win->cellW, h = g_win->cellH;
  px.assign((size_t)w * h, DibColor(bg));
  BuiltinPaint(px.data(), w, RECT{0, 0, w, h}, 0, 0, x, y, c, fg);
  BITMAPINFO bi{};
  bi.bmiHeader = {sizeof bi.bmiHeader, w, -h, 1, 32, BI_RGB};
  SetDIBitsToDevice(dc, x, y, w, h, 0, 0, 0, h, px.data(), &bi, DIB_RGB_COLORS);
}

// Terminal text through DirectWrite, like Windows Terminal: grayscale antialiasing, natural measuring and
// the system's rendering params (gamma, contrast, and the rendering mode DirectWrite picks for the font:
// natural symmetric for most fonts and sizes). Each row is composed in the DIB of a bitmap render target
// and copied to the destination DC in one BitBlt, so the GDI back buffer, row painting and screenshots
// work as before. Without DirectWrite (or with WARPLITE_GDI_TEXT set) the text goes through GDI.
struct DwLayer {  // one color layer of an emoji
  UINT16 glyph, palette;  // palette 0xFFFF: the text color
  COLORREF color;
  float alpha;
};

struct DwState {
  IDWriteFactory1* factory = nullptr;
  IDWriteFactory2* factory2 = nullptr;  // color glyphs (Windows 8.1 and later)
  IDWriteTextAnalyzer* analyzer = nullptr;
  IDWriteGdiInterop* interop = nullptr;
  IDWriteRenderingParams* params = nullptr;
  IDWriteBitmapRenderTarget* rt = nullptr;
  HDC dc = nullptr;  // the render target's memory DC: a 32-bit top-down DIB
  uint32_t* bits = nullptr;
  int stride = 0, w = 0, h = 0;
  RECT strip{};  // destination rectangle being composed
  IDWriteFontFace* faces[4] = {};
  IDWriteFontFace* fallback[kFallbackCount] = {};
  bool on = false;  // fonts bridged: the terminal text goes through DirectWrite
  bool emojiColor = false;                    // Segoe UI Emoji drawn in color
  float emoUpem = 0, emoL = 0, emoT = 0, emoR = 0, emoB = 0;  // its em and the box of 😀 (design units, y up)
  std::unordered_map<UINT16, std::vector<DwLayer>> layers;    // color layers of each emoji glyph
  std::unordered_map<std::u32string, UINT16> joined;          // glyph of each emoji sequence (0: not joined)
};
static DwState g_dw;

// Grows the render target to at least w x h.
static bool DwTarget(int w, int h) {
  if (w <= g_dw.w && h <= g_dw.h) return true;
  w = std::max(w, g_dw.w), h = std::max(h, g_dw.h);
  if (FAILED(g_dw.rt->Resize(w, h))) return false;
  g_dw.dc = g_dw.rt->GetMemoryDC();
  DIBSECTION ds{};
  if (!GetObjectW(GetCurrentObject(g_dw.dc, OBJ_BITMAP), sizeof ds, &ds) || !ds.dsBm.bmBits) return false;
  g_dw.bits = (uint32_t*)ds.dsBm.bmBits;
  g_dw.stride = ds.dsBm.bmWidthBytes / 4;
  g_dw.w = w, g_dw.h = h;
  return true;
}

static bool DwInit() {
  static bool tried = false;
  if (tried) return g_dw.rt != nullptr;
  tried = true;
  if (GetEnvironmentVariableW(L"WARPLITE_GDI_TEXT", nullptr, 0)) return false;  // the old GDI text (tests)
  typedef HRESULT(WINAPI * PFN)(DWRITE_FACTORY_TYPE, REFIID, IUnknown**);
  HMODULE m = LoadLibraryExW(L"dwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  PFN create = m ? (PFN)GetProcAddress(m, "DWriteCreateFactory") : nullptr;
  IDWriteBitmapRenderTarget* rt = nullptr;
  IDWriteBitmapRenderTarget1* rt1 = nullptr;
  if (!create || FAILED(create(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory1), (IUnknown**)&g_dw.factory)) ||
      FAILED(g_dw.factory->GetGdiInterop(&g_dw.interop)) || FAILED(g_dw.factory->CreateRenderingParams(&g_dw.params)) ||
      FAILED(g_dw.interop->CreateBitmapRenderTarget(nullptr, 1, 1, &rt)) ||
      FAILED(rt->QueryInterface(__uuidof(IDWriteBitmapRenderTarget1), (void**)&rt1)) ||
      FAILED(rt1->SetTextAntialiasMode(DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE))) {
    if (rt1) rt1->Release();
    if (rt) rt->Release();
    return false;
  }
  rt1->Release();
  rt->SetPixelsPerDip(1.0f);  // em sizes are already in pixels
  // Color emoji and the shaping of emoji sequences; without them emoji are drawn in the text color.
  if (FAILED(g_dw.factory->QueryInterface(__uuidof(IDWriteFactory2), (void**)&g_dw.factory2))) g_dw.factory2 = nullptr;
  if (FAILED(g_dw.factory->CreateTextAnalyzer(&g_dw.analyzer))) g_dw.analyzer = nullptr;
  g_dw.rt = rt;
  if (!DwTarget(1, 1)) g_dw.rt = nullptr, rt->Release();
  return g_dw.rt != nullptr;
}

// The COLR layers of an emoji glyph, as Windows Terminal draws them (empty: it has no color).
static const std::vector<DwLayer>& DwLayers(UINT16 g) {
  auto it = g_dw.layers.find(g);
  if (it != g_dw.layers.end()) return it->second;
  std::vector<DwLayer>& v = g_dw.layers[g];
  float zero = 0;
  DWRITE_GLYPH_RUN run{g_dw.fallback[kEmojiFace], 16.0f, 1, &g, &zero, nullptr, FALSE, 0};
  IDWriteColorGlyphRunEnumerator* e = nullptr;
  if (SUCCEEDED(g_dw.factory2->TranslateColorGlyphRun(0, 0, &run, nullptr, DWRITE_MEASURING_MODE_NATURAL, nullptr, 0, &e))) {
    BOOL more = FALSE;
    const DWRITE_COLOR_GLYPH_RUN* r = nullptr;
    while (SUCCEEDED(e->MoveNext(&more)) && more && SUCCEEDED(e->GetCurrentRun(&r)))
      for (UINT32 k = 0; k < r->glyphRun.glyphCount; k++)
        v.push_back(DwLayer{r->glyphRun.glyphIndices[k], r->paletteIndex,
                            RGB(std::lround(r->runColor.r * 255), std::lround(r->runColor.g * 255), std::lround(r->runColor.b * 255)),
                            r->runColor.a});
    e->Release();
  }
  return v;
}

// Color emoji: whether the emoji face has color layers, and the box of 😀 (the union of its layers) that every emoji
// is fitted to its cells by.
static void DwEmojiSetup() {
  g_dw.layers.clear(), g_dw.joined.clear(), g_dw.emojiColor = false;
  IDWriteFontFace* f = g_dw.fallback[kEmojiFace];
  IDWriteFontFace2* f2 = nullptr;
  if (!g_dw.on || !g_dw.factory2 || FAILED(f->QueryInterface(__uuidof(IDWriteFontFace2), (void**)&f2))) return;
  bool color = f2->IsColorFont();
  f2->Release();
  DWRITE_FONT_METRICS fm{};
  f->GetMetrics(&fm);
  UINT32 ref = 0x1F600;
  UINT16 gi = 0;
  float l = 1e9f, t = -1e9f, r = -1e9f, b = 1e9f;
  auto add = [&](UINT16 g) {
    DWRITE_GLYPH_METRICS gm{};
    if (FAILED(f->GetDesignGlyphMetrics(&g, 1, &gm, FALSE))) return;
    float x0 = (float)gm.leftSideBearing, x1 = (float)gm.advanceWidth - (float)gm.rightSideBearing;
    float y1 = (float)gm.verticalOriginY - (float)gm.topSideBearing;
    float y0 = (float)gm.verticalOriginY - (float)gm.advanceHeight + (float)gm.bottomSideBearing;
    if (x1 > x0 && y1 > y0) l = std::min(l, x0), r = std::max(r, x1), b = std::min(b, y0), t = std::max(t, y1);
  };
  if (!color || !fm.designUnitsPerEm || FAILED(f->GetGlyphIndices(&ref, 1, &gi)) || !gi) return;
  for (const DwLayer& ly : DwLayers(gi)) add(ly.glyph);
  if (r <= l || t <= b) return;
  g_dw.emoUpem = (float)fm.designUnitsPerEm, g_dw.emoL = l, g_dw.emoT = t, g_dw.emoR = r, g_dw.emoB = b;
  g_dw.emojiColor = true;
}

// DirectWrite faces of the GDI fonts just made: CreateFontFaceFromHdc also sees the embedded and private
// fonts, which DirectWrite's system collection doesn't. If one fails, the text stays on GDI.
static void DwMakeFaces(int em, int ascent) {
  g_win->dwEm = (float)em, g_win->dwAscent = ascent;
  // A face is the same at every size: the windows share them, made again only for another font.
  static std::wstring made;
  wchar_t face[LF_FACESIZE] = L"";
  SelectObject(g_measureDC, g_win->fonts[0]);
  GetTextFaceW(g_measureDC, LF_FACESIZE, face);
  if (g_dw.on && made == face) return;
  made.clear();
  for (auto& f : g_dw.faces)
    if (f) f->Release(), f = nullptr;
  for (auto& f : g_dw.fallback)
    if (f) f->Release(), f = nullptr;
  g_dw.on = false;
  if (!DwInit()) return;
  bool ok = true;
  auto make = [&](HFONT font, IDWriteFontFace** out) {
    SelectObject(g_measureDC, font);
    ok = SUCCEEDED(g_dw.interop->CreateFontFaceFromHdc(g_measureDC, out)) && ok;
  };
  for (int i = 0; i < 4; i++) make(g_win->fonts[i], &g_dw.faces[i]);
  for (int i = 0; i < kFallbackCount; i++) make(g_win->fallback[i], &g_dw.fallback[i]);
  g_dw.on = ok;
  if (ok) made = face;
  DwEmojiSetup();
}

// Fills r (destination coordinates) in the strip being composed.
static void DwFill(RECT r, COLORREF c) {
  const RECT& s = g_dw.strip;
  int x0 = std::max(r.left, s.left) - s.left, x1 = std::min(r.right, s.right) - s.left;
  int y0 = std::max(r.top, s.top) - s.top, y1 = std::min(r.bottom, s.bottom) - s.top;
  uint32_t px = (GetRValue(c) << 16) | (GetGValue(c) << 8) | GetBValue(c);
  for (int y = y0; x0 < x1 && y < y1; y++) std::fill_n(g_dw.bits + (size_t)y * g_dw.stride + x0, x1 - x0, px);
}

// Starts composing the destination rectangle r, filled with bg. False: draw it with GDI instead.
static bool DwBegin(RECT r, COLORREF bg) {
  if (!g_dw.on || r.right <= r.left || r.bottom <= r.top || !DwTarget(r.right - r.left, r.bottom - r.top)) return false;
  GdiFlush();  // GDI batches its calls: none may touch the DIB once it is written directly
  g_dw.strip = r;
  DwFill(r, bg);
  return true;
}

// The script analysis GetGlyphs needs, for one short string.
struct DwAnalysis : IDWriteTextAnalysisSource, IDWriteTextAnalysisSink {
  const wchar_t* text;
  UINT32 len;
  DWRITE_SCRIPT_ANALYSIS sa{};
  DwAnalysis(const wchar_t* t, UINT32 n) : text(t), len(n) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** p) override { return *p = nullptr, E_NOINTERFACE; }
  ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }
  HRESULT STDMETHODCALLTYPE GetTextAtPosition(UINT32 pos, const WCHAR** s, UINT32* n) override {
    *s = pos < len ? text + pos : nullptr, *n = pos < len ? len - pos : 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetTextBeforePosition(UINT32 pos, const WCHAR** s, UINT32* n) override {
    *s = pos && pos <= len ? text : nullptr, *n = pos <= len ? pos : 0;
    return S_OK;
  }
  DWRITE_READING_DIRECTION STDMETHODCALLTYPE GetParagraphReadingDirection() override { return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT; }
  HRESULT STDMETHODCALLTYPE GetLocaleName(UINT32 pos, UINT32* n, const WCHAR** loc) override {
    return *n = len - std::min(pos, len), *loc = L"en-us", S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetNumberSubstitution(UINT32 pos, UINT32* n, IDWriteNumberSubstitution** ns) override {
    return *n = len - std::min(pos, len), *ns = nullptr, S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetScriptAnalysis(UINT32, UINT32, const DWRITE_SCRIPT_ANALYSIS* a) override { return sa = *a, S_OK; }
  HRESULT STDMETHODCALLTYPE SetLineBreakpoints(UINT32, UINT32, const DWRITE_LINE_BREAKPOINT*) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE SetBidiLevel(UINT32, UINT32, UINT8, UINT8) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE SetNumberSubstitution(UINT32, UINT32, IDWriteNumberSubstitution*) override { return S_OK; }
};

// The glyphs the emoji face shapes a sequence into (empty if it can't).
static std::vector<UINT16> DwShapeEmoji(const uint32_t* cps, size_t n) {
  std::wstring t;
  for (size_t i = 0; i < n; i++) AppendUtf16(t, cps[i]);
  DwAnalysis an(t.c_str(), (UINT32)t.size());
  UINT16 clusters[32], glyphs[32];
  DWRITE_SHAPING_TEXT_PROPERTIES tp[32];
  DWRITE_SHAPING_GLYPH_PROPERTIES gp[32];
  UINT32 got = 0;
  if (!g_dw.analyzer || t.size() > 32 || FAILED(g_dw.analyzer->AnalyzeScript(&an, 0, an.len, &an)) ||
      FAILED(g_dw.analyzer->GetGlyphs(t.c_str(), an.len, g_dw.fallback[kEmojiFace], FALSE, FALSE, &an.sa, nullptr, nullptr,
                                      nullptr, nullptr, 0, 32, clusters, tp, glyphs, gp, &got)))
    got = 0;
  return std::vector<UINT16>(glyphs, glyphs + got);
}

// The one glyph the emoji face makes of a sequence (ZWJ, skin tone, flag), or 0: the only one with an advance.
static UINT16 DwJoined(const uint32_t* cps, size_t n) {
  std::u32string key(cps, cps + n);
  auto it = g_dw.joined.find(key);
  if (it != g_dw.joined.end()) return it->second;
  UINT16& r = g_dw.joined[key];
  int wide = 0;
  for (UINT16 g : DwShapeEmoji(cps, n)) {
    DWRITE_GLYPH_METRICS gm{};
    if (SUCCEEDED(g_dw.fallback[kEmojiFace]->GetDesignGlyphMetrics(&g, 1, &gm, FALSE)) && gm.advanceWidth) r = g, wide++;
  }
  if (wide != 1) r = 0;
  return r;
}

// Emoji glyph g in color, fitted to the box from bx (strip coordinates) bw wide and the strip's height: the box of 😀
// scaled to 7/8 of it (the cell's background, a selection or a find match, shows around the emoji) and centred, the
// glyph centred on its advance.
static void DwEmojiGlyph(UINT16 g, float bx, float bw, COLORREF fg) {
  IDWriteFontFace* face = g_dw.fallback[kEmojiFace];
  float h = (float)(g_dw.strip.bottom - g_dw.strip.top), bh = g_dw.emoT - g_dw.emoB;
  float s = std::min(bw / (g_dw.emoR - g_dw.emoL), h / bh) * 0.875f;  // px per design unit
  DWRITE_GLYPH_METRICS gm{};
  face->GetDesignGlyphMetrics(&g, 1, &gm, FALSE);
  float x = bx + (bw - gm.advanceWidth * s) / 2, y = (h - bh * s) / 2 + g_dw.emoT * s, zero = 0;
  auto draw = [&](UINT16 gi, COLORREF c) {
    DWRITE_GLYPH_RUN run{face, s * g_dw.emoUpem, 1, &gi, &zero, nullptr, FALSE, 0};
    g_dw.rt->DrawGlyphRun(x, y, DWRITE_MEASURING_MODE_NATURAL, &run, g_dw.params, c, nullptr);
  };
  const std::vector<DwLayer>& ls = DwLayers(g);
  if (ls.empty()) return draw(g, fg);
  // A translucent layer is mixed with the cell's background (the strip's top row, filled before the glyphs).
  uint32_t under = g_dw.bits[std::clamp((int)bx, 0, (int)(g_dw.strip.right - g_dw.strip.left) - 1)];
  for (const DwLayer& l : ls) {
    COLORREF c = l.palette == 0xFFFF ? fg : l.color;
    if (l.palette != 0xFFFF && l.alpha < 1) {
      auto mix = [&](int fc, int bc) { return (int)std::lround(fc * l.alpha + bc * (1 - l.alpha)); };
      c = RGB(mix(GetRValue(c), under >> 16 & 255), mix(GetGValue(c), under >> 8 & 255), mix(GetBValue(c), under & 255));
    }
    draw(l.glyph, c);
  }
}

static bool IsRegional(uint32_t c) { return c >= 0x1F1E6 && c <= 0x1F1FF; }

// Emoji in color, like Windows Terminal. libvterm gives every code point its own cells, so a sequence the font joins
// into one glyph (ZWJ, skin tone) is drawn in the first two of its cells and the rest stay blank; one it doesn't
// join (flags in Segoe UI Emoji) is drawn code point by code point.
static void DwEmoji(float ox, COLORREF fg, const uint32_t* cps, const float* adv, size_t n) {
  for (size_t i = 0, j; i < n; i = j) {
    j = i + 1;
    if (!adv[i]) continue;  // a mark with no character before it in this run
    float w = adv[i];
    int chars = 1;
    for (; j < n; j++) {
      bool tone = cps[j] >= 0x1F3FB && cps[j] <= 0x1F3FF, flag = chars == 1 && IsRegional(cps[i]) && IsRegional(cps[j]);
      if (adv[j] && cps[j - 1] != 0x200D && !tone && !flag) break;
      if (adv[j]) chars++, w += adv[j];
    }
    if (UINT16 g = chars > 1 ? DwJoined(cps + i, j - i) : 0) {
      DwEmojiGlyph(g, ox, std::min(w, 2.0f * g_win->cellW), fg);
    } else {
      float x = ox;
      for (size_t k = i; k < j; x += adv[k++]) {
        UINT16 gi = 0;
        if (adv[k] && SUCCEEDED(g_dw.fallback[kEmojiFace]->GetGlyphIndices(cps + k, 1, &gi))) DwEmojiGlyph(gi, x, adv[k], fg);
      }
    }
    ox += w;
  }
}

// One glyph per code point with its advance in px (0 for combining marks), the top of the text at y. Box drawing,
// block elements and the powerline separators are builtin glyphs over the whole cell (the strip is one cell tall);
// emoji go to DwEmoji.
static void DwGlyphs(int x, int y, IDWriteFontFace* face, COLORREF fg, const uint32_t* cps, const float* adv, size_t n) {
  static std::vector<UINT16> idx;
  float ox = (float)(x - g_dw.strip.left);
  size_t a = 0;
  while (n > a && cps[n - 1] == L' ') n--;  // blanks draw nothing
  while (a < n && cps[a] == L' ') ox += adv[a++];
  if (a == n) return;
  if (face == g_dw.fallback[kEmojiFace] && g_dw.emojiColor) return DwEmoji(ox, fg, cps + a, adv + a, n - a);
  RECT clip{0, 0, g_dw.strip.right - g_dw.strip.left, g_dw.strip.bottom - g_dw.strip.top};
  for (size_t i = a, j; i < n; i = j) {
    if (IsBuiltin(cps[i])) {
      if (adv[i]) BuiltinPaint(g_dw.bits, g_dw.stride, clip, (int)std::lround(ox), 0, g_dw.strip.left, g_dw.strip.top, cps[i], fg);
      ox += adv[i];
      j = i + 1;
      continue;
    }
    float w = 0;
    for (j = i; j < n && !IsBuiltin(cps[j]); j++) w += adv[j];
    idx.resize(j - i);
    if (FAILED(face->GetGlyphIndices(cps + i, (UINT32)(j - i), idx.data()))) return;
    for (size_t k = 0; k < j - i; k++)
      if (!idx[k] && !adv[i + k]) {  // zero-width character the face lacks (variation selector, ZWJ): no .notdef box
        UINT32 sp = L' ';
        face->GetGlyphIndices(&sp, 1, &idx[k]);
      }
    DWRITE_GLYPH_RUN run{face, g_win->dwEm, (UINT32)(j - i), idx.data(), adv + i, nullptr, FALSE, 0};
    g_dw.rt->DrawGlyphRun(ox, (float)(y - g_dw.strip.top + g_win->dwAscent), DWRITE_MEASURING_MODE_NATURAL, &run, g_dw.params,
                          fg, nullptr);
    ox += w;
  }
}

// Like DwGlyphs with a style of the terminal font; what it lacks goes to its fallback face.
static void DwText(int x, int y, int font, COLORREF fg, const uint32_t* cps, const float* adv, size_t n) {
  for (size_t i = 0, j; i < n; i = j) {
    int fb = FallbackFor(cps[i]);
    float w = adv[i];
    for (j = i + 1; j < n && FallbackFor(cps[j]) == fb; j++) w += adv[j];
    DwGlyphs(x, y, fb ? g_dw.fallback[fb - 1] : g_dw.faces[font], fg, cps + i, adv + i, j - i);
    x += (int)w;
  }
}

// Copies the composed strip to dc.
static void DwEnd(HDC dc) {
  const RECT& s = g_dw.strip;
  BitBlt(dc, s.left, s.top, s.right - s.left, s.bottom - s.top, g_dw.dc, 0, 0, SRCCOPY);
}

static void MakeFonts() {
  for (auto& f : g_win->fonts)
    if (f) DeleteObject(f), f = nullptr;
  if (g_win->uiFont) DeleteObject(g_win->uiFont);
  int h = -MulDiv(g_win->fontPx, ScaledDpi(), 96);
  std::wstring face = g_fontFace;
  HDC dc = GetDC(nullptr);
  for (int i = 0; i < 4; i++) {
    g_win->fonts[i] = CreateFontW(h, 0, 0, 0, (i & 1) ? FW_BOLD : FW_NORMAL, (i & 2) ? TRUE : FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             FIXED_PITCH | FF_MODERN, face.c_str());
    if (i == 0) {
      HGDIOBJ old = SelectObject(dc, g_win->fonts[0]);
      wchar_t got[64];
      GetTextFaceW(dc, 64, got);
      SelectObject(dc, old);
      if (_wcsicmp(got, face.c_str()) != 0 && face != L"Consolas") {  // face missing: Hack, then Consolas
        DeleteObject(g_win->fonts[0]);
        face = face == L"Hack" ? L"Consolas" : L"Hack";
        i = -1;
        continue;
      }
    }
  }
  g_win->uiFont = CreateFontW(-Px(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, 0, L"Segoe UI");
  if (g_win->badgeFont) DeleteObject(g_win->badgeFont);
  g_win->badgeFont = CreateFontW(-Px(10), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, 0, L"Segoe UI");
  if (g_win->iconFont) DeleteObject(g_win->iconFont);
  g_win->iconFont = CreateFontW(-Px(9), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, 0, L"Segoe MDL2 Assets");
  HGDIOBJ old = SelectObject(dc, g_win->fonts[0]);
  TEXTMETRICW tm;
  GetTextMetricsW(dc, &tm);
  SIZE sz;
  GetTextExtentPoint32W(dc, L"M", 1, &sz);
  SelectObject(dc, old);
  ReleaseDC(nullptr, dc);
  g_win->cellW = std::max(1, (int)sz.cx);
  if (!g_measureDC) g_measureDC = CreateCompatibleDC(nullptr);
  for (int i = 0; i < kFallbackCount; i++) {
    if (g_win->fallback[i]) DeleteObject(g_win->fallback[i]);
    g_win->fallback[i] = CreateFontW(h, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, 0, kFallbackFaces[i]);
  }
  DwMakeFaces(-h, (int)tm.tmAscent);
  if (g_dw.on) {  // unhinted glyphs: the cell is the advance of "0" rounded, as in Windows Terminal
    DWRITE_FONT_METRICS fm{};
    g_dw.faces[0]->GetMetrics(&fm);
    UINT32 zero = L'0';
    UINT16 gi = 0;
    DWRITE_GLYPH_METRICS gm{};
    if (fm.designUnitsPerEm && SUCCEEDED(g_dw.faces[0]->GetGlyphIndices(&zero, 1, &gi)) && gi &&
        SUCCEEDED(g_dw.faces[0]->GetDesignGlyphMetrics(&gi, 1, &gm, FALSE)))
      g_win->cellW = std::max(1, (int)(gm.advanceWidth * g_win->dwEm / fm.designUnitsPerEm + 0.5f));
  }
  // Altura de línea de Warp: 1,2 × tamaño de letra (DEFAULT_UI_LINE_HEIGHT_RATIO).
  g_win->cellH = std::max((int)tm.tmHeight, (int)(MulDiv(g_win->fontPx, ScaledDpi(), 96) * 1.2 + 0.5));
  g_win->textDy = (g_win->cellH - (int)tm.tmHeight) / 2;
}

static COLORREF ResolveColor(Pane* p, VTermColor c, bool fg) {
  if (fg && VTERM_COLOR_IS_DEFAULT_FG(&c)) return g_termFg;
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
  bool bi;  // builtin glyph (box drawing, blocks, powerline)
  bool operator!=(const Style& o) const {
    return fg != o.fg || bg != o.bg || font != o.font || fb != o.fb || underline != o.underline || strike != o.strike ||
           bi != o.bi;
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
  SelectObject(dc, g_win->fonts[1]);
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
  SelectObject(dc, g_win->fonts[1]);
  Text(dc, path, RECT{chip.left + iconW, chip.top, chip.right - Px(6), chip.bottom}, kFg, DT_LEFT | DT_PATH_ELLIPSIS);
  // Orden
  int ey = chip.bottom + Px(10);
  RECT er{x, ey, r.right - Px(12), ey + g_win->cellH};
  SelectObject(dc, g_win->fonts[0]);
  bool ready = EditorActive(p);
  int cx = x;
  if (!ready) {
    Text(dc, T(S_RUNNING), er, kTabText, DT_LEFT | DT_END_ELLIPSIS);
  } else if (p->edit.empty()) {
    Text(dc, T(S_TYPE_COMMAND), er, RGB(0x6a, 0x6a, 0x6a), DT_LEFT);
  } else {
    size_t cols = std::max(1, (int)(er.right - er.left) / g_win->cellW);
    size_t off = p->caret >= cols ? p->caret - cols + 1 : 0;
    std::wstring vis = p->edit.substr(off, cols);
    std::vector<INT> dx(vis.size(), g_win->cellW);
    cx = x + (int)(p->caret - off) * g_win->cellW;
    // Como Warp: la orden en verde si existe; subrayado rojo discontinuo si no.
    size_t a = p->edit.find_first_not_of(L' ');
    size_t b = a == std::wstring::npos ? a : p->edit.find_first_of(L" \t|;", a);
    if (b == std::wstring::npos) b = p->edit.size();
    static const std::wstring kNeutral = L"$(\"'@&[{0123456789";
    int ex = -1;
    size_t va = 0, vb = 0;
    if (a != std::wstring::npos && kNeutral.find(p->edit[a]) == std::wstring::npos && !(p->edit[a] == L'.' && b - a == 1)) {
      ex = CommandExists(p, p->edit.substr(a, b - a));
      va = std::max(a, off), vb = std::min(b, off + cols);
    }
    if (DwBegin(er, kBg)) {  // each UTF-16 unit is a cell, as with GDI; the command in green, the rest in white
      std::vector<uint32_t> cps;
      std::vector<float> adv;
      std::vector<char> green;
      for (size_t i = 0; i < vis.size(); i++) {
        uint32_t c = vis[i];
        bool pair = c >= 0xD800 && c < 0xDC00 && i + 1 < vis.size() && vis[i + 1] >= 0xDC00 && vis[i + 1] < 0xE000;
        green.push_back(ex == 1 && off + i >= va && off + i < vb);
        if (pair) c = 0x10000 + ((c - 0xD800) << 10) + (vis[++i] - 0xDC00);
        cps.push_back(c);
        adv.push_back((float)(g_win->cellW * (pair ? 2 : 1)));
      }
      int sx = x;
      for (size_t i = 0, j; i < cps.size(); i = j) {
        float w = 0;
        for (j = i; j < cps.size() && green[j] == green[i]; j++) w += adv[j];
        DwText(sx, ey, 0, green[i] ? kCmdOk : kFg, &cps[i], &adv[i], j - i);
        sx += (int)w;
      }
      DwEnd(dc);
    } else {
      SetTextColor(dc, kFg);
      ExtTextOutW(dc, x, ey, ETO_CLIPPED, &er, vis.c_str(), (UINT)vis.size(), dx.data());
      if (ex == 1 && va < vb) {
        SetTextColor(dc, kCmdOk);
        ExtTextOutW(dc, x + (int)(va - off) * g_win->cellW, ey, ETO_CLIPPED, &er, vis.c_str() + (va - off), (UINT)(vb - va), dx.data());
      }
    }
    if (ex == 0 && va < vb) {
      int x0 = x + (int)(va - off) * g_win->cellW, x1 = x + (int)(vb - off) * g_win->cellW;
      int uy = ey + g_win->cellH - std::max(1, Px(1));
      for (int ux = x0; ux < x1; ux += Px(4)) Fill(dc, RECT{ux, uy, std::min(ux + Px(2), x1), uy + std::max(1, Px(1))}, kCmdBad);
    }
  }
  if (ready && active && focus) Fill(dc, RECT{cx, ey, cx + std::max(2, Px(2)), ey + g_win->cellH}, kFg);
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
  std::vector<uint32_t> cps;  // DirectWrite: code points and advances of the run
  std::vector<float> adv;
  std::map<int, std::wstring> rowCache;  // textos de fila para detectar URLs
  auto rowText = [&](int r) -> const std::wstring& {
    auto it = rowCache.find(r);
    return it != rowCache.end() ? it->second : (rowCache[r] = RowText(p, r));
  };
  for (int row = 0; row < p->rows; row++) {
    int y = oy + row * g_win->cellH;
    if (!full) {
      if (!p->dirtyRows[row]) continue;
      RECT rr{p->content.left, y, p->content.right, y + g_win->cellH};
      Fill(dc, rr, kBg);
      if (drawn.right == drawn.left) drawn = rr;
      else UnionRect(&drawn, &drawn, &rr);
    }
    // DirectWrite: the whole row (padding included) is composed off-screen and copied at the end.
    bool comp = DwBegin(RECT{p->content.left, y, p->content.right, y + g_win->cellH}, kBg);
    std::vector<char> url = UrlMask(p, row, rowText);
    std::vector<char> path = PathMask(p, row, rowText(row));
    int col = 0;
    while (col < p->cols) {
      int start = col;
      Style st{};
      txt.clear();
      dx.clear();
      cps.clear();
      adv.clear();
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
        // Paths that exist: their color over the program's (Claude Code colors inline code), but URLs keep theirs and
        // nothing on a background of its own (diffs, bars) or in reverse video changes.
        if (path[col] && !url[col] && VTERM_COLOR_IS_DEFAULT_BG(&c.bg) && !c.attrs.reverse) s.fg = kPathFg;
        if (c.attrs.dim)  // atenuado (SGR 2): a medio camino entre texto y fondo
          s.fg = RGB((GetRValue(s.fg) + GetRValue(s.bg)) / 2, (GetGValue(s.fg) + GetGValue(s.bg)) / 2,
                     (GetBValue(s.fg) + GetBValue(s.bg)) / 2);
        if (c.attrs.reverse) std::swap(s.fg, s.bg);
        if (InSelection(p, row, col)) s.bg = kSelBg;
        if (int f = FindMark(p, row, col)) s.bg = f == 2 ? kFindCur : kFindBg, s.fg = RGB(0, 0, 0);
        s.font = (c.attrs.bold ? 1 : 0) | (c.attrs.italic ? 2 : 0);
        s.fb = c.chars[0] ? FallbackFor(c.chars[0]) : 0;
        if (s.fb != kEmojiFace + 1 && EmojiPresentation(c)) s.fb = kEmojiFace + 1;
        s.underline = c.attrs.underline;
        s.strike = c.attrs.strike;
        s.bi = IsBuiltin(c.chars[0]);
        if (col != start && s != st) break;
        st = s;
        int w = std::max(1, (int)c.width);
        if (comp || st.bi) {
          cps.push_back(c.chars[0] ? c.chars[0] : L' ');
          adv.push_back((float)(g_win->cellW * w));
          for (int i = 1; i < VTERM_MAX_CHARS_PER_CELL && c.chars[0] && c.chars[i]; i++)
            cps.push_back(c.chars[i]), adv.push_back(0);
        } else if (!c.chars[0]) {
          txt += L' ';
          dx.push_back(g_win->cellW * w);
        } else {
          for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && c.chars[i]; i++) {
            size_t before = txt.size();
            AppendUtf16(txt, c.chars[i]);
            for (size_t k = before; k < txt.size(); k++) dx.push_back(i == 0 && k == before ? g_win->cellW * w : 0);
          }
        }
        col += w;
      }
      RECT r{ox + start * g_win->cellW, y, ox + col * g_win->cellW, y + g_win->cellH};
      if (comp) {
        if (st.bg != kBg) DwFill(r, st.bg);
        DwGlyphs(r.left, y + g_win->textDy, st.fb ? g_dw.fallback[st.fb - 1] : g_dw.faces[st.font], st.fg, cps.data(),
                 adv.data(), cps.size());
      } else if (st.bi) {  // GDI text: the builtin glyphs too
        Fill(dc, r, st.bg);
        float bx = (float)r.left;
        for (size_t k = 0; k < cps.size(); bx += adv[k++])
          if (adv[k]) GdiBuiltin(dc, (int)bx, y, cps[k], st.fg, st.bg);
      } else {
        SelectObject(dc, st.fb ? g_win->fallback[st.fb - 1] : g_win->fonts[st.font]);
        SetTextColor(dc, st.fg);
        SetBkColor(dc, st.bg);
        ExtTextOutW(dc, r.left, y + g_win->textDy, ETO_OPAQUE | ETO_CLIPPED, &r, txt.c_str(), (UINT)txt.size(), dx.data());
      }
      if (st.underline || st.strike) {
        int ly = st.strike ? y + g_win->cellH / 2 : y + g_win->cellH - 2;
        if (comp) DwFill(RECT{r.left, ly, r.right, ly + 1}, st.fg);
        else Fill(dc, RECT{r.left, ly, r.right, ly + 1}, st.fg);
      }
    }
    if (comp) DwEnd(dc);
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
    RECT cr{ox + p->cursor.col * g_win->cellW, oy + p->cursor.row * g_win->cellH, 0, 0};
    cr.right = cr.left + g_win->cellW;
    cr.bottom = cr.top + g_win->cellH;
    if (showFocus) {
      VTermScreenCell c;
      CellAt(p, p->cursor.row, p->cursor.col, &c);
      bool glyph = c.chars[0] && c.chars[0] != (uint32_t)-1;
      if (DwBegin(cr, kFg)) {
        uint32_t cp = glyph ? c.chars[0] : L' ';
        float a = (float)g_win->cellW;
        DwText(cr.left, cr.top + g_win->textDy, 0, kBg, &cp, &a, 1);
        DwEnd(dc);
      } else {
        std::wstring ch;
        if (glyph) AppendUtf16(ch, c.chars[0]);
        SetBkColor(dc, kFg);
        SetTextColor(dc, kBg);
        SelectObject(dc, g_win->fonts[0]);
        ExtTextOutW(dc, cr.left, cr.top + g_win->textDy, ETO_OPAQUE | ETO_CLIPPED, &cr, ch.c_str(), (UINT)ch.size(), nullptr);
      }
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
  if (g_win->link.p != p) return;
  int x = p->content.left + kPad, t = std::max(1, Px(1));
  for (auto& [row, c0, c1] : g_win->link.segs) {  // todos los trozos, también los de otras filas
    int y = p->content.top + kPad + (row + 1) * g_win->cellH - t;
    Fill(dc, RECT{x + c0 * g_win->cellW, y, x + c1 * g_win->cellW, y + t}, kAccent);
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
  std::set<Window*> changed;  // windows to draw again
  for (Pane* p : g_live) {
    if (!p->pid) continue;
    const std::wstring* exe = nullptr;
    for (auto& c : children)
      if (c.first == p->pid) exe = &c.second;
    std::wstring want;
    if (exe) want = p->runningCmd.empty() ? (p->submittedCmd.empty() ? *exe : ToWide(p->submittedCmd)) : p->runningCmd;
    if (want != p->runningCmd) p->runningCmd = want, changed.insert(p->win);
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
    if (name != p->agentName) p->agentName = name, changed.insert(p->win);
  }
  for (Window* w : changed)
    if (w) InvalidateRect(w->hwnd, nullptr, FALSE), SessionChanged(w);  // what runs is in its session (relaunched, resumed)
}

static void Text(HDC dc, const std::wstring& s, RECT r, COLORREF c, UINT flags) {
  SetTextColor(dc, c);
  DrawTextW(dc, s.c_str(), (int)s.size(), &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | flags);
}

// Unread count pill: accent, white bold number, «9+» above 9. BadgeRect centres it on (cx, cy).
static std::wstring BadgeLabel(int n) { return n > 9 ? L"9+" : std::to_wstring(n); }

static RECT BadgeRect(HDC dc, int n, int cx, int cy) {
  std::wstring t = BadgeLabel(n);
  HGDIOBJ of = SelectObject(dc, g_win->badgeFont);
  SIZE sz{};
  GetTextExtentPoint32W(dc, t.c_str(), (int)t.size(), &sz);
  SelectObject(dc, of);
  int h = Px(16), w = std::max(h, (int)sz.cx + Px(8));
  return RECT{cx - w / 2, cy - h / 2, cx - w / 2 + w, cy - h / 2 + h};
}

// How much of pixel (x, y) a w x h capsule (a circle when w == h) covers, antialiased by distance to its spine.
static float CapsuleCover(int x, int y, int w, int h) {
  float rad = h / 2.0f, fx = x + 0.5f, fy = y + 0.5f;
  float dx = std::max(0.0f, std::max(rad - fx, fx - (w - rad))), dy = fy - rad;
  return std::min(1.0f, std::max(0.0f, rad - std::sqrt(dx * dx + dy * dy) + 0.5f));
}

static void DrawBadge(HDC dc, int n, RECT r) {
  int w = r.right - r.left, h = r.bottom - r.top;
  BITMAPINFO bi{};
  bi.bmiHeader = {sizeof bi.bmiHeader, w, -h, 1, 32, BI_RGB};
  void* bits = nullptr;
  HDC mdc = CreateCompatibleDC(dc);
  HBITMAP bmp = CreateDIBSection(mdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (bits) {  // premultiplied accent, blended over what is there
    uint32_t* px = (uint32_t*)bits;
    for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++) {
        float a = CapsuleCover(x, y, w, h);
        px[(size_t)y * w + x] = (uint32_t)(a * 255 + 0.5f) << 24 | (uint32_t)(GetRValue(kAccent) * a + 0.5f) << 16 |
                                (uint32_t)(GetGValue(kAccent) * a + 0.5f) << 8 | (uint32_t)(GetBValue(kAccent) * a + 0.5f);
      }
    HGDIOBJ ob = SelectObject(mdc, bmp);
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    GdiAlphaBlend(dc, r.left, r.top, w, h, mdc, 0, 0, w, h, bf);
    SelectObject(mdc, ob);
  }
  DeleteObject(bmp);
  DeleteDC(mdc);
  HGDIOBJ of = SelectObject(dc, g_win->badgeFont);
  Text(dc, BadgeLabel(n), r, kFg, DT_CENTER);
  SelectObject(dc, of);
}

// The attention pulse (see «Attention»): the pane's outline (DrawAttention), its header and its tab take the accent in
// and out, by PulseLevel.
static const DWORD kPulseMs = 1200;  // one breath, bright to dim and back
static int g_motion = -1;            // Windows animates (SPI_GETCLIENTAREAANIMATION); -1: ask again
static int g_motionTest = -1;        // tests: 0 or 1 instead of Windows' setting
static int g_pulseAt = -1;           // tests: that point of the breath (ms) instead of the clock

static bool Motion() {
  if (g_motionTest >= 0) return g_motionTest;
  if (g_motion < 0) {
    BOOL on = TRUE;
    g_motion = !SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0) || on;
  }
  return g_motion;
}

// How much accent: a cosine from all of it down to 0.15 and back; steady at 0.8 when Windows doesn't animate.
static float PulseLevel() {
  if (!Motion()) return 0.8f;
  DWORD t = g_pulseAt >= 0 ? (DWORD)g_pulseAt : GetTickCount();
  float c = std::cos((float)(t % kPulseMs) / kPulseMs * 6.2831853f);
  return 0.15f + 0.85f * (0.5f + 0.5f * c);
}

static COLORREF Mix(COLORREF a, COLORREF b, float k) {
  auto ch = [k](int x, int y) { return (int)(x + (y - x) * k + 0.5f); };
  return RGB(ch(GetRValue(a), GetRValue(b)), ch(GetGValue(a), GetGValue(b)), ch(GetBValue(a), GetBValue(b)));
}

// The header and the tab behind their text: a share of the outline's accent, so the text stays readable.
static const float kPulseFill = 0.4f;

static bool TabPulses(int i) {
  std::vector<Pane*> ls;
  Leaves(g_win->tabs[i].root, ls);
  return std::any_of(ls.begin(), ls.end(), [](Pane* p) { return p->attention; });
}

static void DrawTabItem(HDC dc, int i, RECT r, bool vertical) {
  bool act = i == g_win->activeTab, hover = g_win->hoverTab == i;
  if (act || hover) Fill(dc, r, act ? kTabActiveBg : RGB(0x1a, 0x1a, 0x1a));
  if (act && TabPulses(i)) Fill(dc, r, Mix(kTabActiveBg, kAccent, kPulseFill * PulseLevel()));  // a pane in view pulses
  if (vertical && act) Fill(dc, RECT{r.left, r.top, r.left + Px(3), r.bottom}, kAccent);
  RECT tr{r.left + (vertical ? Px(14) : Px(28)), r.top, r.right - Px(28), r.bottom};
  UINT align = vertical ? DT_LEFT : DT_CENTER;
  if (i == g_win->renameTab) {  // editando el nombre: texto con cursor
    Fill(dc, r, kTabActiveBg);
    Fill(dc, RECT{r.left, r.bottom - Px(2), r.right, r.bottom}, kAccent);
    SIZE sz{};
    GetTextExtentPoint32W(dc, g_win->renameText.c_str(), (int)g_win->renameText.size(), &sz);
    int cx = vertical ? std::min<int>(tr.left + sz.cx, tr.right) : std::min((int)(tr.left + tr.right + sz.cx) / 2, (int)tr.right);
    if (g_win->renameAll && sz.cx > 0)  // selección de todo el nombre
      Fill(dc, RECT{std::max(cx - (int)sz.cx, (int)tr.left), r.top + Px(8), cx + 1, r.bottom - Px(8)}, kSelBg);
    Text(dc, g_win->renameText, tr, kFg, align);
    Fill(dc, RECT{cx + 1, r.top + Px(9), cx + 1 + std::max(1, Px(1)), r.bottom - Px(9)}, kFg);
    return;
  }
  const std::wstring& label = g_win->tabs[i].name.empty() ? ShortTitle(g_win->tabs[i].active) : g_win->tabs[i].name;
  if (int n = TabUnread(g_win->tabs[i])) {  // across the top: in the empty left padding; at the side: where the × goes
    RECT xr = TabCloseRect(r), b = BadgeRect(dc, n, vertical ? (xr.left + xr.right) / 2 : r.left + Px(15), (r.top + r.bottom) / 2);
    if (vertical && (act || hover)) OffsetRect(&b, xr.left - Px(2) - b.right, 0), tr.right = std::min(tr.right, b.left - Px(4));
    DrawBadge(dc, n, b);
  }
  Text(dc, label, tr, act ? kFg : kTabText, align | DT_END_ELLIPSIS);
  if (act || hover) {
    RECT xr = TabCloseRect(r);
    bool hx = g_win->hoverHit == HIT_TABCLOSE && hover;
    SelectObject(dc, g_win->iconFont);
    Text(dc, L"", xr, hx ? kFg : kTabText, DT_CENTER);
    SelectObject(dc, g_win->uiFont);
  }
}

static void DrawTitleBar(HDC dc, int w, int h) {
  int th = TabBarH();
  Fill(dc, RECT{0, 0, w, th}, kTitleBg);
  if (g_win->vertTabs) {
    Fill(dc, RECT{0, th, SidebarW(), h}, kTitleBg);
    Fill(dc, RECT{SidebarW() - 1, th, SidebarW(), h}, kDivider);
  }
  SetBkMode(dc, TRANSPARENT);
  SelectObject(dc, g_win->uiFont);
  TabLayout L = ComputeTabs();
  // Botón para alternar pestañas arriba / en barra lateral (dos rectángulos)
  {
    RECT r = L.toggle;
    bool hov = g_win->hoverHit == HIT_TOGGLE;
    if (hov || g_win->vertTabs) Fill(dc, r, hov ? RGB(0x2a, 0x2a, 0x2a) : RGB(0x1e, 0x1e, 0x1e));
    COLORREF c = hov || g_win->vertTabs ? kFg : kTabText;
    int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, iw = Px(16), ih = Px(12);
    RECT o{cx - iw / 2, cy - ih / 2, cx + iw / 2, cy + ih / 2};
    HBRUSH b = CreateSolidBrush(c);
    FrameRect(dc, &o, b);
    DeleteObject(b);
    Fill(dc, RECT{o.left + Px(5), o.top, o.left + Px(5) + 1, o.bottom}, c);
  }
  for (auto& [i, r] : L.tabs) DrawTabItem(dc, i, r, g_win->vertTabs);
  if (g_win->hoverHit == HIT_PLUS) Fill(dc, L.plus, RGB(0x1a, 0x1a, 0x1a));
  Text(dc, L"+", L.plus, g_win->hoverHit == HIT_PLUS ? kFg : kTabText, DT_CENTER);
  if (L.hiddenBtn.right > L.hiddenBtn.left) {
    if (g_win->hoverHit == HIT_HIDDEN) Fill(dc, L.hiddenBtn, RGB(0x1a, 0x1a, 0x1a));
    RECT tr = L.hiddenBtn;
    if (int n = HiddenUnread()) {  // unread in the hidden tabs, at its right end
      RECT b = BadgeRect(dc, n, 0, (tr.top + tr.bottom) / 2);
      OffsetRect(&b, tr.right - Px(6) - b.right, 0);
      DrawBadge(dc, n, b);
      tr.right = b.left;
    }
    Text(dc, Tf(S_HIDDEN_N, HiddenCount()), tr, g_win->hoverHit == HIT_HIDDEN ? kFg : kTabText, DT_CENTER);
  }
  std::wstring toast = g_win->toast;
  if (!g_fontLoading.empty()) {  // font download in progress
    wchar_t b[160];
    swprintf(b, 160, T(S_UPD_DOWNLOADING), g_fontLoading.c_str());
    int pct = g_fontPct;
    if (pct >= 0 && g_fontFiles) swprintf(b + wcslen(b), 160 - wcslen(b), L" %d %%", (g_fontFile * 100 + pct) / g_fontFiles);
    toast = b;
  }
  int right = w - 3 * WinBtnW() - Px(12);
  if (g_remotePhones > 0 || g_win->phoneTyping) {  // a phone is connected (warp-remote)
    RECT pr{right - Px(22), 0, right, th};
    SelectObject(dc, g_win->iconFont);
    Text(dc, L"\uE8EA", pr, kAccent, DT_CENTER);  // CellPhone
    SelectObject(dc, g_win->uiFont);
    right = pr.left - Px(6);
    if (g_win->phoneTyping) {  // and it typed into a pane of this window a moment ago
      SIZE sz{};
      GetTextExtentPoint32W(dc, T(S_PHONE_TYPING), (int)wcslen(T(S_PHONE_TYPING)), &sz);
      RECT tr{right - sz.cx, 0, right, th};
      Text(dc, T(S_PHONE_TYPING), tr, kAccent, DT_RIGHT);
      right = tr.left - Px(10);
    }
  }
  if (!toast.empty()) {
    RECT tr{L.end + Px(10), 0, right, th};
    Text(dc, toast, tr, kFg, DT_RIGHT);
  }
  // Minimizar, maximizar/restaurar, cerrar
  SelectObject(dc, g_win->iconFont);
  int bw = WinBtnW();
  const wchar_t* glyphs[3] = {L"", IsZoomed(g_win->hwnd) ? L"" : L"", L""};
  for (int k = 0; k < 3; k++) {
    RECT r{w - (3 - k) * bw, 0, w - (2 - k) * bw, th};
    bool hover = g_win->hoverHit == HIT_MIN + k;
    if (hover) Fill(dc, r, k == 2 ? kCloseHover : RGB(0x2a, 0x2a, 0x2a));
    Text(dc, glyphs[k], r, hover ? kFg : kTabText, DT_CENTER);
  }
  SetBkMode(dc, OPAQUE);
}

static void DrawPaneHeader(HDC dc, Pane* p, bool active) {
  RECT h = HeaderRect(p);
  if (h.bottom <= h.top) return;
  COLORREF bg = active ? kHdrActiveBg : kBg;
  Fill(dc, h, p->attention ? Mix(bg, kAccent, kPulseFill * PulseLevel()) : bg);
  SetBkMode(dc, TRANSPARENT);
  SelectObject(dc, g_win->uiFont);
  RECT tr{h.left + Px(34), h.top, h.right - Px(34), h.bottom};
  if (p->unread) DrawBadge(dc, p->unread, BadgeRect(dc, p->unread, h.left + Px(17), (h.top + h.bottom) / 2));
  Text(dc, ShortTitle(p), tr, active ? kFg : kTabText, DT_CENTER | DT_END_ELLIPSIS);
  RECT xr{h.right - Px(30), h.top, h.right - Px(6), h.bottom};
  SelectObject(dc, g_win->iconFont);
  Text(dc, L"\uE8BB", xr, kTabText, DT_CENTER);
  SetBkMode(dc, OPAQUE);
}

// Attention outline (see «Attention» and PulseLevel): an accent frame on the inner edge of the pane, inside the text's
// kPad margin so it never covers a cell, mixed into what is under it (the header's or the terminal's background).
// Solid fills, no blending: a pulse frame costs a few of them. The rows and the input box of DrawPane cover its sides
// and bottom, so it is drawn again after them.
// The frame's sides: left, right, top, bottom.
static void AttentionSides(Pane* p, RECT s[4]) {
  RECT r = p->rc;
  int w = std::min(Px(3), kPad);
  s[0] = RECT{r.left, r.top, r.left + w, r.bottom};
  s[1] = RECT{r.right - w, r.top, r.right, r.bottom};
  s[2] = RECT{r.left, r.top, r.right, r.top + w};
  s[3] = RECT{r.left, r.bottom - w, r.right, r.bottom};
}

static void DrawAttention(HDC dc, Pane* p) {
  if (!p->attention) return;
  float k = PulseLevel();
  RECT h = HeaderRect(p), s[4];
  AttentionSides(p, s);
  COLORREF head = Mix(p == CurTab().active ? kHdrActiveBg : kBg, kAccent, k), body = Mix(kBg, kAccent, k);
  for (RECT r : s) {
    RECT a = r, b = r;  // the part over the header (split panes only) and the rest
    a.bottom = std::min(r.bottom, h.bottom), b.top = std::max(r.top, h.bottom);
    if (h.bottom > h.top && a.bottom > a.top) Fill(dc, a, head);
    if (b.bottom > b.top) Fill(dc, b, body);
  }
}

// Copies the frame from the back buffer to the window.
static void BlitAttention(HDC to, HDC from, Pane* p) {
  RECT s[4];
  AttentionSides(p, s);
  for (RECT r : s) BitBlt(to, r.left, r.top, r.right - r.left, r.bottom - r.top, from, r.left, r.top, SRCCOPY);
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
  Pane* p = g_win->find.p;
  g_win->find.rc = g_win->find.prev = g_win->find.next = g_win->find.close = RECT{};
  if (!g_win->find.open || !p || !g_live.count(p) || p != CurTab().active) return;
  RECT c = p->content;
  int w = std::min<int>(Px(340), c.right - c.left - Px(16));
  RECT r{c.right - w - Px(8), c.top + Px(8), c.right - Px(8), c.top + Px(8) + Px(34)};
  g_win->find.rc = r;
  Panel(dc, r);
  SetBkMode(dc, TRANSPARENT);
  // Previous, next and close buttons at the right end.
  int bw = Px(26);
  g_win->find.close = RECT{r.right - Px(4) - bw, r.top + Px(4), r.right - Px(4), r.bottom - Px(4)};
  g_win->find.next = RECT{g_win->find.close.left - bw, g_win->find.close.top, g_win->find.close.left, g_win->find.close.bottom};
  g_win->find.prev = RECT{g_win->find.next.left - bw, g_win->find.close.top, g_win->find.next.left, g_win->find.close.bottom};
  POINT mp;
  GetCursorPos(&mp);
  ScreenToClient(g_win->hwnd, &mp);
  bool any = !g_win->find.matches.empty();
  SelectObject(dc, g_win->iconFont);
  const RECT* btn[3] = {&g_win->find.prev, &g_win->find.next, &g_win->find.close};
  const wchar_t* glyph[3] = {L"\uE70E", L"\uE70D", L"\uE711"};  // ChevronUp, ChevronDown, Cancel
  for (int k = 0; k < 3; k++) {
    bool on = k == 2 || any;
    if (on && PtInRect(btn[k], mp)) Fill(dc, *btn[k], RGB(0x3a, 0x3a, 0x3a));
    Text(dc, glyph[k], *btn[k], on ? kFg : RGB(0x5a, 0x5a, 0x5a), DT_CENTER);
  }
  SelectObject(dc, g_win->uiFont);
  std::wstring count = g_win->find.query.empty() ? L""
                       : g_win->find.matches.empty() ? T(S_NO_RESULTS)
                       : std::to_wstring(g_win->find.cur + 1) + L"/" + std::to_wstring(g_win->find.matches.size());
  RECT cr{g_win->find.prev.left - Px(90), r.top, g_win->find.prev.left - Px(6), r.bottom};
  Text(dc, count, cr, kTabText, DT_RIGHT);
  RECT qr{r.left + Px(10), r.top, cr.left - Px(6), r.bottom};
  SelectObject(dc, g_win->fonts[0]);
  if (g_win->find.query.empty()) Text(dc, T(S_FIND), qr, RGB(0x6a, 0x6a, 0x6a), DT_LEFT);
  else Text(dc, g_win->find.query, qr, kFg, DT_LEFT | DT_END_ELLIPSIS);
  SIZE sz{};
  GetTextExtentPoint32W(dc, g_win->find.query.c_str(), (int)g_win->find.query.size(), &sz);
  int cx = std::min<int>(qr.left + sz.cx + 1, qr.right);
  Fill(dc, RECT{cx, r.top + Px(9), cx + std::max(2, Px(2)), r.bottom - Px(9)}, kFg);
  SetBkMode(dc, OPAQUE);
}

// Auto-reconnect status: a small card at the top of the pane.
static void DrawReconnect(HDC dc, Pane* p) {
  std::wstring msg = ReconnectLabel(p);
  if (msg.empty()) return;
  SelectObject(dc, g_win->uiFont);
  SIZE sz{};
  GetTextExtentPoint32W(dc, msg.c_str(), (int)msg.size(), &sz);
  RECT c = p->content;
  int w = std::min<int>(sz.cx + Px(32), c.right - c.left - Px(16)), x = (c.left + c.right - w) / 2;
  RECT r{x, c.top + Px(8), x + w, c.top + Px(8) + Px(34)};
  Panel(dc, r);
  SetBkMode(dc, TRANSPARENT);
  Text(dc, msg, RECT{r.left + Px(16), r.top, r.right - Px(16), r.bottom}, RGB(0xd0, 0xd0, 0xd0), DT_CENTER | DT_END_ELLIPSIS);
  SetBkMode(dc, OPAQUE);
}

static void DrawPicker(HDC dc) {
  Pane* p = g_win->pick.p;
  if (!g_win->pick.open || !p || !g_live.count(p)) {
    g_win->pick.rc = RECT{};
    return;
  }
  int rowH = Px(30), n = std::min<int>(kPickRows, (int)g_win->pick.shown.size());
  int h = Px(40) + std::max(1, n) * rowH + Px(6);
  int w = std::min<int>(Px(440), p->rc.right - p->rc.left - Px(16));
  int left = p->chip.left, bottom = p->chip.top - Px(6);
  RECT r{left, std::max<int>(TabBarH() + Px(4), bottom - h), left + w, bottom};
  g_win->pick.rc = r;
  Panel(dc, r);
  SetBkMode(dc, TRANSPARENT);
  SelectObject(dc, g_win->uiFont);
  RECT fr{r.left + Px(12), r.top, r.right - Px(12), r.top + Px(40)};
  if (g_win->pick.filter.empty()) Text(dc, T(S_SEARCH_DIRS), fr, RGB(0x8a, 0x8a, 0x8a), DT_LEFT);
  else Text(dc, g_win->pick.filter, fr, kFg, DT_LEFT | DT_END_ELLIPSIS);
  Fill(dc, RECT{r.left + 1, r.top + Px(40) - 1, r.right - 1, r.top + Px(40)}, kChipBorder);
  for (int i = 0; i < n; i++) {
    int idx = g_win->pick.top + i;
    if (idx >= (int)g_win->pick.shown.size()) break;
    const DirEntry& e = g_win->pick.shown[idx];
    RECT ir{r.left + Px(4), r.top + Px(40) + i * rowH + Px(2), r.right - Px(4), r.top + Px(40) + (i + 1) * rowH};
    bool sel = idx == g_win->pick.sel;
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
  if (!g_win->sw.open) {
    g_win->sw.rc = RECT{};
    return;
  }
  int rowH = Px(30), n = std::min<int>(kPickRows, (int)g_win->sw.shown.size());
  int h = Px(40) + std::max(1, n) * rowH + Px(6);
  int w = std::min<int>(Px(620), cw - Px(32));
  RECT r{(cw - w) / 2, TabBarH() + Px(12), (cw - w) / 2 + w, TabBarH() + Px(12) + h};
  g_win->sw.rc = r;
  Panel(dc, r);
  SetBkMode(dc, TRANSPARENT);
  SelectObject(dc, g_win->uiFont);
  RECT fr{r.left + Px(12), r.top, r.right - Px(12), r.top + Px(40)};
  if (g_win->sw.filter.empty()) Text(dc, T(S_SWITCH), fr, RGB(0x8a, 0x8a, 0x8a), DT_LEFT);
  else Text(dc, g_win->sw.filter, fr, kFg, DT_LEFT | DT_END_ELLIPSIS);
  Fill(dc, RECT{r.left + 1, r.top + Px(40) - 1, r.right - 1, r.top + Px(40)}, kChipBorder);
  if (g_win->sw.shown.empty()) {
    RECT ir{r.left + Px(16), r.top + Px(40), r.right - Px(8), r.top + Px(40) + rowH};
    Text(dc, T(S_NO_RESULTS), ir, kTabText, DT_LEFT);
  }
  for (int i = 0; i < n; i++) {
    int idx = g_win->sw.top + i;
    if (idx >= (int)g_win->sw.shown.size()) break;
    const SwitchItem& it = g_win->sw.shown[idx];
    RECT ir{r.left + Px(4), r.top + Px(40) + i * rowH + Px(2), r.right - Px(4), r.top + Px(40) + (i + 1) * rowH};
    bool sel = idx == g_win->sw.sel;
    if (sel) Fill(dc, ir, kAccent);
    // Punto verde: tiene algo en marcha (Claude, ssh…).
    int cx = ir.left + Px(12), cy = (ir.top + ir.bottom) / 2;
    if (!it.running.empty()) Fill(dc, RECT{cx - Px(3), cy - Px(3), cx + Px(3), cy + Px(3)}, RGB(0x4e, 0xc9, 0x6b));
    // A la derecha la carpeta (solo su último trozo) y si es de otra ventana.
    std::wstring right = it.cwd;
    size_t cut = right.find_last_of(L'\\');
    if (cut != std::wstring::npos && cut + 1 < right.size()) right = right.substr(cut + 1);
    if (it.hwnd != g_win->hwnd) right += L"  \u00b7  " + std::wstring(T(S_OTHER_WINDOW));
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
  g_win->updRc = g_win->updAction = g_win->updClose = RECT{};
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
  SelectObject(dc, g_win->uiFont);
  SIZE ms{}, as{};
  GetTextExtentPoint32W(dc, msg, (int)wcslen(msg), &ms);
  if (act) GetTextExtentPoint32W(dc, act, (int)wcslen(act), &as);
  int bh = Px(40), aw = act ? as.cx + Px(20) : 0, cw = Px(28);
  int cardW = Px(14) + ms.cx + Px(12) + aw + Px(4) + cw + Px(4);
  if (busy) cardW = std::max<int>(cardW, Px(300));  // que no baile al cambiar el % 
  RECT r{w - cardW - Px(14), h - bh - Px(14), w - Px(14), h - Px(14)};
  g_win->updRc = r;
  Panel(dc, r);
  SetBkMode(dc, TRANSPARENT);
  RECT tr{r.left + Px(14), r.top, r.left + Px(14) + ms.cx + 2, r.bottom};
  Text(dc, msg, tr, RGB(0xd0, 0xd0, 0xd0), DT_LEFT);
  if (act) {
    g_win->updAction = RECT{tr.right + Px(10), r.top + Px(7), tr.right + Px(10) + aw, r.bottom - Px(7)};
    Fill(dc, g_win->updAction, kAccent);
    Text(dc, act, g_win->updAction, kFg, DT_CENTER);
  }
  g_win->updClose = RECT{r.right - cw - Px(4), r.top + Px(6), r.right - Px(4), r.bottom - Px(6)};
  Text(dc, L"\u00d7", g_win->updClose, kTabText, DT_CENTER);
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
  Pane* p = g_win->comp.p;
  if (!g_win->comp.open || !p || !g_live.count(p) || g_win->tabs.empty() || p != CurTab().active || !EditorActive(p)) {
    g_win->comp.rc = RECT{};
    return;
  }
  int rowH = Px(30), n = std::min<int>(kCompRows, (int)g_win->comp.items.size());
  int h = std::max(1, n) * rowH + Px(8);
  int w = std::min<int>(Px(480), p->rc.right - p->rc.left - Px(16));
  int left = p->chip.left, bottom = p->chip.top - Px(6);
  RECT r{left, std::max<int>(TabBarH() + Px(4), bottom - h), left + w, bottom};
  g_win->comp.rc = r;
  Panel(dc, r);
  SetBkMode(dc, TRANSPARENT);
  SelectObject(dc, g_win->uiFont);
  if (!n) {
    RECT tr{r.left + Px(14), r.top + Px(4), r.right - Px(14), r.top + Px(4) + rowH};
    Text(dc, T(S_LOADING), tr, RGB(0x8a, 0x8a, 0x8a), DT_LEFT);
  }
  static const Str kKind[] = {S_KIND_CMD, S_KIND_DIR, S_KIND_FILE, S_KIND_PARAM};
  for (int i = 0; i < n; i++) {
    int idx = g_win->comp.top + i;
    if (idx >= (int)g_win->comp.items.size()) break;
    const CompItem& it = g_win->comp.items[idx];
    RECT ir{r.left + Px(4), r.top + Px(4) + i * rowH, r.right - Px(4), r.top + Px(4) + (i + 1) * rowH};
    bool sel = idx == g_win->comp.sel;
    if (sel) Fill(dc, ir, kAccent);
    RECT kr{ir.right - Px(110), ir.top, ir.right - Px(10), ir.bottom};
    Text(dc, T(kKind[it.kind]), kr, sel ? kFg : RGB(0x8a, 0x8a, 0x8a), DT_RIGHT);
    RECT tr{ir.left + Px(10), ir.top, kr.left - Px(8), ir.bottom};
    Text(dc, it.label, tr, sel ? kFg : RGB(0xd0, 0xd0, 0xd0), DT_LEFT | DT_END_ELLIPSIS);
  }
  if ((int)g_win->comp.items.size() > kCompRows) {  // barra de desplazamiento fina
    int total = (int)g_win->comp.items.size(), ih = r.bottom - r.top - Px(8);
    int th = std::max(Px(12), ih * kCompRows / total), ty = r.top + Px(4) + (ih - th) * g_win->comp.top / std::max(1, total - kCompRows);
    Fill(dc, RECT{r.right - Px(4), ty, r.right - Px(2), ty + th}, kTabText);
  }
  SetBkMode(dc, OPAQUE);
}

// ---------------------------------------------------------------------------
// Phone: a paired phone sees this Warp's terminals from anywhere (iroh: direct UDP when hole punching
// works, n0's relays otherwise). warp-remote.exe, next to Warp.exe, does the networking in its own process:
// one per Warp, started by the main window when a phone is paired or by any window when pairing is asked
// for, and it ends with the last window of this Warp. It lists the panes the same way the terminal switcher
// does (g_msgListPanes), so the windows need nothing new for that. Messages: warp-remote/src/win.rs.

static const ULONG_PTR kCopyRemote = 0x57524d00, kCopyRemoteTicket = 0x57525400;

// Key, allow-list and log of this Warp's warp-remote (a test's next to its session, never the real one).
static std::wstring RemoteDir() { return g_windowDir.empty() ? g_sessionFile + L".remote" : g_windowDir + L"\\remote"; }

struct PairedDevice {
  std::wstring id, name;
  long long when = 0;
};

// devices.txt: one paired device per line, «<id hex>\t<unix seconds>\t<name>» (written by warp-remote).
static std::vector<PairedDevice> PairedDevices() {
  std::vector<PairedDevice> out;
  HANDLE f = CreateFileW((RemoteDir() + L"\\devices.txt").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return out;
  std::string data(std::min<DWORD>(GetFileSize(f, nullptr), 1 << 20), 0);
  DWORD r = 0;
  ReadFile(f, data.data(), (DWORD)data.size(), &r, nullptr);
  CloseHandle(f);
  data.resize(r);
  std::istringstream in(data);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1);
    if (a == std::string::npos || a != 64) continue;
    PairedDevice d;
    d.id = ToWide(line.substr(0, a));
    d.when = atoll(line.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1).c_str());
    d.name = b == std::string::npos ? L"" : ToWide(line.substr(b + 1));
    if (d.name.empty()) d.name = d.id.substr(0, 10);
    out.push_back(d);
  }
  return out;
}

// This Warp's warp-remote: its hidden message window carries our instance key.
static HWND RemoteWindow() {
  for (HWND h = nullptr; (h = FindWindowExW(HWND_MESSAGE, h, L"WarpRemote", nullptr));)
    if (GetPropW(h, kInstanceProp) == InstanceKey()) return h;
  return nullptr;
}

static bool RemoteCommand(const std::wstring& cmd) {
  HWND h = RemoteWindow();
  if (!h) return false;
  COPYDATASTRUCT cd{kCopyRemote, (DWORD)(cmd.size() * sizeof(wchar_t)), (void*)cmd.data()};
  DWORD_PTR r = 0;
  return SendMessageTimeoutW(h, WM_COPYDATA, (WPARAM)g_host, (LPARAM)&cd, SMTO_ABORTIFHUNG, 2000, &r) && r;
}

// Starts this Warp's warp-remote if it isn't running. pair: and asks it for a pairing code for this window
// (the answer comes as WM_COPYDATA kCopyRemoteTicket).
static bool StartRemote(bool pair) {
  wchar_t hwnd[40];
  swprintf(hwnd, 40, L"%llx", (unsigned long long)(ULONG_PTR)g_host);  // the code comes back to it (OnRemoteTicket)
  if (RemoteWindow()) return !pair || RemoteCommand(L"pair " + std::wstring(hwnd));
  wchar_t self[MAX_PATH];
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  std::wstring dir = self;
  dir = dir.substr(0, dir.find_last_of(L'\\'));
  std::wstring exe = dir + L"\\warp-remote.exe";
  if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
  wchar_t args[96];
  swprintf(args, 96, L" --instance %08x --pid %lu", (unsigned)(ULONG_PTR)InstanceKey(), GetCurrentProcessId());
  std::wstring cmd = L"\"" + exe + L"\" --dir \"" + RemoteDir() + L"\"" + args + (pair ? L" --pair " + std::wstring(hwnd) : L"");
  STARTUPINFOW si{sizeof si};
  PROCESS_INFORMATION pi{};
  // Its own folder as working directory: it outlives windows and must not keep their folders in use.
  if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi))
    return false;
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
}

static void ClosePairCard() {
  if (!g_win->pair.open) return;
  g_win->pair.open = false;
  KillTimer(g_win->hwnd, TIMER_PAIR);
  InvalidateRect(g_win->hwnd, nullptr, FALSE);
}

static HWND g_pairAsker;  // the window whose pairing card asked for the code warp-remote is getting

static void RequestPairCode() {
  g_pairAsker = g_win->hwnd;
  g_win->pair.ticket.clear(), g_win->pair.error.clear(), g_win->pair.qr.clear(), g_win->pair.copied = false;
  g_win->pair.shown = GetTickCount();
  if (!StartRemote(true)) g_win->pair.error = L"warp-remote.exe";
  SetTimer(g_win->hwnd, TIMER_PAIR, 1000, nullptr);  // countdown, and giving up on an answer that never comes
  InvalidateRect(g_win->hwnd, nullptr, FALSE);
}

// Title-bar menu → Pair phone…: a card with the pairing code as a QR code and as text.
static void OpenPairCard() {
  g_win->pair = PairCard{};
  g_win->pair.open = true;
  RequestPairCode();
}

static bool PairCodeValid() { return !g_win->pair.ticket.empty() && (int)(g_win->pair.expires - GetTickCount()) > 0; }

// The card's button: copy the code, or ask for a new one when it expired (or failed).
static void PairAction() {
  if (PairCodeValid()) {
    SetClipboardText(g_win->pair.ticket);
    g_win->pair.copied = true;
  } else {
    RequestPairCode();
  }
  InvalidateRect(g_win->hwnd, nullptr, FALSE);
}

// warp-remote's answer: «ticket\t<code>\t<seconds>» or «error\t<why>».
static void OnPairTicket(const std::wstring& t) {
  if (!g_win->pair.open) return;
  size_t a = t.find(L'\t'), b = a == std::wstring::npos ? a : t.find(L'\t', a + 1);
  if (t.rfind(L"ticket\t", 0) != 0 || b == std::wstring::npos) {
    g_win->pair.error = a == std::wstring::npos ? t : t.substr(a + 1);
  } else {
    g_win->pair.ticket = t.substr(a + 1, b - a - 1);
    g_win->pair.expires = GetTickCount() + (DWORD)_wtoi(t.c_str() + b + 1) * 1000;
    try {
      qrcodegen::QrCode q = qrcodegen::QrCode::encodeText(ToUtf8(g_win->pair.ticket).c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
      int n = q.getSize();
      g_win->pair.qr.assign(n, std::vector<bool>(n));
      for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) g_win->pair.qr[y][x] = q.getModule(x, y);
    } catch (const std::exception&) {
      g_win->pair.error = L"QR";
    }
  }
  InvalidateRect(g_win->hwnd, nullptr, FALSE);
}

// typing.txt: the paired devices that may type into the terminals, one id per line (only Warp writes it; none may
// until chosen in Paired devices → the device → Allow typing). warp-remote checks it, and so does OnRemotePane.
static std::set<std::wstring> TypingDevices() {
  std::set<std::wstring> out;
  HANDLE f = CreateFileW((RemoteDir() + L"\\typing.txt").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return out;
  std::string data(std::min<DWORD>(GetFileSize(f, nullptr), 1 << 16), 0);
  DWORD r = 0;
  ReadFile(f, data.data(), (DWORD)data.size(), &r, nullptr);
  CloseHandle(f);
  data.resize(r);
  std::istringstream in(data);
  std::string line;
  while (std::getline(in, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.size() == 64) out.insert(ToWide(line));
  }
  return out;
}

static void SetTyping(const std::wstring& id, bool on) {
  std::set<std::wstring> ids = TypingDevices();
  if (on == (ids.count(id) > 0)) return;
  if (on) ids.insert(id);
  else ids.erase(id);
  std::string out;
  for (auto& d : ids) out += ToUtf8(d) + "\n";
  CreateDirectoryW(RemoteDir().c_str(), nullptr);
  std::wstring path = RemoteDir() + L"\\typing.txt", tmp = path + L".warp";
  HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return;
  DWORD w = 0;
  bool ok = WriteFile(f, out.data(), (DWORD)out.size(), &w, nullptr) && w == out.size();
  CloseHandle(f);
  if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp.c_str());
}

// Removes a paired device: it is disconnected now and turned away from then on.
static void RemoveDevice(const std::wstring& id) {
  SetTyping(id, false);
  std::wstring path = RemoteDir() + L"\\devices.txt", tmp = path + L".warp";
  std::string out;
  for (auto& d : PairedDevices())
    if (d.id != id) out += ToUtf8(d.id) + "\t" + std::to_string(d.when) + "\t" + ToUtf8(d.name) + "\n";
  HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return;
  DWORD w = 0;
  bool ok = WriteFile(f, out.data(), (DWORD)out.size(), &w, nullptr) && w == out.size();
  CloseHandle(f);
  if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    DeleteFileW(tmp.c_str());
    return;
  }
  RemoteCommand(L"reload");
}

// A phone watching a pane (see warp-remote/src/win.rs): warp-remote asks this window for the pane's text and what runs
// in it, about once a second while the phone shows it, and types into it for a device allowed to.
static const ULONG_PTR kCopyRemotePane = 0x57525000, kCopyRemoteAnswer = 0x57524100;

// A pane of this Warp by its id, in whichever window it is now.
static Pane* PaneById(unsigned id) {
  for (Pane* p : g_live)
    if (p->id == id && p->win && !p->win->dead) return p;
  return nullptr;
}

// The pane's text for a phone: its last `back` scrollback rows, then its screen, one line per row without the spaces
// at its end; wide characters once. «screen\t<rows>\t<cols>» first.
static std::wstring RemoteScreenText(Pane* p, int back) {
  auto row = [](std::wstring& out, const VTermScreenCell* cells, int n) {
    size_t start = out.size();
    for (int c = 0; c < n; c++) {
      const VTermScreenCell& cell = cells[c];
      if (cell.chars[0] == (uint32_t)-1) continue;
      if (!cell.chars[0]) out += L' ';
      for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i]; i++) AppendUtf16(out, cell.chars[i]);
    }
    while (out.size() > start && out.back() == L' ') out.pop_back();
    out += L'\n';
  };
  wchar_t head[64];
  EnterCriticalSection(&p->cs);
  swprintf(head, 64, L"screen\t%d\t%d\n", p->rows, p->cols);
  std::wstring out = head;
  int n = p->altScreen ? 0 : std::min(std::max(back, 0), (int)p->sb.size());
  for (int i = (int)p->sb.size() - n; i < (int)p->sb.size(); i++) row(out, p->sb[i].data(), (int)p->sb[i].size());
  std::vector<VTermScreenCell> cells(p->cols);
  for (int r = 0; r < p->rows; r++) {
    for (int c = 0; c < p->cols; c++)
      if (!vterm_screen_get_cell(p->scr, VTermPos{r, c}, &cells[c])) BlankCell(&cells[c]);
    row(out, cells.data(), p->cols);
  }
  LeaveCriticalSection(&p->cs);
  out.pop_back();
  return out;
}

// The transcript of the Claude Code or Codex running in that pane on this computer, for a phone to read: Claude Code's
// session of the pane's own process (see ClaudeSessionJson), or Codex's newest conversation in its folder. Empty when
// there is none (yet) or it runs elsewhere (over ssh, warp-remote looks for it there).
static std::wstring LocalTranscript(Pane* p) {
  if (p->agent == AGENT_CLAUDE && p->pid) {
    std::wstring root = EnvDir(L"CLAUDE_CONFIG_DIR", L"\\.claude");
    if (root.empty()) return L"";
    for (DWORD pid : Descendants(ChildrenByParent(), p->pid)) {
      std::string j = ClaudeSessionJson(pid), id = JsonStr(j, "sessionId", 0), cwd = JsonStr(j, "cwd", 0);
      if (id.size() != 36 || cwd.empty() || id.find_first_not_of("0123456789abcdefABCDEF-") != std::string::npos) continue;
      for (size_t k = 0; (k = cwd.find("\\\\", k)) != std::string::npos; k++) cwd.erase(k, 1);
      std::wstring name = ToWide(cwd);
      for (wchar_t& c : name)
        if (!iswascii(c) || !iswalnum(c)) c = L'-';
      return root + L"\\projects\\" + name + L"\\" + ToWide(id) + L".jsonl";
    }
  } else if (p->agent == AGENT_CODEX) {
    std::vector<AgentSession> s = CodexSessions(p->cwd);
    if (!s.empty()) return s[0].path;
  }
  return L"";
}

// Typing from a phone, as if on this keyboard: text as a paste (one line already), keys by name. The title bar says
// «Phone typing» for a few seconds.
static bool RemoteInput(Pane* p, const std::wstring& kind, const std::wstring& arg) {
  if (kind == L"text") {
    PasteText(p, arg);
  } else if (kind != L"key") {
    return false;
  } else if (EditorActive(p)) {  // the input box: what these keys do there
    StopReconnect(p);
    if (arg == L"enter") SubmitEditor(p);
    else if (arg == L"esc") p->edit.clear(), p->caret = 0, p->histPos = -1;
    else if (arg == L"up") EditorKey(p, VK_UP);
    else if (arg == L"down") EditorKey(p, VK_DOWN);
    else if (arg == L"left") p->caret = p->caret ? p->caret - 1 : 0;
    else if (arg == L"right") p->caret = std::min(p->caret + 1, p->edit.size());
    else if (arg == L"backspace" && p->caret > 0) p->edit.erase(--p->caret, 1);
    else if (arg == L"ctrl_c" && !p->edit.empty()) p->edit.clear(), p->caret = 0;
    else if (arg == L"ctrl_c") SendChar(p, 3, VTERM_MOD_NONE);
    else if (arg != L"tab" && arg != L"backspace") return false;
  } else {
    StopReconnect(p);
    VTermKey key = arg == L"enter" ? VTERM_KEY_ENTER : arg == L"esc" ? VTERM_KEY_ESCAPE : arg == L"up" ? VTERM_KEY_UP
                 : arg == L"down" ? VTERM_KEY_DOWN : arg == L"left" ? VTERM_KEY_LEFT : arg == L"right" ? VTERM_KEY_RIGHT
                 : arg == L"tab" ? VTERM_KEY_TAB : arg == L"backspace" ? VTERM_KEY_BACKSPACE : VTERM_KEY_NONE;
    if (arg == L"ctrl_c") SendChar(p, 3, VTERM_MOD_NONE);
    else if (key != VTERM_KEY_NONE) SendKey(p, key, VTERM_MOD_NONE);
    else return false;
  }
  g_win->phoneTyping = true;
  SetTimer(g_win->hwnd, TIMER_PHONE, 4000, nullptr);  // once: the mark goes away 4 s after the last key
  InvalidateRect(g_win->hwnd, nullptr, FALSE);
  return true;
}

// warp-remote about one of this window's panes (only from this Warp's warp-remote: see the caller): «screen <id>
// <rows>» and «info <id>» are answered with kCopyRemoteAnswer; «input <id> <device> text|key\t<what>» types, only for a
// device that is paired and allowed to type (checked here too).
static BOOL OnRemotePane(HWND from, const std::wstring& t) {
  size_t tab = t.find(L'\t');
  std::wistringstream in(t.substr(0, tab));
  std::wstring cmd, dev, kind;
  unsigned id = 0;
  in >> cmd >> id;
  Pane* p = PaneById(id);
  if (!p) return FALSE;
  InWindow iw(p->win);
  std::wstring answer;
  if (cmd == L"screen") {
    int back = 0;
    in >> back;
    answer = RemoteScreenText(p, std::min(back, 1000));
  } else if (cmd == L"info") {
    auto clean = [](std::wstring s) {
      for (wchar_t& c : s)
        if (c == L'\t' || c == L'\n' || c == L'\r') c = L' ';
      return s;
    };
    const wchar_t* agent = p->agent == AGENT_CLAUDE ? L"claude" : p->agent == AGENT_CODEX ? L"codex" : L"";
    answer = L"info\t" + std::wstring(agent) + L"\t" + clean(p->runningCmd) + L"\t" + clean(p->cwd) + L"\t" + clean(LocalTranscript(p));
  } else if (cmd == L"input") {
    in >> dev >> kind;
    if (tab == std::wstring::npos || !TypingDevices().count(dev)) return FALSE;
    std::vector<PairedDevice> devs = PairedDevices();
    if (std::none_of(devs.begin(), devs.end(), [&](const PairedDevice& d) { return d.id == dev; })) return FALSE;
    return RemoteInput(p, kind, t.substr(tab + 1));
  } else {
    return FALSE;
  }
  COPYDATASTRUCT cd{kCopyRemoteAnswer, (DWORD)(answer.size() * sizeof(wchar_t)), (void*)answer.data()};
  DWORD_PTR r = 0;
  SendMessageTimeoutW(from, WM_COPYDATA, (WPARAM)g_host, (LPARAM)&cd, SMTO_ABORTIFHUNG, 2000, &r);
  return TRUE;
}

// At start (once, a moment after the first window shows): warp-remote starts when a phone is paired, and says how
// many phones are connected. Nothing at all when none is paired.
static void RemoteAtStart() {
  if (!PairedDevices().empty()) StartRemote(false);
  if (HWND r = RemoteWindow()) {
    DWORD_PTR n = 0;
    if (SendMessageTimeoutW(r, g_msgRemoteState, 0, 0, SMTO_ABORTIFHUNG, 1000, &n)) g_remotePhones = (int)n;
  }
}

// What warp-remote tells this Warp: phones connected (lp 0: the window in front says when one more is), a phone was
// paired (1: the pairing card closes, saying so), the code ended (2).
static void OnRemoteEvent(WPARAM wp, LPARAM lp) {
  if (lp == 0) {
    Window* front = (int)wp > g_remotePhones ? FrontWindow() : nullptr;
    g_remotePhones = (int)wp;
    if (front && !front->pair.open) {
      InWindow in(front);
      Toast(T(S_PHONE_CONNECTED));
    }
  } else if (lp == 1) {
    for (Window* w : g_windows)
      if (w->pair.open) {
        InWindow in(w);
        ClosePairCard();
        Toast(T(S_PHONE_PAIRED), 3000);
      }
  }
  InvalidateAll();
}

// warp-remote's pairing code, for the window that asked for it.
static void OnRemoteTicket(const std::wstring& t) {
  if (Window* w = LiveWindow(g_pairAsker)) {
    InWindow in(w);
    OnPairTicket(t);
  }
}

static void DrawPairCard(HDC dc, int w, int h) {
  g_win->pair.rc = g_win->pair.action = g_win->pair.close = RECT{};
  if (!g_win->pair.open) return;
  int n = g_win->pair.qr.empty() ? 57 : (int)g_win->pair.qr.size();  // version 10 until the code arrives (~200 characters)
  int mod = std::max(3, Px(4)), quiet = 4 * mod, side = n * mod + 2 * quiet;
  int cw = std::max(side + Px(48), Px(470));
  int perLine = std::max(16, (cw - Px(48)) / std::max(1, g_win->cellW));
  int lines = g_win->pair.ticket.empty() ? 1 : ((int)g_win->pair.ticket.size() + perLine - 1) / perLine;
  int ch = Px(52) + Px(26) + side + Px(14) + lines * g_win->cellH + Px(10) + Px(46);
  int top = TabBarH() + std::max(Px(12), (h - TabBarH() - ch) / 2);
  RECT r{(w - cw) / 2, top, (w - cw) / 2 + cw, top + ch};
  g_win->pair.rc = r;
  Panel(dc, r);
  SetBkMode(dc, TRANSPARENT);
  SelectObject(dc, g_win->uiFont);
  int x0 = r.left + Px(24), x1 = r.right - Px(24), y = r.top + Px(10);
  Text(dc, T(S_PAIR_TITLE), RECT{x0, y, x1, y + Px(36)}, kFg, DT_LEFT);
  g_win->pair.close = RECT{r.right - Px(40), r.top + Px(8), r.right - Px(8), r.top + Px(40)};
  Text(dc, L"×", g_win->pair.close, kTabText, DT_CENTER);
  y += Px(42);
  Text(dc, T(S_PAIR_HINT), RECT{x0, y, x1, y + Px(22)}, RGB(0x9b, 0x9b, 0x9b), DT_LEFT | DT_END_ELLIPSIS);
  y += Px(26);
  // The QR code: dark modules on white with its quiet zone, as phones' cameras expect.
  RECT q{(r.left + r.right - side) / 2, y, (r.left + r.right - side) / 2 + side, y + side};
  bool expired = !g_win->pair.ticket.empty() && !PairCodeValid();
  if (!g_win->pair.qr.empty() && !expired) {
    Fill(dc, q, RGB(0xff, 0xff, 0xff));
    for (int my = 0; my < n; my++)
      for (int mx = 0; mx < n;) {
        if (!g_win->pair.qr[my][mx]) {
          mx++;
          continue;
        }
        int run = mx;
        while (run < n && g_win->pair.qr[my][run]) run++;
        Fill(dc, RECT{q.left + quiet + mx * mod, q.top + quiet + my * mod, q.left + quiet + run * mod, q.top + quiet + (my + 1) * mod}, RGB(0, 0, 0));
        mx = run;
      }
  } else {
    Fill(dc, q, kBg);
    std::wstring msg = expired ? T(S_PAIR_EXPIRED) : g_win->pair.error.empty() ? T(S_PAIR_STARTING) : L"";
    if (!g_win->pair.error.empty()) {
      wchar_t b[300];
      swprintf(b, 300, T(S_PAIR_FAILED), g_win->pair.error.c_str());
      msg = b;
    }
    Text(dc, msg, RECT{q.left + Px(8), q.top, q.right - Px(8), q.bottom}, kTabText, DT_CENTER | DT_END_ELLIPSIS);
  }
  y += side + Px(14);
  // The same code as text, for pasting (also in the clipboard with the button).
  SelectObject(dc, g_win->fonts[0]);
  for (int i = 0; i < lines && !g_win->pair.ticket.empty(); i++) {
    std::wstring part = g_win->pair.ticket.substr((size_t)i * perLine, perLine);
    Text(dc, part, RECT{x0, y + i * g_win->cellH, x1, y + (i + 1) * g_win->cellH}, expired ? RGB(0x5a, 0x5a, 0x5a) : RGB(0xd0, 0xd0, 0xd0), DT_LEFT);
  }
  y += lines * g_win->cellH + Px(10);
  SelectObject(dc, g_win->uiFont);
  std::wstring left;
  if (PairCodeValid()) {
    int s = (int)(g_win->pair.expires - GetTickCount() + 999) / 1000;
    wchar_t b[80];
    swprintf(b, 80, T(S_PAIR_EXPIRES), s / 60, s % 60);
    left = b;
  } else if (expired) {
    left = T(S_PAIR_EXPIRED);
  }
  const wchar_t* act = PairCodeValid() ? T(g_win->pair.copied ? S_COPIED : S_COPY)
                       : expired || !g_win->pair.error.empty() ? T(S_PAIR_NEW)
                                                          : nullptr;
  if (act) {
    SIZE as{};
    GetTextExtentPoint32W(dc, act, (int)wcslen(act), &as);
    g_win->pair.action = RECT{x1 - as.cx - Px(24), y + Px(4), x1, y + Px(36)};
    Fill(dc, g_win->pair.action, kAccent);
    Text(dc, act, g_win->pair.action, kFg, DT_CENTER);
  }
  Text(dc, left, RECT{x0, y + Px(4), (act ? g_win->pair.action.left : x1) - Px(8), y + Px(36)}, kTabText, DT_LEFT);
  SetBkMode(dc, OPAQUE);
}

// Where a tab or pane dragged from another window would go (see «Join»): the part of the pane it would take, washed
// with the accent and outlined, or the insertion bar on the tabs.
static void DrawJoinHint(HDC dc) {
  POINT pt = g_win->joinHint.at;
  ScreenToClient(g_win->hwnd, &pt);
  JoinSpot s = JoinSpotAt(pt);
  RECT r = s.mark;
  int w = r.right - r.left, h = r.bottom - r.top;
  if (!s.pane || w <= 0 || h <= 0) {
    Fill(dc, r, kAccent);
    return;
  }
  BITMAPINFO bi{};
  bi.bmiHeader = {sizeof bi.bmiHeader, w, -h, 1, 32, BI_RGB};
  void* bits = nullptr;
  HDC mem = CreateCompatibleDC(dc);
  if (HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0)) {
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, dc, r.left, r.top, SRCCOPY);
    GdiFlush();
    uint32_t* px = (uint32_t*)bits;
    uint32_t ar = GetRValue(kAccent), ag = GetGValue(kAccent), ab = GetBValue(kAccent);  // a quarter of the way to it
    for (size_t i = 0; i < (size_t)w * h; i++) {
      uint32_t c = px[i];
      px[i] = (((c >> 16 & 255) * 3 + ar) / 4) << 16 | (((c >> 8 & 255) * 3 + ag) / 4) << 8 | ((c & 255) * 3 + ab) / 4;
    }
    BitBlt(dc, r.left, r.top, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
  }
  DeleteDC(mem);
  HBRUSH b = CreateSolidBrush(kAccent);
  for (int i = 0; i < Px(2); i++, InflateRect(&r, -1, -1)) FrameRect(dc, &r, b);
  DeleteObject(b);
}

static void Render(HDC dc, int w, int h) {
  RECT all{0, 0, w, h};
  Fill(dc, all, kBg);
  DrawTitleBar(dc, w, h);
  if (g_win->tabs.empty()) return;
  Tab& t = CurTab();
  std::vector<Pane*> ls;
  Leaves(t.root, ls);
  for (Pane* p : ls) {
    DrawPane(dc, p, p == t.active, g_win->focused);
    DrawPaneHeader(dc, p, p == t.active);
    DrawAttention(dc, p);
  }
  DrawDividers(dc, t.root);
  for (Pane* p : ls) DrawReconnect(dc, p);
  DrawFindBar(dc);
  DrawPicker(dc);
  DrawCompletion(dc);
  DrawSwitcher(dc, w);
  DrawUpdateNotice(dc, w, h);
  DrawPairCard(dc, w, h);
  if (g_win->dropTarget) {  // destino al arrastrar un panel por su cabecera
    HBRUSH b = CreateSolidBrush(kAccent);
    RECT r = g_win->dropTarget->rc;
    for (int i = 0; i < Px(2); i++, InflateRect(&r, -1, -1)) FrameRect(dc, &r, b);
    DeleteObject(b);
  }
  if (g_win->joinHint.from) DrawJoinHint(dc);  // a tab or pane of another window over this one
}

// ---------------------------------------------------------------------------
// Pintado: búfer fijo; la salida de la shell solo repinta sus filas, como mucho a 60 fps.


static void EnsureBack(HDC ref, int w, int h) {
  if (g_win->backDC && w == g_win->backW && h == g_win->backH) return;
  if (!g_win->backDC) g_win->backDC = CreateCompatibleDC(ref);
  HBITMAP bmp = CreateCompatibleBitmap(ref, std::max(1, w), std::max(1, h));
  HGDIOBJ old = SelectObject(g_win->backDC, bmp);
  if (g_win->backBmp) DeleteObject(g_win->backBmp);
  else (void)old;
  g_win->backBmp = bmp;
  g_win->backW = w, g_win->backH = h;
}

static void PaintDirty() {
  DWORD now = GetTickCount();
  g_win->lastFrame = now;
  if (!g_win->backDC || g_win->tabs.empty() || IsIconic(g_win->hwnd)) return;
  std::vector<Pane*> ls;
  Leaves(CurTab().root, ls);
  bool overlay = g_win->find.open || g_win->pick.open || g_win->sw.open || g_win->comp.open || (g_upd.stage != UPD_NONE && !g_upd.hidden) || g_win->dragPane || g_win->link.p || g_win->renameTab >= 0 || g_win->pair.open || g_win->joinHint.from ||
                 std::any_of(ls.begin(), ls.end(), [](Pane* p) { return p->recon.state != RC_OFF; });
  if (overlay || (g_win->titleDirty && now - g_win->lastFull >= 250)) {  // títulos: como mucho 4 veces por segundo
    g_win->titleDirty = false;
    InvalidateRect(g_win->hwnd, nullptr, FALSE);
    return;
  }
  if (g_win->titleDirty && !g_win->frameTimer) SetTimer(g_win->hwnd, TIMER_FRAME, 250, nullptr), g_win->frameTimer = true;
  HDC wdc = GetDC(g_win->hwnd);
  for (Pane* p : ls) {
    RECT u = DrawPane(g_win->backDC, p, p == CurTab().active, g_win->focused, true);
    if (u.right <= u.left) continue;
    bool frame = p->attention;  // the rows were drawn across its sides
    if (frame) DrawAttention(g_win->backDC, p);
    BitBlt(wdc, u.left, u.top, u.right - u.left, u.bottom - u.top, g_win->backDC, u.left, u.top, SRCCOPY);
    if (frame) BlitAttention(wdc, g_win->backDC, p);
  }
  ReleaseDC(g_win->hwnd, wdc);
}

// The attention pulse (see «Attention»): TIMER_PULSE runs only while a pane of the tab in view has attention, the
// window isn't minimized and Windows animates; otherwise the outline, if any, is steady and nothing runs. Checked
// before every message (SyncUnread), so a minimize, a tab switch or a click stops it at once.
static const UINT kPulseFrameMs = 33;

static void SyncPulse() {
  bool want = false;
  if (!g_win->tabs.empty()) {
    std::vector<Pane*> ls;
    Leaves(CurTab().root, ls);
    want = !IsIconic(g_win->hwnd) && Motion() && std::any_of(ls.begin(), ls.end(), [](Pane* p) { return p->attention; });
  }
  if (want == g_win->pulseTimer) return;
  g_win->pulseTimer = want;
  if (want) SetTimer(g_win->hwnd, TIMER_PULSE, kPulseFrameMs, nullptr);
  else KillTimer(g_win->hwnd, TIMER_PULSE);
}

// One frame of the pulse: only the outlines, the headers and the tab in view, into the back buffer and onto the window.
// What is drawn over the panes (update notice, find bar, menus) keeps its place: their last rectangles are left out.
static void PulseTick() {
  if (!g_win->backDC || g_win->tabs.empty() || IsIconic(g_win->hwnd) || g_win->dragPane || g_win->joinHint.from) return;
  std::vector<Pane*> ls;
  Leaves(CurTab().root, ls);
  HDC wdc = GetDC(g_win->hwnd);
  int saved = SaveDC(g_win->backDC);
  for (const RECT* o : {&g_win->updRc, &g_win->find.rc, &g_win->sw.rc, &g_win->pick.rc, &g_win->comp.rc})
    if (o->right > o->left) {
      ExcludeClipRect(g_win->backDC, o->left, o->top, o->right, o->bottom);
      ExcludeClipRect(wdc, o->left, o->top, o->right, o->bottom);
    }
  for (Pane* p : ls)
    if (p->attention) {
      RECT h = HeaderRect(p);  // split panes only
      DrawPaneHeader(g_win->backDC, p, p == CurTab().active);
      DrawAttention(g_win->backDC, p);
      if (h.bottom > h.top) BitBlt(wdc, h.left, h.top, h.right - h.left, h.bottom - h.top, g_win->backDC, h.left, h.top, SRCCOPY);
      BlitAttention(wdc, g_win->backDC, p);
    }
  if (g_win->renameTab < 0)  // its tab (not while it is being renamed)
    for (auto& [i, r] : ComputeTabs().tabs)
      if (i == g_win->activeTab) {
        SelectObject(g_win->backDC, g_win->uiFont);
        SetBkMode(g_win->backDC, TRANSPARENT);
        DrawTabItem(g_win->backDC, i, r, g_win->vertTabs);
        SetBkMode(g_win->backDC, OPAQUE);
        BitBlt(wdc, r.left, r.top, r.right - r.left, r.bottom - r.top, g_win->backDC, r.left, r.top, SRCCOPY);
      }
  RestoreDC(g_win->backDC, saved);
  ReleaseDC(g_win->hwnd, wdc);
}

// Pruebas: prepara el búfer como tras un WM_PAINT (en la sesión de pruebas Windows no pinta).
static void PrimeBack() {
  RECT rc;
  GetClientRect(g_win->hwnd, &rc);
  HDC wdc = GetDC(g_win->hwnd);
  EnsureBack(wdc, rc.right, rc.bottom);
  ReleaseDC(g_win->hwnd, wdc);
  Render(g_win->backDC, rc.right, rc.bottom);
  g_win->lastFull = GetTickCount();
}

// Pruebas: ¿lo pintado fila a fila coincide con un pintado completo? Devuelve píxeles distintos.
static long VerifyPartial() {
  // Con los paneles bloqueados no entra salida nueva entre las dos imágenes (el lock es reentrante).
  std::vector<Pane*> locked(g_live.begin(), g_live.end());
  for (Pane* p : locked) EnterCriticalSection(&p->cs);
  PaintDirty();
  long r = [&] {
  RECT rc;
  GetClientRect(g_win->hwnd, &rc);
  int w = rc.right, h = rc.bottom;
  BITMAPINFO bi{};
  bi.bmiHeader = {sizeof bi.bmiHeader, w, -h, 1, 32, BI_RGB};
  std::vector<uint32_t> a((size_t)w * h), b((size_t)w * h);
  GetDIBits(g_win->backDC, g_win->backBmp, 0, h, a.data(), &bi, DIB_RGB_COLORS);
  HDC tmp = CreateCompatibleDC(g_win->backDC);
  HBITMAP bmp = CreateCompatibleBitmap(g_win->backDC, w, h);
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
  GetClientRect(g_win->hwnd, &rc);
  HDC wdc = GetDC(g_win->hwnd);
  EnsureBack(wdc, rc.right, rc.bottom);
  ReleaseDC(g_win->hwnd, wdc);
  LARGE_INTEGER f, a, b, c;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&a);
  for (int i = 0; i < 20; i++) Render(g_win->backDC, rc.right, rc.bottom);
  QueryPerformanceCounter(&b);
  std::vector<Pane*> ls;
  Leaves(CurTab().root, ls);
  for (int i = 0; i < 20; i++)
    for (Pane* p : ls) {
      EnterCriticalSection(&p->cs);
      MarkRow(p, p->cursor.row);
      LeaveCriticalSection(&p->cs);
      DrawPane(g_win->backDC, p, p == CurTab().active, true, true);
    }
  QueryPerformanceCounter(&c);
  wchar_t out[200];
  swprintf(out, 200, L"%dx%d, %d paneles: completo %.2f ms, solo filas %.3f ms", (int)rc.right, (int)rc.bottom,
           (int)ls.size(), (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart / 20,
           (c.QuadPart - b.QuadPart) * 1000.0 / f.QuadPart / 20);
  return out;
}

// 32-bit top-down BMP of w x h pixels.
static bool WriteBmp(const std::wstring& path, const void* bits, int w, int h) {
  BITMAPINFOHEADER bh{sizeof bh, w, -h, 1, 32, BI_RGB};
  BITMAPFILEHEADER fh{};
  DWORD size = (DWORD)w * h * 4;
  fh.bfType = 0x4D42;
  fh.bfOffBits = sizeof fh + sizeof bh;
  fh.bfSize = fh.bfOffBits + size;
  HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  DWORD wr;
  WriteFile(f, &fh, sizeof fh, &wr, nullptr);
  WriteFile(f, &bh, sizeof bh, &wr, nullptr);
  WriteFile(f, bits, size, &wr, nullptr);
  CloseHandle(f);
  return true;
}

static bool SaveScreenshot(const std::wstring& path) {
  RECT rc;
  GetClientRect(g_win->hwnd, &rc);
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
  bool ok = WriteBmp(path, bits, w, h);
  SelectObject(dc, old);
  DeleteObject(bmp);
  DeleteDC(dc);
  return ok;
}

// ---------------------------------------------------------------------------
// Windows notifications (title-bar menu → «Windows notifications», off by default): each event a badge counts (see
// «Unread badges», same dedupe) also shows a toast, silent, one per pane (the next one replaces it), gone from the
// action center once the pane is looked at, and none over a full-screen game or a presentation (Focus Assist decides
// the rest). Its title is the notification's own (OSC 9/777/99) or the pane's name; its text, the notification's body
// or the pane's folder.
// Clicking it opens warp-lite:focus/<window>/<pid>/<pane id>, a protocol the installer registers: that Warp.exe passes
// it to the window (ToastClicked), which goes to the pane, still pulsing, and comes to front. A protocol and not the
// toast's Activated event: it reaches whichever window process showed it, also from the action center, and it is the
// same path the tests drive. A closing window takes its toasts with it.
// Windows shows a desktop app's toasts when its AppUserModelID is on a Start menu shortcut: the installer puts kAppId
// on Warp.lnk, and every Warp process takes it before its window exists, so the taskbar keeps them with that shortcut.
// WinRT through combase.dll, loaded with the first toast: nothing of it loads or runs before.

static const wchar_t* kAppId = L"justdiego.Warp";  // the same in setup.cpp
static const wchar_t* kToastScheme = L"warp-lite";  // the same in setup.cpp

// notify.txt: «toasts 1». Read each time: a toggle is for every window, and for a Warp that starts later.
static std::wstring NotifySettingsPath() {
  return g_windowDir.empty() ? g_sessionFile + L".notify" : g_windowDir + L"\\notify.txt";
}

static bool ToastsOn() {
  int v = 0;
  if (FILE* f = _wfopen(NotifySettingsPath().c_str(), L"r")) {
    if (fscanf(f, "toasts %d", &v) != 1) v = 0;
    fclose(f);
  }
  return v != 0;
}

static void SetToasts(bool on) {
  if (FILE* f = _wfopen(NotifySettingsPath().c_str(), L"w")) {
    fprintf(f, "toasts %d\n", on ? 1 : 0);
    fclose(f);
  }
}

// Windows.UI.Notifications and Windows.Data.Xml.Dom by hand (the toolchain has no WinRT headers): each interface up to
// the last method used, in the order of its IDL.
struct IToastNotification;
struct IToastNotifier : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE Show(IToastNotification* n) = 0;
  virtual HRESULT STDMETHODCALLTYPE Hide(IToastNotification* n) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Setting(int* value) = 0;  // NotificationSetting: 0 enabled, 1-4 why not
};
struct IToastNotificationHistory : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE RemoveGroup(HSTRING group) = 0;
  virtual HRESULT STDMETHODCALLTYPE RemoveGroupWithId(HSTRING group, HSTRING app) = 0;
  virtual HRESULT STDMETHODCALLTYPE RemoveGroupedTagWithId(HSTRING tag, HSTRING group, HSTRING app) = 0;
};
struct IToastNotificationManagerStatics : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE CreateToastNotifier(IToastNotifier** result) = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateToastNotifierWithId(HSTRING app, IToastNotifier** result) = 0;
};
struct IToastNotificationManagerStatics2 : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_History(IToastNotificationHistory** value) = 0;
};
struct IToastNotificationFactory : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE CreateToastNotification(IInspectable* xml, IToastNotification** value) = 0;
};
struct IToastNotification : IInspectable {};
struct IToastNotification2 : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE put_Tag(HSTRING value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Tag(HSTRING* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_Group(HSTRING value) = 0;
};
struct IXmlDocumentIO : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE LoadXml(HSTRING xml) = 0;
};
static const GUID kIidToastStatics = {0x50ac103f, 0xd235, 0x4598, {0xbb, 0xef, 0x98, 0xfe, 0x4d, 0x1a, 0x3a, 0xd4}};
static const GUID kIidToastStatics2 = {0x7ab93c52, 0x0e48, 0x4750, {0xba, 0x9d, 0x1a, 0x41, 0x13, 0x98, 0x18, 0x47}};
static const GUID kIidToastFactory = {0x04124b20, 0x82c6, 0x4229, {0xb1, 0x09, 0xfd, 0x9e, 0xd4, 0x66, 0x2b, 0x53}};
static const GUID kIidToast2 = {0x9dfb9fd1, 0x143a, 0x490e, {0x90, 0xbf, 0xb9, 0xfb, 0xa7, 0x13, 0x2d, 0xe7}};
static const GUID kIidXmlIO = {0x6cd0e74e, 0xee65, 0x4489, {0x9e, 0xbf, 0xca, 0x43, 0xe8, 0x7b, 0xa6, 0x37}};
static const GUID kIidXmlDoc = {0xf7f3a506, 0x1e87, 0x42d6, {0xbc, 0xfb, 0xb8, 0xc8, 0x09, 0xfa, 0x54, 0x94}};

typedef HRESULT(WINAPI* PFN_RoGetActivationFactory)(HSTRING, REFIID, void**);
typedef HRESULT(WINAPI* PFN_RoActivateInstance)(HSTRING, IInspectable**);
typedef HRESULT(WINAPI* PFN_WindowsCreateString)(const wchar_t*, UINT32, HSTRING*);
typedef HRESULT(WINAPI* PFN_WindowsDeleteString)(HSTRING);
struct Toaster {
  int state = 0;  // 0 not tried yet, 1 ready, -1 Windows can't
  PFN_RoGetActivationFactory factoryOf = nullptr;
  PFN_RoActivateInstance activate = nullptr;
  PFN_WindowsCreateString hstring = nullptr;
  PFN_WindowsDeleteString hfree = nullptr;
  IToastNotifier* notifier = nullptr;
  IToastNotificationHistory* history = nullptr;
  IToastNotificationFactory* factory = nullptr;
  // tests (step «toast-probe»): what each call returned
  HRESULT staticsHr = S_FALSE, notifierHr = S_FALSE, historyHr = S_FALSE, factoryHr = S_FALSE;
};
static Toaster g_toaster;

// An HSTRING for the length of one call.
struct HStr {
  HSTRING h = nullptr;
  HStr(const std::wstring& s) {
    if (g_toaster.hstring) g_toaster.hstring(s.c_str(), (UINT32)s.size(), &h);
  }
  ~HStr() {
    if (h) g_toaster.hfree(h);
  }
  operator HSTRING() const { return h; }
};

static bool LoadCombase() {
  if (g_toaster.hfree) return true;
  HMODULE cb = LoadLibraryW(L"combase.dll");
  if (!cb) return false;
  g_toaster.factoryOf = (PFN_RoGetActivationFactory)(void*)GetProcAddress(cb, "RoGetActivationFactory");
  g_toaster.activate = (PFN_RoActivateInstance)(void*)GetProcAddress(cb, "RoActivateInstance");
  g_toaster.hstring = (PFN_WindowsCreateString)(void*)GetProcAddress(cb, "WindowsCreateString");
  auto hfree = (PFN_WindowsDeleteString)(void*)GetProcAddress(cb, "WindowsDeleteString");
  if (!g_toaster.factoryOf || !g_toaster.activate || !g_toaster.hstring || !hfree) return false;
  g_toaster.hfree = hfree;
  return true;
}

// The notifier for kAppId, the action center's history and the toast factory, once per process.
static bool ToastInit() {
  if (g_toaster.state) return g_toaster.state > 0;
  g_toaster.state = -1;
  if (!LoadCombase()) return false;
  IToastNotificationManagerStatics* st = nullptr;
  IToastNotificationManagerStatics2* st2 = nullptr;
  g_toaster.staticsHr = g_toaster.factoryOf(HStr(L"Windows.UI.Notifications.ToastNotificationManager"), kIidToastStatics, (void**)&st);
  if (FAILED(g_toaster.staticsHr) || !st) return false;
  g_toaster.notifierHr = st->CreateToastNotifierWithId(HStr(kAppId), &g_toaster.notifier);
  g_toaster.historyHr = st->QueryInterface(kIidToastStatics2, (void**)&st2);
  if (SUCCEEDED(g_toaster.historyHr) && st2) g_toaster.historyHr = st2->get_History(&g_toaster.history), st2->Release();
  st->Release();
  g_toaster.factoryHr = g_toaster.factoryOf(HStr(L"Windows.UI.Notifications.ToastNotification"), kIidToastFactory, (void**)&g_toaster.factory);
  if (FAILED(g_toaster.notifierHr) || !g_toaster.notifier || FAILED(g_toaster.factoryHr) || !g_toaster.factory) return false;
  g_toaster.state = 1;
  return true;
}

// A toast from its XML, with its tag and group (what replaces and removes it). hr: each step's result, for the tests.
static IToastNotification* MakeToast(const std::wstring& xml, const std::wstring& tag, const std::wstring& group,
                                     HRESULT hr[3] = nullptr) {
  HRESULT local[3] = {S_FALSE, S_FALSE, S_FALSE};
  if (!hr) hr = local;
  IInspectable* doc = nullptr;
  IXmlDocumentIO* io = nullptr;
  IInspectable* xdoc = nullptr;
  IToastNotification* n = nullptr;
  IToastNotification2* n2 = nullptr;
  hr[0] = g_toaster.activate(HStr(L"Windows.Data.Xml.Dom.XmlDocument"), &doc);
  if (SUCCEEDED(hr[0]) && doc && SUCCEEDED(hr[0] = doc->QueryInterface(kIidXmlIO, (void**)&io)) && io &&
      SUCCEEDED(hr[0] = io->LoadXml(HStr(xml))) && SUCCEEDED(hr[0] = doc->QueryInterface(kIidXmlDoc, (void**)&xdoc)) && xdoc)
    hr[1] = g_toaster.factory->CreateToastNotification(xdoc, &n);
  if (n && SUCCEEDED(hr[2] = n->QueryInterface(kIidToast2, (void**)&n2)) && n2) {
    hr[2] = n2->put_Tag(HStr(tag));
    if (SUCCEEDED(hr[2])) hr[2] = n2->put_Group(HStr(group));
    n2->Release();
  }
  if (xdoc) xdoc->Release();
  if (io) io->Release();
  if (doc) doc->Release();
  if (n && FAILED(hr[2])) n->Release(), n = nullptr;
  return n;
}

// Text for the toast's XML: escaped, without control characters or broken surrogates, cut at max characters.
static std::wstring XmlText(const std::wstring& s, size_t max) {
  std::wstring o;
  size_t n = 0;
  for (size_t i = 0; i < s.size(); i++) {
    wchar_t c = s[i];
    bool pair = c >= 0xD800 && c < 0xDC00 && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000;
    if (c >= 0xD800 && c < 0xE000 && !pair) continue;
    if (c == 0xFFFE || c == 0xFFFF) continue;
    if (n++ == max) {
      o += L'…';
      break;
    }
    if (c < 0x20 || (c >= 0x7f && c < 0xa0)) c = L' ';
    switch (c) {
      case L'&': o += L"&amp;"; break;
      case L'<': o += L"&lt;"; break;
      case L'>': o += L"&gt;"; break;
      case L'"': o += L"&quot;"; break;
      case L'\'': o += L"&apos;"; break;
      default: o += c;
    }
    if (pair) o += s[++i];
  }
  return o;
}

// What a click opens: the protocol with this window and pane (only digits: nothing to escape).
static std::wstring ToastLaunch(Pane* p) {
  wchar_t launch[96];
  swprintf(launch, 96, L"%ls:focus/%llu/%lu/%u", kToastScheme, (unsigned long long)(ULONG_PTR)g_win->hwnd,
           (unsigned long)GetCurrentProcessId(), p->id);
  return launch;
}

// The toast: no sound, and nothing from the program but text.
static std::wstring ToastXml(Pane* p, const std::wstring& head, const std::wstring& text, const std::wstring& from) {
  std::wstring x = L"<toast launch=\"" + ToastLaunch(p) + L"\" activationType=\"protocol\"><visual><binding template=\"ToastGeneric\"><text>" +
                   XmlText(head, 120) + L"</text>";
  if (!text.empty()) x += L"<text>" + XmlText(text, 300) + L"</text>";
  if (!from.empty()) x += L"<text placement=\"attribution\">" + XmlText(from, 80) + L"</text>";
  return x + L"</binding></visual><audio silent=\"true\"/></toast>";
}

static std::wstring ToastTag(Pane* p) { return L"p" + std::to_wstring(p->id); }
static std::wstring ToastGroup() { return L"w" + std::to_wstring(GetCurrentProcessId()); }

static void NotifyWindows(Pane* p, const std::wstring& title, const std::wstring& body) {
  if (!ToastsOn()) return;
  QUERY_USER_NOTIFICATION_STATE qs = (QUERY_USER_NOTIFICATION_STATE)0;  // 0: Windows didn't say
  if (FAILED(SHQueryUserNotificationState(&qs))) qs = (QUERY_USER_NOTIFICATION_STATE)0;
  bool accepts = qs == QUNS_ACCEPTS_NOTIFICATIONS;
  std::wstring label = PaneLabel(p);
  std::wstring xml = ToastXml(p, title.empty() ? label : title, body.empty() ? p->cwd : body, title.empty() ? L"" : label);
  if (!g_win->script.empty()) {  // tests: what would be shown (and whether Windows would take one now), never shown
    g_win->toastLog.push_back(L"tag=" + ToastTag(p) + L" group=" + ToastGroup() + L" accepts=" + std::to_wstring((int)qs) + L" " + xml);
    g_win->toastLaunch = ToastLaunch(p);
    return;
  }
  if (!accepts || !ToastInit()) return;
  IToastNotification* n = MakeToast(xml, ToastTag(p), ToastGroup());
  if (!n) return;
  // The pane's previous one goes first: replaced in place (same tag), Windows may leave its old text on screen.
  if (p->toasted && g_toaster.history) g_toaster.history->RemoveGroupedTagWithId(HStr(ToastTag(p)), HStr(ToastGroup()), HStr(kAppId));
  p->toasted = SUCCEEDED(g_toaster.notifier->Show(n));
  n->Release();
}

static void ToastRemove(Pane* p) {
  p->toasted = false;
  if (g_toaster.state > 0 && g_toaster.history)
    g_toaster.history->RemoveGroupedTagWithId(HStr(ToastTag(p)), HStr(ToastGroup()), HStr(kAppId));
}

// A click on a toast, in the process the protocol started (wWinMain, before anything else): the window it came from
// goes to its pane and comes to front. With that window gone, the Warp window in front comes forward; with none,
// nothing. Anything can open the protocol: only its exact form is taken (warp-lite:focus/<window>/<pid>/<pane id>,
// digits only), and it only ever focuses one of this Warp's own windows.
static void ToastClicked(const std::wstring& uri) {
  std::wstring head = std::wstring(kToastScheme) + L":focus/";
  if (uri.size() > 96 || _wcsnicmp(uri.c_str(), head.c_str(), head.size())) return;
  unsigned long long v[3] = {0, 0, 0};
  size_t i = head.size();
  for (int k = 0; k < 3; k++) {
    size_t d = i;
    while (i < uri.size() && uri[i] >= L'0' && uri[i] <= L'9') i++;
    if (i == d || i - d > 19) return;
    v[k] = wcstoull(uri.substr(d, i - d).c_str(), nullptr, 10);
    if (k < 2 && (i >= uri.size() || uri[i++] != L'/')) return;
  }
  if (i < uri.size() && uri[i] == L'/') i++;  // a trailing slash, if Windows adds one
  if (i != uri.size()) return;
  // The pane by its id, in whichever window it is now (it may have moved since); a toast of another run of Warp, or
  // of a pane gone, only brings the window in front forward.
  for (Window* w : v[1] == GetCurrentProcessId() ? g_windows : std::vector<Window*>{})
    for (int t = 0; t < (int)w->tabs.size(); t++) {
      std::vector<Pane*> ls;
      Leaves(w->tabs[t].root, ls);
      for (int j = 0; j < (int)ls.size(); j++)
        if (ls[j]->id == v[2]) {
          InWindow in(w);
          FocusPane(t, j);  // it keeps pulsing until clicked or typed in
          BringToFront(w->hwnd);  // allowed by the process the click started
          return;
        }
    }
  if (Window* w = FrontWindow()) BringToFront(w->hwnd);
}

// A closing window takes its toasts out of the action center: nothing would answer their click.
static void ToastsForget() {
  if (g_toaster.state <= 0 || !g_toaster.history) return;
  for (Pane* p : WindowPanes())
    if (p->toasted) ToastRemove(p);
}

// --script and its steps (edit:, submit...) run only in a test's Warp, with its own session (WARPLITE_SESSION or
// WARPLITE_DIR): never in the real one, whatever its command line says.
static bool TestInstance() {
  return GetEnvironmentVariableW(L"WARPLITE_SESSION", nullptr, 0) || GetEnvironmentVariableW(L"WARPLITE_DIR", nullptr, 0);
}

// ---------------------------------------------------------------------------
// Guion de prueba: pasos separados por '|', uno cada ~1,2 s.
//   type:<texto> enter splitR splitD tab close-pane left right up down swap-<dir> rename:<nombre>
//   zoom:<%> move:x,y,w,h maximize rename-start rename-type:<texto> shot:<fichero.bmp> wait quit
//   auto-update:on|off update-check update-click update-state key:<tab|down|up|enter|esc> edit-text comp drag-tab:<de>,<a> tabs clipboard (estos y hover/hover-text dejan el resultado en <sesión>.bench)
//   font:<family> font-wait font font-others toast (font, font-others and toast also write to <session>.bench)
//   cells join:<hex>,<hex>... brightness font-size (these write to <session>.bench) select:<text> select-to:<text> copy brightness:<RRGGBB>
//   font-size:<px> link-action:<col>,<row> hover-time:<col>,<row> path-probes path-spans:<row> path-scan-time (these
//   write to <session>.bench; hover reports paths as "path <full path>")
//   focus:on|off feed:<tab>[/<pane>]:<hex> badge-icon:<n>,<px>,<file.bmp> badges notes taskbar (the last three write to <session>.bench)
//   click-pane:<j> pulse-at:<ms>|off motion:on|off|windows minimize restore toasts:on|off toast-click[:<uri>|+:<text>] attention
//   where toasts toast-probe toast-click-done (the last five write to <session>.bench) back-sample (too) backshot:<file.bmp>
//   remote-pair remote-wait remote-close remote-remove:<n> wait-file:<path> remote-ticket remote-state (these two
//   write to <session>.bench)
//   drag-start:tab:<i>|pane:<j> drag-to:<window>,<x>,<y> drag-drop drop-tab:<i>,<window>,<x>,<y> drop-pane:<j>,<window>,<x>,<y>
//   (<window>: self, 0 for the screen, the label of a window of this Warp or the pid of another Warp) join-hint windows
//   label:<name> raise crash window-count pids grep:<text> desk-shot:<x>,<y>,<w>,<h>,<file.bmp> (drag-to, drop-*, join-hint, windows,
//   window-count, pids and grep write to <session>.bench)

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
  return g_windowDir.empty() ? g_sessionFile + L".update" : g_windowDir + L"\\update.txt";
}

// update.log: una línea por intento (para saber por qué falló, si falla).
static void UpdateLog(const std::wstring& line) {
  std::wstring path = g_windowDir.empty() ? g_sessionFile + L".updlog" : g_windowDir + L"\\update.log";
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
        if (!WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                 &total, &sz, WINHTTP_NO_HEADER_INDEX))
          total = 0;
        DWORD avail = 0;
        while (ok) {
          // A dropped connection ends the body early: that is an error, not the end of the file.
          if (!WinHttpQueryDataAvailable(r, &avail)) {
            ok = fail(L"read");
            break;
          }
          if (!avail) break;
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
            if (pct->exchange(v) != v) PostMessage(g_host, WM_APP_UPDPROG, 0, 0);
          }
        }
        if (ok && total && out.size() != total) {
          if (err) *err = L"cut off at " + std::to_wstring(out.size()) + L" of " + std::to_wstring(total);
          ok = false;
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
  if (!PostMessage(g_host, WM_APP_UPDATE, 0, (LPARAM)r)) delete r;
  return 0;
}

static HWND g_updAsker;  // the window where «Check for updates» was chosen: it says what came of it

static void CheckForUpdates(bool manual) {
  if (manual && g_win) g_updAsker = g_win->hwnd;
  if (g_upd.checking || g_upd.stage == UPD_INSTALLING) return;
  g_upd.checking = true;
  auto* r = new UpdateReply;
  r->manual = manual;
  HANDLE t = CreateThread(nullptr, 0, UpdateCheckThread, r, 0, nullptr);
  if (t) CloseHandle(t);
  else delete r, g_upd.checking = false;
  if (manual && g_win) Toast(T(S_UPD_CHECKING), 3000);
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
    // A flaky connection can cut the download: try again a few times before giving up.
    bool got = false;
    for (int attempt = 0; attempt < 3 && !got; attempt++) {
      if (attempt) Sleep(2000);
      data.clear(), why.clear();
      got = HttpGet(job->url, data, 64 << 20, g_upd.testUrl, &why, &g_updPct);
    }
    if (!got) {
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
      PostMessage(g_host, WM_APP_UPDPROG, 0, 0);
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
  if (!PostMessage(g_host, WM_APP_UPDATED, 0, (LPARAM)job)) delete job;
  return 0;
}

static void InstallUpdate() {
  if (g_upd.stage == UPD_INSTALLING || g_upd.url.empty()) return;
  g_upd.stage = UPD_INSTALLING;
  g_upd.hidden = false;
  g_updPhase = 0, g_updPct = -1;
  SetTimer(g_host, TIMER_UPDANIM, 50, nullptr);
  UpdateLog(L"instalando " + g_upd.latest + L" desde " + g_upd.running);
  auto* job = new InstallJob{g_upd.latest, g_upd.url, g_upd.sha};
  HANDLE t = CreateThread(nullptr, 0, InstallThread, job, 0, nullptr);
  if (t) CloseHandle(t);
  else delete job, g_upd.stage = UPD_FAILED;
  InvalidateAll();
}

// Other Warps (another instance, or one of a version before) learn that an update is installed; this one's windows
// share g_upd.
static BOOL CALLBACK SendUpdated(HWND h, LPARAM) {
  wchar_t cls[32];
  DWORD pid = 0;
  if (GetWindowThreadProcessId(h, &pid) && pid != GetCurrentProcessId() && GetClassNameW(h, cls, 32) && !wcscmp(cls, L"WarpLite"))
    PostMessageW(h, g_msgUpdated, 0, 0);
  return TRUE;
}

// ¿Ya está instalada (en disco) una versión más nueva que la que corre? Entonces toca reiniciar.
static bool UpdateReadyOnDisk() {
  std::wstring disk = ExeVersion(InstalledExe());
  return !disk.empty() && ParseVersion(disk) > ParseVersion(g_upd.running);
}

static void OnUpdateReply(UpdateReply* r) {
  g_upd.checking = false;
  Window* asker = r->manual ? LiveWindow(g_updAsker) : nullptr;
  InWindow in(asker);
  if (!r->ok) {
    if (asker) Toast(T(S_UPD_OFFLINE), 3000);
    UpdateLog(L"no se pudo comprobar" + std::wstring(r->manual ? L" (a mano)" : L""));
    return;
  }
  if (ParseVersion(r->latest) <= ParseVersion(g_upd.running)) {
    if (asker) Toast(std::wstring(T(S_UPD_LATEST)) + L" (" + g_upd.running + L")", 3000);
    return;
  }
  bool newer = r->latest != g_upd.latest;
  g_upd.latest = r->latest, g_upd.url = r->url, g_upd.sha = r->sha;
  if (newer || r->manual) g_upd.hidden = false;
  if (ParseVersion(ExeVersion(InstalledExe())) >= ParseVersion(r->latest)) g_upd.stage = UPD_READY;
  else if (g_upd.autoUpdate || (r->manual && g_upd.stage == UPD_AVAILABLE)) InstallUpdate();
  else g_upd.stage = UPD_AVAILABLE;
  InvalidateAll();
}

static void OnUpdateInstalled(InstallJob* job) {
  KillTimer(g_host, TIMER_UPDANIM);
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
  InvalidateAll();
}

// Reiniciar: se abre el Warp instalado (espera a que se cierren todas) y se cierran todas
// guardándose; al abrir vuelven todas las ventanas, also those that don't reopen at a normal start
// (they keep that, and a later close discards them). exe: the installed one (a test: itself).
static void RestartForUpdate(const std::wstring& exe) {
  std::wstring cmd = L"\"" + exe + L"\" --relaunch";
  STARTUPINFOW si{sizeof si};
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return;
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  QuitAll(true);
}

static void UpdateAction() {
  if (g_upd.stage == UPD_AVAILABLE || g_upd.stage == UPD_FAILED) InstallUpdate();
  else if (g_upd.stage == UPD_READY) RestartForUpdate(InstalledExe());
}

// ---------------------------------------------------------------------------
// Fonts (title-bar menu → Font). Hack is embedded and Consolas comes with Windows; the others are
// downloaded on first use from the google/fonts repo (Cascadia Mono only if Windows lacks it) into
// %LOCALAPPDATA%\warp-lite\fonts and loaded for this process only. The choice applies to every
// window of this Warp and survives restarts.

struct FontChoice {
  const wchar_t* face;
  const wchar_t* files[4];  // paths in google/fonts; the last segment is the cached file name
};
static const FontChoice kFonts[] = {
    {L"Hack", {}},
    {L"Cascadia Mono", {L"ofl/cascadiamono/CascadiaMono[wght].ttf", L"ofl/cascadiamono/CascadiaMono-Italic[wght].ttf"}},
    {L"Consolas", {}},
    {L"JetBrains Mono", {L"ofl/jetbrainsmono/JetBrainsMono[wght].ttf", L"ofl/jetbrainsmono/JetBrainsMono-Italic[wght].ttf"}},
    {L"Fira Code", {L"ofl/firacode/FiraCode[wght].ttf"}},
    {L"Source Code Pro", {L"ofl/sourcecodepro/SourceCodePro[wght].ttf", L"ofl/sourcecodepro/SourceCodePro-Italic[wght].ttf"}},
    {L"Roboto Mono", {L"ofl/robotomono/RobotoMono[wght].ttf", L"ofl/robotomono/RobotoMono-Italic[wght].ttf"}},
    {L"IBM Plex Mono", {L"ofl/ibmplexmono/IBMPlexMono-Regular.ttf", L"ofl/ibmplexmono/IBMPlexMono-Bold.ttf",
                        L"ofl/ibmplexmono/IBMPlexMono-Italic.ttf", L"ofl/ibmplexmono/IBMPlexMono-BoldItalic.ttf"}},
    {L"Ubuntu Mono", {L"ufl/ubuntumono/UbuntuMono-Regular.ttf", L"ufl/ubuntumono/UbuntuMono-Bold.ttf",
                      L"ufl/ubuntumono/UbuntuMono-Italic.ttf", L"ufl/ubuntumono/UbuntuMono-BoldItalic.ttf"}},
    {L"Inconsolata", {L"ofl/inconsolata/Inconsolata[wdth,wght].ttf"}},
};
static const int kFontCount = sizeof kFonts / sizeof kFonts[0];
static const int kBuiltinFonts = 3;  // Hack, Cascadia Mono, Consolas; the downloaded ones go after a separator
static const wchar_t* kFontUrl = L"https://raw.githubusercontent.com/google/fonts/main/";
static std::wstring g_fontWanted;   // switch to this font when its download finishes
static std::wstring g_fontError;    // why the last download failed (tests)
static std::set<std::wstring> g_fontFilesLoaded;  // cached files already added to this process

// Tests (WARPLITE_SESSION / WARPLITE_DIR) keep their downloads next to their session.
static std::wstring FontCacheDir() {
  wchar_t buf[MAX_PATH];
  if (g_windowDir.empty()) return g_sessionFile + L".fonts";
  if (GetEnvironmentVariableW(L"WARPLITE_DIR", buf, MAX_PATH) || !GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH))
    return g_windowDir + L"\\fonts";
  return std::wstring(buf) + L"\\warp-lite\\fonts";
}

static std::wstring FontSettingsPath() {
  return g_windowDir.empty() ? g_sessionFile + L".font" : g_windowDir + L"\\font.txt";
}

static std::wstring ReadFontSetting() {
  std::string s;
  if (FILE* f = _wfopen(FontSettingsPath().c_str(), L"rb")) {
    char b[256];
    s.assign(b, fread(b, 1, sizeof b, f));
    fclose(f);
  }
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
  return ToWide(s);
}

static void SaveFontSetting() {
  if (FILE* f = _wfopen(FontSettingsPath().c_str(), L"wb")) {
    std::string s = ToUtf8(g_fontFace) + "\n";
    fwrite(s.data(), 1, s.size(), f);
    fclose(f);
  }
}

static const FontChoice* FindFont(const std::wstring& face) {
  for (auto& f : kFonts)
    if (!_wcsicmp(f.face, face.c_str())) return &f;
  return nullptr;
}

static std::wstring FontFileName(const wchar_t* path) {
  const wchar_t* s = wcsrchr(path, L'/');
  return s ? s + 1 : path;
}

// Can GDI use this family in this process (installed, embedded or loaded from the cache)?
static bool FontInstalled(const std::wstring& face) {
  if (face.empty() || face.size() >= LF_FACESIZE) return false;
  if (!_wcsicmp(face.c_str(), L"Hack")) return true;  // embedded (AddFontMemResourceEx fonts are not enumerated)
  LOGFONTW lf{};
  lf.lfCharSet = DEFAULT_CHARSET;
  wcscpy(lf.lfFaceName, face.c_str());
  bool found = false;
  HDC dc = GetDC(nullptr);
  EnumFontFamiliesExW(dc, &lf, [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM lp) -> int { return *(bool*)lp = true, 0; },
                      (LPARAM)&found, 0);
  ReleaseDC(nullptr, dc);
  return found;
}

// Adds the cached files of a downloadable font to this process. False if one is missing or broken
// (a broken one is deleted, so that the next try downloads it again).
static bool LoadFontFiles(const std::wstring& face) {
  const FontChoice* c = FindFont(face);
  if (!c || !c->files[0]) return false;
  for (const wchar_t* f : c->files) {
    if (!f) break;
    std::wstring path = FontCacheDir() + L"\\" + FontFileName(f);
    if (g_fontFilesLoaded.count(path)) continue;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    if (!AddFontResourceExW(path.c_str(), FR_PRIVATE, nullptr)) {
      DeleteFileW(path.c_str());
      return false;
    }
    g_fontFilesLoaded.insert(path);
  }
  return FontInstalled(face);
}

struct FontJob {
  std::wstring face, dir;
  std::vector<std::wstring> urls, files;  // what to download and its name in dir
  bool allowHttp = false;
  bool ok = false;
  std::wstring error;
};

static DWORD WINAPI FontDownloadThread(LPVOID arg) {
  auto* job = (FontJob*)arg;
  SHCreateDirectoryExW(nullptr, job->dir.c_str(), nullptr);
  job->ok = true;
  for (size_t i = 0; job->ok && i < job->urls.size(); i++) {
    g_fontFile = (int)i, g_fontPct = -1;
    std::string data;
    std::wstring path = job->dir + L"\\" + job->files[i], tmp = path + L".tmp";
    if (!HttpGet(job->urls[i], data, 32 << 20, job->allowHttp, &job->error, &g_fontPct)) {
      job->ok = false;
    } else if (data.size() < 12 || (data.compare(0, 4, std::string("\0\1\0\0", 4)) && data.compare(0, 4, "OTTO") &&
                                    data.compare(0, 4, "true"))) {
      job->ok = false, job->error = L"not a font";
    } else {
      HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
      DWORD w = 0;
      bool written = f != INVALID_HANDLE_VALUE && WriteFile(f, data.data(), (DWORD)data.size(), &w, nullptr) && w == data.size();
      if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
      // If another window has the file loaded already, it is complete: that one stays.
      if (!written || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) job->ok = false, job->error = L"write";
      }
    }
  }
  if (!PostMessage(g_host, WM_APP_FONT, 0, (LPARAM)job)) delete job;
  return 0;
}

static HWND g_fontAsker;  // the window where the font being downloaded was chosen: it says if it fails

static void FontFailed(const std::wstring& face) {
  wchar_t b[160];
  swprintf(b, 160, T(S_FONT_FAILED), face.c_str());
  Toast(b, 4000);
}

static void StartFontDownload(const FontChoice& c) {
  auto* job = new FontJob;
  job->face = c.face;
  job->dir = FontCacheDir();
  wchar_t buf[2048];
  std::wstring base = GetEnvironmentVariableW(L"WARPLITE_FONT_URL", buf, 2048) ? buf : kFontUrl;  // tests: another server
  job->allowHttp = base != kFontUrl;
  for (const wchar_t* f : c.files) {
    if (!f) break;
    std::wstring name = FontFileName(f);
    if (GetFileAttributesW((job->dir + L"\\" + name).c_str()) != INVALID_FILE_ATTRIBUTES) continue;  // from an earlier try
    std::wstring url = base;
    for (const wchar_t* p = f; *p; p++) {
      if (*p == L'[') url += L"%5B";
      else if (*p == L']') url += L"%5D";
      else if (*p == L',') url += L"%2C";
      else url += *p;
    }
    job->urls.push_back(url);
    job->files.push_back(name);
  }
  g_fontWanted = g_fontLoading = c.face;
  g_fontError.clear();
  g_fontFiles = std::max(1, (int)job->urls.size());
  g_fontFile = 0, g_fontPct = -1;
  g_fontAsker = g_win->hwnd;
  HANDLE t = CreateThread(nullptr, 0, FontDownloadThread, job, 0, nullptr);
  if (t) CloseHandle(t);
  else delete job, g_fontLoading.clear(), FontFailed(c.face);
  InvalidateAll();  // the download's progress is in every window's title bar
}

// Switches this window to face: new fonts and cell size, and every terminal resized to it.
static void ApplyTerminalFont(const std::wstring& face) {
  g_fontFace = face;
  g_fbCache.clear();  // which characters the main font lacks
  MakeFonts();
  Relayout();
}

// Chosen in a window (g_win, or none): every window of this Warp switches to it, and it is saved. The one where it was
// chosen says so.
static void UseFont(const std::wstring& face) {
  for (Window* w : g_windows) {
    InWindow in(w);
    ApplyTerminalFont(face);
  }
  SaveFontSetting();
  if (g_win) Toast(face);
}

static void OnFontDownloaded(FontJob* job) {
  g_fontLoading.clear();
  bool wanted = job->face == g_fontWanted;
  g_fontWanted.clear();
  InWindow in(LiveWindow(g_fontAsker));
  if (job->ok && LoadFontFiles(job->face)) {
    if (wanted && _wcsicmp(job->face.c_str(), g_fontFace.c_str())) UseFont(job->face);
  } else {
    g_fontError = job->ok ? L"load" : job->error;
    if (g_win) FontFailed(job->face);  // the current font stays
  }
  InvalidateAll();
}

// Title-bar menu: an available font applies at once; one from the list is downloaded first.
static void SetTerminalFont(const std::wstring& face) {
  if (FontInstalled(face) || LoadFontFiles(face)) {
    g_fontWanted.clear();  // a download in progress no longer switches the font
    if (_wcsicmp(face.c_str(), g_fontFace.c_str())) UseFont(face);
    return;
  }
  const FontChoice* c = FindFont(face);
  if (!c || !c->files[0]) return;
  if (g_fontLoading.empty()) StartFontDownload(*c);
  else if (face == g_fontLoading) g_fontWanted = face;  // one download at a time
}

// Saved font, before the window is created (after the embedded fonts: it may be Hack).
static void LoadFontSetting() {
  std::wstring face = ReadFontSetting();
  if (!face.empty() && (FontInstalled(face) || LoadFontFiles(face))) g_fontFace = face;
}

// Text brightness, the default terminal foreground: shared by the windows of this Warp like the font, as RRGGBB.
static std::wstring TextSettingsPath() {
  return g_windowDir.empty() ? g_sessionFile + L".brightness" : g_windowDir + L"\\brightness.txt";
}

static bool ReadTextSetting(COLORREF* c) {
  char b[8] = {};
  FILE* f = _wfopen(TextSettingsPath().c_str(), L"rb");
  if (!f) return false;
  size_t n = fread(b, 1, 6, f);
  fclose(f);
  if (n < 6) return false;
  for (size_t i = 0; i < 6; i++)
    if (!isxdigit((unsigned char)b[i])) return false;
  unsigned long v = strtoul(b, nullptr, 16);
  *c = RGB(v >> 16 & 255, v >> 8 & 255, v & 255);
  return true;
}

// Switches this window to text color c: the panes' default foreground, repainted.
static void ApplyTextColor(COLORREF c) {
  g_termFg = c;
  VTermColor fg;
  vterm_color_rgb(&fg, GetRValue(c), GetGValue(c), GetBValue(c));
  for (Pane* p : g_live) {
    EnterCriticalSection(&p->cs);
    vterm_state_set_default_colors(vterm_obtain_state(p->vt), &fg, nullptr);
    p->fullDirty = true;
    LeaveCriticalSection(&p->cs);
  }
  InvalidateAll();
}

// Chosen in this window: every window of this Warp takes it, and it is saved.
static void SetTextBrightness(COLORREF c) {
  ApplyTextColor(c);
  if (FILE* f = _wfopen(TextSettingsPath().c_str(), L"wb")) {
    fprintf(f, "%02X%02X%02X\n", GetRValue(c), GetGValue(c), GetBValue(c));
    fclose(f);
  }
  wchar_t t[16];
  swprintf(t, 16, L"#%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
  Toast(t);
}

// Other monospaced families installed in Windows (menu → Font → Other installed), sorted. Without
// vertical (@) fonts, the ones already in the menu, and the weights of a family (Cascadia Mono SemiBold).
static std::vector<std::wstring> OtherMonoFonts() {
  std::set<std::wstring> all;
  LOGFONTW lf{};
  lf.lfCharSet = DEFAULT_CHARSET;
  HDC dc = GetDC(nullptr);
  EnumFontFamiliesExW(
      dc, &lf,
      [](const LOGFONTW* l, const TEXTMETRICW*, DWORD type, LPARAM lp) -> int {
        if (!(type & RASTER_FONTTYPE) && (l->lfPitchAndFamily & 3) == FIXED_PITCH && l->lfFaceName[0] != L'@' &&
            l->lfCharSet != SYMBOL_CHARSET)
          ((std::set<std::wstring>*)lp)->insert(l->lfFaceName);
        return 1;
      },
      (LPARAM)&all, 0);
  ReleaseDC(nullptr, dc);
  auto weightOf = [](const std::wstring& n, const wchar_t* family) {
    size_t k = wcslen(family);
    return n.size() > k + 1 && n[k] == L' ' && !_wcsnicmp(n.c_str(), family, k);
  };
  std::vector<std::wstring> out;
  for (auto& n : all) {
    bool skip = !_wcsicmp(n.c_str(), kFallbackFaces[0]);  // Symbols Nerd Font Mono: the embedded icons
    for (auto& f : kFonts) skip = skip || !_wcsicmp(n.c_str(), f.face) || weightOf(n, f.face);
    for (auto& k : all) skip = skip || weightOf(n, k.c_str());
    if (!skip) out.push_back(n);
  }
  std::sort(out.begin(), out.end(), [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
  return out;
}

// Tests: the rendering mode DirectWrite picks and the face it really draws each style with (family, weight
// —the wght axis of a variable font— and simulations). No brackets: the GDI part above is parsed by them.
static std::wstring DwReport() {
  if (!g_dw.on) return L" dwrite: off";
  static const wchar_t* kMode[] = {L"default", L"aliased", L"gdi_classic", L"gdi_natural", L"natural", L"natural_symmetric", L"outline"};
  DWRITE_RENDERING_MODE m = DWRITE_RENDERING_MODE_DEFAULT;
  g_dw.faces[0]->GetRecommendedRenderingMode(g_win->dwEm, 1.0f, DWRITE_MEASURING_MODE_NATURAL, g_dw.params, &m);
  std::wstring r = L" dwrite: " + std::wstring((unsigned)m < 7 ? kMode[m] : L"?");
  for (IDWriteFontFace* f : g_dw.faces) {
    std::wstring name = L"?";
    int weight = 0;
    IDWriteFontFace3* f3 = nullptr;
    if (SUCCEEDED(f->QueryInterface(__uuidof(IDWriteFontFace3), (void**)&f3))) {
      IDWriteLocalizedStrings* s = nullptr;
      UINT32 i = 0, len = 0;
      BOOL en = FALSE;
      if (SUCCEEDED(f3->GetFamilyNames(&s)) && SUCCEEDED(s->FindLocaleName(L"en-us", &i, &en)) &&
          SUCCEEDED(s->GetStringLength(en ? i : 0, &len))) {
        name.assign(len + 1, L'\0');
        s->GetString(en ? i : 0, &name[0], len + 1);
        name.resize(len);
      }
      if (s) s->Release();
      weight = f3->GetWeight();
      if (f3->GetStyle() != DWRITE_FONT_STYLE_NORMAL) name += L" italic";
      f3->Release();
    }
    IDWriteFontFace5* f5 = nullptr;
    if (SUCCEEDED(f->QueryInterface(__uuidof(IDWriteFontFace5), (void**)&f5))) {
      DWRITE_FONT_AXIS_VALUE v[8];
      UINT32 n = std::min<UINT32>(f5->GetFontAxisValueCount(), 8);
      if (f5->HasVariations() && SUCCEEDED(f5->GetFontAxisValues(v, n)))
        for (UINT32 k = 0; k < n; k++)
          if (v[k].axisTag == DWRITE_FONT_AXIS_TAG_WEIGHT) weight = (int)v[k].value;
      f5->Release();
    }
    r += L" / " + name + L" " + std::to_wstring(weight);
    if (f->GetSimulations() & DWRITE_FONT_SIMULATIONS_BOLD) r += L" +bold";
    if (f->GetSimulations() & DWRITE_FONT_SIMULATIONS_OBLIQUE) r += L" +oblique";
  }
  r += g_dw.emojiColor ? L" / emoji: color" : L" / emoji: mono";
  // The system's gamma and contrasts, which DirectWrite applies when blending (Windows Terminal reproduces the same
  // blend in its shader; the grayscale contrast fades out for light text on a dark background).
  IDWriteRenderingParams1* p1 = nullptr;
  if (SUCCEEDED(g_dw.params->QueryInterface(__uuidof(IDWriteRenderingParams1), (void**)&p1))) {
    wchar_t b[96];
    swprintf(b, 96, L" / gamma %.2f contrast %.2f grayscale %.2f", p1->GetGamma(), p1->GetEnhancedContrast(),
             p1->GetGrayscaleEnhancedContrast());
    r += b;
    p1->Release();
  }
  return r;
}

// Tests: the face GDI really uses for each style («X Bold» is a real bold; «X Regular» with weight
// 700 would be a synthesized one) and the cell size.
static std::wstring FontReport() {
  static const wchar_t* kStyle[] = {L"regular", L"bold", L"italic", L"bolditalic"};
  wchar_t face[64] = L"";
  SelectObject(g_measureDC, g_win->fonts[0]);
  GetTextFaceW(g_measureDC, 64, face);
  std::wstring r = L"font: [" + std::wstring(face) + L"] " + std::to_wstring(g_win->cellW) + L"x" + std::to_wstring(g_win->cellH);
  if (_wcsicmp(face, g_fontFace.c_str())) r += L" (setting " + g_fontFace + L")";
  for (int i = 0; i < 4; i++) {
    SelectObject(g_measureDC, g_win->fonts[i]);
    std::vector<char> buf(GetOutlineTextMetricsW(g_measureDC, 0, nullptr));
    auto* otm = (OUTLINETEXTMETRICW*)buf.data();
    if (buf.size() >= sizeof *otm && GetOutlineTextMetricsW(g_measureDC, (UINT)buf.size(), otm))
      r += L" " + std::wstring(kStyle[i]) + L"=[" + (const wchar_t*)(buf.data() + (size_t)otm->otmpFullName) + L" " +
           std::to_wstring(otm->otmTextMetrics.tmWeight) + L"]";
  }
  r += DwReport();
  if (!g_fontError.empty()) r += L" error=[" + g_fontError + L"]";
  return r;
}

// What the hover steps report: the URL, "path <full path>" or "(sin enlace)".
static std::wstring LinkLabel(const LinkHit& h) {
  return !h.url.empty() ? h.url : !h.path.empty() ? L"path " + h.path : L"(sin enlace)";
}

// Resultado de un paso, una línea en <sesión>.bench (lo leen los wintest-*.ps1).
static void ScriptLog(const std::wstring& r) {
  std::string u = ToUtf8(r) + "\n";
  HANDLE fh = CreateFileW((g_win->sessionPath + L".bench").c_str(), FILE_APPEND_DATA, 0, nullptr, OPEN_ALWAYS, 0, nullptr);
  if (fh == INVALID_HANDLE_VALUE) return;
  DWORD w;
  WriteFile(fh, u.data(), (DWORD)u.size(), &w, nullptr);
  CloseHandle(fh);
}

// Tests: a mouse drag made of this window's own mouse messages (drag-start:, drag-to:, drag-drop), so a drop on another
// window goes through the same code as one by hand. Where the mouse is (client coordinates; outside it while captured).

// The Warp window of another process (another Warp).
static HWND WarpWindowOf(DWORD pid) {
  struct Ctx {
    DWORD pid;
    HWND h;
  } c{pid, nullptr};
  EnumWindows(
      [](HWND h, LPARAM lp) -> BOOL {
        Ctx* c = (Ctx*)lp;
        DWORD owner = 0;
        wchar_t cls[32];
        if (GetWindowThreadProcessId(h, &owner) && owner == c->pid && GetClassNameW(h, cls, 32) && !wcscmp(cls, L"WarpLite"))
          c->h = h;
        return c->h ? FALSE : TRUE;
      },
      (LPARAM)&c);
  return c.h;
}

// A window of this Warp by its label (label: step; w<n> otherwise).
static Window* WindowLabeled(const std::wstring& label) {
  for (Window* w : g_windows)
    if (w->label == label) return w;
  return nullptr;
}

// What a test calls a window: the label of one of this Warp's, the pid of another Warp's, or none.
static std::wstring WindowName(HWND h) {
  if (Window* w = LiveWindow(h)) return w->label;
  DWORD pid = 0;
  return h && GetWindowThreadProcessId(h, &pid) ? std::to_wstring(pid) : L"none";
}

// Presses the mouse on that tab (pane: on the header of that pane of the tab in view, which a split tab has).
static bool ScriptDragStart(bool pane, int i) {
  POINT a{-1, -1};
  if (!pane) {
    for (auto& [k, r] : ComputeTabs().tabs)
      if (k == i) a = POINT{(r.left + r.right) / 2 - Px(30), (r.top + r.bottom) / 2};
  } else {
    std::vector<Pane*> ls;
    Leaves(CurTab().root, ls);
    RECT h = i >= 0 && i < (int)ls.size() ? HeaderRect(ls[i]) : RECT{};
    if (h.bottom > h.top) a = POINT{(h.left + h.right) / 2, (h.top + h.bottom) / 2};
  }
  if (a.x < 0) return false;
  g_win->scriptDrag = a;
  SendMessageW(g_win->hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(a.x, a.y));
  return true;
}

// Moves the pressed mouse to (x, y) in the client area of window `to` (none: of the screen) and logs what is under it
// there: the window it would join, the window found there at all (labels of this Warp's, pids of others'), and whether
// it is outside this one.
static void ScriptDragTo(HWND to, int x, int y) {
  POINT sb{x, y};
  if (to) ClientToScreen(to, &sb);
  POINT b = sb, a = g_win->scriptDrag;
  ScreenToClient(g_win->hwnd, &b);
  for (int k = 1; k <= 8; k++) {
    POINT q{a.x + (b.x - a.x) * k / 8, a.y + (b.y - a.y) * k / 8};
    SendMessageW(g_win->hwnd, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(q.x, q.y));
  }
  g_win->scriptDrag = b;
  Window* join = JoinTargetAt(sb);
  ScriptLog(L"drag-to: " + std::to_wstring(sb.x) + L"," + std::to_wstring(sb.y) + L" join=" + (join ? join->label : L"none") +
            L" under=" + WindowName(TopWindowAt(sb)) + L" outside=" + (OutsideWindow(sb) ? L"1" : L"0"));
}

// «<window>,<x>,<y>» of a drag step: «self» (this window), 0 (x, y on the screen), a number (the pid of another Warp) or
// the label of one of this Warp's windows.
static void ScriptDragToText(const wchar_t* s) {
  const wchar_t* c = wcschr(s, L',');
  std::wstring who(s, c ? c - s : wcslen(s));
  int x = 0, y = 0;
  if (c) swscanf(c + 1, L"%d,%d", &x, &y);
  HWND to = nullptr;
  if (who == L"self") to = g_win->hwnd;
  else if (who != L"0" && who.find_first_not_of(L"0123456789") == std::wstring::npos) to = WarpWindowOf(wcstoul(who.c_str(), nullptr, 10));
  else if (Window* w = WindowLabeled(who)) to = w->hwnd;
  if (who != L"0" && !to) return ScriptLog(L"drag-to: no window " + who);
  ScriptDragTo(to, x, y);
}

// The windows of this Warp by label, in order: «<label>=[<tab>:<panes>] …», with «:<running>,…» in a tab whose panes
// run something.
static std::wstring WindowsText() {
  std::map<std::wstring, std::wstring> out;
  for (Window* w : g_windows) {
    InWindow in(w);
    std::wstring t;
    for (auto& tab : w->tabs) {
      std::vector<Pane*> ls;
      Leaves(tab.root, ls);
      std::wstring r;
      for (Pane* q : ls)
        if (!q->runningCmd.empty()) r += (r.empty() ? L":" : L",") + q->runningCmd;
      t += L" [" + (tab.name.empty() ? ShortTitle(tab.active) : tab.name) + L":" + std::to_wstring(ls.size()) + r + L"]";
    }
    out[w->label] = w->label + L"=" + (t.empty() ? t : t.substr(1));
  }
  std::wstring r;
  for (auto& [label, w] : out) r += (r.empty() ? L"" : L" | ") + w;
  return r;
}

// Every pane of this Warp as «<window label>/<tab name>/<pane>», in order, with f's text for it.
static std::wstring EachPane(const std::function<std::wstring(Pane*)>& f) {
  std::map<std::wstring, std::wstring> out;
  for (Window* w : g_windows) {
    InWindow in(w);
    for (auto& tab : w->tabs) {
      std::vector<Pane*> ls;
      Leaves(tab.root, ls);
      for (size_t j = 0; j < ls.size(); j++) {
        std::wstring v = f(ls[j]);
        if (!v.empty())
          out[w->label + L"/" + (tab.name.empty() ? ShortTitle(tab.active) : tab.name) + L"/" + std::to_wstring(j)] = v;
      }
    }
  }
  std::wstring r;
  for (auto& [k, v] : out) r += L" " + k + L"=" + v;
  return r.empty() ? L" (none)" : r;
}

// The lines of a pane's scrollback and screen that have that text: how many, the first and the last.
static std::wstring PaneGrep(Pane* p, const std::wstring& text) {
  EnterCriticalSection(&p->cs);
  int n = 0;
  std::wstring first, last;
  for (int l = 0, total = (int)p->sb.size() + p->rows; l < total; l++) {
    std::wstring t = AbsLineText(p, l);
    if (t.find(text) == std::wstring::npos) continue;
    while (!t.empty() && t.back() == L' ') t.pop_back();
    if (!n++) first = t;
    last = t;
  }
  LeaveCriticalSection(&p->cs);
  return n ? std::to_wstring(n) + L"[" + first + L"][" + last + L"]" : L"";
}

// Tests (desk-shot): this Warp's windows as they are on the screen, the one behind first, over a dark desk, with the
// mouse where a drag left it: what someone in front of it would see (a test's desktop shows nothing).
static bool SaveDeskShot(const std::wstring& path, RECT area) {
  int w = area.right - area.left, h = area.bottom - area.top;
  if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return false;
  BITMAPINFO bi{};
  bi.bmiHeader = {sizeof bi.bmiHeader, w, -h, 1, 32, BI_RGB};
  void* bits = nullptr;
  HDC dc = CreateCompatibleDC(nullptr);
  HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!bmp) return DeleteDC(dc), false;
  HGDIOBJ old = SelectObject(dc, bmp);
  Fill(dc, RECT{0, 0, w, h}, RGB(0x1b, 0x23, 0x2e));
  std::vector<Window*> order;  // back to front
  for (HWND x = GetTopWindow(nullptr); x; x = GetWindow(x, GW_HWNDNEXT))
    if (Window* win = LiveWindow(x)) order.insert(order.begin(), win);
  for (Window* win : order) {
    if (IsIconic(win->hwnd) || !(GetWindowLongW(win->hwnd, GWL_STYLE) & WS_VISIBLE)) continue;
    InWindow in(win);
    RECT c;
    GetClientRect(win->hwnd, &c);
    POINT o{0, 0};
    ClientToScreen(win->hwnd, &o);
    int saved = SaveDC(dc);
    SetViewportOrgEx(dc, o.x - area.left, o.y - area.top, nullptr);
    IntersectClipRect(dc, 0, 0, c.right, c.bottom);
    Render(dc, c.right, c.bottom);
    RestoreDC(dc, saved);
    HBRUSH b = CreateSolidBrush(RGB(0x3a, 0x3a, 0x3a));
    RECT fr{o.x - area.left - 1, o.y - area.top - 1, o.x - area.left + c.right + 1, o.y - area.top + c.bottom + 1};
    FrameRect(dc, &fr, b);
    DeleteObject(b);
  }
  if (g_win && GetCapture() == g_win->hwnd) {  // the mouse of a drag in progress
    POINT m = g_win->scriptDrag;
    ClientToScreen(g_win->hwnd, &m);
    DrawIconEx(dc, m.x - area.left, m.y - area.top, LoadCursor(nullptr, IDC_SIZEALL), 0, 0, 0, nullptr, DI_NORMAL);
  }
  GdiFlush();
  bool ok = WriteBmp(path, bits, w, h);
  SelectObject(dc, old);
  DeleteObject(bmp);
  DeleteDC(dc);
  return ok;
}

static void RunScriptStep() {
  if (g_win->scriptPos >= g_win->script.size()) return;
  std::wstring s = g_win->script[g_win->scriptPos++];
  Pane* p = g_win->tabs.empty() ? nullptr : CurTab().active;
  int delay = 1200;
  if (s.rfind(L"type:", 0) == 0 && p) SendText(p, s.substr(5)), delay = 300;
  else if (s == L"enter" && p) SendKey(p, VTERM_KEY_ENTER, VTERM_MOD_NONE);
  else if (s == L"splitR") SplitActive(true), delay = 2500;
  else if (s == L"splitD") SplitActive(false), delay = 2500;
  else if (s == L"tab") NewTab(g_win->activeTab + 1), delay = 2500;
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
    ShowWindow(g_win->hwnd, SW_RESTORE);
    SetWindowPos(g_win->hwnd, nullptr, x, y, w, h, SWP_NOZORDER);
  } else if (s == L"maximize") {
    ShowWindow(g_win->hwnd, SW_MAXIMIZE);
  } else if (s.rfind(L"edit:", 0) == 0 && p) {  // escribe en la caja de entrada
    for (wchar_t c : s.substr(5)) SendMessageW(g_win->hwnd, WM_CHAR, c, 0);
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
    GetClientRect(g_win->hwnd, &rc);
    df->pt = POINT{rc.right / 2, rc.bottom / 2};
    df->fWide = TRUE;
    memcpy((char*)df + sizeof(DROPFILES), files.data(), files.size() * sizeof(wchar_t));
    GlobalUnlock(hg);
    SendMessageW(g_win->hwnd, WM_DROPFILES, (WPARAM)hg, 0);
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
    g_win->link = LinkAt(p, p->content.left + kPad + c * g_win->cellW + 1, p->content.top + kPad + r * g_win->cellH + 1);
    Toast(LinkLabel(g_win->link));
    ScriptLog(L"hover " + std::to_wstring(c) + L"," + std::to_wstring(r) + L": " + LinkLabel(g_win->link));
  } else if (s.rfind(L"link-action:", 0) == 0 && p) {  // link-action:col,row — what Ctrl+click there would do (opens nothing)
    int c = 0, r = 0;
    swscanf(s.c_str() + 12, L"%d,%d", &c, &r);
    LinkHit h = LinkAt(p, p->content.left + kPad + c * g_win->cellW + 1, p->content.top + kPad + r * g_win->cellH + 1);
    ScriptLog(L"link-action " + std::to_wstring(c) + L"," + std::to_wstring(r) + L": " +
              (h.p ? kLinkActions[LinkAction(h)] + std::wstring(L" ") + (h.url.empty() ? h.path : h.url) : L"(sin enlace)"));
  } else if (s == L"path-probes") {  // disk lookups made for path links so far, and how many reached the network (0)
    ScriptLog(L"path-probes: " + std::to_wstring(g_pathProbes.load()) + L" remote " + std::to_wstring(g_remoteProbes.load()));
  } else if (s.rfind(L"path-spans:", 0) == 0 && p) {  // path-spans:row — text drawn there in the path color, as DrawPane would
    int r = _wtoi(s.c_str() + 11);
    EnterCriticalSection(&p->cs);
    std::wstring t = r >= 0 && r < p->rows ? RowText(p, r) : std::wstring();
    const auto* spans = PathSpansFor(p, r, std::hash<std::wstring>{}(t));
    std::wstring out;
    for (size_t i = 0; spans && i < spans->size(); i++) {
      auto [c0, c1] = (*spans)[i];
      if (c0 >= 0 && c0 < c1 && c0 < (int)t.size()) out += (out.empty() ? L"" : L" | ") + t.substr(c0, c1 - c0);
    }
    LeaveCriticalSection(&p->cs);
    ScriptLog(L"path-spans " + std::to_wstring(r) + L": " + (!spans ? L"(not looked at)" : out.empty() ? L"(none)" : out));
  } else if (s == L"path-scan-time" && p) {  // µs of a highlighting pass over the active pane (with what's cached)
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < 20; i++) PathScanPane(p, false);
    QueryPerformanceCounter(&t1);
    ScriptLog(L"path-scan-time: " + std::to_wstring((t1.QuadPart - t0.QuadPart) * 50000 / f.QuadPart) + L"us");
  } else if (s.rfind(L"hover-time:", 0) == 0 && p) {  // hover-time:col,row — µs of that hover with the path cache empty, then full
    int c = 0, r = 0;
    swscanf(s.c_str() + 11, L"%d,%d", &c, &r);
    int x = p->content.left + kPad + c * g_win->cellW + 1, y = p->content.top + kPad + r * g_win->cellH + 1;
    g_paths.attrs.clear(), g_paths.dirs.clear(), g_paths.drives.clear(), g_paths.memoTick = 0;
    LARGE_INTEGER f, t0, t1, t2;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    LinkAt(p, x, y);
    QueryPerformanceCounter(&t1);
    for (int i = 0; i < 100; i++) g_paths.memoTick = 0, LinkAt(p, x, y);  // every time worked out again (moving the mouse)
    QueryPerformanceCounter(&t2);
    ScriptLog(L"hover-time " + std::to_wstring(c) + L"," + std::to_wstring(r) + L": cold " +
              std::to_wstring((t1.QuadPart - t0.QuadPart) * 1000000 / f.QuadPart) + L"us warm " +
              std::to_wstring((t2.QuadPart - t1.QuadPart) * 10000 / f.QuadPart) + L"us");
  } else if (s.rfind(L"find:", 0) == 0 && p) {
    OpenFind(p);
    for (wchar_t c : s.substr(5)) FindChar(c);
  } else if (s == L"find-next") {
    FindStep(1);
  } else if (s == L"find-close") {
    CloseFind();
  } else if (s.rfind(L"find-click:", 0) == 0) {  // find-click:prev|next|close — clicks that button
    std::wstring b = s.substr(11);
    const RECT& r = b == L"prev" ? g_win->find.prev : b == L"next" ? g_win->find.next : g_win->find.close;
    SendMessageW(g_win->hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM((r.left + r.right) / 2, (r.top + r.bottom) / 2));
    SendMessageW(g_win->hwnd, WM_LBUTTONUP, 0, MAKELPARAM((r.left + r.right) / 2, (r.top + r.bottom) / 2));
  } else if (s == L"find-state") {
    ScriptLog(L"find: " + std::wstring(g_win->find.open ? L"open " : L"closed ") + std::to_wstring(g_win->find.cur + 1) + L"/" +
              std::to_wstring(g_win->find.matches.size()));
  } else if (s == L"cells" && p) {  // every row with something past U+24FF: column:code points/width of those cells
    EnterCriticalSection(&p->cs);
    for (int r = 0; r < p->rows; r++) {
      std::wstring line;
      for (int col = 0; col < p->cols; col++) {
        VTermScreenCell c;
        CellAt(p, r, col, &c);
        if (c.chars[0] == (uint32_t)-1 || (c.chars[0] < 0x2500 && !c.chars[1])) continue;
        wchar_t b[16];
        line += L" " + std::to_wstring(col) + L":";
        for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && c.chars[i]; i++) swprintf(b, 16, i ? L"+%X" : L"%X", c.chars[i]), line += b;
        line += L"/" + std::to_wstring(c.width);
      }
      if (!line.empty()) ScriptLog(L"cells " + std::to_wstring(r) + L":" + line);
    }
    LeaveCriticalSection(&p->cs);
  } else if (s.rfind(L"join:", 0) == 0) {  // join:1F44D,1F3FD — the glyphs Segoe UI Emoji shapes those code points into
    std::vector<uint32_t> cps;
    for (const wchar_t* q = s.c_str() + 5; *q;) {
      wchar_t* end = nullptr;
      cps.push_back((uint32_t)wcstoul(q, &end, 16));
      q = *end ? end + 1 : end;
    }
    std::wstring r = L"join " + s.substr(5) + L":";
    if (g_dw.on)
      for (UINT16 g : DwShapeEmoji(cps.data(), cps.size())) r += L" " + std::to_wstring(g);
    ScriptLog(r + L" joined=" + std::to_wstring(g_dw.on ? DwJoined(cps.data(), cps.size()) : 0));
  } else if (s.rfind(L"select:", 0) == 0 && p) {  // from the first match of the text to the end of its row
    for (int r = 0; r < p->rows; r++) {
      EnterCriticalSection(&p->cs);
      size_t a = RowText(p, r).find(s.substr(7));
      LeaveCriticalSection(&p->cs);
      if (a != std::wstring::npos) {
        g_win->selPane = p, g_win->selA = VTermPos{r, (int)a}, g_win->selB = VTermPos{r, p->cols - 1}, g_win->hasSel = true;
        break;
      }
    }
  } else if (s.rfind(L"select-to:", 0) == 0 && p && g_win->hasSel) {  // extends the selection to the end of the row with the text
    for (int r = g_win->selA.row; r < p->rows; r++) {
      EnterCriticalSection(&p->cs);
      bool hit = RowText(p, r).find(s.substr(10)) != std::wstring::npos;
      LeaveCriticalSection(&p->cs);
      if (hit) {
        g_win->selB = VTermPos{r, p->cols - 1};
        break;
      }
    }
  } else if (s == L"copy") {
    CopySelection();
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
      ClientToScreen(g_win->hwnd, &q);
      SetCursorPos(q.x, q.y);
    };
    cursor(a);
    SendMessageW(g_win->hwnd, WM_LBUTTONDOWN, MK_LBUTTON, at(a));
    for (int k = 1; k <= 8; k++) {
      POINT q{a.x + (b.x - a.x) * k / 8, a.y + (b.y - a.y) * k / 8};
      cursor(q);
      SendMessageW(g_win->hwnd, WM_MOUSEMOVE, MK_LBUTTON, at(q));
    }
    SendMessageW(g_win->hwnd, WM_LBUTTONUP, 0, at(b));
  } else if (s.rfind(L"drag-start:", 0) == 0) {  // drag-start:tab:<i> | drag-start:pane:<j> — presses the mouse on it
    bool pane = s.rfind(L"drag-start:pane:", 0) == 0;
    if (!ScriptDragStart(pane, _wtoi(s.c_str() + (pane ? 16 : 15)))) ScriptLog(L"drag-start: nothing at " + s.substr(11));
    delay = 300;
  } else if (s.rfind(L"drag-to:", 0) == 0) {  // drag-to:<pid>,<x>,<y> — see ScriptDragTo
    ScriptDragToText(s.c_str() + 8);
    delay = 400;
  } else if (s == L"drag-drop") {  // releases the mouse where drag-to left it
    SendMessageW(g_win->hwnd, WM_LBUTTONUP, 0, MAKELPARAM(g_win->scriptDrag.x, g_win->scriptDrag.y));
    delay = 2500;
  } else if (s.rfind(L"drop-tab:", 0) == 0 || s.rfind(L"drop-pane:", 0) == 0) {  // drop-tab:<i>,<pid>,<x>,<y> — the three at once
    bool pane = s[5] == L'p';
    const wchar_t* a = s.c_str() + (pane ? 10 : 9);
    const wchar_t* c = wcschr(a, L',');
    int i = _wtoi(a);
    if (c && ScriptDragStart(pane, i)) {
      ScriptDragToText(c + 1);
      SendMessageW(g_win->hwnd, WM_LBUTTONUP, 0, MAKELPARAM(g_win->scriptDrag.x, g_win->scriptDrag.y));
    } else {
      ScriptLog(L"drop: nothing to drag at " + std::to_wstring(i));
    }
    delay = 2500;
  } else if (s == L"join-hint") {  // what a tab or pane of another window over this one would become: tab <i>, pane <j> <side>, none
    std::wstring r = L"none";
    if (g_win->joinHint.from) {
      POINT pt = g_win->joinHint.at;
      ScreenToClient(g_win->hwnd, &pt);
      JoinSpot j = JoinSpotAt(pt);
      std::vector<Pane*> ls;
      Leaves(CurTab().root, ls);
      r = j.pane ? L"pane " + std::to_wstring(std::find(ls.begin(), ls.end(), j.pane) - ls.begin()) + L" " + kSideNames[j.side]
                 : L"tab " + std::to_wstring(j.tab);
    }
    ScriptLog(L"join-hint: " + r);
  } else if (s == L"windows") {  // every window of this Warp: see WindowsText
    ScriptLog(L"windows: " + WindowsText());
  } else if (s.rfind(L"label:", 0) == 0) {  // this window's name in the steps and results of the tests
    g_win->label = s.substr(6);
  } else if (s == L"crash") {  // an access violation, as a bug would cause (see «Crashes»; scripts run only in a test's Warp)
    volatile int* nowhere = (volatile int*)(ULONG_PTR)(g_win->scriptPos >> 30);  // 0, but not known before it runs
    *nowhere = 86;
  } else if (s == L"raise") {  // this window over the others, without taking the focus
    SetWindowPos(g_win->hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  } else if (s == L"window-count") {  // windows of this Warp open
    ScriptLog(L"window-count: " + std::to_wstring(g_windows.size()));
  } else if (s == L"pids") {  // the shell of every pane of this Warp: <window>/<tab>/<pane>=<pid>
    ScriptLog(L"pids:" + EachPane([](Pane* q) { return std::to_wstring(q->pid); }));
  } else if (s.rfind(L"grep:", 0) == 0) {  // grep:<text> — the panes with lines that have it: how many, the first and the last
    std::wstring text = s.substr(5);
    ScriptLog(L"grep " + text + L":" + EachPane([&](Pane* q) { return PaneGrep(q, text); }));
  } else if (s.rfind(L"desk-shot:", 0) == 0) {  // desk-shot:<x>,<y>,<w>,<h>,<file.bmp> — that part of the screen (SaveDeskShot)
    RECT a{};
    int w = 0, h = 0;
    swscanf(s.c_str() + 10, L"%ld,%ld,%d,%d", &a.left, &a.top, &w, &h);
    a.right = a.left + w, a.bottom = a.top + h;
    size_t c = s.find(L',', s.find(L',', s.find(L',', s.find(L',', 10) + 1) + 1) + 1);
    if (c != std::wstring::npos) SaveDeskShot(s.substr(c + 1), a);
    delay = 100;
  } else if (s.rfind(L"key:", 0) == 0) {  // key:tab|down|up|enter|shift|esc — por el bucle de mensajes, como una tecla
    std::wstring k = s.substr(4);
    WPARAM vk = k == L"tab" ? VK_TAB : k == L"down" ? VK_DOWN : k == L"up" ? VK_UP : k == L"enter" ? VK_RETURN
              : k == L"shift" ? VK_SHIFT : VK_ESCAPE;
    PostMessageW(g_win->hwnd, WM_KEYDOWN, vk, 0);
    delay = 600;
  } else if (s == L"edit-text" && p) {
    ScriptLog(L"edit: [" + p->edit + L"]");
  } else if (s == L"comp") {
    std::wstring r = L"comp:";
    if (!g_win->comp.open) r += L" (cerrado)";
    else if (!g_win->comp.loading.empty()) r += L" (cargando)";
    for (size_t i = 0; g_win->comp.open && i < g_win->comp.items.size() && i < 6; i++) r += L" " + g_win->comp.items[i].label;
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
  } else if (s.rfind(L"reconnect-cmd:", 0) == 0) {  // would that command be launched again if its connection drops?
    ScriptLog(L"reconnect-cmd: " + std::wstring(Reconnectable(ToUtf8(s.substr(14))) ? L"yes" : L"no"));
    delay = 50;
  } else if (s.rfind(L"reconnect-text:", 0) == 0) {  // what those last lines of ssh mean
    ScriptLog(L"reconnect-text: " + std::wstring(kSshKind[SshFailure(s.substr(15))]));
    delay = 50;
  } else if (s.rfind(L"reconnect-wait:", 0) == 0 && p) {  // until the active pane is in that state (90 s at most)
    int& waited = g_win->waited;
    if (ReconnectState(p) != s.substr(15) && ++waited < 360) g_win->scriptPos--, delay = 250;
    else waited = 0, delay = 300;
  } else if (s == L"reconnect" && p) {
    ScriptLog(L"reconnect: " + ReconnectState(p));
  } else if (s == L"net-change") {  // as if Windows said the network changed
    OnNetworkChange();
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
    for (auto& it : g_win->sw.shown) {
      std::wstring dir = it.cwd.substr(it.cwd.find_last_of(L'\\') == std::wstring::npos ? 0 : it.cwd.find_last_of(L'\\') + 1);
      r += L" [" + it.title + L" | " + dir + (it.hwnd == g_win->hwnd ? L"" : L" | otra") + L"]";
    }
    ScriptLog(r + L" sel=" + std::to_wstring(g_win->sw.sel));
    delay = 300;
  } else if (s == L"remote-pair") {  // as if chosen in the title-bar menu (Pair phone…)
    OpenPairCard();
  } else if (s == L"remote-wait") {  // until the pairing code (or an error) arrives (30 s at most)
    int& waited = g_win->waited;
    if (g_win->pair.open && g_win->pair.ticket.empty() && g_win->pair.error.empty() && ++waited < 120) g_win->scriptPos--, delay = 250;
    else waited = 0, delay = 300;
  } else if (s == L"remote-ticket") {
    ScriptLog(L"remote-ticket: " + (g_win->pair.ticket.empty() ? L"(none) " + g_win->pair.error : g_win->pair.ticket));
  } else if (s == L"remote-close") {
    ClosePairCard();
  } else if (s == L"remote-state") {  // phones connected, paired devices, warp-remote running, pairing card open
    ScriptLog(L"remote: phones=" + std::to_wstring(g_remotePhones) + L" devices=" + std::to_wstring(PairedDevices().size()) +
              L" sidecar=" + (RemoteWindow() ? L"1" : L"0") + L" card=" + (g_win->pair.open ? L"1" : L"0"));
  } else if (s.rfind(L"remote-remove:", 0) == 0) {  // remote-remove:<n> — as if chosen in Paired devices
    std::vector<PairedDevice> devs = PairedDevices();
    size_t i = (size_t)_wtoi(s.c_str() + 14);
    if (i < devs.size()) RemoveDevice(devs[i].id);
  } else if (s.rfind(L"remote-typing:", 0) == 0) {  // remote-typing:<n>:on|off — as if toggled in Paired devices
    std::vector<PairedDevice> devs = PairedDevices();
    size_t i = (size_t)_wtoi(s.c_str() + 14);
    if (i < devs.size()) SetTyping(devs[i].id, s.find(L":on") != std::wstring::npos);
  } else if (s == L"remote-typing-state") {  // devices allowed to type, and whether «Phone typing» is shown
    ScriptLog(L"remote-typing: allowed=" + std::to_wstring(TypingDevices().size()) + L" shown=" + (g_win->phoneTyping ? L"1" : L"0"));
  } else if (s.rfind(L"remote-wait-device", 0) == 0) {  // remote-wait-device[:<n>] — until n devices are paired (5 min at most)
    int& waited = g_win->waited;
    size_t want = s.size() > 19 ? (size_t)_wtoi(s.c_str() + 19) : 1;
    if (PairedDevices().size() < want && ++waited < 1200) g_win->scriptPos--, delay = 250;
    else waited = 0, delay = 300;
  } else if (s.rfind(L"wait-file:", 0) == 0) {  // until that file exists (the other side of a test is done; 5 min at most)
    int& waited = g_win->waited;
    if (GetFileAttributesW(s.c_str() + 10) == INVALID_FILE_ATTRIBUTES && ++waited < 1200) g_win->scriptPos--, delay = 250;
    else waited = 0, delay = 300;
  } else if (s.rfind(L"sw-type:", 0) == 0) {
    for (wchar_t c : s.substr(8)) SwChar(c);
    delay = 200;
  } else if (s == L"active") {  // terminal activa: pestaña, título y carpeta (y si la ventana está delante)
    Pane* a = CurTab().active;
    ScriptLog(L"active: tab " + std::to_wstring(g_win->activeTab) + L" [" + ShortTitle(a) + L"] " + a->cwd +
              (GetForegroundWindow() == g_win->hwnd ? L" (delante)" : L""));
  } else if (s == L"titles") {  // título de cada pestaña
    std::wstring r = L"titles:";
    for (auto& t : g_win->tabs) r += L" [" + (t.name.empty() ? ShortTitle(t.active) : t.name) + L"]";
    ScriptLog(r);
  } else if (s == L"tabs") {  // orden de las pestañas (por su nombre) y cuál está activa
    std::wstring r = L"tabs:";
    for (int i = 0; i < (int)g_win->tabs.size(); i++) r += L" " + (i == g_win->activeTab ? L"*" + g_win->tabs[i].name : g_win->tabs[i].name);
    ScriptLog(r);
  } else if (s.rfind(L"focus:", 0) == 0) {  // focus:on|off — as if the window got or lost the keyboard focus
    SendMessageW(g_win->hwnd, s.substr(6) == L"on" ? WM_SETFOCUS : WM_KILLFOCUS, 0, 0);
  } else if (s.rfind(L"feed:", 0) == 0) {  // feed:<tab>[/<pane>]:<hex> — bytes to that pane (or the tab's active one) as if its program wrote them
    int ti = _wtoi(s.c_str() + 5);
    size_t c = s.find(L':', 5), slash = s.find(L'/', 5);
    std::vector<Pane*> ls;
    if (ti >= 0 && ti < (int)g_win->tabs.size()) Leaves(g_win->tabs[ti].root, ls);
    size_t pj = slash < c ? (size_t)_wtoi(s.c_str() + slash + 1) : ls.size();
    if (ti >= 0 && ti < (int)g_win->tabs.size() && c != std::wstring::npos) {
      Pane* q = pj < ls.size() ? ls[pj] : g_win->tabs[ti].active;
      std::string bytes = HexDecode(ToUtf8(s.substr(c + 1)));
      EnterCriticalSection(&q->cs);
      vterm_input_write(q->vt, bytes.data(), bytes.size());
      vterm_screen_flush_damage(q->scr);
      LeaveCriticalSection(&q->cs);
      FlushOut(q);
      RequestRepaint(q);
    }
    delay = 600;
  } else if (s == L"badges") {  // unread per tab ([per pane] when split), window total, in hidden tabs, window in front
    SyncUnread();
    std::wstring r = L"badges:";
    int total = 0;
    for (int i = 0; i < (int)g_win->tabs.size(); i++) {
      std::vector<Pane*> ls;
      Leaves(g_win->tabs[i].root, ls);
      int n = TabUnread(g_win->tabs[i]);
      total += n;
      r += L" tab" + std::to_wstring(i) + L"=" + std::to_wstring(n);
      for (size_t k = 0; ls.size() > 1 && k < ls.size(); k++)
        r += (k ? L"," : L"[") + std::to_wstring(ls[k]->unread) + (k + 1 == ls.size() ? L"]" : L"");
    }
    ScriptLog(r + L" window=" + std::to_wstring(total) + L" hidden=" + std::to_wstring(HiddenUnread()) + L" fg=" +
              (Foreground() ? L"1" : L"0"));
  } else if (s == L"notes") {  // what each event since the last «notes» did (<kind>:counted|seen|merged|quiet)
    ScriptLog(L"notes: " + (g_win->noteLog.empty() ? std::wstring(L"(none)") : g_win->noteLog));
    g_win->noteLog.clear();
  } else if (s == L"taskbar") {  // the taskbar button: CoCreateInstance+HrInit and the last SetOverlayIcon (tried now
    bool told = g_taskbar != nullptr;  // if Explorer never said the button exists)
    if (!g_taskbar) TaskbarReady();
    wchar_t b[160];
    swprintf(b, 160, L"taskbar: %ls init=0x%08lX overlay=0x%08lX total=%d", told ? L"button" : L"no-button",
             (unsigned long)g_taskbarHr, (unsigned long)g_overlayHr, g_win->unreadShown);
    ScriptLog(b);
  } else if (s.rfind(L"badge-icon:", 0) == 0) {  // badge-icon:<n>,<px>,<file.bmp> — the taskbar overlay, over a dark taskbar
    int n = 0, px = 0;
    swscanf(s.c_str() + 11, L"%d,%d", &n, &px);
    size_t c = s.find(L',', s.find(L',', 11) + 1);
    if (n > 0 && px > 0 && px <= 256 && c != std::wstring::npos) {
      std::vector<uint32_t> v = BadgePixels(n, px);
      for (auto& q : v) {
        uint32_t a = q >> 24, bg = 0x20;
        auto ch = [&](int sh) { return (((q >> sh & 255) * a + bg * (255 - a)) / 255) << sh; };
        q = ch(16) | ch(8) | ch(0);
      }
      WriteBmp(s.substr(c + 1), v.data(), px, px);
    }
  } else if (s == L"attention") {  // which panes pulse (tab<i>/pane<j>=1), the pulse timer and whether Windows animates
    SyncUnread();
    std::wstring r = L"attention:";
    for (int i = 0; i < (int)g_win->tabs.size(); i++) {
      std::vector<Pane*> ls;
      Leaves(g_win->tabs[i].root, ls);
      for (size_t k = 0; k < ls.size(); k++)
        r += L" tab" + std::to_wstring(i) + L"/pane" + std::to_wstring(k) + L"=" + (ls[k]->attention ? L"1" : L"0");
    }
    ScriptLog(r + L" timer=" + (g_win->pulseTimer ? L"1" : L"0") + L" motion=" + (Motion() ? L"1" : L"0") + L" back=" +
              (g_win->backDC ? L"1" : L"0"));
  } else if (s == L"where") {  // the pane in view: tab<i>/pane<j>
    std::vector<Pane*> ls;
    Leaves(CurTab().root, ls);
    ScriptLog(L"where: tab" + std::to_wstring(g_win->activeTab) + L"/pane" +
              std::to_wstring(std::find(ls.begin(), ls.end(), CurTab().active) - ls.begin()));
  } else if (s == L"back-sample") {  // the back buffer (what the window shows) at the outline, header (-: none) and tab of the first pane in view with attention
    std::wstring r = L"back:";
    std::vector<Pane*> ls;
    Leaves(CurTab().root, ls);
    auto at = [](int x, int y) {
      COLORREF c = g_win->backDC ? GetPixel(g_win->backDC, x, y) : CLR_INVALID;
      wchar_t b[8];
      swprintf(b, 8, L"%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
      return c == CLR_INVALID ? std::wstring(L"?") : std::wstring(b);
    };
    for (Pane* q : ls)
      if (q->attention && r.size() == 5) {
        RECT h = HeaderRect(q), side[4];
        AttentionSides(q, side);
        r += L" outline=" + at(side[0].left, (side[0].top + side[0].bottom) / 2);
        r += L" header=" + (h.bottom > h.top ? at(h.left + (h.right - h.left) / 4, side[2].bottom + 1) : std::wstring(L"-"));
        for (auto& [i, t] : ComputeTabs().tabs)
          if (i == g_win->activeTab) r += L" tab=" + at(t.left + Px(4), t.top + Px(3));
      }
    ScriptLog(r.size() == 5 ? r + L" (none)" : r);
    delay = 150;
  } else if (s.rfind(L"backshot:", 0) == 0) {  // backshot:<file.bmp> — the back buffer as it is (what the window shows)
    if (g_win->backDC && g_win->backBmp) {
      BITMAPINFO bi{};
      bi.bmiHeader = {sizeof bi.bmiHeader, g_win->backW, -g_win->backH, 1, 32, BI_RGB};
      std::vector<uint32_t> px((size_t)g_win->backW * g_win->backH);
      GetDIBits(g_win->backDC, g_win->backBmp, 0, g_win->backH, px.data(), &bi, DIB_RGB_COLORS);
      WriteBmp(s.substr(9), px.data(), g_win->backW, g_win->backH);
    }
    delay = 100;
  } else if (s.rfind(L"click-pane:", 0) == 0) {  // click-pane:<j> — a left click in the middle of pane j of the tab in view
    std::vector<Pane*> ls;
    Leaves(CurTab().root, ls);
    size_t j = (size_t)_wtoi(s.c_str() + 11);
    if (j < ls.size()) {
      RECT c = ls[j]->content;
      LPARAM at = MAKELPARAM((c.left + c.right) / 2, (c.top + c.bottom) / 2);
      SendMessageW(g_win->hwnd, WM_LBUTTONDOWN, MK_LBUTTON, at);
      SendMessageW(g_win->hwnd, WM_LBUTTONUP, 0, at);
    }
  } else if (s.rfind(L"pulse-at:", 0) == 0) {  // pulse-at:<ms>|off — the pulse frozen at that point of its breath
    g_pulseAt = s.substr(9) == L"off" ? -1 : _wtoi(s.c_str() + 9);
  } else if (s.rfind(L"motion:", 0) == 0) {  // motion:on|off|windows — as if Windows animated or not
    std::wstring m = s.substr(7);
    g_motionTest = m == L"on" ? 1 : m == L"off" ? 0 : -1;
  } else if (s == L"minimize" || s == L"restore") {
    ShowWindow(g_win->hwnd, s == L"minimize" ? SW_MINIMIZE : SW_RESTORE);
  } else if (s.rfind(L"toasts:", 0) == 0) {  // as if toggled in the title-bar menu
    SetToasts(s.substr(7) == L"on");
  } else if (s == L"toasts") {  // the toasts asked for since the last «toasts»: tag, group, accepts=<QUNS> and the XML
    for (auto& t : g_win->toastLog) ScriptLog(L"toast: " + t);
    if (g_win->toastLog.empty()) ScriptLog(L"toast: (none)");
    g_win->toastLog.clear();
  } else if (s == L"toast-probe" && p) {  // the real WinRT calls of a toast, all but Show: what each returned
    bool init = ToastInit();
    HRESULT hr[3] = {S_FALSE, S_FALSE, S_FALSE};
    int setting = -1;
    HRESULT sh = g_toaster.notifier ? g_toaster.notifier->get_Setting(&setting) : E_POINTER;
    if (IToastNotification* n = init ? MakeToast(ToastXml(p, L"Warp", L"probe", L""), L"probe", ToastGroup(), hr) : nullptr)
      n->Release();
    wchar_t b[400];
    swprintf(b, 400, L"toast-probe: combase=%d statics=0x%08lX notifier=0x%08lX history=0x%08lX factory=0x%08lX xml=0x%08lX "
             L"create=0x%08lX tag=0x%08lX setting=0x%08lX/%d",
             g_toaster.hfree ? 1 : 0, (unsigned long)g_toaster.staticsHr, (unsigned long)g_toaster.notifierHr,
             (unsigned long)g_toaster.historyHr, (unsigned long)g_toaster.factoryHr, (unsigned long)hr[0], (unsigned long)hr[1],
             (unsigned long)hr[2], (unsigned long)sh, setting);
    ScriptLog(b);
  } else if (s == L"toast-click-done") {  // the process of the last «toast-click»: ended (its exit code) or still running (ended now)
    DWORD code = 0;
    bool ended = g_win->toastProc && WaitForSingleObject(g_win->toastProc, 0) == WAIT_OBJECT_0 && GetExitCodeProcess(g_win->toastProc, &code);
    ScriptLog(L"toast-click: " + (!g_win->toastProc ? std::wstring(L"none") : ended ? L"ended " + std::to_wstring(code) : L"running"));
    if (g_win->toastProc && !ended) TerminateProcess(g_win->toastProc, 1);
    if (g_win->toastProc) CloseHandle(g_win->toastProc), g_win->toastProc = nullptr;
  } else if (s == L"toast-click" || s.rfind(L"toast-click:", 0) == 0 || s.rfind(L"toast-click+:", 0) == 0) {
    // As Windows does on a click: «Warp.exe --toast "<URI>"», another process. toast-click: the last toast's URI, for
    // this test's Warp; toast-click+:<text> the same with that text after it (a crafted link that must do nothing);
    // toast-click:<uri> that text as is, from a clean environment (no test session, APPDATA in <session>.appdata).
    bool crafted = s.rfind(L"toast-click:", 0) == 0;
    std::wstring uri = crafted ? s.substr(12) : s.size() > 11 ? g_win->toastLaunch + s.substr(13) : g_win->toastLaunch, env;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --toast \"" + uri + L"\"";
    if (crafted) {
      wchar_t* block = GetEnvironmentStringsW();
      for (wchar_t* v = block; *v; v += wcslen(v) + 1)
        if (_wcsnicmp(v, L"WARPLITE_", 9) && _wcsnicmp(v, L"APPDATA=", 8) && _wcsnicmp(v, L"LOCALAPPDATA=", 13)) env.append(v), env.push_back(0);
      FreeEnvironmentStringsW(block);
      for (const wchar_t* k : {L"APPDATA=", L"LOCALAPPDATA="}) env += k + g_sessionFile + L".appdata", env.push_back(0);
      env.push_back(0);
    }
    if (g_win->toastProc) CloseHandle(g_win->toastProc), g_win->toastProc = nullptr;
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!uri.empty() && CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, crafted ? CREATE_UNICODE_ENVIRONMENT : 0,
                                       crafted ? (void*)env.data() : nullptr, nullptr, &si, &pi))
      CloseHandle(pi.hThread), g_win->toastProc = pi.hProcess;
    delay = 3000;
  } else if (s.rfind(L"link-menu:", 0) == 0 && p) {  // link-menu:col,row[,n] — the right-click menu there; n runs item n (never «Open»)
    int c = 0, r = 0, n = 0;
    swscanf(s.c_str() + 10, L"%d,%d,%d", &c, &r, &n);
    LinkHit h = LinkAt(p, p->content.left + kPad + c * g_win->cellW + 1, p->content.top + kPad + r * g_win->cellH + 1);
    std::wstring out;
    auto items = h.p ? LinkMenuItems(h) : std::vector<Str>{};
    for (Str i : items) out += (out.empty() ? L"" : L" | ") + std::wstring(T(i));
    ScriptLog(L"link-menu " + std::to_wstring(c) + L"," + std::to_wstring(r) + L": " + (out.empty() ? L"(sin enlace)" : out));
    if (n > 1 && n <= (int)items.size()) LinkMenuRun(h, items[n - 1]);
    delay = 1500;
  } else if (s.rfind(L"osc52-drag:", 0) == 0) {  // OSC 52 as tmux sends it when a mouse drag ends
    g_appMouseUp = std::max<DWORD>(GetTickCount(), 1);
    SendMessageW(g_host, WM_APP_CLIP, 0, (LPARAM) new std::wstring(s.substr(11)));  // as the reader thread posts it
  } else if (s == L"ctrl-c") {  // what Ctrl+C does with no selection: copies a pending tmux copy, if any
    ScriptLog(std::wstring(L"ctrl-c: ") + (CopyPending() ? L"copia" : L"al programa"));
  } else if (s == L"clipboard") {
    std::wstring w = L"(vacío)";
    if (OpenClipboard(g_win->hwnd)) {
      if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) w = (const wchar_t*)GlobalLock(h), GlobalUnlock(h);
      CloseClipboard();
    }
    for (size_t i; (i = w.find(L"\r\n")) != std::wstring::npos;) w.replace(i, 2, L"\\n");
    ScriptLog(L"clipboard: " + w);
  } else if (s == L"paste" && p) {
    Paste(p);
  } else if (s == L"detach-tab") {
    RECT wr;
    GetWindowRect(g_win->hwnd, &wr);
    Detach(g_win->activeTab, nullptr, POINT{wr.left + 200, wr.top + 120});
    delay = 3000;
  } else if (s.rfind(L"startup:", 0) == 0) {
    SetStartWithWindows(s.substr(8) == L"on");
  } else if (s.rfind(L"goto:", 0) == 0) {
    ActivateTab(_wtoi(s.c_str() + 5));
  } else if (s == L"close") {  // como pulsar la X de esta ventana
    PostMessage(g_win->hwnd, WM_CLOSE, 0, 0);
    return;
  } else if (s == L"quit-all-targets") {  // what quit-all would close (sends nothing): this Warp's other windows
    int mine = (int)SiblingWindows().size();
    ScriptLog(L"quit-all-targets: " + std::to_wstring(mine) + L" of this Warp, " + std::to_wstring(OtherWindows() - mine) +
              L" of others");
  } else if (s == L"quit-all") {
    QuitAll(false);
    return;
  } else if (s == L"update-restart") {  // like «Restart» after an update, relaunching this same exe
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    RestartForUpdate(exe);
    return;
  } else if (s.rfind(L"reopen:", 0) == 0) {  // as if toggled in the title-bar menu
    if (!g_win->isMain) g_win->reopen = s.substr(7) == L"on", SaveSession();
  } else if (s == L"window-info") {  // main or not, «Reopen on restart» and its tabs
    std::wstring r = std::wstring(L"window: ") + (g_win->isMain ? L"main" : L"other") + L" reopen=" + (g_win->reopen ? L"1" : L"0");
    for (auto& t : g_win->tabs) r += L" [" + (t.name.empty() ? ShortTitle(t.active) : t.name) + L"]";
    ScriptLog(r);
  } else if (s.rfind(L"hide-tab", 0) == 0) {
    HideTab(g_win->activeTab);
  } else if (s == L"vtabs") {
    ToggleVerticalTabs();
  } else if (s == L"picker" && p) {
    OpenPicker(p);
  } else if (s.rfind(L"picker-type:", 0) == 0) {
    for (wchar_t c : s.substr(12)) PickChar(c);
  } else if (s == L"picker-enter") {
    PickChoose(g_win->pick.sel);
    delay = 2000;
  } else if (s.rfind(L"hover-text:", 0) == 0 && p) {  // Ctrl sobre la primera fila cuyo texto es ese
    std::wstring want = s.substr(11);
    for (int r = 0; r < p->rows; r++) {
      EnterCriticalSection(&p->cs);
      std::wstring t = RowText(p, r);
      LeaveCriticalSection(&p->cs);
      size_t a = t.find_first_not_of(L' '), b = t.find_last_not_of(L' ');
      if (a != std::wstring::npos && t.substr(a, b - a + 1) == want) {
        g_win->link = LinkAt(p, p->content.left + kPad + (int)a * g_win->cellW + 1, p->content.top + kPad + r * g_win->cellH + 1);
        Toast(LinkLabel(g_win->link));
        ScriptLog(L"hover-text " + want + L": " + LinkLabel(g_win->link));
        break;
      }
    }
  } else if (s.rfind(L"lang:", 0) == 0) {
    for (int i = 0; i < kLangCount; i++)
      if (ToUtf8(s.substr(5)) == kLangCodes[i]) g_lang = i;
  } else if (s == L"rename-start") {
    BeginRename(g_win->activeTab);
  } else if (s.rfind(L"rename-type:", 0) == 0) {
    for (wchar_t c : s.substr(12)) SendMessageW(g_win->hwnd, WM_CHAR, c, 0);
  } else if (s.rfind(L"rename:", 0) == 0) {
    BeginRename(g_win->activeTab);
    g_win->renameText = s.substr(7);
    EndRename(true);
  } else if (s.rfind(L"zoom:", 0) == 0) {
    SetZoom(_wtoi(s.substr(5).c_str()));
    delay = 600;
  } else if (s.rfind(L"font:", 0) == 0) {  // as if chosen in the title-bar menu
    SetTerminalFont(s.substr(5));
    delay = 600;
  } else if (s == L"font-wait") {  // until the font download finishes (a minute at most)
    int& waited = g_win->waited;
    if (!g_fontLoading.empty() && ++waited < 200) g_win->scriptPos--, delay = 300;
    else waited = 0, delay = 300;
  } else if (s.rfind(L"brightness:", 0) == 0) {  // as if chosen in the title-bar menu (RRGGBB)
    unsigned long v = wcstoul(s.c_str() + 11, nullptr, 16);
    SetTextBrightness(RGB(v >> 16 & 255, v >> 8 & 255, v & 255));
  } else if (s == L"brightness") {
    wchar_t b[32];
    swprintf(b, 32, L"brightness: %02X%02X%02X", GetRValue(g_termFg), GetGValue(g_termFg), GetBValue(g_termFg));
    ScriptLog(b);
  } else if (s.rfind(L"font-size:", 0) == 0) {  // as if chosen in the title-bar menu
    SetFontSize(_wtoi(s.c_str() + 10));
    delay = 600;
  } else if (s == L"font-size") {
    ScriptLog(L"font-size: " + std::to_wstring(g_win->fontPx) + L" " + std::to_wstring(g_win->cellW) + L"x" + std::to_wstring(g_win->cellH));
  } else if (s == L"font") {
    ScriptLog(FontReport());
  } else if (s == L"font-others") {  // menu → Font → Other installed
    std::vector<std::wstring> v = OtherMonoFonts();
    std::wstring r = L"font-others: " + std::to_wstring(v.size());
    for (auto& n : v) r += L" [" + n + L"]";
    ScriptLog(r);
  } else if (s == L"toast") {
    ScriptLog(L"toast: " + g_win->toast);
  } else if (s.rfind(L"shot:", 0) == 0) SaveScreenshot(s.substr(5)), delay = 200;
  else if (s == L"quit") {
    PostMessage(g_win->hwnd, WM_CLOSE, 0, 0);
    return;
  }
  InvalidateRect(g_win->hwnd, nullptr, FALSE);
  SetTimer(g_win->hwnd, TIMER_SCRIPT, delay, nullptr);
}

// ---------------------------------------------------------------------------
// Ventana

static void ApplyDpi(UINT dpi) {
  g_win->dpi = dpi ? dpi : 96;
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

// Taskbar button: the unread total of this window as an overlay icon (see «Unread badges»). Explorer says when the
// button exists (TaskbarButtonCreated, again if Explorer restarts); without it there is nothing to set.

// The badge as straight-alpha BGRA, size x size: an accent circle with the count in white. GDI leaves alpha at 0,
// so the text is drawn white on black and used as coverage over the accent; the circle is CapsuleCover's.
static std::vector<uint32_t> BadgePixels(int n, int size) {
  std::vector<uint32_t> px((size_t)size * size);
  BITMAPINFO bi{};
  bi.bmiHeader = {sizeof bi.bmiHeader, size, -size, 1, 32, BI_RGB};
  void* bits = nullptr;
  HDC dc = CreateCompatibleDC(nullptr);
  HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  HGDIOBJ ob = SelectObject(dc, bmp);
  HFONT f = CreateFontW(-(n > 9 ? size * 9 / 16 : size * 11 / 16), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, 0, L"Segoe UI");
  HGDIOBJ of = SelectObject(dc, f);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, RGB(255, 255, 255));
  RECT r{0, 0, size, size};
  std::wstring t = BadgeLabel(n);
  DrawTextW(dc, t.c_str(), (int)t.size(), &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  GdiFlush();
  const uint32_t* text = (const uint32_t*)bits;
  for (int y = 0; y < size; y++)
    for (int x = 0; x < size; x++) {
      size_t i = (size_t)y * size + x;
      int k = text ? text[i] & 0xff : 0;
      auto mix = [k](int v) { return (uint32_t)((v * (255 - k) + 255 * k) / 255); };
      float a = CapsuleCover(x, y, size, size);
      px[i] = (uint32_t)(a * 255 + 0.5f) << 24 | mix(GetRValue(kAccent)) << 16 | mix(GetGValue(kAccent)) << 8 | mix(GetBValue(kAccent));
    }
  SelectObject(dc, of);
  DeleteObject(f);
  SelectObject(dc, ob);
  DeleteObject(bmp);
  DeleteDC(dc);
  return px;
}

static void SetOverlay(int total) {
  if (!g_taskbar) return;
  HICON icon = nullptr;
  if (total) {
    int size = SysMetric(SM_CXSMICON, WindowDpi(g_win->hwnd));
    std::vector<uint32_t> px = BadgePixels(total, size);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof bi.bmiHeader, size, -size, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bits) memcpy(bits, px.data(), px.size() * 4);
    std::vector<BYTE> zeros((size_t)((size + 15) / 16 * 2) * size);  // the alpha decides; the mask is all opaque
    HBITMAP mask = CreateBitmap(size, size, 1, 1, zeros.data());
    ICONINFO ii{TRUE, 0, 0, mask, color};
    icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
  }
  wchar_t d[64];
  swprintf(d, 64, T(S_UNREAD), total);
  g_overlayHr = g_taskbar->SetOverlayIcon(g_win->hwnd, icon, total ? d : L"");  // the taskbar keeps its own copy
  if (icon) DestroyIcon(icon);
}

static void TaskbarReady() {
  if (!g_taskbar) {
    g_taskbarHr = CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, __uuidof(ITaskbarList3), (void**)&g_taskbar);
    if (SUCCEEDED(g_taskbarHr)) g_taskbarHr = g_taskbar->HrInit();
    if (FAILED(g_taskbarHr) && g_taskbar) g_taskbar->Release(), g_taskbar = nullptr;
  }
  g_win->unreadShown = -1;  // a new button: set it again
  SyncUnread();
}

static void SetHover(int hit, int tab) {
  if (hit == g_win->hoverHit && tab == g_win->hoverTab) return;
  g_win->hoverHit = hit;
  g_win->hoverTab = tab;
  InvalidateRect(g_win->hwnd, nullptr, FALSE);
}

static Window* WindowOf(HWND h) {
  return h ? (Window*)GetWindowLongPtrW(h, GWLP_USERDATA) : nullptr;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_NCCREATE) SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
  Window* self = WindowOf(hwnd);
  if (!self) return DefWindowProcW(hwnd, msg, wp, lp);  // WM_GETMINMAXINFO comes before WM_NCCREATE
  InWindow in(self);
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
        HMENU menu = CreatePopupMenu(), sub = CreatePopupMenu(), fonts = CreatePopupMenu(), others = CreatePopupMenu();
        for (int i = 0; i < kLangCount; i++)
          AppendMenuW(sub, MF_STRING | (i == g_lang ? MF_CHECKED : 0), 100 + i, kLangNames[i]);
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)sub, T(S_LANGUAGE));
        // Font: built-in, downloaded on first use, and the other monospaced ones installed.
        for (int i = 0; i < kFontCount; i++) {
          if (i == kBuiltinFonts) AppendMenuW(fonts, MF_SEPARATOR, 0, nullptr);
          AppendMenuW(fonts, MF_STRING | (!_wcsicmp(g_fontFace.c_str(), kFonts[i].face) ? MF_CHECKED : 0), 200 + i, kFonts[i].face);
        }
        std::vector<std::wstring> mono = OtherMonoFonts();
        if (mono.size() > 500) mono.resize(500);
        for (size_t i = 0; i < mono.size(); i++)
          AppendMenuW(others, MF_STRING | (mono[i] == g_fontFace ? MF_CHECKED : 0), 1000 + i, mono[i].c_str());
        AppendMenuW(fonts, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(fonts, MF_POPUP | (mono.empty() ? MF_GRAYED : 0), (UINT_PTR)others, T(S_OTHER_FONTS));
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)fonts, T(S_FONT_MENU));
        // Font size of this window (like Ctrl+Shift +/-; a size not in the list shows up in it) and text brightness.
        HMENU sizes = CreatePopupMenu(), bright = CreatePopupMenu();
        std::vector<int> px = {11, 12, 13, 14, 15, 16, 18, 20, 22, 24};
        if (std::find(px.begin(), px.end(), g_win->fontPx) == px.end()) px.insert(std::upper_bound(px.begin(), px.end(), g_win->fontPx), g_win->fontPx);
        for (int v : px) {
          std::wstring t = std::to_wstring(v) + L" px" + (v == kDefaultFontPx ? L"  (" + std::wstring(T(S_DEFAULT)) + L")" : L"");
          AppendMenuW(sizes, MF_STRING | (v == g_win->fontPx ? MF_CHECKED : 0), 700 + v, t.c_str());
        }
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)sizes, T(S_FONT_SIZE));
        for (int i = 0; i < kTextLevelCount; i++) {
          wchar_t hex[16];
          COLORREF c = kTextLevels[i];
          swprintf(hex, 16, L"#%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
          std::wstring t = hex + (i == 1 ? std::wstring(L"  (Windows Terminal)")
                                         : i == kTextDefault ? L"  (" + std::wstring(T(S_DEFAULT)) + L")" : L"");
          AppendMenuW(bright, MF_STRING | (c == g_termFg ? MF_CHECKED : 0), 600 + i, t.c_str());
        }
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)bright, T(S_TEXT_BRIGHTNESS));
        // This window at the next start: the main one always comes back (checked, can't be changed).
        if (!g_windowDir.empty())
          AppendMenuW(menu, MF_STRING | (g_win->isMain || g_win->reopen ? MF_CHECKED : 0) | (g_win->isMain ? MF_GRAYED : 0), 5, T(S_REOPEN));
        AppendMenuW(menu, MF_STRING | (StartsWithWindows() ? MF_CHECKED : 0), 1, T(S_STARTUP));
        AppendMenuW(menu, MF_STRING | (ToastsOn() ? MF_CHECKED : 0), 7, T(S_WIN_NOTIFY));
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        // Phone: pair one; each paired one has a menu: may it type into the terminals (off until chosen), remove it.
        std::vector<PairedDevice> devs = PairedDevices();
        if (devs.size() > 50) devs.resize(50);
        std::set<std::wstring> typing = TypingDevices();
        HMENU phones = CreatePopupMenu();
        if (devs.empty()) AppendMenuW(phones, MF_STRING | MF_GRAYED, 0, T(S_NO_DEVICES));
        for (size_t i = 0; i < devs.size(); i++) {
          HMENU dev = CreatePopupMenu();
          AppendMenuW(dev, MF_STRING | (typing.count(devs[i].id) ? MF_CHECKED : 0), 3000 + i, T(S_ALLOW_TYPING));
          wchar_t b[200];
          swprintf(b, 200, T(S_REMOVE_DEVICE), devs[i].name.c_str());
          AppendMenuW(dev, MF_STRING, 2000 + i, b);
          AppendMenuW(phones, MF_POPUP, (UINT_PTR)dev, devs[i].name.c_str());
        }
        AppendMenuW(menu, MF_STRING, 6, T(S_PAIR_PHONE));
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)phones, T(S_PAIRED_DEVICES));
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
        if (cmd == 6) OpenPairCard();
        if (cmd >= 2000 && cmd < 2000 + (int)devs.size()) RemoveDevice(devs[cmd - 2000].id);
        if (cmd >= 3000 && cmd < 3000 + (int)devs.size()) SetTyping(devs[cmd - 3000].id, !typing.count(devs[cmd - 3000].id));
        if (cmd == 5 && !g_win->isMain) {
          g_win->reopen = !g_win->reopen;
          SaveSession();
        }
        if (cmd == 7) SetToasts(!ToastsOn());  // every window reads it when it has something to say
        if (cmd >= 100 && cmd < 100 + kLangCount) {  // every window
          g_lang = cmd - 100;
          for (Window* w : g_windows) {
            InWindow in(w);
            SaveSession();
          }
          InvalidateAll();
        }
        if (cmd >= 200 && cmd < 200 + kFontCount) SetTerminalFont(kFonts[cmd - 200].face);
        if (cmd >= 1000 && cmd < 1000 + (int)mono.size()) SetTerminalFont(mono[cmd - 1000]);
        if (cmd >= 600 && cmd < 600 + kTextLevelCount) SetTextBrightness(kTextLevels[cmd - 600]);
        if (cmd >= 706 && cmd <= 748) SetFontSize(cmd - 700);
        return 0;
      }
      break;
    case WM_NCMOUSEMOVE:
    case WM_MOUSELEAVE:
      SetHover(HIT_NONE, -1);
      break;
    case WM_CREATE: {
      g_win->hwnd = hwnd;
      ApplyDpi(WindowDpi(hwnd));
      SetPropW(hwnd, kInstanceProp, InstanceKey());
      if (g_win->adopt.root) {  // a tab moved here (Detach): it goes on running as it was
        g_win->tabs.push_back(g_win->adopt);
        g_win->adopt = Tab{};
        for (Pane* p : WindowPanes()) p->win = g_win;
      } else if (!LoadSession() && g_win->openDir.empty()) {
        NewTab();
      }
      MakeFonts();  // the session may have restored zoom and font size
      if (!g_win->openDir.empty()) NewTab(g_win->tabs.empty() ? -1 : g_win->activeTab + 1, g_win->openDir);
      if (!g_win->script.empty()) SetTimer(hwnd, TIMER_SCRIPT, 3000, nullptr);
      return 0;
    }
    case WM_SIZE:
      Relayout();
      SessionChanged();  // its place and size are in its session
      return 0;
    case WM_EXITSIZEMOVE: SessionChanged(); return 0;
    case WM_DPICHANGED: {
      ApplyDpi(HIWORD(wp));
      g_win->unreadShown = -1;  // the taskbar badge at the new size
      RECT* r = (RECT*)lp;
      SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
      Relayout();
      return 0;
    }
    case WM_TIMER:
      if (wp == TIMER_FRAME) {
        KillTimer(hwnd, TIMER_FRAME);
        g_win->frameTimer = false;
        PaintDirty();
      } else if (wp == TIMER_PULSE) {
        PulseTick();
      } else if (wp == TIMER_JOIN) {  // the window dragging a tab or pane over this one closed without saying it left
        if (!IsWindow(g_win->joinHint.from)) JoinHintClear();
      } else if (wp == TIMER_PATHS) {
        KillTimer(hwnd, TIMER_PATHS);
        g_win->pathTimer = false;
        ScanPaths();
      } else if (wp == TIMER_PHONE) {  // «Phone typing» goes away
        KillTimer(hwnd, TIMER_PHONE);
        g_win->phoneTyping = false;
        InvalidateRect(hwnd, nullptr, FALSE);
      } else if (wp == TIMER_PAIR) {  // pairing card: countdown; no answer from warp-remote in 20 s is an error
        if (!g_win->pair.open) KillTimer(hwnd, TIMER_PAIR);
        else if (g_win->pair.ticket.empty() && g_win->pair.error.empty() && GetTickCount() - g_win->pair.shown > 20000) g_win->pair.error = L"no answer";
        InvalidateRect(hwnd, &g_win->pair.rc, FALSE);
      } else if (wp == TIMER_TOAST) {
        KillTimer(hwnd, TIMER_TOAST);
        g_win->toast.clear();
        InvalidateRect(hwnd, nullptr, FALSE);
      } else if (wp == TIMER_SCRIPT) {
        KillTimer(hwnd, TIMER_SCRIPT);
        RunScriptStep();
      }
      return 0;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hwnd, &ps);
      RECT rc;
      GetClientRect(hwnd, &rc);
      EnsureBack(dc, rc.right, rc.bottom);
      Render(g_win->backDC, rc.right, rc.bottom);
      BitBlt(dc, 0, 0, rc.right, rc.bottom, g_win->backDC, 0, 0, SRCCOPY);
      EndPaint(hwnd, &ps);
      g_win->lastFrame = g_win->lastFull = GetTickCount();
      g_win->titleDirty = false;
      return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SETFOCUS: g_win->focused = true; SyncUnread(); InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_KILLFOCUS: g_win->focused = false; InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_CHAR:
    case WM_SYSCHAR: {
      if (g_win->tabs.empty()) return 0;
      if (g_win->sw.open) {
        if (msg == WM_CHAR) SwChar((wchar_t)wp);
        return 0;
      }
      if (g_win->pick.open) {
        if (msg == WM_CHAR) PickChar((wchar_t)wp);
        return 0;
      }
      if (g_win->find.open && g_win->renameTab < 0) {
        if (msg == WM_CHAR) FindChar((wchar_t)wp);
        return 0;
      }
      if (g_win->renameTab >= 0) {
        if (wp >= 0x20 && msg == WM_CHAR) {
          if (g_win->renameAll) g_win->renameText.clear(), g_win->renameAll = false;
          if (g_win->renameText.size() < 60) g_win->renameText += (wchar_t)wp;
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
      StopReconnect(CurTab().active);  // typing in the pane: the user takes over
      Attend(CurTab().active);
      if (EditorActive(CurTab().active)) {
        if (msg == WM_CHAR && cp >= 0x20 && cp != 0x7f) {
          std::wstring t;
          AppendUtf16(t, cp);
          EditorInsert(CurTab().active, t);
          if (g_win->comp.open && g_win->comp.p == CurTab().active) g_win->comp.autoAccept = false, CompRefresh();
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
      if (g_win->tabs.empty()) return 0;
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
    case WM_SETTINGCHANGE:  // animations on or off: the attention outline pulses or stays steady
      g_motion = -1;
      if (wp == SPI_SETCLIENTAREAANIMATION) InvalidateRect(hwnd, nullptr, FALSE);
      break;
    case WM_MOUSEWHEEL: {
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      ScreenToClient(hwnd, &pt);
      if (g_win->pick.open && PtInRect(&g_win->pick.rc, pt)) {
        int maxTop = std::max(0, (int)g_win->pick.shown.size() - kPickRows);
        g_win->pick.top = std::clamp(g_win->pick.top + (GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -3 : 3), 0, maxTop);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      if (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) SetZoom(g_win->zoom + (GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 10 : -10));
      else if (!g_win->tabs.empty()) OnMouseWheel(pt.x, pt.y, GET_WHEEL_DELTA_WPARAM(wp));
      return 0;
    }
    case WM_LBUTTONDBLCLK: {
      int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp), i;
      if (!g_win->tabs.empty() && (y < TabBarH() || x < SidebarW()) && TitleHit(x, y, &i) == HIT_TAB) {
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
      if (g_win->tabs.empty()) return 0;
      SetFocus(hwnd);
      g_clipPending.clear();
      if (g_win->renameTab >= 0) EndRename(true);  // clic fuera: guarda el nombre
      if (POINT pt{x, y}; PtInRect(&g_win->updRc, pt)) {  // aviso de actualización
        if (msg == WM_LBUTTONDOWN && PtInRect(&g_win->updClose, pt)) g_upd.hidden = true;
        else if (msg == WM_LBUTTONDOWN && PtInRect(&g_win->updAction, pt)) UpdateAction();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      if (g_win->pair.open) {  // pairing card: its button and ×; a click outside closes it
        POINT pt{x, y};
        if (msg == WM_LBUTTONDOWN && PtInRect(&g_win->pair.close, pt)) ClosePairCard();
        else if (msg == WM_LBUTTONDOWN && PtInRect(&g_win->pair.action, pt)) PairAction();
        else if (!PtInRect(&g_win->pair.rc, pt)) ClosePairCard();
        return 0;
      }
      if (POINT pt{x, y}; g_win->find.open && PtInRect(&g_win->find.rc, pt)) {  // find bar: previous, next, close
        if (msg == WM_LBUTTONDOWN) {
          if (PtInRect(&g_win->find.prev, pt)) FindStep(-1);
          else if (PtInRect(&g_win->find.next, pt)) FindStep(1);
          else if (PtInRect(&g_win->find.close, pt)) CloseFind();
        }
        return 0;
      }
      if (g_win->comp.open) {  // menú de autocompletado: clic en una opción la elige; fuera, se cierra
        POINT pt{x, y};
        if (PtInRect(&g_win->comp.rc, pt)) {
          if (msg == WM_LBUTTONDOWN) CompAccept(g_win->comp.top + (y - g_win->comp.rc.top - Px(4)) / Px(30));
          return 0;
        }
        CompClose();
      }
      if (g_win->sw.open) {  // selector de terminales: elegir o cerrar
        POINT pt{x, y};
        if (msg == WM_LBUTTONDOWN && PtInRect(&g_win->sw.rc, pt) && y >= g_win->sw.rc.top + Px(40))
          SwChoose(g_win->sw.top + (y - g_win->sw.rc.top - Px(40)) / Px(30));
        else if (!PtInRect(&g_win->sw.rc, pt)) CloseSwitcher();
        return 0;
      }
      if (g_win->pick.open) {  // selector de carpetas abierto: elegir o cerrar
        POINT pt{x, y};
        int row = (y - g_win->pick.rc.top - Px(40)) / Px(30);
        if (msg == WM_LBUTTONDOWN && PtInRect(&g_win->pick.rc, pt) && y >= g_win->pick.rc.top + Px(40)) PickChoose(g_win->pick.top + row);
        else if (!PtInRect(&g_win->pick.rc, pt)) ClosePicker();
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
          if (hit == HIT_TAB) ActivateTab(i), g_win->tabDrag = i, g_win->tabDragStart = POINT{x, y}, SetCapture(hwnd);
          else if (hit == HIT_PLUS) NewTab();
          else if (hit == HIT_TOGGLE) ToggleVerticalTabs();
          else if (hit == HIT_HIDDEN) HiddenMenu(sp);
          else if (hit >= HIT_MIN && hit <= HIT_CLOSE) g_win->pressedHit = hit;  // se ejecuta al soltar
        }
        return 0;
      }
      Attend(PaneAt(x, y));  // a click anywhere in a pane (header, text, input box) ends its attention pulse
      if (msg == WM_LBUTTONDOWN) {
        bool close;
        if (Pane* p = HeaderAt(x, y, &close)) {
          if (close) {
            ClosePane(p);
          } else {  // activar y empezar a arrastrar el panel
            CurTab().active = p;
            g_win->dragPane = p;
            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
          }
          return 0;
        }
      }
      if (msg == WM_LBUTTONDOWN)
        if (Node* d = DividerAt(CurTab().root, x, y)) {
          g_win->dragNode = d;
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
        if (!h.path.empty()) {  // file: its app; folder: Explorer; program or script: selected, never run
          OpenPathLink(h);
          return 0;
        }
      }
      if (msg == WM_RBUTTONDOWN && !g_win->hasSel) {  // right-click on a link: its menu (also over tmux or a TUI)
        LinkHit h = LinkAt(p, x, y);
        if (!h.url.empty() || !h.path.empty()) {
          POINT sp{x, y};
          ClientToScreen(hwnd, &sp);
          LinkMenu(h, sp);
          return 0;
        }
      }
      if (p->mouseMode && !shift) {
        MouseToApp(p, x, y, msg == WM_LBUTTONDOWN ? 1 : msg == WM_MBUTTONDOWN ? 2 : 3, true);
      } else if (msg == WM_LBUTTONDOWN) {
        g_win->selPane = p;
        g_win->selA = g_win->selB = CellFromPoint(p, x, y);
        g_win->selecting = true;
        g_win->hasSel = false;
        SetCapture(hwnd);
      } else if (msg == WM_RBUTTONDOWN) {  // clic derecho: copiar si hay selección, si no pegar
        if (g_win->hasSel) {
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
      if (g_win->tabs.empty()) return 0;
      {
        int tab = -1;
        int hit = y < TabBarH() || x < SidebarW() ? TitleHit(x, y, &tab) : HIT_NONE;
        SetHover(hit, tab);
        TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
      }
      if (g_win->find.open) {  // repaint the find bar when the hovered button changes
        static int findHover = -1;
        POINT pt{x, y};
        int h = PtInRect(&g_win->find.prev, pt) ? 0 : PtInRect(&g_win->find.next, pt) ? 1 : PtInRect(&g_win->find.close, pt) ? 2 : -1;
        if (h != findHover) findHover = h, InvalidateRect(hwnd, nullptr, FALSE);
      }
      if (g_win->tabDrag >= 0) {  // arrastrando una pestaña: sobre otra se cambian de sitio; fuera, a otra ventana
        POINT sp{x, y};
        ClientToScreen(hwnd, &sp);
        Window* over = JoinHover(sp);  // another window of this Warp in view there: it shows where the tab would go
        int j;
        bool moved = abs(x - g_win->tabDragStart.x) > Px(6) || abs(y - g_win->tabDragStart.y) > Px(6);
        int hit = moved && !over && !OutsideWindow(sp) ? TitleHit(x, y, &j) : HIT_NONE;
        if ((hit == HIT_TAB || hit == HIT_TABCLOSE) && j != g_win->tabDrag) {
          if (j > g_win->tabDrag) std::rotate(g_win->tabs.begin() + g_win->tabDrag, g_win->tabs.begin() + g_win->tabDrag + 1, g_win->tabs.begin() + j + 1);
          else std::rotate(g_win->tabs.begin() + j, g_win->tabs.begin() + g_win->tabDrag, g_win->tabs.begin() + g_win->tabDrag + 1);
          g_win->activeTab = g_win->tabDrag = j;
          SessionChanged();
          InvalidateRect(hwnd, nullptr, FALSE);
        }
        SetCursor(LoadCursor(nullptr, over || OutsideWindow(sp) ? IDC_SIZEALL : IDC_ARROW));
        return 0;
      }
      if (g_win->dragPane) {
        POINT sp{x, y};
        ClientToScreen(hwnd, &sp);
        Pane* t = JoinHover(sp) ? nullptr : PaneAt(x, y);  // over another window of this Warp: not over a pane here
        if (t == g_win->dragPane) t = nullptr;
        if (t != g_win->dropTarget) g_win->dropTarget = t, InvalidateRect(hwnd, nullptr, FALSE);
        SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
        return 0;
      }
      if (g_win->dragNode) {
        RECT r = g_win->dragNode->rc;
        float ratio = g_win->dragNode->vertical ? (float)(x - r.left) / std::max(1L, r.right - r.left)
                                           : (float)(y - r.top) / std::max(1L, r.bottom - r.top);
        g_win->dragNode->ratio = std::clamp(ratio, 0.08f, 0.92f);
        Relayout();
        return 0;
      }
      if (g_win->selecting && g_win->selPane) {
        g_win->selB = CellFromPoint(g_win->selPane, x, y);
        g_win->hasSel = g_win->selA.row != g_win->selB.row || g_win->selA.col != g_win->selB.col;
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
        if (h.p != g_win->link.p || h.row != g_win->link.row || h.c0 != g_win->link.c0 || h.path != g_win->link.path)
          InvalidateRect(hwnd, nullptr, FALSE);
        g_win->link = h;
      }
      Node* d = y >= TabBarH() ? DividerAt(CurTab().root, x, y) : nullptr;
      bool onHeader;
      // Flecha en todas partes, como Warp; cursor de texto solo en la línea de la caja de entrada
      // y mano en lo que se puede pulsar (carpeta, enlaces con Ctrl).
      LPCWSTR cur = IDC_ARROW;
      if (d) cur = d->vertical ? IDC_SIZEWE : IDC_SIZENS;
      else if (g_win->link.p) cur = IDC_HAND;
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
      if (g_win->pressedHit != HIT_NONE) {
        int tab, hit = TitleHit(x, y, &tab);
        if (hit == g_win->pressedHit) {
          if (hit == HIT_MIN) ShowWindow(hwnd, SW_MINIMIZE);
          else if (hit == HIT_MAX) ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
          else if (hit == HIT_CLOSE) PostMessage(hwnd, WM_CLOSE, 0, 0);
        }
        g_win->pressedHit = HIT_NONE;
        return 0;
      }
      if (g_win->tabDrag >= 0) {  // soltar una pestaña sobre otra ventana de este Warp: a ella; fuera: a una ventana nueva
        int ti = g_win->tabDrag;
        g_win->tabDrag = -1;
        ReleaseCapture();
        JoinLeave();
        POINT sp{x, y};
        ClientToScreen(hwnd, &sp);
        if (msg == WM_LBUTTONUP) DropAway(ti, nullptr, sp);
        return 0;
      }
      if (g_win->dragPane) {  // soltar un panel sobre otro: intercambiarlos; sobre otra ventana de este Warp: a ella; fuera: ventana nueva
        Pane* target = g_win->dropTarget;
        Pane* dragged = g_win->dragPane;
        g_win->dragPane = g_win->dropTarget = nullptr;
        ReleaseCapture();
        JoinLeave();
        POINT sp{x, y};  // captured: where it was released, also outside the window
        ClientToScreen(hwnd, &sp);
        if (target && g_live.count(target) && g_live.count(dragged) && !JoinTargetAt(sp)) {
          SwapPanes(dragged, target);
        } else if (g_live.count(dragged)) {
          DropAway(TabIndexOf(dragged), dragged->node->parent ? dragged : nullptr, sp);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      if (g_win->dragNode || g_win->selecting) {
        if (g_win->dragNode) SessionChanged();  // a divider moved
        g_win->dragNode = nullptr;
        g_win->selecting = false;
        ReleaseCapture();
        return 0;
      }
      if (g_win->tabs.empty() || y < TabBarH()) return 0;
      if (Pane* p = PaneAt(x, y); p && p->mouseMode && !(GetKeyState(VK_SHIFT) & 0x8000)) {
        MouseToApp(p, x, y, msg == WM_LBUTTONUP ? 1 : msg == WM_MBUTTONUP ? 2 : 3, false);
        g_appMouseUp = std::max<DWORD>(GetTickCount(), 1);  // not ahead of the clock: WM_APP_CLIP's 2 s check would wrap
      }
      return 0;
    }
    case WM_SETCURSOR:
      if (LOWORD(lp) == HTCLIENT) return TRUE;  // lo fija WM_MOUSEMOVE
      break;
    case WM_CAPTURECHANGED:
      // Capture taken away in the middle of dragging a tab or pane (Alt+Tab, another window took it): the drag ends
      // there and drops nothing; a window showing where it would go stops showing it.
      if ((HWND)lp != hwnd && (g_win->tabDrag >= 0 || g_win->dragPane)) {
        g_win->tabDrag = -1;
        g_win->dragPane = g_win->dropTarget = nullptr;
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      JoinLeave();
      return 0;
    case WM_CLOSE:
      // Closing with the X (or quit-all): the main window and those with «Reopen on restart» are kept
      // and come back at the next start; the others are discarded (WM_DESTROY).
      SaveSession();
      DestroyWindow(hwnd);
      return 0;
    case WM_QUERYENDSESSION:
      // Windows shutting down or logging off: every window of this process at once, whichever is asked first (the
      // process may be ended before the others are).
      for (Window* w : g_windows) {
        InWindow in(w);
        w->keepSession = true;
        SaveSession();
      }
      return TRUE;
    case WM_ENDSESSION:
      // Like Ctrl+Shift+Q (the process may end without WM_DESTROY).
      for (Window* w : g_windows) {
        InWindow in(w);
        if (!wp) w->keepSession = false;  // cancelled
        else if (ReopensAtStart()) SaveSession();
        else if (!g_windowDir.empty() && !w->sessionPath.empty()) DeleteFileW(w->sessionPath.c_str());
      }
      return 0;
    case WM_DESTROY: {
      // Se cerraron todas sus pestañas: la ventana se descarta (the main one keeps an empty session,
      // ClosePane). Closed with tabs: only the windows that come back at the next start are kept.
      bool keep = g_win->isMain || (ReopensAtStart() && (!g_win->tabs.empty() || g_win->keepSession));
      ToastsForget();
      if (!keep && !g_windowDir.empty() && !g_win->sessionPath.empty()) DeleteFileW(g_win->sessionPath.c_str());
      CloseWindowState();
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
  if (msg == g_msgTaskbar && g_msgTaskbar) {  // Explorer made (or remade) this window's taskbar button
    TaskbarReady();
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

static const wchar_t* kMainFile = L"\\window-main.txt";  // the main window's, in g_windowDir

static std::wstring FileName(const std::wstring& path) { return path.substr(path.find_last_of(L'\\') + 1); }

// Mutex names of this Warp (instance key): a test's window-main.txt never clashes with the real one.
static std::wstring InstanceMutex(const std::wstring& what) {
  wchar_t key[16];
  swprintf(key, 16, L"%08x", (unsigned)(ULONG_PTR)InstanceKey());
  return L"Local\\warp-lite-" + std::wstring(key) + L"-" + what;
}

// Is that file open in another window of this Warp? Also in one of the version before, which named the
// mutex without the instance key (still open if the new one starts before «Restart»). wait: after an
// update restart, how long the old windows may still take to exit.
static bool WindowFileOpen(const std::wstring& file, DWORD wait) {
  for (const std::wstring& name : {InstanceMutex(FileName(file)), L"Local\\warp-lite-" + FileName(file)}) {
    HANDLE m = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, name.c_str());
    if (!m) continue;
    DWORD r = WaitForSingleObject(m, wait);
    bool gone = r == WAIT_OBJECT_0 || r == WAIT_ABANDONED;
    if (gone) ReleaseMutex(m);
    CloseHandle(m);
    if (!gone) return true;
  }
  return false;
}

// Each window «owns» its file while it is open (a mutex named after the file): an older Warp, one process per window,
// starting meanwhile leaves it alone, and so does this one with theirs.
static bool ClaimWindowFile(Window* w, const std::wstring& file) {
  if (HANDLE old = OpenMutexW(SYNCHRONIZE, FALSE, (L"Local\\warp-lite-" + FileName(file)).c_str())) {
    CloseHandle(old);  // open in a window of the version before
    return false;
  }
  HANDLE m = CreateMutexW(nullptr, TRUE, InstanceMutex(FileName(file)).c_str());
  if (!m) return false;
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(m);
    return false;
  }
  w->fileMutex = m;
  return true;
}

// Does that saved window come back at a start? «reopen 1» or «restart 1» (SessionFlags), or a file of a
// version before the main window, which reopened them all: then this once too.
static bool ReopensFile(const std::wstring& file) {
  HANDLE f = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  std::string data(GetFileSize(f, nullptr), 0);
  DWORD r = 0;
  ReadFile(f, data.data(), (DWORD)data.size(), &r, nullptr);
  CloseHandle(f);
  data.resize(r);
  return data.rfind("BW1", 0) == 0 && (data.find("\nreopen ") == std::string::npos ||
                                       data.find("\nreopen 1\n") != std::string::npos ||
                                       data.find("\nrestart 1\n") != std::string::npos);
}

// Where this Warp keeps its windows: %APPDATA%\warp-lite (WARPLITE_DIR in the tests), or a single session file
// (WARPLITE_SESSION in the tests, or no %APPDATA%).
static void FindWindowDir() {
  wchar_t buf[MAX_PATH];
  if (GetEnvironmentVariableW(L"WARPLITE_SESSION", buf, MAX_PATH)) g_sessionFile = buf;
  else if (GetEnvironmentVariableW(L"WARPLITE_DIR", buf, MAX_PATH)) g_windowDir = buf;
  else if (GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH)) g_windowDir = std::wstring(buf) + L"\\warp-lite";
  else g_sessionFile = L"warp-lite-session.txt";
}

// The folder, made by the Warp that runs (not by a start that hands off).
static void PrepareWindowDir() {
  if (g_windowDir.empty()) return;
  CreateDirectoryW(g_windowDir.c_str(), nullptr);
  wchar_t buf[MAX_PATH];
  if (GetEnvironmentVariableW(L"WARPLITE_DIR", buf, MAX_PATH) || !GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH)) return;
  // Se llamaba better-warp, y antes había una sola sesión: se convierten en la primera ventana
  // (only into a folder without windows: copied on every start, it came back whenever window-1.txt was gone).
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((g_windowDir + L"\\window-*.txt").c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    FindClose(h);
  } else {
    std::wstring single = g_windowDir + L"\\session.txt";
    CopyFileW((std::wstring(buf) + L"\\BetterWarp\\session.txt").c_str(), single.c_str(), TRUE);
    if (GetFileAttributesW(single.c_str()) != INVALID_FILE_ATTRIBUTES)
      MoveFileExW(single.c_str(), (g_windowDir + L"\\window-1.txt").c_str(), 0);
  }
}

// The saved windows a start opens, the first one being the one the start itself asked for: the main window
// (window-main.txt) and the others that come back («Reopen on restart», or all of them after an update restart); the
// rest are discarded. None: a new window (""). relaunch: after an update, the windows of a Warp before this one may
// still be exiting. recover: after a crash, all of them come back (see «Crashes»).
static std::vector<std::wstring> StartFiles(bool relaunch, bool recover) {
  if (g_windowDir.empty()) return {g_sessionFile};
  // One start at a time (an older Warp may be starting too): only one of them takes the main window.
  HANDLE lock = CreateMutexW(nullptr, FALSE, InstanceMutex(L"start").c_str());
  if (lock) WaitForSingleObject(lock, 15000);
  DWORD until = GetTickCount() + (relaunch ? 5000 : 0);
  auto isOpen = [&](const std::wstring& f) {
    DWORD left = until - GetTickCount();
    return WindowFileOpen(f, (int)left > 0 ? left : 0);
  };
  std::wstring mainFile = g_windowDir + kMainFile;
  std::vector<std::wstring> files, out;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((g_windowDir + L"\\window-*.txt").c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do
      if (_wcsicmp(fd.cFileName, kMainFile + 1)) files.push_back(g_windowDir + L"\\" + fd.cFileName);
    while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  std::sort(files.begin(), files.end());
  // No main window yet (first start of this version, or its file is gone): the first saved window
  // that comes back becomes it (window-1.txt if there is one); with none, this new one.
  if (GetFileAttributesW(mainFile.c_str()) == INVALID_FILE_ATTRIBUTES && !isOpen(mainFile))
    for (auto it = files.begin(); it != files.end(); ++it)
      if (!isOpen(*it) && ReopensFile(*it) && MoveFileExW(it->c_str(), mainFile.c_str(), 0)) {
        files.erase(it);
        break;
      }
  if (!isOpen(mainFile)) out.push_back(mainFile);
  for (auto& file : files) {
    if (isOpen(file)) continue;  // open in an older Warp
    if (!recover && !ReopensFile(file)) {
      DeleteFileW(file.c_str());  // still open when Warp quit (or Windows shut down): discarded
      continue;
    }
    out.push_back(file);
  }
  if (out.empty()) out.push_back(L"");
  if (lock) ReleaseMutex(lock), CloseHandle(lock);
  return out;
}

static std::vector<std::wstring> SplitScript(std::wstring s) {
  std::vector<std::wstring> out;
  size_t pos;
  while ((pos = s.find(L'|')) != std::wstring::npos) out.push_back(s.substr(0, pos)), s.erase(0, pos + 1);
  if (!s.empty()) out.push_back(s);
  return out;
}

static Window* OpenWindow(const WindowInit& in) {
  Window* w = new Window;
  w->sessionPath = in.file.empty() ? NewWindowFile() : in.file;
  if (g_windowDir.empty()) {
    w->isMain = w->sessionPath == g_sessionFile;
  } else if (!ClaimWindowFile(w, w->sessionPath)) {  // already open (in an older Warp)
    delete w;
    return nullptr;
  } else {
    w->isMain = !_wcsicmp(w->sessionPath.c_str(), (g_windowDir + kMainFile).c_str());
  }
  static unsigned serial = 0;
  w->label = L"w" + std::to_wstring(++serial);
  w->openDir = in.openDir;
  w->adopt = in.adopt;
  if (in.like) {
    w->zoom = in.like->zoom, w->fontPx = in.like->fontPx, w->vertTabs = in.like->vertTabs;
    w->hasWinPos = true, w->winRect = in.place;
  }
  if (TestInstance()) w->script = SplitScript(in.script), w->childScript = in.childScript;
  g_windows.push_back(w);
  InWindow iw(w);
  HWND h = CreateWindowExW(0, L"WarpLite", L"Warp", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 760, nullptr,
                           nullptr, GetModuleHandleW(nullptr), w);
  if (!h) {
    g_windows.erase(std::remove(g_windows.begin(), g_windows.end(), w), g_windows.end());
    if (w->fileMutex) ReleaseMutex(w->fileMutex), CloseHandle(w->fileMutex);
    delete w;
    return nullptr;
  }
  SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
  DragAcceptFiles(h, TRUE);  // soltar archivos: se escribe su ruta
  // Mismo tamaño y estado (maximizada o en ventana) que la última vez, si ese sitio sigue en pantalla.
  if (w->hasWinPos && MonitorFromRect(&w->winRect, MONITOR_DEFAULTTONULL)) {
    WINDOWPLACEMENT wp{sizeof wp};
    wp.rcNormalPosition = w->winRect;
    wp.showCmd = w->winMax ? SW_SHOWMAXIMIZED : in.show == SW_SHOWNOACTIVATE ? SW_SHOWNOACTIVATE : SW_SHOWNORMAL;
    if (in.show == SW_SHOWMINNOACTIVE || in.show == SW_SHOWMINIMIZED || in.show == SW_MINIMIZE) wp.showCmd = in.show;
    SetWindowPlacement(h, &wp);
  } else {
    ShowWindow(h, in.show);
  }
  UpdateWindow(h);
  return w;
}

// The window of this Warp in front (z-order), or none.
static Window* FrontWindow() {
  for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT))
    for (Window* w : g_windows)
      if (w->hwnd == h && !w->dead) return w;
  return g_windows.empty() ? nullptr : g_windows.front();
}

static void InvalidateAll() {
  for (Window* w : g_windows) InvalidateRect(w->hwnd, nullptr, FALSE);
}

// The end of a window (WM_DESTROY): its panes end, their consoles closed together first. It leaves g_windows and is
// freed by the message loop (FreeDeadWindows): whatever destroyed it may still be using it. The last one ends the
// process.
static void CloseWindowState() {
  std::vector<Pane*> all = WindowPanes();
  for (Pane* p : all) StopPane(p);
  for (Pane* p : all) DestroyPane(p);
  g_win->tabs.clear();
  if (g_win->fileMutex) ReleaseMutex(g_win->fileMutex), CloseHandle(g_win->fileMutex), g_win->fileMutex = nullptr;
  g_win->dead = true;
  g_windows.erase(std::remove(g_windows.begin(), g_windows.end(), g_win), g_windows.end());
  g_dead.push_back(g_win);
  if (g_windows.empty()) g_crashArmed = 0, PostQuitMessage(0);
}

static void FreeDeadWindows() {
  if (g_dead.empty()) return;
  if (g_measureDC) SelectObject(g_measureDC, GetStockObject(SYSTEM_FONT));  // none of their fonts stays selected
  for (Window* w : g_dead) {
    for (HFONT f : w->fonts) if (f) DeleteObject(f);
    for (HFONT f : w->fallback) if (f) DeleteObject(f);
    for (HFONT f : {w->uiFont, w->badgeFont, w->iconFont}) if (f) DeleteObject(f);
    if (w->backDC) DeleteDC(w->backDC);
    if (w->backBmp) DeleteObject(w->backBmp);
    if (w->toastProc) CloseHandle(w->toastProc);
    delete w;
  }
  g_dead.clear();
}

// ---------------------------------------------------------------------------
// Crashes: one process holds every window of a Warp, so a crash would close them all at once. Their sessions are on
// disk (SessionChanged), and an unhandled exception (an access violation, an uncaught C++ exception, abort) relaunches
// Warp at once with --relaunch --crashed: every window comes back, as after an update restart, with ssh + tmux
// launched again and Claude Code and Codex resumed. The new process first writes a minidump of the one that crashed
// (crash-<time>.dmp in this Warp's folder, the last 3 kept), and both leave a line in crash.log there. At most once a
// minute: a crash less than 60 s after the last relaunch only leaves its line. Nothing of this runs until a crash.

static wchar_t g_crashExe[MAX_PATH], g_crashLog[MAX_PATH + 40], g_crashStamp[MAX_PATH + 40], g_crashVersion[32];

// A line in crash.log, without the heap (it may be what broke).
static void CrashLog(const char* text) {
  HANDLE f = CreateFileW(g_crashLog, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return;
  SYSTEMTIME t;
  GetLocalTime(&t);
  char b[640];
  int n = snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d:%02d %s\r\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, text);
  DWORD w;
  if (n > 0) WriteFile(f, b, (DWORD)std::min(n, (int)sizeof b - 1), &w, nullptr);
  CloseHandle(f);
}

static LONG WINAPI OnCrash(EXCEPTION_POINTERS* ep) {
  static volatile LONG once = 0;
  if (InterlockedExchange(&once, 1)) Sleep(INFINITE);  // another thread is dealing with it already
  DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0xE0000086;
  void* at = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr;
  HMODULE mod = nullptr;
  wchar_t path[MAX_PATH] = L"?";
  if (at && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)at, &mod))
    GetModuleFileNameW(mod, path, MAX_PATH);
  const wchar_t* name = wcsrchr(path, L'\\') ? wcsrchr(path, L'\\') + 1 : path;
  char line[400];
  snprintf(line, sizeof line, "crash %08lx at %ls+%llx (Warp %ls, process %lu, thread %lu)", (unsigned long)code, name,
           (unsigned long long)((char*)at - (char*)mod), g_crashVersion, GetCurrentProcessId(), GetCurrentThreadId());
  CrashLog(line);
  if (!g_crashArmed) {
    CrashLog("  not relaunched: no window was open");
    TerminateProcess(GetCurrentProcess(), code);
  }
  // At most once a minute (crash.stamp: when the last relaunch was).
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  ULONGLONG now = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime, last = 0;
  HANDLE f = CreateFileW(g_crashStamp, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, 0, nullptr);
  if (f != INVALID_HANDLE_VALUE) {
    DWORD r = 0;
    if (!ReadFile(f, &last, sizeof last, &r, nullptr) || r != sizeof last) last = 0;
    SetFilePointer(f, 0, nullptr, FILE_BEGIN);
    WriteFile(f, &now, sizeof now, &r, nullptr);
    CloseHandle(f);
  }
  if (last && now > last && now - last < 60ull * 10000000) {
    snprintf(line, sizeof line, "  not relaunched: %llu s after the last relaunch", (now - last) / 10000000);
    CrashLog(line);
    TerminateProcess(GetCurrentProcess(), code);
  }
  // The new Warp takes a minidump of this one (DumpCrashed), which waits for it, then ends.
  wchar_t name2[64], cmd[MAX_PATH + 160];
  swprintf(name2, 64, L"Local\\warp-lite-dumped-%lu", GetCurrentProcessId());
  HANDLE dumped = CreateEventW(nullptr, TRUE, FALSE, name2);
  swprintf(cmd, MAX_PATH + 160, L"\"%ls\" --relaunch --crashed %lu %lu %llx", g_crashExe, GetCurrentProcessId(), GetCurrentThreadId(),
           (unsigned long long)(ULONG_PTR)ep);
  STARTUPINFOW si{sizeof si};
  PROCESS_INFORMATION pi{};
  if (CreateProcessW(g_crashExe, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CrashLog("  relaunched");
    if (dumped) WaitForSingleObject(dumped, 15000);
  } else {
    CrashLog("  couldn't relaunch");
  }
  TerminateProcess(GetCurrentProcess(), code);
  return EXCEPTION_EXECUTE_HANDLER;
}

// What the C++ runtime ends the process with (an exception nobody caught, abort) is a crash too.
static void OnTerminate() { OnCrash(nullptr); }
static void OnAbort(int) { OnCrash(nullptr); }

static void CrashPaths() {
  GetModuleFileNameW(nullptr, g_crashExe, MAX_PATH);
  std::wstring base = g_windowDir.empty() ? g_sessionFile + L"." : g_windowDir + L"\\";
  wcsncpy(g_crashLog, (base + L"crash.log").c_str(), MAX_PATH + 39);
  wcsncpy(g_crashStamp, (base + L"crash.stamp").c_str(), MAX_PATH + 39);
}

static void ArmCrashes() {
  CrashPaths();
  wcsncpy(g_crashVersion, g_upd.running.c_str(), 31);
  ULONG stack = 64 * 1024;  // room for OnCrash after a stack overflow of this thread
  SetThreadStackGuarantee(&stack);
  SetUnhandledExceptionFilter(OnCrash);
  std::set_terminate(OnTerminate);
  signal(SIGABRT, OnAbort);
}

// --crashed <pid> <thread> <exception pointers>: the Warp that crashed waits while this one writes its minidump.
static void DumpCrashed(DWORD pid, DWORD tid, ULONG_PTR ep) {
  wchar_t name[64];
  swprintf(name, 64, L"Local\\warp-lite-dumped-%lu", pid);
  HANDLE dumped = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
  HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
  wchar_t img[MAX_PATH] = L"";
  DWORD n = MAX_PATH;
  bool ours = proc && QueryFullProcessImageNameW(proc, 0, img, &n) && !_wcsicmp(img, g_crashExe);  // only a Warp like this one
  typedef BOOL(WINAPI * PFN_Dump)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                   PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
  HMODULE dbg = ours && dumped ? LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32) : nullptr;
  auto dump = dbg ? (PFN_Dump)(void*)GetProcAddress(dbg, "MiniDumpWriteDump") : nullptr;
  SYSTEMTIME t;
  GetLocalTime(&t);
  wchar_t file[64];
  swprintf(file, 64, L"crash-%04d%02d%02d-%02d%02d%02d.dmp", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
  std::wstring base = g_windowDir.empty() ? g_sessionFile + L"." : g_windowDir + L"\\", path = base + file;
  bool ok = false;
  if (dump) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    MINIDUMP_EXCEPTION_INFORMATION mei{tid, (PEXCEPTION_POINTERS)ep, TRUE};
    ok = f != INVALID_HANDLE_VALUE && dump(proc, pid, f, (MINIDUMP_TYPE)(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules),
                                           ep ? &mei : nullptr, nullptr, nullptr);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    if (!ok) DeleteFileW(path.c_str());
  }
  if (dumped) SetEvent(dumped), CloseHandle(dumped);
  if (proc) CloseHandle(proc);
  CrashLog(ok ? ("  minidump " + ToUtf8(file)).c_str() : "  no minidump");
  // The last 3 are kept.
  std::vector<std::wstring> dumps;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((base + L"crash-*.dmp").c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do dumps.push_back(fd.cFileName);
    while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  std::sort(dumps.begin(), dumps.end());
  for (size_t i = 0; i + 3 < dumps.size(); i++) DeleteFileW((base + dumps[i]).c_str());
}

// ---------------------------------------------------------------------------
// One Warp, many windows: every window of a Warp (an instance: %APPDATA%\warp-lite, or a test's own folder or session
// file) lives in one process, so a tab or pane moves between windows with its shell, its scrollback and what runs in it.
// The first start takes the instance (a mutex named after it); any later start (the Start menu, «Open Warp here», a
// notification's click, a test's next window) hands its request to that process's hidden window, g_host, and ends.
// g_host is also where the reader threads and the workers post (see HostProc).

static const ULONG_PTR kCopyLaunch = 0x574c4e00;  // a start's request: lines «<what>\t<value>» (see OnLaunch)
static std::wstring g_childEnv;  // tests: this process's WARPLITE_CHILD_SCRIPT

static HWND FindHost() {
  for (HWND h = nullptr; (h = FindWindowExW(HWND_MESSAGE, h, L"WarpLiteHost", nullptr));)
    if (GetPropW(h, kInstanceProp) == InstanceKey()) return h;
  return nullptr;
}

// A request from another start of this Warp: «toast <URI>» (a notification's click), «cd <folder>» (a new tab in the
// window in front), «window <file>» (a saved window of this Warp), otherwise a new window; in a test's Warp, «script»
// and «child» give it its script.
static BOOL OnLaunch(const std::wstring& req) {
  std::map<std::wstring, std::wstring> kv;
  for (size_t a = 0; a < req.size();) {
    size_t e = req.find(L'\n', a), t = req.find(L'\t', a);
    if (e == std::wstring::npos) e = req.size();
    if (t < e) kv[req.substr(a, t - a)] = req.substr(t + 1, e - t - 1);
    a = e + 1;
  }
  if (kv.count(L"toast")) return ToastClicked(kv[L"toast"]), TRUE;
  WindowInit in;
  in.script = kv[L"script"], in.childScript = kv[L"child"];
  std::wstring dir = kv[L"cd"];
  DWORD a = dir.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(dir.c_str());
  if (!IsLocalDir(dir) || a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) dir.clear();
  if (!dir.empty())
    if (Window* w = FrontWindow()) {  // «Open Warp here» with Warp open: a new tab in its window in front
      InWindow iw(w);
      OpenDirTab(dir);
      return TRUE;
    }
  in.openDir = dir;
  std::wstring file = kv[L"window"];  // only one of this Warp's window files
  if (!file.empty() && (g_windowDir.empty() || _wcsnicmp(file.c_str(), (g_windowDir + L"\\window-").c_str(), g_windowDir.size() + 8) ||
                        file.find(L"..") != std::wstring::npos))
    return FALSE;
  if (file.empty() && !g_windowDir.empty()) {  // Warp opened again with its main window closed: it comes back, as at a start
    std::wstring mainFile = g_windowDir + kMainFile;
    bool open = WindowFileOpen(mainFile, 0);
    for (Window* w : g_windows) open = open || !_wcsicmp(w->sessionPath.c_str(), mainFile.c_str());
    if (!open) file = mainFile;
  } else {
    for (Window* w : g_windows)
      if (!_wcsicmp(w->sessionPath.c_str(), file.c_str())) return TRUE;  // open already
  }
  in.file = file;
  return OpenWindow(in) ? TRUE : FALSE;
}

// Paints the panes whose program wrote something (WM_APP_DIRTY): in each window, at most 60 frames a second.
static void OnDirty() {
  g_dirtyPosted = false;
  for (Window* w : g_windows) {
    InWindow in(w);
    bool any = false;
    for (Pane* p : WindowPanes()) {
      any = p->repaint.exchange(false) || any;
      if (p->titleChanged.exchange(false)) w->titleDirty = any = true;
    }
    if (!any) continue;
    PathsMaybeChanged();
    if (GetTickCount() - w->lastFrame < 16) {  // como mucho 60 fotogramas por segundo
      if (!w->frameTimer) SetTimer(w->hwnd, TIMER_FRAME, 16, nullptr), w->frameTimer = true;
    } else {
      PaintDirty();
    }
  }
}

// Every 250 ms: what each pane runs (every second), the reconnect loops, and the last command written at the prompt of
// a shell without integration once it has started.
static void OnRestoreTick() {
  static int ticks = 0;
  if (++ticks % 4 == 0) {  // cada segundo
    UpdateRunning();
    for (Pane* p : g_live) LoadShellCommands(p);
  }
  ReconnectTick();
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

// The window a pane is in, if it is still open (for what the reader threads post about it).
static Window* PaneWindow(LPARAM lp) {
  Pane* p = (Pane*)lp;
  return g_live.count(p) && p->win && !p->win->dead ? p->win : nullptr;
}

static LRESULT CALLBACK HostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  InWindow none(nullptr);  // no window until a handler picks one (a menu's modal loop may have one set)
  switch (msg) {
    case WM_APP_DIRTY: OnDirty(); return 0;
    case WM_APP_UPDPROG:
      for (Window* w : g_windows) InvalidateRect(w->hwnd, &w->updRc, FALSE);
      return 0;
    case WM_APP_UPDATE: OnUpdateReply(std::unique_ptr<UpdateReply>((UpdateReply*)lp).get()); return 0;
    case WM_APP_UPDATED: OnUpdateInstalled(std::unique_ptr<InstallJob>((InstallJob*)lp).get()); return 0;
    case WM_APP_FONT: OnFontDownloaded(std::unique_ptr<FontJob>((FontJob*)lp).get()); return 0;
    case WM_APP_PATHS: OnPathChecks(std::unique_ptr<PathChecks>((PathChecks*)lp).get()); return 0;
    case WM_APP_PARAMS: {  // parámetros de una orden, de ParamsThread
      std::unique_ptr<ParamsReply> r((ParamsReply*)lp);
      std::wstring low = Lower(r->cmd);
      g_paramsPending.erase(low);
      g_params[low] = r->names;
      for (Window* w : g_windows)
        if (w->comp.open && w->comp.loading == low) {
          InWindow in(w);
          CompRefresh();
        }
      return 0;
    }
    case WM_APP_CLIP: {  // OSC 52: texto copiado en tmux → portapapeles de Windows (con \r\n)
      std::unique_ptr<std::wstring> t((std::wstring*)lp);
      std::wstring w;
      for (wchar_t c : *t) {
        if (c == L'\n' && (w.empty() || w.back() != L'\r')) w += L'\r';
        w += c;
      }
      if (g_appMouseUp && GetTickCount() - g_appMouseUp < 2000) g_clipPending = w;  // drag in tmux: until Ctrl+C
      else SetClipboardText(w);
      return 0;
    }
    case WM_APP_LAYOUT:
      if (Window* w = PaneWindow(lp)) {
        InWindow in(w);
        Pane* p = (Pane*)lp;
        if (wp && !p->restoreCmd.empty()) {  // primer prompt: la última orden queda en la caja
          p->edit = ToWide(p->restoreCmd);
          p->caret = p->edit.size();
          p->restoreCmd.clear();
          if (p->resumeRun) {  // estaba en marcha al cerrar: se relanza (tmux new -A reengancha)
            p->resumeRun = false;
            SubmitEditor(p);
            p->recon.byWarp = true;  // if the network isn't up yet (Warp starting with Windows), it reconnects
          } else {
            p->history.push_back(p->edit);
          }
        }
        if (p->cwd != p->savedCwd) SessionChanged();  // a new folder
        Relayout();
      }
      return 0;
    case WM_APP_EXITED:  // its shell ended
      if (Window* w = PaneWindow(lp)) {
        InWindow in(w);
        ClosePane((Pane*)lp);
      }
      return 0;
    case WM_APP_PROMPT:  // a command sent from the box has finished (wp: its exit code)
      if (Window* w = PaneWindow(lp)) {
        InWindow in(w);
        CommandEnded((Pane*)lp, (int)(INT_PTR)wp);
      }
      return 0;
    case WM_APP_NOTE:  // a program has finished or wants an answer (wp: NOTE_*), from the reader thread
      if (Window* w = PaneWindow(lp)) {
        InWindow in(w);
        OnNote((Pane*)lp, (int)wp);
      }
      return 0;
    case WM_APP_NETCHANGE: OnNetworkChange(); return 0;
    case WM_APP_OPEN: {  // the other windows of this start (posted: the first one is on screen first)
      std::unique_ptr<std::vector<WindowInit>> list((std::vector<WindowInit>*)lp);
      for (auto& in : *list) OpenWindow(in);
      return 0;
    }
    case WM_TIMER:
      if (wp == TIMER_RESTORE) {
        OnRestoreTick();
      } else if (wp == TIMER_UPDANIM) {  // barra de «Instalando…» en movimiento
        if (g_upd.stage == UPD_INSTALLING && !g_upd.hidden)
          for (Window* w : g_windows) InvalidateRect(w->hwnd, &w->updRc, FALSE);
      } else if (wp == TIMER_UPDATE) {
        SetTimer(hwnd, TIMER_UPDATE, 6 * 3600 * 1000, nullptr);
        CheckForUpdates(false);
      } else if (wp == TIMER_REMOTE) {
        KillTimer(hwnd, TIMER_REMOTE);
        RemoteAtStart();
      } else if (wp == TIMER_SAVE) {  // the sessions that changed (SessionChanged)
        KillTimer(hwnd, TIMER_SAVE);
        g_saveTimer = false;
        for (Window* w : g_windows)
          if (w->saveDue) {
            InWindow in(w);
            SaveSession();
          }
      }
      return 0;
    case WM_COPYDATA: {
      auto* cd = (COPYDATASTRUCT*)lp;
      if (!cd->lpData) return FALSE;
      std::wstring t((const wchar_t*)cd->lpData, cd->cbData / sizeof(wchar_t));
      if (cd->dwData == kCopyLaunch) return OnLaunch(t);
      HWND from = (HWND)wp;  // warp-remote, only this Warp's (see «Phone»)
      if (!from || from != RemoteWindow()) return FALSE;
      if (cd->dwData == kCopyRemoteTicket) return OnRemoteTicket(t), TRUE;  // the pairing code asked for
      if (cd->dwData == kCopyRemotePane) return OnRemotePane(from, t);       // a phone, about a pane
      return FALSE;
    }
  }
  if (msg == g_msgRemote && g_msgRemote) {  // warp-remote: phones connected, a phone paired
    OnRemoteEvent(wp, lp);
    return 0;
  }
  if (msg == g_msgListPanes && g_msgListPanes) {  // warp-remote asks for every pane of this Warp: they go to it
    HWND to = (HWND)wp;
    if (!IsWindow(to) || GetPropW(to, kInstanceProp) != InstanceKey()) return 0;
    std::wstring t;
    for (Window* w : g_windows) {
      InWindow in(w);
      t += PaneListText();
    }
    COPYDATASTRUCT cd{kCopyPaneList, (DWORD)(t.size() * sizeof(wchar_t)), (void*)t.data()};
    SendMessageW(to, WM_COPYDATA, (WPARAM)hwnd, (LPARAM)&cd);
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

// Before the window handles them: the keys of what is open over its panes, and Warp's shortcuts.
static bool PreTranslate(Window* w, MSG& m) {
  InWindow in(w);
  bool keyDown = m.message == WM_KEYDOWN || m.message == WM_SYSKEYDOWN;
  if (g_win->pair.open && m.message == WM_KEYDOWN) {  // pairing card: Esc closes, Enter or Ctrl+C is its button; nothing reaches the terminal
    bool ctrl = GetKeyState(VK_CONTROL) & 0x8000;
    if (m.wParam == VK_ESCAPE) ClosePairCard();
    else if (m.wParam == VK_RETURN || (ctrl && m.wParam == 'C')) PairAction();
    return true;
  }
  if (g_win->sw.open && keyDown) {
    if (!SwKey(m.wParam)) TranslateMessage(&m), DispatchMessageW(&m);
    return true;
  }
  if (g_win->pick.open && keyDown) {
    if (!PickKey(m.wParam)) TranslateMessage(&m), DispatchMessageW(&m);
    return true;
  }
  if (g_win->comp.open && keyDown) {
    if (g_win->tabs.empty() || g_win->comp.p != CurTab().active || !EditorActive(g_win->comp.p)) CompClose();
    else if (CompKey(m.wParam)) return true;
  }
  if (g_win->find.open && keyDown && g_win->renameTab < 0) {
    if (!FindKey(m.wParam)) TranslateMessage(&m), DispatchMessageW(&m);
    return true;
  }
  if (g_win->renameTab >= 0 && keyDown) {
    if (!RenameKey(m.wParam)) TranslateMessage(&m), DispatchMessageW(&m);
    return true;
  }
  if (keyDown && m.hwnd == g_win->hwnd && HandleKeyDown(m.wParam, m.lParam)) {
    InvalidateRect(g_win->hwnd, nullptr, FALSE);
    return true;
  }
  return false;
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
  std::wstring windowArg, openDir, script;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  // A click on one of Warp's toasts (setup.cpp registers its protocol as «Warp.exe --toast "%1"»): exactly that, and
  // only the URI is read, then handed to the running Warp (ToastClicked). A line with anything more (a crafted link
  // breaking out of the quotes) does nothing; this process never opens a window, a session or a script.
  if (argc >= 2 && !wcscmp(argv[1], L"--toast")) {
    std::wstring uri = argc == 3 ? argv[2] : L"";
    LocalFree(argv);
    HWND host = uri.empty() || uri.size() > 96 ? nullptr : FindHost();
    if (host) {
      DWORD pid = 0;
      GetWindowThreadProcessId(host, &pid);
      AllowSetForegroundWindow(pid);
      std::wstring req = L"toast\t" + uri;
      COPYDATASTRUCT cd{kCopyLaunch, (DWORD)(req.size() * sizeof(wchar_t)), (void*)req.data()};
      DWORD_PTR r = 0;
      SendMessageTimeoutW(host, WM_COPYDATA, 0, (LPARAM)&cd, SMTO_ABORTIFHUNG, 5000, &r);
    }
    return 0;
  }
  bool relaunch = false, crashed = false;
  DWORD crashPid = 0, crashTid = 0;
  ULONG_PTR crashEp = 0;
  for (int i = 1; i < argc; i++)
    if (!wcscmp(argv[i], L"--relaunch")) relaunch = true;  // after an update or a crash: the Warp before this one is closing
  for (int i = 1; i + 1 < argc; i++) {
    if (!wcscmp(argv[i], L"--window")) windowArg = argv[++i];
    else if (!wcscmp(argv[i], L"--cd")) openDir = argv[++i];
    else if (!wcscmp(argv[i], L"--script")) script = argv[++i];
    else if (!wcscmp(argv[i], L"--crashed") && i + 3 < argc) {  // see «Crashes»
      crashed = true;
      crashPid = wcstoul(argv[i + 1], nullptr, 10), crashTid = wcstoul(argv[i + 2], nullptr, 10);
      crashEp = (ULONG_PTR)wcstoull(argv[i + 3], nullptr, 16);
      i += 3;
    }
  }
  LocalFree(argv);
  FindWindowDir();
  if (crashed && relaunch) CrashPaths(), DumpCrashed(crashPid, crashTid, crashEp);
  if (!TestInstance()) script.clear();  // --script runs only in a test's Warp
  if (!openDir.empty()) {
    // El Explorador pasa "C:\" y Windows lo lee como «C:"»: se arregla la barra del final.
    while (!openDir.empty() && (openDir.back() == L' ' || openDir.back() == L'\t')) openDir.pop_back();
    if (!openDir.empty() && openDir.back() == L'"') openDir.back() = L'\\';
    if (openDir.size() > 3 && openDir.back() == L'\\') openDir.pop_back();
    if (openDir.size() == 2 && openDir[1] == L':') openDir += L'\\';
    DWORD a = GetFileAttributesW(openDir.c_str());
    if (!IsLocalDir(openDir) || a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) openDir.clear();
  }
  if (TestInstance()) {
    wchar_t sc[2048];
    if (GetEnvironmentVariableW(L"WARPLITE_CHILD_SCRIPT", sc, 2048)) g_childEnv = sc;
  }

  // This Warp running already: it takes the request and this process ends. After an update (--relaunch) the one
  // running is closing: this one waits for it and takes over.
  HANDLE instance = CreateMutexW(nullptr, FALSE, InstanceMutex(L"instance").c_str());
  DWORD got = instance ? WaitForSingleObject(instance, relaunch ? 30000 : 0) : WAIT_FAILED;
  if (instance && got != WAIT_OBJECT_0 && got != WAIT_ABANDONED) {
    std::wstring req = L"new\t1\n";
    if (!openDir.empty()) req += L"cd\t" + openDir + L"\n";
    if (!windowArg.empty()) req += L"window\t" + windowArg + L"\n";
    if (!script.empty()) req += L"script\t" + script + L"\nchild\t" + g_childEnv + L"\n";
    for (DWORD until = GetTickCount() + 15000;;) {
      if (HWND host = FindHost()) {
        DWORD pid = 0;
        GetWindowThreadProcessId(host, &pid);
        AllowSetForegroundWindow(pid);
        COPYDATASTRUCT cd{kCopyLaunch, (DWORD)(req.size() * sizeof(wchar_t)), (void*)req.data()};
        DWORD_PTR r = 0;
        if (SendMessageTimeoutW(host, WM_COPYDATA, 0, (LPARAM)&cd, SMTO_ABORTIFHUNG, 10000, &r)) return r ? 0 : 1;
      }
      got = WaitForSingleObject(instance, 200);  // the one running may be exiting: then this one takes over
      if (got == WAIT_OBJECT_0 || got == WAIT_ABANDONED) break;
      if ((int)(GetTickCount() - until) > 0) return 1;
    }
  }

  g_msgUpdated = RegisterWindowMessageW(L"warp-lite-updated");
  g_msgListPanes = RegisterWindowMessageW(L"warp-lite-list-panes");
  g_msgTaskbar = RegisterWindowMessageW(L"TaskbarButtonCreated");
  g_msgRemote = RegisterWindowMessageW(L"warp-lite-remote");
  g_msgRemoteState = RegisterWindowMessageW(L"warp-lite-remote-state");
  // The hidden window first: a start right after this one finds it at once.
  WNDCLASSEXW hc{sizeof hc};
  hc.lpfnWndProc = HostProc;
  hc.hInstance = inst;
  hc.lpszClassName = L"WarpLiteHost";
  RegisterClassExW(&hc);
  g_host = CreateWindowExW(0, L"WarpLiteHost", nullptr, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
  if (!g_host) return 1;
  SetPropW(g_host, kInstanceProp, InstanceKey());
  {
    wchar_t self[MAX_PATH], buf[64];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    g_upd.running = GetEnvironmentVariableW(L"WARPLITE_FAKE_VERSION", buf, 64) ? buf : ExeVersion(self);
    g_upd.testUrl = GetEnvironmentVariableW(L"WARPLITE_UPDATE_URL", nullptr, 0) > 0;
  }
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // WIC (pegar imágenes)
  // After an update from a Warp of one process per window: its windows may still be closing.
  if (relaunch)
    for (int t = 0; t < 200 && !SiblingWindows().empty(); t++) Sleep(100);
  PrepareWindowDir();
  ArmCrashes();
  LoadUpdateSettings();
  struct { const unsigned char *a, *b; } fonts[] = {{hack_regular, hack_regular_end},
                                                     {hack_bold, hack_bold_end},
                                                     {hack_italic, hack_italic_end},
                                                     {hack_bolditalic, hack_bolditalic_end},
                                                     {nerd_symbols, nerd_symbols_end}};
  for (auto& f : fonts) {
    DWORD n = 0;
    AddFontMemResourceEx((void*)f.a, (DWORD)(f.b - f.a), nullptr, &n);
  }
  LoadFontSetting();
  ReadTextSetting(&g_termFg);

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
  // Before the window: the taskbar groups it with the Start menu shortcut, which has the same ID (see «Windows
  // notifications»).
  SetCurrentProcessExplicitAppUserModelID(kAppId);
  // This start's own window first (the script and --cd are its), on screen before the others open.
  std::vector<std::wstring> files = windowArg.empty() ? StartFiles(relaunch, crashed && relaunch) : std::vector<std::wstring>{windowArg};
  WindowInit first;
  first.file = files[0], first.openDir = openDir, first.script = script, first.childScript = g_childEnv, first.show = show;
  if (!OpenWindow(first)) return 1;
  g_crashArmed = 1;
  if (files.size() > 1) {
    auto* rest = new std::vector<WindowInit>;
    for (size_t i = 1; i < files.size(); i++) {
      WindowInit in;
      in.file = files[i], in.script = in.childScript = g_childEnv, in.show = SW_SHOWNOACTIVATE;
      rest->push_back(in);
    }
    PostMessageW(g_host, WM_APP_OPEN, 0, (LPARAM)rest);
  }
  SetTimer(g_host, TIMER_RESTORE, 250, nullptr);
  if (!GetEnvironmentVariableW(L"WARPLITE_NO_UPDATE", nullptr, 0)) SetTimer(g_host, TIMER_UPDATE, 20000, nullptr);
  SetTimer(g_host, TIMER_REMOTE, 1500, nullptr);  // once: warp-remote, if a phone is paired

  MSG m;
  while (GetMessageW(&m, nullptr, 0, 0)) {
    FreeDeadWindows();
    g_win = nullptr;
    // The previous message may have changed the pane or tab in view (there are many ways to).
    for (Window* w : g_windows) {
      InWindow in(w);
      SyncUnread();
    }
    Window* w = nullptr;
    if (HWND root = m.hwnd ? GetAncestor(m.hwnd, GA_ROOT) : nullptr)
      for (Window* x : g_windows)
        if (x->hwnd == root) w = x;
    if (w && PreTranslate(w, m)) continue;
    TranslateMessage(&m);
    DispatchMessageW(&m);
  }
  g_crashArmed = 0;  // ending: a crash from here on doesn't bring the windows back
  return 0;
}
