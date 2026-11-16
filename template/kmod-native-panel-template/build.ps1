# Builds the native mod for Android arm64 into native/arm64-v8a/. Needs the Android NDK (ANDROID_NDK_HOME, or the Android Studio default) and CMake.
$ndk = if ($env:ANDROID_NDK_HOME) { $env:ANDROID_NDK_HOME } else { "$env:LOCALAPPDATA\Android\Sdk\ndk\28.2.13676358" }
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$cm = if (Get-Command cmake -ErrorAction SilentlyContinue) { 'cmake' } else { 'C:\Program Files\CMake\bin\cmake.exe' }
& $cm -S $here -B "$here\build" -G 'Unix Makefiles' "-DCMAKE_MAKE_PROGRAM=$ndk\prebuilt\windows-x86_64\bin\make.exe" `
  "-DCMAKE_TOOLCHAIN_FILE=$ndk\build\cmake\android.toolchain.cmake" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DCMAKE_BUILD_TYPE=Release
& $cm --build "$here\build" -- -j6
Write-Host "Done: $here\native\arm64-v8a"
