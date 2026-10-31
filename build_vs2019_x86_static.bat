cmake -G "Visual Studio 16 2019" -A Win32 -B build86_static -DXINPUT_STATIC_SDL=ON
cmake --build build86_static --config Release
pause