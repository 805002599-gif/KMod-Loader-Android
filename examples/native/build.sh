#!/bin/sh
# Builds one native example for Android arm64:  ./build.sh float_button   (needs $ANDROID_NDK_HOME and CMake)
here="$(cd "$(dirname "$0")" && pwd)/$1"
cmake -S "$here" -B "$here/build" -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DCMAKE_BUILD_TYPE=Release && cmake --build "$here/build" -j && echo "Done: $here/native/arm64-v8a"
