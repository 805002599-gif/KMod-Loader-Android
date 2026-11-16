// kmod-art: reads UE patch paks (card art mods), extracts their textures as PNG and reports which assets several paks replace.
//
//   kmod-art list    <pak>... [--filter <substr>]
//   kmod-art extract <pak>... --out <dir> [--filter <substr>] [--limit N] [--usmap <file>] [--aes <hex>]
//
// Output of `extract`: <out>/<pak name>/<path under kards/Content>.png  and  <out>/<pak name>/index.json
// Only ASTC textures (the Android cook) are decoded, with astcenc (see vendor/astcenc). The pak is only read, never mounted by the game.
using System.Diagnostics;
using System.Text.Json;
using System.Text.RegularExpressions;
using CUE4Parse.Compression;
using CUE4Parse.Encryption.Aes;
using CUE4Parse.FileProvider;
using CUE4Parse.MappingsProvider;
using CUE4Parse.UE4.Assets.Exports.Texture;
using CUE4Parse.UE4.Objects.Core.Misc;
using CUE4Parse.UE4.Versions;

namespace KmodArt;

record Entry(string Asset, string File, int Width, int Height, string Format);

static class Program
{
    const string DefaultFilter = "Textures/Images/";
    // known key of the game's own paks; patch paks are normally not encrypted and then it is not used
    const string DefaultAes = "0xC257932734957B6D467FA16FCF728D5A16A5BDD07F6E20EE6E3155E898305CC0";

