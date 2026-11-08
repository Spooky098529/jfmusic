# Cross-compiles jfmusic.exe (64-bit, Windows 7+) on Linux with clang + lld-link,
# using the MSVC CRT and Windows SDK downloaded by xwin (see README).
#
# The CRT is linked statically (/MT) because Windows 7 has no Universal CRT,
# and the PE is marked subsystem 6.01. `make check` lists the imports so
# nothing newer than Windows 7 sneaks in.
#
# Set WINSYSROOT to your xwin splat directory, on the command line or in a
# (git-ignored) config.mk:   WINSYSROOT = /path/to/xwin-sdk

-include config.mk
WINSYSROOT ?= xwin-sdk

SDKVER  = $(shell ls "$(WINSYSROOT)/Windows Kits/10/Include" 2>/dev/null | tail -n 1)
SDKINC  = $(WINSYSROOT)/Windows Kits/10/Include/$(SDKVER)

CC      = clang
CFLAGS  = --target=x86_64-pc-windows-msvc -Xmicrosoft-windows-sys-root $(WINSYSROOT) \
          -fms-runtime-lib=static -O2 -Wall -Wextra \
          -D_WIN32_WINNT=0x0601 -DWINVER=0x0601 -DUNICODE -D_UNICODE -D_CRT_SECURE_NO_WARNINGS
LDFLAGS = -fuse-ld=lld --target=x86_64-pc-windows-msvc -Xmicrosoft-windows-sys-root $(WINSYSROOT) \
          -fms-runtime-lib=static -Xlinker /subsystem:windows,6.01 -Wl,/opt:ref
LIBS    = -lwinhttp -lcomctl32 -luxtheme -luser32 -lgdi32 -lshell32 -lkernel32
RC      = llvm-rc

OBJS    = build/main.o build/api.o build/json.o build/player.o build/ma_impl.o build/app.res

jfmusic.exe: sdkcheck $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

build/%.o: src/%.c src/jf.h src/ma_cfg.h | build
	$(CC) $(CFLAGS) -c -o $@ $<

# miniaudio is third-party; don't drown our warnings in its.
build/ma_impl.o: src/ma_impl.c src/ma_cfg.h vendor/miniaudio.h | build
	$(CC) $(CFLAGS) -w -c -o $@ $<

build/app.res: src/app.rc src/resource.h src/app.manifest src/app.ico | build
	$(RC) /nologo /I src /I "$(SDKINC)/um" /I "$(SDKINC)/shared" /FO $@ src/app.rc

build:
	mkdir -p build

sdkcheck:
	@test -d "$(WINSYSROOT)/VC" || { echo "WINSYSROOT=$(WINSYSROOT) is not an xwin splat (see README)"; exit 1; }

check: jfmusic.exe
	llvm-readobj --coff-imports jfmusic.exe | grep -E 'Name:|Symbol:'

clean:
	rm -rf build jfmusic.exe jfmusic.pdb

.PHONY: check clean sdkcheck
