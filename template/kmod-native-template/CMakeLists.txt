cmake_minimum_required(VERSION 3.18)
project(my_native_mod CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)           # compile_commands.json for clangd / VS Code / CLion code completion

add_library(my_native_mod SHARED src/main.cpp)
target_include_directories(my_native_mod PRIVATE include)
target_compile_options(my_native_mod PRIVATE -fvisibility=hidden -O2)
target_link_options(my_native_mod PRIVATE -static-libstdc++)      # the game has no libc++_shared.so for you
set_target_properties(my_native_mod PROPERTIES OUTPUT_NAME my_native_mod PREFIX "lib")
# the package layout the loader expects
set_target_properties(my_native_mod PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/native/arm64-v8a)
