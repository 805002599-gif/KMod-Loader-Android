param([ValidateSet('dev','release')][string]$Flavor = 'dev')
# Builds loader + core for the chosen flavour, repacks the APK, aligns and signs it.
#   dev     -> kards_mod_dev.apk  (Lua console, lua.log, hot-load core from kmod/dev, dev override of the built-in manager)
#   release -> kards_mod.apk      (bundled core only)
$ErrorActionPreference = 'Continue'
$t = 'D:\KARDS\default\android'
$sdk = "$env:LOCALAPPDATA\Android\Sdk"; $bt = "$sdk\build-tools\36.0.0"
$ndk = "$sdk\ndk\28.2.13676358"; $cm = 'C:\Program Files\CMake\bin\cmake.exe'
$devOn = if ($Flavor -eq 'dev') { 'ON' } else { 'OFF' }
$suffix = if ($Flavor -eq 'dev') { 'build' } else { 'build-release' }
$out = if ($Flavor -eq 'dev') { "$t\kards_mod_dev.apk" } else { "$t\kards_mod.apk" }
$common = @('-G','Unix Makefiles',"-DCMAKE_MAKE_PROGRAM=$ndk\prebuilt\windows-x86_64\bin\make.exe","-DCMAKE_TOOLCHAIN_FILE=$ndk\build\cmake\android.toolchain.cmake",'-DANDROID_ABI=arm64-v8a','-DANDROID_PLATFORM=android-28','-DCMAKE_BUILD_TYPE=Release',"-DKMOD_DEV=$devOn")

foreach ($proj in 'loader','core') {
  & $cm -S "$t\$proj" -B "$t\$proj\$suffix" @common | Out-Null
  & $cm --build "$t\$proj\$suffix" -- -j6 | Select-Object -Last 1
}
$strip = "$ndk\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-strip.exe"
$coreOut = "$t\core\libkmodcore.$Flavor.so"; Copy-Item "$t\core\$suffix\libkmodcore.so" $coreOut -Force
& $strip --strip-all $coreOut
# libs are added under their real names, so stage copies with the right file names
$stage = "$t\stage-$Flavor"; Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue; New-Item -ItemType Directory $stage | Out-Null
Copy-Item "$t\loader\$suffix\libkmod.so" "$stage\libkmod.so"; Copy-Item $coreOut "$stage\libkmodcore.so"
$libs = @("$stage\libkmod.so", "$stage\libkmodcore.so") + (Get-ChildItem $t\loader\extra\*.so -ErrorAction SilentlyContinue | % FullName)

# SplashActivity.<clinit> -> System.loadLibrary("kmod"): regenerate the patched classes.dex from the original every time
$jbr = 'D:\AndroidStudio\jbr\bin'
$dexJars = 'D:\AndroidStudio\plugins\android\lib\smali-dexlib2-3.0.9.jar;D:\AndroidStudio\plugins\android\lib\smali-util-3.0.9.jar;D:\AndroidStudio\plugins\android\lib\jsr305-2.0.1.jar;D:\AndroidStudio\lib\*'
$env:JAVA_TOOL_OPTIONS = ''
& "$jbr\java.exe" -cp "$t\tools\out;$dexJars" DexTool patch "$t\x\classes.dex" "$t\classes_patched.dex" 'Lcom/epicgames/unreal/SplashActivity;' kmod 2>&1 | Where-Object { $_ -match 'patched|Exception' }

$extra = @()
if ($Flavor -eq 'dev') {
  $androidJar = "$sdk\platforms\android-35\android.jar"
  $jo = "$t\overlay\out"; Remove-Item $jo -Recurse -Force -ErrorAction SilentlyContinue; New-Item -ItemType Directory $jo | Out-Null
  $env:JAVA_TOOL_OPTIONS = ''
  & "$jbr\javac.exe" --release 11 -cp $androidJar -d $jo "$t\overlay\kmod\Overlay.java" 2>&1 | Where-Object { $_ -notmatch 'warning|Picked up|^\s*$' }
  $classes = Get-ChildItem $jo -Recurse -Filter *.class | ForEach-Object FullName
  $env:JAVA_HOME = 'D:\AndroidStudio\jbr'
  & "$bt\d8.bat" --release --lib $androidJar --output $jo @classes 2>&1 | Where-Object { $_ -notmatch 'Picked up|^\s*$' }
  Move-Item "$jo\classes.dex" "$jo\classes2.dex" -Force
  $extra = @('--classes2', "$jo\classes2.dex")
}
$un = "$t\kards_unaligned_$Flavor.apk"; $al = "$t\kards_aligned_$Flavor.apk"
python -I $t\tools\repack.py D:\KARDS\default\game\kards-1.apk $un $t\classes_patched.dex @libs @extra
Remove-Item $al, $out -ErrorAction SilentlyContinue
& "$bt\zipalign.exe" -p -f 4 $un $al 2>&1 | Out-Null
& "$bt\apksigner.bat" sign --ks $t\dev.keystore --ks-pass pass:android --key-pass pass:android --ks-key-alias dev --out $out $al 2>&1 | Out-Null
Remove-Item $un, $al -ErrorAction SilentlyContinue
Get-Item $out | Select-Object Name, Length
