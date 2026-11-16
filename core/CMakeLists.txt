cmake_minimum_required(VERSION 3.22)
project(kmodcore C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(KMOD_SDK_DIR "D:/KARDS/default/android/dump/out/CppSDK" CACHE PATH "Dumper CppSDK root")

option(KMOD_DEV "Developer build: Lua dev console, verbose logs" ON)
set(KMOD_LUA_DIR "D:/CCB-TEAM/RE-UE4SS/deps/first/LuaRaw" CACHE PATH "Lua 5.4 sources (from UE4SS deps)")

# Lua 5.4 (MIT) - all sources except the stand-alone interpreter and UE4SS' Windows lock shim
file(GLOB KMOD_LUA_SRC ${KMOD_LUA_DIR}/src/*.c)
list(FILTER KMOD_LUA_SRC EXCLUDE REGEX ".*/(lua|luac|luauser)\.c$")
add_library(kmodlua OBJECT ${KMOD_LUA_SRC})
target_include_directories(kmodlua PUBLIC ${KMOD_LUA_DIR}/include)
target_compile_definitions(kmodlua PRIVATE LUA_USE_POSIX)
target_compile_options(kmodlua PRIVATE -O2 -w)

# SDK objects (slow, built once)
add_library(kmodsdk OBJECT
  ${KMOD_SDK_DIR}/SDK/Basic.cpp
  ${KMOD_SDK_DIR}/SDK/CoreUObject_functions.cpp
  ${KMOD_SDK_DIR}/SDK/Engine_functions.cpp
  ${KMOD_SDK_DIR}/SDK/UMG_functions.cpp)
target_include_directories(kmodsdk PUBLIC ${KMOD_SDK_DIR} ${KMOD_SDK_DIR}/SDK)
target_compile_options(kmodsdk PRIVATE -O1 -w)

# Embed the built-in Lua scripts as C++ raw strings (kModLoaderLua, kModManagerLua, kCommandsLua)
foreach(NAME modloader modmanager commands json)
  file(READ ${CMAKE_CURRENT_SOURCE_DIR}/lua/${NAME}.lua KMOD_LUA_CONTENT)
  if(NAME STREQUAL "modloader")
    set(VAR "kModLoaderLua")
  elseif(NAME STREQUAL "json")
    set(VAR "kJsonLua")
  elseif(NAME STREQUAL "commands")
    set(VAR "kCommandsLua")
  else()
    set(VAR "kModManagerLua")
  endif()
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/gen/${NAME}_lua.h "static const char* ${VAR} = R\"KMODLUA(${KMOD_LUA_CONTENT})KMODLUA\";\n")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/lua/${NAME}.lua)
endforeach()

add_library(kmodcore SHARED src/core.cpp src/lua_host.cpp src/lua_worker.cpp src/resolver.cpp src/kmod_boot.cpp src/kmod_log.cpp $<TARGET_OBJECTS:kmodsdk> $<TARGET_OBJECTS:kmodlua>)
target_include_directories(kmodcore PRIVATE ${KMOD_SDK_DIR} ${KMOD_SDK_DIR}/SDK ${KMOD_LUA_DIR}/include ${CMAKE_CURRENT_BINARY_DIR}/gen)
if(KMOD_DEV)
  target_compile_definitions(kmodcore PRIVATE KMOD_DEV=1)
endif()
target_compile_options(kmodcore PRIVATE -O1 -w)
target_link_libraries(kmodcore log dl z)
target_link_options(kmodcore PRIVATE -Wl,-z,max-page-size=16384)
