# Native (C++) mod examples

Each folder is a complete mod package. Build one with `build.ps1 <name>` (Windows) or `./build.sh <name>`; it needs the Android NDK and CMake. The library lands in `<name>/native/arm64-v8a/`. To try an example, copy `mod.json`, `main.lua` (if present) and `native/` to `kmod/mods/<id>/` on the phone and enable it in the Mods page. They are written against `core/include/kmod.h` (`CMakeLists.txt` finds it).

| Example | Shows |
|---|---|
| [`lua_functions`](lua_functions) | native functions called from Lua: strings, varargs, building a result table, raising errors |
| [`param_reader`](param_reader) | hooking a game function and reading its parameters by name, no game headers |
| [`float_button`](float_button) | a draggable floating button with its own Java class (embedded dex) through raw JNI: `jni_env`, `app_class_loader`, `activity`; click events forwarded from the UI thread to the game thread |

The loader only hands out the JNI handles; the floating button itself lives entirely in the mod. See `template/kmod-native-template` (minimal) and `template/kmod-native-panel-template` (a settings panel built with `kmod::ShowPanel`) for starting points.
