# kmod native panel template

A native mod with a settings panel built from C++ (Android controls: switch, slider, button, ...). See `src/main.cpp`.

1. Edit `mod.json` (`id` must equal the folder name).
2. Build with `build.ps1` / `build.sh` (needs the Android NDK and CMake); the library lands in `native/arm64-v8a/`.
3. Copy the folder to `kmod/mods/<id>/` on the phone and enable it in the Mods page.
