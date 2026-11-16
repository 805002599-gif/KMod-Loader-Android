# kmod native mod template

1. Edit `mod.json` (`id` must equal the folder name; `native` is the library file name).
2. Write the mod in `src/main.cpp`. Everything comes from `include/kmod.h`; no game headers are needed.
3. Build: `build.ps1` (Windows) or `build.sh`. The library lands in `native/arm64-v8a/`.
4. Copy the whole folder to `kmod/mods/<id>/` on the phone (or zip it into a `.kmod`) and enable it in the Mods page.

Code completion: after the first build, `build/compile_commands.json` exists and clangd (VS Code, CLion, ...) picks it up.
A native mod runs inside the game process: a crash takes the game down. kmod notices and keeps the mod switched off on the next start.
