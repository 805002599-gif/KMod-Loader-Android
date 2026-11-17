# Compiles tools/DexTool.java (the dex patcher kmod_inject.py runs) into tools/out. Needs a JDK and the smali dexlib2 jars
# (the ones in Android Studio's plugins\android\lib; set $env:ANDROID_STUDIO if it is not in D:\AndroidStudio).
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$studio = if ($env:ANDROID_STUDIO) { $env:ANDROID_STUDIO } else { 'D:\AndroidStudio' }
$javac = if ($env:JAVA_HOME) { "$env:JAVA_HOME\bin\javac.exe" } else { "$studio\jbr\bin\javac.exe" }
$cp = "$studio\plugins\android\lib\smali-dexlib2-3.0.9.jar;$studio\plugins\android\lib\smali-util-3.0.9.jar;$studio\plugins\android\lib\jsr305-2.0.1.jar;$studio\lib\*"
New-Item -ItemType Directory -Force "$here\out" | Out-Null
& $javac -cp $cp -d "$here\out" "$here\DexTool.java"
Write-Host "Done: $here\out"
