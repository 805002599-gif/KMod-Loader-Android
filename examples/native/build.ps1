# Builds one native example (or any native mod folder with a CMakeLists.txt) for Android arm64:  .\build.ps1 float_button
# Needs the Android NDK (ANDROID_NDK_HOME, or the default Android Studio location) and CMake. The library lands in <example>\native\arm64-v8a\.
param([Parameter(Mandatory = $true)][string]$Name)
$ndk = if ($env:ANDROID_NDK_HOME) { $env:ANDROID_NDK_HOME } else { "$env:LOCALAPPDATA\Android\Sdk\ndk\28.2.13676358" }
$here = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) $Name
$cm = if (Get-Command cmake -ErrorAction SilentlyContinue) { 'cmake' } else { 'C:\Program Files\CMake\bin\cmake.exe' }
$make = "$ndk\prebuilt\windows-x86_64\bin\make.exe"
& $cm -S $here -B "$here\build" -G 'Unix Makefiles' "-DCMAKE_MAKE_PROGRAM=$make" `
  "-DCMAKE_TOOLCHAIN_FILE=$ndk\build\cmake\android.toolchain.cmake" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DCMAKE_BUILD_TYPE=Release
& $cm --build "$here\build" -- -j6
Write-Host "Done: $here\native\arm64-v8a"
