// Instalador de warp-lite: lo instala para el usuario actual (sin administrador) en
// %LOCALAPPDATA%\Programs\warp-lite, crea accesos en Inicio y escritorio y se registra
// en «Aplicaciones instaladas» con su desinstalador.
//   warp-lite-setup.exe [--silent]                 instala
//   uninstall.exe --uninstall [--silent]          desinstala (el que se copia al instalar)

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
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <string>

// warp-lite.exe va dentro del instalador.
__asm__(".section .rdata,\"dr\"\n.balign 4\n"
        ".global warp_lite_exe\nwarp_lite_exe:\n.incbin \"dist/warp-lite.exe\"\n"
        ".global warp_lite_exe_end\nwarp_lite_exe_end:\n.text\n");
extern "C" const unsigned char warp_lite_exe[], warp_lite_exe_end[];

static const wchar_t* kUninstallKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\warp-lite";
static const wchar_t* kVersion = L"0.3.4";
// Nombre visible (se llamó warp-lite; la carpeta de instalación y la clave del registro lo conservan).
static const wchar_t* kAppName = L"Warp";
static bool g_silent = false;
static bool g_es = false;  // textos en español si Windows está en español

static const wchar_t* Tr(const wchar_t* en, const wchar_t* es) { return g_es ? es : en; }

static void Message(const std::wstring& text, UINT flags = MB_ICONINFORMATION) {
  if (!g_silent) MessageBoxW(nullptr, text.c_str(), kAppName, flags);
}

static bool Ask(const std::wstring& text) {
  return g_silent || MessageBoxW(nullptr, text.c_str(), kAppName, MB_YESNO | MB_ICONQUESTION) == IDYES;
}

static std::wstring KnownFolder(REFKNOWNFOLDERID id) {
  PWSTR p = nullptr;
  std::wstring s;
  if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) s = p;
  CoTaskMemFree(p);
  return s;
}

static std::wstring InstallDir() { return KnownFolder(FOLDERID_LocalAppData) + L"\\Programs\\warp-lite"; }
static std::wstring StartMenuLink() { return KnownFolder(FOLDERID_Programs) + L"\\Warp.lnk"; }
static std::wstring DesktopLink() { return KnownFolder(FOLDERID_Desktop) + L"\\Warp.lnk"; }

static bool MakeShortcut(const std::wstring& lnk, const std::wstring& target, const std::wstring& workDir) {
  IShellLinkW* sl = nullptr;
  if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&sl)))
    return false;
  sl->SetPath(target.c_str());
  sl->SetWorkingDirectory(workDir.c_str());
  sl->SetIconLocation(target.c_str(), 0);
  sl->SetDescription(kAppName);
  IPersistFile* pf = nullptr;
  bool ok = SUCCEEDED(sl->QueryInterface(IID_IPersistFile, (void**)&pf)) && SUCCEEDED(pf->Save(lnk.c_str(), TRUE));
  if (pf) pf->Release();
  sl->Release();
  return ok;
}

static bool WriteAll(const std::wstring& path, const void* data, DWORD size) {
  HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  DWORD w = 0;
  bool ok = WriteFile(f, data, size, &w, nullptr) && w == size;
  CloseHandle(f);
  return ok;
}

// Un .exe en uso no se puede sobrescribir, pero sí renombrar: se aparta y se borra en la
// siguiente instalación.
static bool ReplaceFile(const std::wstring& path, const void* data, DWORD size) {
  if (WriteAll(path, data, size)) return true;
  std::wstring old = path + L".old-" + std::to_wstring(GetTickCount64());
  return MoveFileExW(path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING) && WriteAll(path, data, size);
}

