cmake_minimum_required(VERSION 3.15)
project(xinput_sdl_wrapper LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Which system DLL to impersonate: xinput1_3 | xinput1_4 | xinput9_1_0
set(XINPUT_DLL_NAME "xinput1_3" CACHE STRING "Name of the XInput DLL to generate")
set_property(CACHE XINPUT_DLL_NAME PROPERTY STRINGS xinput1_3 xinput1_4 xinput9_1_0)

option(XINPUT_STATIC_SDL "Link SDL3 statically (no SDL3.dll needed)" OFF)
option(XINPUT_BUILD_TEST "Build the xinput_test command line tool" ON)

if(NOT WIN32)
  message(FATAL_ERROR "This project targets Windows (MSVC or MinGW-w64).")
endif()

# Static CRT so the wrapper has no VC++ redistributable dependency
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

find_package(SDL3 CONFIG REQUIRED)

set(DEF_FILE "${CMAKE_SOURCE_DIR}/src/def/${XINPUT_DLL_NAME}.def")
if(NOT EXISTS "${DEF_FILE}")
  message(FATAL_ERROR "No export definition for '${XINPUT_DLL_NAME}' (${DEF_FILE})")
endif()

add_library(xinput_wrapper SHARED src/xinput_sdl.cpp src/xinput_types.h "${DEF_FILE}")
set_target_properties(xinput_wrapper PROPERTIES
  OUTPUT_NAME "${XINPUT_DLL_NAME}"
  PREFIX ""
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
target_compile_definitions(xinput_wrapper PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

# SDL3 exports SDL3::SDL3-shared, SDL3::SDL3-static (when built) and SDL3::SDL3
# (an alias for whichever one is available).
if(XINPUT_STATIC_SDL)
  if(NOT TARGET SDL3::SDL3-static)
    message(FATAL_ERROR "XINPUT_STATIC_SDL=ON but the SDL3 package has no static library (SDL3::SDL3-static).")
  endif()
  target_link_libraries(xinput_wrapper PRIVATE SDL3::SDL3-static)
elseif(TARGET SDL3::SDL3-shared)
  target_link_libraries(xinput_wrapper PRIVATE SDL3::SDL3-shared)
elseif(TARGET SDL3::SDL3)
  target_link_libraries(xinput_wrapper PRIVATE SDL3::SDL3)
else()
  target_link_libraries(xinput_wrapper PRIVATE SDL3::SDL3-static)
endif()

if(MINGW)
  target_link_options(xinput_wrapper PRIVATE -static-libgcc -static-libstdc++ -static)
endif()

# Copy ini + a sample controller-db hint next to the DLL
add_custom_command(TARGET xinput_wrapper POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_SOURCE_DIR}/xinput_sdl.ini" "$<TARGET_FILE_DIR:xinput_wrapper>/xinput_sdl.ini")

option(DINPUT8_BUILD_HIDE "Build the dinput8 proxy that hides gamepads from DirectInput" ON)

if(DINPUT8_BUILD_HIDE)
  add_library(dinput8_hide SHARED
    src/dinput8_hide.cpp
    "${CMAKE_SOURCE_DIR}/src/def/dinput8.def")
  set_target_properties(dinput8_hide PROPERTIES
    OUTPUT_NAME "dinput8"
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  )
  target_compile_definitions(dinput8_hide PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

  # IID_IUnknown etc. come from uuid; the DirectInput GUIDs are defined in the
  # source via initguid.h, so neither dxguid nor dinput8.lib is needed.
  target_link_libraries(dinput8_hide PRIVATE uuid)

  if(MINGW)
    target_link_options(dinput8_hide PRIVATE -static-libgcc -static-libstdc++ -static)
  endif()
endif()

if(XINPUT_BUILD_TEST)
  add_executable(xinput_test tools/xinput_test.cpp src/xinput_types.h)
  target_include_directories(xinput_test PRIVATE src)
  target_compile_definitions(xinput_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  set_target_properties(xinput_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()