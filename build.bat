mkdir build86
cd build86
cmake .. -A "Win32" -DSDL3_DIR=".\ThirdParty\SDL3\cmake"
cmake --build . --config Release
cd ..
pause