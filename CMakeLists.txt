cmake_minimum_required(VERSION 3.13)
project(bloodbro_recomp C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(SDL2 REQUIRED)
find_package(OpenGL REQUIRED)

set(TP ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# The game runs the statically recompiled 68000 program. It is generated from YOUR ROM by
# tools/recomp_pipeline.sh and is not part of the project.
if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/generated/recomp_insns.inc OR
   NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/generated/recomp_blocks.inc)
  message(FATAL_ERROR "generated/recomp_insns.inc is missing: run  bash tools/recomp_pipeline.sh roms/bloodbro.zip  first")
endif()

# --- Z80 (superzazu/z80, MIT, with a small interrupt-ack patch) ---
add_library(z80core STATIC ${TP}/z80/z80.c)
target_include_directories(z80core PUBLIC ${TP}/z80)
target_compile_options(z80core PRIVATE -w -O2)

# --- ymfm (YM3812) ---
add_library(ymfm STATIC
  ${TP}/ymfm/ymfm_opl.cpp
  ${TP}/ymfm/ymfm_adpcm.cpp
  ${TP}/ymfm/ymfm_pcm.cpp
  ${TP}/ymfm/ymfm_ssg.cpp)
target_include_directories(ymfm PUBLIC ${TP}/ymfm)
target_compile_options(ymfm PRIVATE -w -O2)

# --- miniz (zip reading, PNG writing) ---
add_library(miniz STATIC ${TP}/miniz/miniz.c)
target_include_directories(miniz PUBLIC ${TP}/miniz)
target_compile_options(miniz PRIVATE -w -O2)

set(BLOODBRO_SOURCES
  src/main.cpp
  src/machine.cpp
  src/video.cpp
  src/oki6295.cpp
  src/recomp_cpu.cpp
  src/cheats.cpp
  src/ini.cpp
  src/menu.cpp
  src/gl_renderer.cpp
  src/bezel.cpp
  src/romset.cpp)
if(WIN32)
  # the launcher window (bloodbro.exe without arguments) and the manifest for its controls
  enable_language(RC)
  list(APPEND BLOODBRO_SOURCES src/launcher.cpp src/bloodbro.rc)
  # its texts use the Windows ANSI code page, like the paths in bloodbro.ini
  set_source_files_properties(src/launcher.cpp PROPERTIES COMPILE_OPTIONS "-fexec-charset=CP1252")
endif()

add_executable(bloodbro ${BLOODBRO_SOURCES})
target_include_directories(bloodbro PRIVATE recomp generated third_party/stb)
target_link_libraries(bloodbro PRIVATE z80core ymfm miniz OpenGL::GL)
target_compile_options(bloodbro PRIVATE -Wall -Wextra -Wno-unused-parameter)

if(MINGW)
  # one self-contained exe: SDL2, libstdc++, libgcc and winpthread linked in (only Windows system DLLs remain)
  if(NOT TARGET SDL2::SDL2-static)
    message(FATAL_ERROR "the static SDL2 library is missing (MSYS2: pacman -S mingw-w64-x86_64-SDL2)")
  endif()
  target_link_libraries(bloodbro PRIVATE mingw32 SDL2::SDL2main SDL2::SDL2-static comdlg32)
  target_link_options(bloodbro PRIVATE -static)
  # GUI program: no console window opens when the game is started by double-click. When it is started
  # from a terminal, the program attaches to that terminal (see attach_console_if_needed()).
  option(BLOODBRO_CONSOLE "Build bloodbro.exe as a console program (always shows a console window)" OFF)
  if(NOT BLOODBRO_CONSOLE)
    target_link_options(bloodbro PRIVATE -mwindows)
  endif()
elseif(TARGET SDL2::SDL2)
  target_link_libraries(bloodbro PRIVATE SDL2::SDL2)
else()
  target_include_directories(bloodbro PRIVATE ${SDL2_INCLUDE_DIRS})
  target_link_libraries(bloodbro PRIVATE ${SDL2_LIBRARIES})
endif()

# stb_image.h defines a few helpers that this program does not use
set_source_files_properties(src/bezel.cpp PROPERTIES COMPILE_OPTIONS "-Wno-unused-function")

# Tests of the menu logic, the cheat engine and the ini reader (no ROM needed), only built on request:
# cmake -S . -B build -DBLOODBRO_TESTS=ON && cmake --build build, then build/menu_test and build/bezel_test
option(BLOODBRO_TESTS "Build the test programs menu_test and bezel_test" OFF)
# Memory checks for the tests: cmake -S . -B build_asan -DBLOODBRO_TESTS=ON -DBLOODBRO_SANITIZE=ON
option(BLOODBRO_SANITIZE "Build the test programs with AddressSanitizer and UBSan" OFF)

if(BLOODBRO_TESTS)
  add_executable(menu_test tests/menu_test.cpp src/cheats.cpp src/ini.cpp src/menu.cpp)
  target_include_directories(menu_test PRIVATE src)

  add_executable(bezel_test tests/bezel_test.cpp src/bezel.cpp third_party/miniz/miniz.c)
  target_include_directories(bezel_test PRIVATE src third_party/stb third_party/miniz)
  target_compile_options(bezel_test PRIVATE -w)

  if(MINGW)
    foreach(t menu_test bezel_test)
      target_link_options(${t} PRIVATE -static)
    endforeach()
  endif()

  if(BLOODBRO_SANITIZE)
    foreach(t menu_test bezel_test)
      target_compile_options(${t} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -g)
      target_link_options(${t} PRIVATE -fsanitize=address,undefined)
    endforeach()
  endif()
endif()
