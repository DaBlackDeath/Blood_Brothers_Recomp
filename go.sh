#!/bin/bash

set -e
bash tools/recomp_pipeline.sh roms/bloodbro.zip
cmake -S . -B build_tmp -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build_tmp
mkdir -p BloodBros_Recompiled
cp build_tmp/bloodbro.exe BloodBros_Recompiled/bloodbro.exe
strip -s BloodBros_Recompiled/bloodbro.exe
rm -rf build_tmp
rm -rf BloodBros_Recompiled/CMakeFiles BloodBros_Recompiled/CMakeCache.txt BloodBros_Recompiled/build.ninja BloodBros_Recompiled/.ninja_deps BloodBros_Recompiled/.ninja_log \
       BloodBros_Recompiled/cmake_install.cmake BloodBros_Recompiled/*.a

echo "Done: BloodBros_Recompiled/bloodbro.exe"