    static int Main(string[] args)
    {
        if (args.Length == 0 || args[0] is "-h" or "--help") { Usage(); return 1; }
        string cmd = args[0];
        if (cmd == "getoodle")
        {
            // downloads a current Oodle library with CUE4Parse's own downloader (WorkingRobot/OodleUE on GitHub) into <exe dir>/oodle/
            string dir = Path.Combine(AppContext.BaseDirectory, "oodle");
            Directory.CreateDirectory(dir);
            string target = Path.Combine(dir, OodleHelper.OODLE_NAME_CURRENT);
            bool got = OodleHelper.DownloadOodleDll(ref target);
            Console.WriteLine(got ? $"downloaded {target} ({new FileInfo(target).Length} bytes)" : "download failed");
            return got ? 0 : 1;
        }
        var paks = new List<string>();
        string outDir = null, filter = DefaultFilter, usmap = null, aes = DefaultAes;
        int limit = int.MaxValue;
        for (int i = 1; i < args.Length; i++)
        {
            switch (args[i])
            {
                case "--out": outDir = args[++i]; break;
                case "--filter": filter = args[++i]; break;
                case "--limit": limit = int.Parse(args[++i]); break;
                case "--usmap": usmap = args[++i]; break;
                case "--aes": aes = args[++i]; break;
                default:
                    if (args[i].StartsWith("--")) { Console.Error.WriteLine("unknown option " + args[i]); return 1; }
                    paks.Add(args[i]); break;
            }
        }
        if (paks.Count == 0 || (cmd == "extract" && outDir == null)) { Usage(); return 1; }
        if (cmd != "list" && cmd != "extract" && cmd != "info" && cmd != "probe") { Usage(); return 1; }

        try { ZlibHelper.Initialize(); } catch { }
        try
        {
            string own = Path.Combine(AppContext.BaseDirectory, "oodle", OodleHelper.OODLE_NAME_CURRENT);       // from `kmod-art getoodle`
            if (File.Exists(own)) OodleHelper.Initialize(own); else OodleHelper.Initialize();
        }
        catch (Exception e) { Console.Error.WriteLine("warning: Oodle not available: " + e.Message); }

        usmap ??= FindUsmap();
        var perPak = new Dictionary<string, List<Entry>>();
        foreach (var pak in paks)
        {
            string name = Path.GetFileNameWithoutExtension(pak);
            Console.WriteLine($"== {name}");
            var provider = Mount(pak, usmap, aes, out int files);
            var keys = provider.Files.Keys
                .Where(k => k.EndsWith(".uasset", StringComparison.OrdinalIgnoreCase) && filter.Split('|').Any(f => k.Contains(f, StringComparison.OrdinalIgnoreCase)))
                .OrderBy(k => k, StringComparer.OrdinalIgnoreCase).Take(limit).ToList();
            Console.WriteLine($"   {files} files in the pak, {keys.Count} asset(s) match '{filter}'");

            var entries = new List<Entry>();
            if (cmd == "probe")
            {
                // reads raw entries one by one (a bad Oodle call can kill the process: the last line printed is the culprit)
                int n = 0;
                foreach (var e in provider.Files.Values.OfType<CUE4Parse.UE4.VirtualFileSystem.VfsEntry>().Where(e => e.Path.Contains(filter, StringComparison.OrdinalIgnoreCase)).Take(limit == int.MaxValue ? 40 : limit))
                {
                    Console.Write($"   [{n++}] {e.Path} size={e.Size} comp={e.CompressionMethod} offset={e.Offset} ... "); Console.Out.Flush();
                    if (n == 1)
                    {
                        using var fs = File.OpenRead(pak); fs.Seek(e.Offset, SeekOrigin.Begin);
                        var buf = new byte[96]; fs.ReadExactly(buf);
                        Console.WriteLine();
                        Console.WriteLine("      raw @offset: " + BitConverter.ToString(buf).Replace("-", " "));
                        Console.Out.Flush();
                    }
                    var bytes = provider.SaveAsset(e.Path);
                    Console.WriteLine($"ok {bytes.Length} bytes");
                }
                continue;
            }
            if (cmd == "info")
            {
                var all = provider.Files.Values.OfType<CUE4Parse.UE4.VirtualFileSystem.VfsEntry>().ToList();
                Console.WriteLine($"   entries: {all.Count}, encrypted: {all.Count(e => e.IsEncrypted)}");
                foreach (var g in all.GroupBy(e => e.CompressionMethod.ToString())) Console.WriteLine($"   compression {g.Key}: {g.Count()}");
                var first = all.FirstOrDefault(e => e.Path.Contains(filter, StringComparison.OrdinalIgnoreCase) && e.Path.EndsWith(".uasset", StringComparison.OrdinalIgnoreCase));
                if (first != null) Console.WriteLine($"   sample: {first.Path} size={first.Size} enc={first.IsEncrypted} comp={first.CompressionMethod}");
                continue;
            }
            if (cmd == "list")
            {
                foreach (var k in keys) entries.Add(new Entry(AssetPath(k), null, 0, 0, null));
            }
            else
            {
                string dest = Path.Combine(outDir, name);
                Directory.CreateDirectory(dest);
                int ok = 0, skipped = 0;
                var tasks = new List<(string key, string astc, string png, int w, int h, string fmt, bool srgb)>();
                foreach (var k in keys)
                {
                    try
                    {
                        var info = ReadTexture(provider, k, dest);
                        if (info == null) { skipped++; continue; }
                        tasks.Add(info.Value);
                    }
                    catch (Exception e) { skipped++; Console.WriteLine($"   skip {k}: {e.GetType().Name}: {FirstLine(e.Message)}"); }
                }
                // decode in parallel (astcenc is fast, but there can be hundreds of files)
                Parallel.ForEach(tasks, new ParallelOptions { MaxDegreeOfParallelism = 4 }, t =>
                {
                    bool good = t.astc == null || RunAstc(t.astc, t.png, t.srgb);
                    if (t.astc != null) try { File.Delete(t.astc); } catch { }
                    lock (entries)
                    {
                        if (good) { entries.Add(new Entry(AssetPath(t.key), Path.GetRelativePath(dest, t.png).Replace('\\', '/'), t.w, t.h, t.fmt)); ok++; }
                        else { skipped++; Console.WriteLine($"   astcenc failed for {t.key}"); }
                    }
                });
                entries.Sort((a, b) => string.Compare(a.Asset, b.Asset, StringComparison.OrdinalIgnoreCase));
                File.WriteAllText(Path.Combine(dest, "index.json"), JsonSerializer.Serialize(entries, new JsonSerializerOptions { WriteIndented = true }));
                Console.WriteLine($"   decoded {ok}, skipped {skipped} -> {dest}");
            }
            perPak[name] = entries;
        }

        if (perPak.Count > 1)
        {
            var count = new Dictionary<string, List<string>>(StringComparer.OrdinalIgnoreCase);
            foreach (var (name, list) in perPak) foreach (var e in list) { if (!count.TryGetValue(e.Asset, out var l)) count[e.Asset] = l = new(); l.Add(name); }
            var shared = count.Where(kv => kv.Value.Count > 1).OrderBy(kv => kv.Key, StringComparer.OrdinalIgnoreCase).ToList();
            Console.WriteLine($"== assets that more than one pak replaces: {shared.Count}");
            foreach (var kv in shared.Take(10)) Console.WriteLine($"   {kv.Key}   [{string.Join(", ", kv.Value)}]");
            if (shared.Count > 10) Console.WriteLine($"   ... and {shared.Count - 10} more");
            if (outDir != null)
                File.WriteAllText(Path.Combine(outDir, "conflicts.json"), JsonSerializer.Serialize(shared.ToDictionary(kv => kv.Key, kv => kv.Value), new JsonSerializerOptions { WriteIndented = true }));
        }
        return 0;
    }

