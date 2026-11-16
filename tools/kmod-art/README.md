# kmod-art

A small command line tool that reads **UE patch paks** (card art mods), extracts their textures as PNG and tells you which
assets more than one pak replaces. It is the first piece of a "card art switcher": the paks are never mounted by the game,
so they cannot overwrite each other; kmod can load the PNG variants at runtime instead.

```
kmod-art info    <pak>... [--filter <substr>]      entry counts, encryption, compression
kmod-art list    <pak>... [--filter <substr>]      assets that match the filter
kmod-art extract <pak>... --out <dir> [--filter <substr>] [--limit N] [--usmap <file>] [--aes <hex>]
kmod-art probe   <pak> [--filter <substr>] [--limit N]    debugging: read raw entries one by one
kmod-art getoodle                                  download an Oodle library next to the exe (see below)
```

Default filter: `Textures/Images/` (the card art folder: `Textures/Images/<nation>/<set>/T_<card>.uasset`).

`extract` writes, for every pak:

```
<out>/<pak name>/Assets/Textures/Images/Germany/01_Base/T_annihilation.png
<out>/<pak name>/index.json          [{ asset, file, width, height, format }, ...]
<out>/conflicts.json                 asset path -> the paks that contain it   (when you pass 2+ paks)
```

## Formats

| Texture format | Status |
|---|---|
| `PF_ASTC_*` (Android cook) | decoded with astcenc |
| `PF_B8G8R8A8` (uncompressed) | written directly |
| `PF_DXT1/5`, `PF_BC*` (PC cook) | not implemented: skipped with a message |

Only mip 0 is read. Android mod paks usually have a single mip level.

## Build

Needs the .NET 8 (or newer) SDK. Three things are not in the repository and have to be put in place first:

1. `lib/`: CUE4Parse (`CUE4Parse.dll`) and the DLLs it depends on, built from the CUE4Parse sources (UE 5.6 support).
2. `vendor/astcenc/bin/`: the `astcenc-*.exe` files from https://github.com/ARM-software/astc-encoder/releases (tested with 5.7.1).
   The tool picks `avx2`, `sse4.1` or `sse2` to match the CPU.
3. A `.usmap` for the game (`--usmap`, or the `KMOD_USMAP` environment variable). The one dumped by MobileDumper-7 works.

```
dotnet build -c Release
bin\Release\net8.0\kmod-art.exe extract A.pak B.pak --out out
```

The built-in AES key is the game's own key (used only when a pak is encrypted; mod paks normally are not).

## Known problem: Oodle

Paks compressed with Oodle (`info` shows `compression Oodle`) need the Oodle library. Current Oodle builds use AVX2
instructions: on a CPU without AVX2 (for example a Xeon E5-2680 v2) the very first decompression dies with an illegal
instruction (exit code `0xC000001D`, shown by .NET as `SEHException`). Run the tool on a machine with AVX2, or use an
open source Oodle decoder. Uncompressed paks are not affected.
