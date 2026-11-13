# Lua 5.4.7

Sources of [Lua 5.4.7](https://www.lua.org) (from the official
`lua/lua` repository, tag `v5.4.7`), without the stand-alone interpreter, the
compiler, the test library and the `io`, `package` and `debug` libraries, which
the script runtime does not offer. The only change is in `src/luaconf.h`, which
also includes `<climits>` when compiled as C++: the ESP32 toolchain (GCC 8)
defines `LLONG_MAX` only there in C++ mode, and without it Lua stops with
"Compiler does not support 'long long'".

`src/lua_all.cpp` builds all of it as a single C++ translation unit; that is the
only file compiled (see `library.json`). Lua's licence (MIT) is at the end of
`src/lua.h`.

To update: replace the `.c` / `.h` files with those of a newer 5.4 release and
make the same change in `luaconf.h`.