    static void Usage() => Console.WriteLine(
        "kmod-art list    <pak>... [--filter <substr>]\n" +
        "kmod-art extract <pak>... --out <dir> [--filter <substr>] [--limit N] [--usmap <file>] [--aes <hex>]\n" +
        $"default filter: {DefaultFilter}");

    static string FindUsmap()
    {
        string[] candidates = {
            Environment.GetEnvironmentVariable("KMOD_USMAP"),
            @"D:\KARDS\default\android\dump\out\Mappings\com.android1939.kardsapk-v1.61.27596.usmap",
        };
        return candidates.FirstOrDefault(c => !string.IsNullOrEmpty(c) && File.Exists(c));
    }

    static DefaultFileProvider Mount(string pak, string usmap, string aes, out int fileCount)
    {
        var tmp = Directory.CreateTempSubdirectory("kmod-art-").FullName;      // empty folder: the pak is registered by hand, alone
        var provider = new DefaultFileProvider(tmp, SearchOption.TopDirectoryOnly, new VersionContainer(EGame.GAME_UE5_6), StringComparer.OrdinalIgnoreCase);
        provider.ReadScriptData = true;
        if (usmap != null) provider.MappingsContainer = new FileUsmapTypeMappingsProvider(usmap);
        provider.Initialize();
        provider.RegisterVfs(pak);
        provider.SubmitKey(new FGuid(), new FAesKey(aes));
        provider.PostMount();
        fileCount = provider.Files.Count;
        return provider;
    }

    static string AssetPath(string key)
    {
        // "kards/Content/Assets/Textures/Images/X.uasset" -> "/Game/Assets/Textures/Images/X"
        string p = key.Replace('\\', '/');
        if (p.StartsWith("kards/Content/", StringComparison.OrdinalIgnoreCase)) p = "/Game/" + p["kards/Content/".Length..];
        return Path.ChangeExtension(p, null).Replace('\\', '/');
    }

    static string FirstLine(string s) => s.Split('\n')[0].Trim();

    // reads the first mip of the first texture in the asset and writes an .astc file next to the future png
    static (string key, string astc, string png, int w, int h, string fmt, bool srgb)? ReadTexture(DefaultFileProvider provider, string key, string dest)
    {
        var file = provider.Files[key];
        var pkg = provider.LoadPackage(file);
        for (int i = 0; i < pkg.ExportMapLength; i++)
        {
            if (pkg.GetExport(i) is not UTexture2D tex) continue;
            string fmt = tex.Format.ToString();
            var mip = tex.GetFirstMip();
            if (mip == null) return null;
            int w = mip.SizeX, h = mip.SizeY;
            byte[] data = mip.BulkData.Data;
            string rel = Path.ChangeExtension(key.StartsWith("kards/Content/", StringComparison.OrdinalIgnoreCase) ? key["kards/Content/".Length..] : key, null);
            string png = Path.Combine(dest, rel.Replace('/', Path.DirectorySeparatorChar) + ".png");
            Directory.CreateDirectory(Path.GetDirectoryName(png));

            if (fmt == "PF_B8G8R8A8")                                   // uncompressed: write the PNG directly, no astcenc needed
            {
                if (data == null || data.Length < (long)w * h * 4) { Console.WriteLine($"   skip {key}: mip data too short"); return null; }
                WritePngBgra(png, w, h, data);
                return (key, null, png, w, h, fmt, tex.SRGB);
            }

            var m = Regex.Match(fmt, @"PF_ASTC_(\d+)x(\d+)");
            if (!m.Success) { Console.WriteLine($"   skip {key}: unsupported format {fmt}"); return null; }
            byte bx = byte.Parse(m.Groups[1].Value), by = byte.Parse(m.Groups[2].Value);
            long expect = (long)((w + bx - 1) / bx) * ((h + by - 1) / by) * 16;
            if (data == null || data.Length < expect) { Console.WriteLine($"   skip {key}: mip data {data?.Length ?? 0} bytes, expected {expect}"); return null; }
            string astc = png + ".astc";
            using (var f = File.Create(astc))
            {
                f.Write(new byte[] { 0x13, 0xAB, 0xA1, 0x5C, bx, by, 1 });                      // magic, block size
                f.Write(new[] { (byte)w, (byte)(w >> 8), (byte)(w >> 16), (byte)h, (byte)(h >> 8), (byte)(h >> 16), (byte)1, (byte)0, (byte)0 });
                f.Write(data, 0, (int)expect);
            }
            return (key, astc, png, w, h, fmt, tex.SRGB);
        }
        return null;
    }

