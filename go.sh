#!/bin/bash

set -e
bash tools/recomp_pipeline.sh roms/bloodbro.zip
cmake -S . -B build_tmp -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build_tmp
mkdir -p BloodBros_Recompiled/roms
cp build_tmp/bloodbro.exe BloodBros_Recompiled/bloodbro.exe
strip -s BloodBros_Recompiled/bloodbro.exe
[ -f BloodBros_Recompiled/roms/bloodbro.zip ] || cp roms/bloodbro.zip BloodBros_Recompiled/roms/
rm -rf build_tmp

echo "Done: BloodBros_Recompiled/bloodbro.exe"
