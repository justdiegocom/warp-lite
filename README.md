# warp-lite (se instala como «Warp»)

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
| Ctrl + `+` / `-` / `0`, Ctrl + rueda | Escala de toda la interfaz (se guarda) |
| Ctrl+Shift + `+` / `-` | Tamaño de letra (se guarda) |
| Doble clic en una pestaña | Renombrarla (Enter guarda, Esc cancela, vacío = título automático) |
| Shift+PgUp / PgDn, rueda | Historial de pantalla |
| **Ctrl+Alt+Shift+← → ↑ ↓** | **Mover el panel** (intercambiarlo con el de ese lado). No es de Warp: allí se arrastra. |

Aspecto: tema «Dark» de Warp (fondo casi negro y su paleta ANSI), pestañas en la barra de título y una
cabecera con título y ✕ en cada panel cuando la pestaña está dividida.

Fuente: Hack a 13 px, la de Warp por defecto, incluida en el `.exe` (`third_party/hack`, licencia MIT/Bitstream Vera).

La cabecera de cada panel (y su pestaña) muestra la orden en marcha mientras corre un programa, como en Warp (p. ej. `ssh frhm1`); al terminar vuelve al título de la shell.

Los caracteres que Hack no tiene (el `⎿` de Claude, iconos de Nerd Fonts, emoji, CJK…) se dibujan con
la primera fuente de respaldo que los tenga: «Symbols Nerd Font Mono» (incluida, `third_party/nerd-symbols`,
MIT), Segoe UI Symbol, Cascadia, Segoe UI Emoji… Las URL de la salida van en blanco azulado.

Al dividir o cerrar, los paneles de una misma fila o columna se reparten el espacio a partes iguales, como en Warp.

Ratón: arrastrar la cabecera de un panel y soltarla sobre otro los intercambia, arrastrar los divisores para redimensionar, arrastrar para seleccionar, clic derecho copia (si hay
selección) o pega, clic central en una pestaña la cierra, `+` abre otra.

## Caja de entrada (como Warp)

Con PowerShell (lo normal), la orden se escribe en una caja al pie de cada panel, con la carpeta actual
en un apartado encima. warp-lite arranca PowerShell con un prompt propio que, cada vez que espera una
orden, avisa en silencio (`OSC 9;9;<ruta>`); el resto del tiempo (ssh, tmux, un «¿seguro? [s/n]»…) las
teclas van directas al programa. En la caja: ←/→ (con Ctrl, por palabras), Inicio/Fin, Retroceso y
Supr (con Ctrl, palabra entera), ↑/↓ historial, Esc o Ctrl+C borran, Ctrl+V pega. Aún sin autocompletado
con Tab. Cada panel recuerda su carpeta al reabrir. Con `WARPLITE_SHELL` (otra shell) no hay caja.

## Última orden

- Se guarda al pulsar Enter: el texto escrito en el prompt (incluido lo que venga del historial o del
  autocompletado). No se guardan `exit`/`logout` ni lo que se escriba dentro de programas de pantalla
  completa (tmux, vim…), así que si entras con `frhm1` y dentro usas tmux, al reabrir verás `frhm1`.
- Sesión en `%APPDATA%\warp-lite\session.txt` (`WARPLITE_SESSION` para cambiarla).
- Shell: `pwsh` si está instalado, si no `powershell`. Se cambia con `WARPLITE_SHELL` (p. ej. `cmd.exe`).

## Compilar

`tools/build.sh` (copia al Mac y compila con `zig c++ -target x86_64-windows-gnu`; ~3 s) → `dist/warp-lite.exe`.
`tools/wintest.ps1` es la prueba de humo en Windows (dos arranques con guion y capturas).

## Instalar en Windows

Se instala con el nombre visible «Warp» (`Warp.exe`, accesos `Warp.lnk`, «Aplicaciones instaladas»);
la carpeta de instalación y la de datos siguen llamándose `warp-lite` para conservar las sesiones.

`dist/warp-lite-setup.exe`: instala para el usuario actual (sin administrador) en
`%LOCALAPPDATA%\Programs\warp-lite`, crea accesos en Inicio y escritorio y aparece en «Aplicaciones
instaladas» con su desinstalador (conserva sesiones y ajustes). `--silent` para hacerlo sin preguntas.
Icono propio en `assets/` (`warp-lite.ico`, incrustado en los dos `.exe`). `tools/wintest-setup.ps1` lo prueba.

## Idioma, colores y enlaces

- Interfaz en inglés por defecto; clic derecho en la zona vacía de la barra de título → Language.
- La orden de la caja sale en verde si existe (alias, funciones y cmdlets de PowerShell, programas del
  PATH, rutas) y con subrayado rojo discontinuo si no, como en Warp. La lista de PowerShell se genera
  en segundo plano al arrancar (evento OnIdle) para no retrasar el prompt.
- La shell recibe `TERM=xterm-256color` y `COLORTERM=truecolor` (como Warp) para que ssh/tmux usen color.
  libvterm lleva un parche para el atenuado (SGR 2).
- Ctrl + ratón sobre un enlace http(s) lo subraya; Ctrl+clic lo abre, aunque esté partido en varias filas.

## Pestañas, búsqueda y ventanas

- Botón a la izquierda: pestañas arriba o en barra lateral. Clic derecho en una pestaña: renombrar,
  ocultar, cerrar; «N ocultas» las vuelve a mostrar. Ctrl+Tab y Ctrl+1…9 se saltan las ocultas.
- Ctrl+F (y Ctrl+Shift+F, el de Warp): buscar en pantalla e historial; Enter / Shift+Enter, Esc cierra.
- Clic en el apartado de la carpeta: selector con buscador (carpeta: cd; archivo: su ruta a la caja).
- Arrastrar una pestaña o la cabecera de un panel fuera de la ventana: se abre en otra ventana (un
  proceso por ventana, sesión en `%APPDATA%\warp-lite\window-<id>.txt`). Al abrir warp-lite vuelven
  todas: cerrar con la X guarda la ventana; cerrar todas sus pestañas la descarta.
- Al reabrir, los paneles que tenían algo en marcha (ssh + tmux) lo relanzan; el resto deja la orden
  en la caja.

## Rendimiento

La salida de la shell solo repinta las filas que cambian (libvterm avisa por filas), como mucho a
60 fps, sobre un búfer fijo; el pintado completo queda para cambios de interfaz. Medido en el PC con
7 paneles: 3,6-4,8 ms un pintado completo frente a 0,07-0,1 ms por filas; el resultado es idéntico
píxel a píxel (`tools/wintest-cpu.ps1`, pasos `prime`/`verify`/`bench`).

## Licencias

libvterm (MIT) en `third_party/libvterm`. No se ha copiado código de Warp (AGPL-3.0); solo se replican
sus atajos.