    static void WritePngBgra(string path, int w, int h, byte[] bgra)
    {
        var raw = new byte[h * (w * 4 + 1)];
        for (int y = 0; y < h; y++)
        {
            int o = y * (w * 4 + 1);
            raw[o] = 0;                                                  // filter: none
            for (int x = 0; x < w; x++)
            {
                int si = (y * w + x) * 4, di = o + 1 + x * 4;
                raw[di] = bgra[si + 2]; raw[di + 1] = bgra[si + 1]; raw[di + 2] = bgra[si]; raw[di + 3] = bgra[si + 3];
            }
        }
        using var ms = new MemoryStream();
        using (var z = new System.IO.Compression.ZLibStream(ms, System.IO.Compression.CompressionLevel.Optimal, true)) z.Write(raw);
        using var f = File.Create(path);
        f.Write(new byte[] { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A });
        var ihdr = new byte[13];
        System.Buffers.Binary.BinaryPrimitives.WriteInt32BigEndian(ihdr, w); System.Buffers.Binary.BinaryPrimitives.WriteInt32BigEndian(ihdr.AsSpan(4), h);
        ihdr[8] = 8; ihdr[9] = 6;                                         // 8 bit, RGBA
        PngChunk(f, "IHDR", ihdr); PngChunk(f, "IDAT", ms.ToArray()); PngChunk(f, "IEND", Array.Empty<byte>());
    }

    static readonly uint[] CrcTable = Enumerable.Range(0, 256).Select(n => { uint c = (uint)n; for (int k = 0; k < 8; k++) c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1; return c; }).ToArray();
    static uint Crc32(byte[] a, byte[] b)
    {
        uint c = 0xFFFFFFFFu;
        foreach (var x in a) c = CrcTable[(c ^ x) & 0xFF] ^ (c >> 8);
        foreach (var x in b) c = CrcTable[(c ^ x) & 0xFF] ^ (c >> 8);
        return c ^ 0xFFFFFFFFu;
    }

    static void PngChunk(Stream f, string type, byte[] data)
    {
        var len = new byte[4]; System.Buffers.Binary.BinaryPrimitives.WriteInt32BigEndian(len, data.Length); f.Write(len);
        var t = System.Text.Encoding.ASCII.GetBytes(type); f.Write(t); f.Write(data);
        uint crc = Crc32(t, data);
        var c = new byte[4]; System.Buffers.Binary.BinaryPrimitives.WriteUInt32BigEndian(c, crc); f.Write(c);
    }

    static bool RunAstc(string astc, string png, bool srgb)
    {
        string variant = System.Runtime.Intrinsics.X86.Avx2.IsSupported ? "avx2" : System.Runtime.Intrinsics.X86.Sse41.IsSupported ? "sse4.1" : "sse2";
        string exe = Path.Combine(AppContext.BaseDirectory, "astcenc", $"astcenc-{variant}.exe");
        var psi = new ProcessStartInfo(exe) { UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true, CreateNoWindow = true };
        psi.ArgumentList.Add(srgb ? "-ds" : "-dl");
        psi.ArgumentList.Add(astc);
        psi.ArgumentList.Add(png);
        using var p = Process.Start(psi);
        string so = p.StandardOutput.ReadToEnd(), se = p.StandardError.ReadToEnd();
        p.WaitForExit(60000);
        bool ok = p.ExitCode == 0 && File.Exists(png);
        if (!ok) Console.WriteLine($"   astcenc exit {p.ExitCode}: {(se + so).Trim().Replace('\n', ' ')}");
        return ok;
    }
}
