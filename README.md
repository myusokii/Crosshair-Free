# CrosshairFree

A free, open-source crosshair **editor + overlay** for Windows, written in plain C (Win32, no dependencies).

## Features
- **Layer editor** (like an emblem creator): Cross, Rectangle, Ellipse, Triangle and **your own PNG images**
  - every layer has its own colour (full colour picker + hex), opacity, rotation, X/Y position, size, border/hollow width
  - per-layer outline with its own colour, thickness and opacity
  - cross layers have a gap and individually switchable arms (T-style etc.)
  - PNG layers: original colours, tint, or flat recolour; lock aspect ratio
  - drag the selected layer in the live preview, arrow keys nudge it, Delete removes it
  - up to 16 layers, reorder / copy / hide
- **Library**: save your crosshairs and switch between them. Every card has a live preview. 6 built-in presets.
- **Import game codes** (Paste & Import / Ctrl+V): CS2 (`CS...` and old `CSGO-xxxxx-...`) and Valorant. They become normal layers you can keep editing.
- **Export** as a Valorant code
- **Rebindable hotkeys** (default F8 show/hide crosshair, F9 menu), tray icon, start with Windows
- **Themeable UI**: flat and plain. Presets + custom colours for background, panels, buttons, borders, accent, text; adjustable corner roundness
- Window and text scale to your screen / DPI and shrink to fit small displays
- Tiny: ~180 KB exe, no timers running while idle

## Build
- MinGW-w64: `gcc -std=gnu99 -O2 -s -static -mwindows -o CrosshairFree.exe main.c -lgdi32 -luser32 -lshell32 -ladvapi32 -lcomdlg32 -lm`
- MSVC: run `build.bat` from a Developer Command Prompt
- Cross-compile from Linux: `make CC=x86_64-w64-mingw32-gcc`

## Code map
- `xh.h` - layer model, SDF renderer, PNG image pool, game-code importers/exporters (no OS code)
- `main.c` - Win32 overlay, immediate-mode UI, library, tray, hotkeys, settings
- `stb_image.h` - public-domain PNG decoder by Sean Barrett

**Add another game:** write a function that parses its code into an `XHS` struct and register it in `xh_import_auto()`.

## Notes / known limits
- Works over windowed/borderless games. Exclusive fullscreen can hide overlays.
- It never reads or writes game memory, but some games restrict overlays - check the rules of yours.
- Settings and saves are stored next to the exe in `crosshair_free.cfg`.
- CS dynamic styles (spread/recoil) are static. CS2 pixel sizes are scaled from the code's reference
  height to your screen height. Legacy `CSGO-` sizes use an approximate scale (see `xh_from_cs`).
- Valorant export is best-effort (first two cross layers + a small dot) and not verified in-game.
- Not affiliated with Valve, Riot Games or the Crosshair X developers.

MIT licensed.
