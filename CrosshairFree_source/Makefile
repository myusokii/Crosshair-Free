# Linux/macOS cross-compile:  make CC=x86_64-w64-mingw32-gcc WINDRES=x86_64-w64-mingw32-windres
# (On Windows with MinGW-w64 just run build.bat)
CC ?= gcc
WINDRES ?= windres

CrosshairFree.exe: main.c xh.h stb_image.h app.o
	$(CC) -std=gnu99 -O2 -s -static -mwindows -o $@ main.c app.o -lgdi32 -luser32 -lshell32 -ladvapi32 -lcomdlg32 -lm

# icon + embedded font + version info
app.o: app.rc CrosshairFree.ico ValveRg-Bold.ttf
	$(WINDRES) app.rc -O coff -o $@

clean:
	rm -f app.o CrosshairFree.exe
