@echo off
REM Needs these files next to each other: main.c xh.h stb_image.h app.rc CrosshairFree.ico ValveRg-Bold.ttf
REM ---- MinGW-w64 (gcc) ----
where gcc >nul 2>nul && (
  windres app.rc -O coff -o app.o
  gcc -std=gnu99 -O2 -s -static -mwindows -o CrosshairFree.exe main.c app.o -lgdi32 -luser32 -lshell32 -ladvapi32 -lcomdlg32 -lm
  echo Built with gcc: CrosshairFree.exe
  goto :eof
)
REM ---- MSVC (run from a "Developer Command Prompt") ----
where cl >nul 2>nul && (
  rc /nologo /fo app.res app.rc
  cl /nologo /O2 /Fe:CrosshairFree.exe main.c app.res user32.lib gdi32.lib shell32.lib advapi32.lib comdlg32.lib /link /SUBSYSTEM:WINDOWS
  echo Built with MSVC: CrosshairFree.exe
  goto :eof
)
echo No compiler found. Install MinGW-w64 (winlibs.com) or Visual Studio Build Tools.
