mkdir build86
cd build86
cmake .. -G "Visual Studio 16 2019" -A "Win32" -DSDL3_DIR=".\ThirdParty\SDL3\cmake"
cmake --build . --config Release
cd ..
pause