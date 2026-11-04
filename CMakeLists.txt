cmake_minimum_required(VERSION 3.16)
project(bloodbro_recomp C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(SDL3 REQUIRED CONFIG)
find_package(OpenGL REQUIRED)

# The SDL3 library to link: the static one on MinGW (one self-contained exe), otherwise the shared one.
if(MINGW)
  if(NOT TARGET SDL3::SDL3-static)
    message(FATAL_ERROR "the static SDL3 library is missing (MSYS2: pacman -S mingw-w64-x86_64-sdl3)")
  endif()
  set(BB_SDL3 SDL3::SDL3-static)
elseif(TARGET SDL3::SDL3)
  set(BB_SDL3 SDL3::SDL3)
else()
  set(BB_SDL3 SDL3::SDL3-static)
endif()

# --- Vulkan renderer (optional): needs the Vulkan headers and glslang to compile the shaders to SPIR-V.
# No Vulkan library is linked: SDL loads vulkan-1.dll at run time (it comes with the graphics driver).
option(BLOODBRO_VULKAN "Build the Vulkan renderer" ON)
set(BB_HAVE_VULKAN OFF)
if(BLOODBRO_VULKAN)
  find_path(VULKAN_HEADERS_DIR vulkan/vulkan.h HINTS $ENV{VULKAN_SDK}/include)
  find_program(GLSLANG_EXE NAMES glslang glslangValidator HINTS $ENV{VULKAN_SDK}/bin)
  if(VULKAN_HEADERS_DIR AND GLSLANG_EXE)
    set(BB_HAVE_VULKAN ON)
  else()
    message(WARNING "Vulkan renderer disabled: Vulkan headers or glslang not found "
                    "(MSYS2: pacman -S mingw-w64-x86_64-vulkan-headers mingw-w64-x86_64-glslang)")
  endif()
endif()

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
  src/renderer.cpp
  src/gl_renderer.cpp
  src/bezel.cpp
  src/romset.cpp)

if(BB_HAVE_VULKAN)
  # src/shaders/*.vert|frag -> build/shaders/<name>_spv.h (const uint32_t k<Name>Spv[] with the SPIR-V words)
  set(BB_SHADER_OUT ${CMAKE_CURRENT_BINARY_DIR}/shaders)
  file(MAKE_DIRECTORY ${BB_SHADER_OUT})
  set(BB_SHADER_HEADERS)
  foreach(sh quad.vert:kQuadVertSpv crt.frag:kCrtFragSpv blit.frag:kBlitFragSpv)
    string(REPLACE ":" ";" parts ${sh})
    list(GET parts 0 file)
    list(GET parts 1 var)
    string(REPLACE "." "_" stem ${file})
    set(out ${BB_SHADER_OUT}/${stem}_spv.h)
    add_custom_command(
      OUTPUT ${out}
      COMMAND ${GLSLANG_EXE} -V --target-env vulkan1.0 --vn ${var} -o ${out} ${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/${file}
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/${file}
      COMMENT "SPIR-V: ${file}"
      VERBATIM)
    list(APPEND BB_SHADER_HEADERS ${out})
  endforeach()
  list(APPEND BLOODBRO_SOURCES src/vk_renderer.cpp ${BB_SHADER_HEADERS})
  # Vulkan structs are written as T x{VK_STRUCTURE_TYPE_...}: the remaining members are zero-initialised on purpose
  set_source_files_properties(src/vk_renderer.cpp PROPERTIES COMPILE_OPTIONS "-Wno-missing-field-initializers")
endif()

if(WIN32)
  # the launcher window (bloodbro.exe without arguments, Dear ImGui) and the program manifest
  enable_language(RC)
  list(APPEND BLOODBRO_SOURCES src/launcher.cpp src/bloodbro.rc)

  # --- Dear ImGui (MIT) with the SDL3 and OpenGL 2 backends ---
  add_library(imgui STATIC
    ${TP}/imgui/imgui.cpp
    ${TP}/imgui/imgui_draw.cpp
    ${TP}/imgui/imgui_tables.cpp
    ${TP}/imgui/imgui_widgets.cpp
    ${TP}/imgui/imgui_impl_sdl3.cpp
    ${TP}/imgui/imgui_impl_opengl2.cpp)
  target_include_directories(imgui PUBLIC ${TP}/imgui)
  target_include_directories(imgui PRIVATE $<TARGET_PROPERTY:${BB_SDL3},INTERFACE_INCLUDE_DIRECTORIES>)
  target_compile_options(imgui PRIVATE -w -O2)
  target_link_libraries(imgui PUBLIC OpenGL::GL imm32)   # imm32: IME support of the text fields
endif()

add_executable(bloodbro ${BLOODBRO_SOURCES})
target_include_directories(bloodbro PRIVATE recomp generated third_party/stb)
target_link_libraries(bloodbro PRIVATE z80core ymfm miniz OpenGL::GL ${BB_SDL3})
if(BB_HAVE_VULKAN)
  target_compile_definitions(bloodbro PRIVATE BLOODBRO_HAVE_VULKAN VK_NO_PROTOTYPES)
  target_include_directories(bloodbro PRIVATE ${BB_SHADER_OUT})
  target_include_directories(bloodbro SYSTEM PRIVATE ${VULKAN_HEADERS_DIR})
endif()
if(WIN32)
  target_link_libraries(bloodbro PRIVATE imgui)
endif()
target_compile_options(bloodbro PRIVATE -Wall -Wextra -Wno-unused-parameter)

if(MINGW)
  # one self-contained exe: SDL3, libstdc++, libgcc and winpthread linked in (only Windows system DLLs remain)
  target_link_libraries(bloodbro PRIVATE comdlg32)
  target_link_options(bloodbro PRIVATE -static)
  # GUI program: no console window opens when the game is started by double-click. When it is started
  # from a terminal, the program attaches to that terminal (see attach_console_if_needed()).
  option(BLOODBRO_CONSOLE "Build bloodbro.exe as a console program (always shows a console window)" OFF)
  if(NOT BLOODBRO_CONSOLE)
    target_link_options(bloodbro PRIVATE -mwindows)
  endif()
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
