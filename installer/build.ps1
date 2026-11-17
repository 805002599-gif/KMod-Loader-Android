# Builds the kmod installer APK (installer\kmod-installer.apk). No Gradle: javac + d8 + aapt2 + apksigner, like the other tools here.
# Needs: JDK (javac), Android SDK build-tools + platform android-35, the smali dexlib2 jars (Android Studio plugins\android\lib),
# and the RELEASE loader libraries (stage-release\libkmod.so, libkmodcore.so: run ..\build.ps1 -Flavor release first).
$t = Split-Path -Parent $MyInvocation.MyCommand.Path
$sdk = "$env:LOCALAPPDATA\Android\Sdk"; $bt = "$sdk\build-tools\36.0.0"; $jar = "$sdk\platforms\android-35\android.jar"
$studio = if ($env:ANDROID_STUDIO) { $env:ANDROID_STUDIO } else { 'D:\AndroidStudio' }
$jbr = if ($env:JAVA_HOME) { "$env:JAVA_HOME\bin" } else { "$studio\jbr\bin" }
$libsDir = "$studio\plugins\android\lib"
$deps = @("$libsDir\smali-dexlib2-3.0.9.jar", "$libsDir\smali-util-3.0.9.jar", "$studio\lib\intellij.libraries.guava.jar", "$bt\lib\apksigner.jar")
$build = "$t\build"; Remove-Item $build -Recurse -Force -ErrorAction SilentlyContinue; New-Item -ItemType Directory "$build\classes", "$build\res", "$build\assets" | Out-Null
$env:JAVA_TOOL_OPTIONS = ''
Copy-Item "$t\..\stage-release\libkmod.so", "$t\..\stage-release\libkmodcore.so" "$build\assets"

& "$jbr\javac.exe" --release 8 -encoding UTF-8 -cp ($jar + ';' + ($deps -join ';')) -d "$build\classes" (Get-ChildItem "$t\src" -Recurse -Filter *.java | ForEach-Object FullName) 2>&1 | Where-Object { $_ -notmatch 'warning|Picked up|^\s*$|^Note:' }
$env:JAVA_HOME = Split-Path -Parent $jbr
& "$bt\d8.bat" --release --min-api 26 --lib $jar --output $build (@(Get-ChildItem "$build\classes" -Recurse -Filter *.class | ForEach-Object FullName) + $deps) 2>&1 | Where-Object { $_ -notmatch 'Picked up|^\s*$' }

& "$bt\aapt2.exe" compile --dir "$t\res" -o "$build\res.zip"
& "$bt\aapt2.exe" link -o "$build\base.apk" --manifest "$t\AndroidManifest.xml" -I $jar --min-sdk-version 26 --target-sdk-version 34 -A "$build\assets" "$build\res.zip"
python -I -c "import zipfile,sys; z=zipfile.ZipFile(sys.argv[1],'a'); z.write(sys.argv[2],'classes.dex'); z.close()" "$build\base.apk" "$build\classes.dex"
& "$bt\zipalign.exe" -p -f 4 "$build\base.apk" "$build\aligned.apk"
$ks = "$t\..\dev.keystore"
& "$bt\apksigner.bat" sign --ks $ks --ks-pass pass:android --key-pass pass:android --ks-key-alias dev --out "$t\kmod-installer.apk" "$build\aligned.apk" 2>&1 | Where-Object { $_ -notmatch 'WARNING' }
Get-Item "$t\kmod-installer.apk" | Select-Object Name, Length
