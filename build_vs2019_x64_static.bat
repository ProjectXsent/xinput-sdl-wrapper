cmake -G "Visual Studio 16 2019" -A x64 -B build_static -DXINPUT_STATIC_SDL=ON
cmake --build build_static --config Release
pause