static void DeleteOldCopies(const std::wstring& dir) {
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((dir + L"\\*.old-*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
  while (FindNextFileW(h, &fd));
  FindClose(h);
}

static void SetString(HKEY k, const wchar_t* name, const std::wstring& v) {
  RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
}

static void SetDword(HKEY k, const wchar_t* name, DWORD v) {
  RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&v, sizeof v);
}

static int Install() {
  std::wstring dir = InstallDir(), exe = dir + L"\\Warp.exe", uninst = dir + L"\\uninstall.exe";
  if (!Ask(Tr(L"Install Warp for this user?", L"¿Instalar Warp para este usuario?"))) return 1;
  SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
  DeleteOldCopies(dir);
  if (!ReplaceFile(exe, warp_lite_exe, (DWORD)(warp_lite_exe_end - warp_lite_exe))) {
    Message(Tr(L"Could not copy Warp.exe.", L"No se ha podido copiar Warp.exe."), MB_ICONERROR);
    return 2;
  }
  wchar_t self[MAX_PATH];
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  if (_wcsicmp(self, uninst.c_str()) != 0 && !CopyFileW(self, uninst.c_str(), FALSE)) {
    std::wstring old = uninst + L".old-" + std::to_wstring(GetTickCount64());
    MoveFileExW(uninst.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
    CopyFileW(self, uninst.c_str(), FALSE);
  }
  std::wstring home = KnownFolder(FOLDERID_Profile);
  MakeShortcut(StartMenuLink(), exe, home);
  MakeShortcut(DesktopLink(), exe, home);
  // Antes se llamaba better-warp y warp-lite: fuera sus accesos y el .exe con el nombre viejo.
  for (const wchar_t* old : {L"better-warp.lnk", L"warp-lite.lnk"}) {
    DeleteFileW((KnownFolder(FOLDERID_Programs) + L"\\" + old).c_str());
    DeleteFileW((KnownFolder(FOLDERID_Desktop) + L"\\" + old).c_str());
  }
  std::wstring legacy = dir + L"\\warp-lite.exe";
  if (!DeleteFileW(legacy.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)  // abierto: se aparta
    MoveFileExW(legacy.c_str(), (legacy + L".old-" + std::to_wstring(GetTickCount64())).c_str(), MOVEFILE_REPLACE_EXISTING);

  HKEY k;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) ==
      ERROR_SUCCESS) {
    SetString(k, L"DisplayName", kAppName);
    SetString(k, L"DisplayVersion", kVersion);
    SetString(k, L"Publisher", L"justdiego");
    SetString(k, L"DisplayIcon", exe);
    SetString(k, L"InstallLocation", dir);
    SetString(k, L"UninstallString", L"\"" + uninst + L"\" --uninstall");
    SetString(k, L"QuietUninstallString", L"\"" + uninst + L"\" --uninstall --silent");
    SetDword(k, L"NoModify", 1);
    SetDword(k, L"NoRepair", 1);
    SetDword(k, L"EstimatedSize", (DWORD)((warp_lite_exe_end - warp_lite_exe) / 1024 + 600));
    RegCloseKey(k);
  }
  if (!g_silent &&
      MessageBoxW(nullptr, Tr(L"Warp is installed. Open it now?", L"Warp está instalado. ¿Abrirlo ahora?"),
                  kAppName, MB_YESNO | MB_ICONINFORMATION) == IDYES)
    ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, home.c_str(), SW_SHOWNORMAL);
  return 0;
}

// El desinstalador no puede borrarse a sí mismo: se copia a %TEMP% y sigue desde allí.
static int Uninstall(bool fromTemp) {
  std::wstring dir = InstallDir();
  if (!fromTemp) {
    if (!Ask(Tr(L"Uninstall Warp? Your sessions and settings are kept.",
                L"¿Desinstalar Warp? Se conservan tus sesiones y ajustes.")))
      return 1;
    wchar_t self[MAX_PATH], tmp[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    GetTempPathW(MAX_PATH, tmp);
    std::wstring copy = std::wstring(tmp) + L"warp-lite-uninstall.exe";
    std::wstring args = L"--uninstall-temp";
    if (g_silent) args += L" --silent";
    if (CopyFileW(self, copy.c_str(), FALSE)) {
      SHELLEXECUTEINFOW sei{sizeof sei};
      sei.lpFile = copy.c_str();
      sei.lpParameters = args.c_str();
      sei.nShow = SW_SHOWNORMAL;
      if (ShellExecuteExW(&sei)) return 0;
    }
    fromTemp = true;  // sin copia: se intenta desde aquí (uninstall.exe quedará)
  }
  DeleteFileW(StartMenuLink().c_str());
  DeleteFileW(DesktopLink().c_str());
  RegDeleteKeyW(HKEY_CURRENT_USER, kUninstallKey);
  HKEY run;  // «Iniciar con Windows»
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_WRITE, &run) ==
      ERROR_SUCCESS) {
    RegDeleteValueW(run, L"Warp");
    RegCloseKey(run);
  }
  DeleteOldCopies(dir);
  DeleteFileW((dir + L"\\warp-lite.exe").c_str());  // nombre antiguo
  bool ok = DeleteFileW((dir + L"\\Warp.exe").c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND;
  // uninstall.exe sigue abierto unos instantes (es quien ha lanzado esta copia).
  for (int i = 0; i < 50 && !DeleteFileW((dir + L"\\uninstall.exe").c_str()) &&
                  GetLastError() != ERROR_FILE_NOT_FOUND;
       i++)
    Sleep(200);
  RemoveDirectoryW(dir.c_str());
  if (ok) Message(Tr(L"Warp has been uninstalled.", L"Warp se ha desinstalado."));
  else Message(Tr(L"Close Warp and try again.", L"Cierra Warp y vuelve a intentarlo."), MB_ICONWARNING);
  return ok ? 0 : 3;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  g_es = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_SPANISH;
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  bool uninstall = false, fromTemp = false;
  for (int i = 1; i < argc; i++) {
    if (!wcscmp(argv[i], L"--silent")) g_silent = true;
    else if (!wcscmp(argv[i], L"--uninstall")) uninstall = true;
    else if (!wcscmp(argv[i], L"--uninstall-temp")) uninstall = fromTemp = true;
  }
  LocalFree(argv);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  int rc = uninstall ? Uninstall(fromTemp) : Install();
  CoUninitialize();
  return rc;
}
