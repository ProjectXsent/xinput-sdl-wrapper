mkdir build
cd build
cmake .. -A "x64" -DSDL3_DIR=".\ThirdParty\SDL3\cmake"
cmake --build . --config Release
cd ..
pause