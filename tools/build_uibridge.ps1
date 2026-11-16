# Compiles overlay/kmodui/kmod/UiBridge.java to a dex and writes core/src/ui_bridge_dex.h (the core embeds it and loads it with a DexClassLoader).
$t = Split-Path -Parent $PSScriptRoot
$sdk = "$env:LOCALAPPDATA\Android\Sdk"; $bt = "$sdk\build-tools\36.0.0"; $jbr = 'D:\AndroidStudio\jbr\bin'
$out = "$t\overlay\kmodui\out"; Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue; New-Item -ItemType Directory $out | Out-Null
$env:JAVA_TOOL_OPTIONS = ''
& "$jbr\javac.exe" --release 11 -cp "$sdk\platforms\android-35\android.jar" -d $out "$t\overlay\kmodui\kmod\UiBridge.java" 2>&1 | Where-Object { $_ -notmatch 'warning|Picked up|^\s*$' }
$env:JAVA_HOME = 'D:\AndroidStudio\jbr'
$classes = Get-ChildItem $out -Recurse -Filter *.class | ForEach-Object FullName
& "$bt\d8.bat" --release --min-api 26 --lib "$sdk\platforms\android-35\android.jar" --output $out @classes 2>&1 | Where-Object { $_ -notmatch 'Picked up|^\s*$' }
python -I "$t\tools\dex_to_header.py" "$out\classes.dex" "$t\core\src\ui_bridge_dex.h"
