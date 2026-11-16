# Compiles java/kmodex/FloatButton.java and writes src/float_button_dex.h (the C++ side embeds the dex and loads it with a DexClassLoader).
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$sdk = "$env:LOCALAPPDATA\Android\Sdk"; $bt = "$sdk\build-tools\36.0.0"; $jbr = if ($env:JAVA_HOME) { "$env:JAVA_HOME\bin" } else { 'D:\AndroidStudio\jbr\bin' }
$jar = "$sdk\platforms\android-35\android.jar"; $out = "$here\build\dex"
Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue; New-Item -ItemType Directory $out | Out-Null
& "$jbr\javac.exe" --release 11 -cp $jar -d $out "$here\java\kmodex\FloatButton.java"
$env:JAVA_HOME = Split-Path -Parent $jbr
& "$bt\d8.bat" --release --min-api 26 --lib $jar --output $out (Get-ChildItem $out -Recurse -Filter *.class | ForEach-Object FullName)
python -I "$here\dex_to_header.py" "$out\classes.dex" "$here\src\float_button_dex.h"
