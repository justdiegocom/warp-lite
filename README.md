# better-warp

Terminal mínima para Windows con pestañas y paneles al estilo de [Warp](https://github.com/warpdotdev/warp),
sin nada más: ni IA, ni bloques, ni cuenta, ni telemetría. Un único `.exe` de ~1 MB (Win32 + ConPTY + libvterm + GDI).

Lo único que añade: **al cerrar y volver a abrir, recupera las pestañas, los paneles y la última orden
de cada panel escrita en el prompt, sin ejecutarla** (pulsa Enter para lanzarla).

## Atajos (los de Warp en Windows)

| Atajo | Acción |
|---|---|
| Ctrl+Shift+D | Dividir panel a la derecha |
| Ctrl+Shift+E | Dividir panel hacia abajo |
| Ctrl+Shift+W | Cerrar panel |
| Ctrl+Shift+T | Nueva pestaña |
| Ctrl+Alt+← → ↑ ↓ | Ir al panel de ese lado |
| Ctrl+Shift+[ / ] | Panel anterior / siguiente |
| Ctrl+PgUp / Ctrl+PgDn, Ctrl+Tab / Ctrl+Shift+Tab | Pestaña anterior / siguiente |
| Ctrl+Shift+PgUp / PgDn | Mover la pestaña a la izquierda / derecha |
| Ctrl+1 … Ctrl+8, Ctrl+9 | Ir a la pestaña N / a la última |
| Ctrl+Shift+C, Ctrl+C con selección | Copiar |
| Ctrl+Shift+V, Ctrl+V | Pegar |
| Ctrl+= / Ctrl+- / Ctrl+0 | Tamaño de letra |
| Shift+PgUp / PgDn, rueda | Historial de pantalla |
| **Ctrl+Alt+Shift+← → ↑ ↓** | **Mover el panel** (intercambiarlo con el de ese lado). No es de Warp: allí se arrastra. |

Aspecto: tema «Dark» de Warp (fondo casi negro y su paleta ANSI), pestañas en la barra de título y una
cabecera con título y ✕ en cada panel cuando la pestaña está dividida.

Ratón: arrastrar la cabecera de un panel y soltarla sobre otro los intercambia, arrastrar los divisores para redimensionar, arrastrar para seleccionar, clic derecho copia (si hay
selección) o pega, clic central en una pestaña la cierra, `+` abre otra.

## Última orden

- Se guarda al pulsar Enter: el texto escrito en el prompt (incluido lo que venga del historial o del
  autocompletado). No se guardan `exit`/`logout` ni lo que se escriba dentro de programas de pantalla
  completa (tmux, vim…), así que si entras con `frhm1` y dentro usas tmux, al reabrir verás `frhm1`.
- Sesión en `%APPDATA%\BetterWarp\session.txt` (`BETTERWARP_SESSION` para cambiarla).
- Shell: `pwsh` si está instalado, si no `powershell`. Se cambia con `BETTERWARP_SHELL` (p. ej. `cmd.exe`).

## Compilar

`tools/build.sh` (copia al Mac y compila con `zig c++ -target x86_64-windows-gnu`; ~3 s) → `dist/better-warp.exe`.
`tools/wintest.ps1` es la prueba de humo en Windows (dos arranques con guion y capturas).

## Dictado por voz

Mismo motor que Warp: **Wispr Flow**. Warp graba a 16 kHz mono, lo manda en WAV/base64 a su servidor
(`/ai/transcribe`, `provider: wispr`) y este llama a Wispr. Aquí se llama a la API de Wispr directamente
(`platform-api.wisprflow.ai/api/v1/dash/api`) con tu propia clave, sin cuenta de Warp.

- **Mantén Alt Gr** (Alt derecho, la tecla por defecto de Warp en Windows), habla y suelta; o pulsa el
  micrófono de la barra de título para empezar y otra vez para terminar. El texto queda escrito en el
  prompt, sin ejecutarse. Alt Gr + otra tecla sigue escribiendo @, #, etc.
- Claves en `%APPDATA%\BetterWarp\config.ini` (se crea solo la primera vez):
  `wispr_api_key=…` (de https://platform.wisprflow.ai, API Keys; Wispr da acceso a su API por solicitud).
  Sin clave de Wispr se usa `openai_api_key=…` con Whisper (`whisper-1`), el otro proveedor de Warp.
  `voice_language=es` (vacío = detección automática). También valen `WISPR_API_KEY`/`OPENAI_API_KEY`.
- `tools/wintest-voice.ps1` prueba la petición contra un servidor que imita a Wispr.

## Licencias

libvterm (MIT) en `third_party/libvterm`. No se ha copiado código de Warp (AGPL-3.0); solo se replican
sus atajos.
