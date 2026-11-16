#!/bin/sh
# Builds the native mod for Android arm64 into native/arm64-v8a/. Needs the Android NDK ($ANDROID_NDK_HOME) and CMake.
here="$(cd "$(dirname "$0")" && pwd)"
cmake -S "$here" -B "$here/build" -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DCMAKE_BUILD_TYPE=Release && cmake --build "$here/build" -j && echo "Done: $here/native/arm64-v8a"
