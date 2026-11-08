# jfmusic

A Jellyfin music player for Windows 7 and later.

## Building

The exe is cross-compiled on Linux with clang, lld-link and llvm-rc against Microsoft's CRT
and Windows SDK, which you download with [xwin](https://github.com/Jake-Shadle/xwin).
Running xwin accepts Microsoft's license.

```sh
xwin --accept-license --arch x86_64 splat --use-winsysroot-style --preserve-ms-arch-notation --output xwin-sdk
make                          # or: make WINSYSROOT=/path/to/xwin-sdk
make check                    # list imports
```

You can put `WINSYSROOT = /path/to/xwin-sdk` in a `config.mk` (git-ignored) instead of
passing it each time. `make check` lists the exe's imports so you can confirm they all exist
on Windows 7.
