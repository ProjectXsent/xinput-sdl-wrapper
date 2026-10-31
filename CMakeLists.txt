cmake_minimum_required(VERSION 3.15)
project(xinput_sdl_wrapper LANGUAGES C CXX)   # C is needed for MinHook

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# System DLLs to impersonate. All of them are built on every run; each needs
# src/def/<name>.def.
set(XINPUT_DLL_NAMES xinput1_3 xinput1_4 xinput9_1_0)

option(XINPUT_STATIC_SDL "Link SDL3 statically (no SDL3.dll needed)" OFF)
option(XINPUT_BUILD_TEST "Build the xinput_test command line tool" ON)

if(NOT WIN32)
  message(FATAL_ERROR "This project targets Windows (MSVC or MinGW-w64).")
endif()

# Static CRT so the wrapper has no VC++ redistributable dependency.
# Must be set before SDL3 is added as a subdirectory so both use the same CRT.
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ---- third party (ThirdParty/SDL3, ThirdParty/MinHook) -----------------------
set(THIRDPARTY_DIR "${CMAKE_SOURCE_DIR}/ThirdParty" CACHE PATH "Folder that contains SDL3 and MinHook")
set(SDL3_ROOT      "${THIRDPARTY_DIR}/SDL3"         CACHE PATH "SDL3 source tree or prebuilt dev package")
set(MINHOOK_ROOT   "${THIRDPARTY_DIR}/MinHook"      CACHE PATH "MinHook source tree (needs include/MinHook.h and src/)")

# SDL3: either a source checkout (has CMakeLists.txt) or a prebuilt dev package.
if(EXISTS "${SDL3_ROOT}/CMakeLists.txt")
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  set(SDL_TESTS        OFF CACHE BOOL "" FORCE)
  set(SDL_EXAMPLES     OFF CACHE BOOL "" FORCE)
  set(SDL_INSTALL      OFF CACHE BOOL "" FORCE)
  if(XINPUT_STATIC_SDL)
    set(SDL_STATIC ON  CACHE BOOL "" FORCE)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  else()
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    set(SDL_SHARED ON  CACHE BOOL "" FORCE)
  endif()
  add_subdirectory("${SDL3_ROOT}" "${CMAKE_BINARY_DIR}/SDL3" EXCLUDE_FROM_ALL)
else()
  list(APPEND CMAKE_PREFIX_PATH
       "${SDL3_ROOT}"
       "${SDL3_ROOT}/x86_64-w64-mingw32"
       "${SDL3_ROOT}/i686-w64-mingw32")
  find_package(SDL3 CONFIG REQUIRED)
endif()

if(XINPUT_STATIC_SDL)
  if(NOT TARGET SDL3::SDL3-static)
    message(FATAL_ERROR
      "XINPUT_STATIC_SDL=ON but no static SDL3 was found. The official SDL3 "
      "prebuilt VC/MinGW packages only ship the DLL + import lib. Put the SDL3 "
      "SOURCE tree in ${SDL3_ROOT} (it must contain CMakeLists.txt) so it can "
      "be built statically, or configure with -DXINPUT_STATIC_SDL=OFF.")
  endif()
  set(XINPUT_SDL_TARGET SDL3::SDL3-static)
elseif(TARGET SDL3::SDL3)
  set(XINPUT_SDL_TARGET SDL3::SDL3)
elseif(TARGET SDL3::SDL3-static)
  set(XINPUT_SDL_TARGET SDL3::SDL3-static)
else()
  message(FATAL_ERROR "No SDL3 target found in ${SDL3_ROOT}")
endif()

# MinHook: compiled straight from its sources (no dependency on its CMake file).
if(NOT EXISTS "${MINHOOK_ROOT}/include/MinHook.h")
  message(FATAL_ERROR "MinHook not found at ${MINHOOK_ROOT} (expected include/MinHook.h)")
endif()
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(MINHOOK_HDE "${MINHOOK_ROOT}/src/hde/hde64.c")
else()
  set(MINHOOK_HDE "${MINHOOK_ROOT}/src/hde/hde32.c")
endif()
add_library(minhook STATIC
  "${MINHOOK_ROOT}/src/buffer.c"
  "${MINHOOK_ROOT}/src/hook.c"
  "${MINHOOK_ROOT}/src/trampoline.c"
  "${MINHOOK_HDE}")
target_include_directories(minhook PUBLIC "${MINHOOK_ROOT}/include")
target_compile_definitions(minhook PRIVATE WIN32_LEAN_AND_MEAN)

# ---- wrapper DLL ----------------------------------------------------------------
set(XINPUT_TARGETS "")
foreach(dll_name IN LISTS XINPUT_DLL_NAMES)
  set(def_file "${CMAKE_SOURCE_DIR}/src/def/${dll_name}.def")
  if(NOT EXISTS "${def_file}")
    message(FATAL_ERROR "No export definition for '${dll_name}' (${def_file})")
  endif()

  set(tgt "xinput_wrapper_${dll_name}")
  add_library(${tgt} SHARED
    src/xinput_sdl.cpp
    src/log.cpp
    src/log.h
    src/dinput_hide.cpp
    src/dinput_hide.h
    src/xinput_types.h
    "${def_file}")
  set_target_properties(${tgt} PROPERTIES
    OUTPUT_NAME "${dll_name}"
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  )
  target_include_directories(${tgt} PRIVATE src)
  target_compile_definitions(${tgt} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(${tgt} PRIVATE ${XINPUT_SDL_TARGET} minhook)

  if(MINGW)
    target_link_options(${tgt} PRIVATE -static-libgcc -static-libstdc++ -static)
  endif()

  # Copy ini next to the DLL (and SDL3.dll when SDL is linked dynamically)
  add_custom_command(TARGET ${tgt} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/xinput_sdl.ini" "$<TARGET_FILE_DIR:${tgt}>/xinput_sdl.ini")

  if(NOT XINPUT_SDL_TARGET STREQUAL "SDL3::SDL3-static")
    add_custom_command(TARGET ${tgt} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "$<TARGET_FILE:${XINPUT_SDL_TARGET}>" "$<TARGET_FILE_DIR:${tgt}>")
  endif()

  list(APPEND XINPUT_TARGETS ${tgt})
endforeach()

# Convenience target: cmake --build build --target xinput_all
add_custom_target(xinput_all DEPENDS ${XINPUT_TARGETS})

if(XINPUT_BUILD_TEST)
  add_executable(xinput_test tools/xinput_test.cpp src/xinput_types.h)
  target_include_directories(xinput_test PRIVATE src)
  target_compile_definitions(xinput_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  set_target_properties(xinput_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()