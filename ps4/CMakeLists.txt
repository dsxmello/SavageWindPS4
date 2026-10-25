# Savage Wind for the PS4 (docs/ps4.md): the runtime, its Vulkan backend on RADV (Mesa on GNM), SDL3 (SceAudioOut,
# scePad), FFmpeg and, when SW_GENERATED points at it, the game code generated from the player's own game files
# (`make gen`), all linked into one eboot. Built by ps4/build.sh with DolphinPS4's toolchain as TriAevum PS4 set it up
# (ps4/env.sh).
cmake_minimum_required(VERSION 3.13)
project(savagewind_ps4 C CXX)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()
foreach (v DOLPHINPS4 PS4_SYSROOT SW_PS4_BUILD PS4_MESA_SRC)
    if (NOT DEFINED ENV{${v}})
        message(FATAL_ERROR "source ps4/env.sh first (${v} missing)")
    endif ()
    set(${v} $ENV{${v}})
endforeach ()

include(${DOLPHINPS4}/toolchain/ps4-love-style.cmake)  # PS4_RUNTIME_SOURCES, ps4_link_mesa, ps4_add_eboot

set(SW ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(SW_GENERATED "" CACHE PATH "The generated game code (make gen's generated/); empty: a runtime-only eboot")
set(SW_VERSION "0.1.0" CACHE STRING "Version shown in the log")

# ------------------------------------------------------------------------------------------------ dependencies --
set(SDL3_SRC ${SW_PS4_BUILD}/src/sdl3)
set(SDL3_LIB ${SW_PS4_BUILD}/build/sdl3/libSDL3.a)
set(FFMPEG ${SW_PS4_BUILD}/ffmpeg)
set(VOLK ${SW_PS4_BUILD}/src/volk)
set(GLSLANG ${PS4_SYSROOT}/glslang)
set(IMGUI ${SW}/third_party/imgui)
foreach (f ${SDL3_LIB} ${FFMPEG}/lib/libavcodec.a ${VOLK}/volk.h ${GLSLANG}/lib/libglslang.a)
    if (NOT EXISTS ${f})
        message(FATAL_ERROR "missing ${f}: run ps4/build.sh (it builds the dependencies first)")
    endif ()
endforeach ()

# ----------------------------------------------------------------------------------------------------- runtime --
file(GLOB RT_SRC CONFIGURE_DEPENDS
    ${SW}/runtime/*.c ${SW}/runtime/hle/*.c ${SW}/runtime/services/*.c
    ${SW}/runtime/nvn/*.c ${SW}/runtime/nvn/maxwell/*.c
    ${SW}/runtime/nvn/vulkan/*.c ${SW}/runtime/nvn/vulkan/*.cpp)
file(GLOB UI_SRC CONFIGURE_DEPENDS ${SW}/runtime/ui/*.mm)
set(IMGUI_SRC ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp
    ${IMGUI}/backends/imgui_impl_sdl3.cpp ${IMGUI}/backends/imgui_impl_vulkan.cpp)
set_source_files_properties(${UI_SRC} PROPERTIES LANGUAGE CXX COMPILE_OPTIONS "-xc++")

# SMAA's reference shader as a C string (runtime/nvn/smaa_data.c), as the Makefile makes it
set(SMAA_INC ${CMAKE_CURRENT_BINARY_DIR}/inc/smaa_hlsl.inc)
add_custom_command(OUTPUT ${SMAA_INC}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/inc
    COMMAND sh -c "LC_ALL=C tr -d '\\r' < '${SW}/third_party/smaa/SMAA.hlsl' | LC_ALL=C tr '\\200-\\377' '?' | sed -e 's/\\\\/\\\\\\\\/g' -e 's/\"/\\\\\"/g' -e 's/^/\"/' -e 's/$/\\\\n\"/' > '${SMAA_INC}'"
    DEPENDS ${SW}/third_party/smaa/SMAA.hlsl VERBATIM)
set_source_files_properties(${SW}/runtime/nvn/smaa_data.c PROPERTIES OBJECT_DEPENDS ${SMAA_INC})

# GAMELIB_ID: the runtime and the game code linked with it must agree (runtime/gamelib.h); here both come from this
# build, so a hash of the generator and the headers the game code is compiled against (as the Makefile's) is enough.
set(GENFLAGS -O1 -fno-strict-aliasing -Wno-return-type -Wno-unused-label -fvisibility=hidden -march=btver2
    -fno-omit-frame-pointer)
set(GAMELIB_SRCS tools/gen.py tools/lifter.py tools/analyze.py tools/nso.py tools/passes.py tools/patches.py
    tools/simd_c.py tools/gamelib_entry.c runtime/hle/hle_symbols.txt runtime/cpu.h runtime/simd.h
    runtime/simd_fp.h runtime/simd_softfma.h runtime/gamelib.h tools/layout.py tools/game_sites.py)
set(_id_text "")
foreach (f ${GAMELIB_SRCS})
    file(READ ${SW}/${f} _t)
    string(APPEND _id_text "${_t}")
endforeach ()
string(APPEND _id_text "ps4 ${GENFLAGS}")
string(SHA256 _id "${_id_text}")
string(SUBSTRING ${_id} 0 16 GAMELIB_ID)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${GAMELIB_SRCS})

# TRANSLATOR_ID / SPIRV_ID: what the shader caches are made from (the Makefile's), so a new eboot keeps
# /data/savagewind/cache while the Maxwell translator and the GLSL -> SPIR-V step stay the same
file(SHA256 ${SW}/runtime/nvn/vulkan/spirv.c _spirv)
string(SUBSTRING ${_spirv} 0 16 SPIRV_ID)
file(READ ${SW}/runtime/nvn/maxwell/translate.c _t1)
file(READ ${SW}/runtime/nvn/maxwell/shader.h _t2)
string(SHA256 _xlate "${_t1}${_t2}")
string(SUBSTRING ${_xlate} 0 16 TRANSLATOR_ID)
set_source_files_properties(${SW}/runtime/nvn/vulkan/spirv.c PROPERTIES COMPILE_DEFINITIONS SPIRV_ID="${SPIRV_ID}")
set_source_files_properties(${SW}/runtime/nvn/maxwell/translate.c PROPERTIES
    COMPILE_DEFINITIONS TRANSLATOR_ID="${TRANSLATOR_ID}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${SW}/runtime/nvn/vulkan/spirv.c ${SW}/runtime/nvn/maxwell/translate.c ${SW}/runtime/nvn/maxwell/shader.h)

add_executable(savagewind
    ${RT_SRC} ${UI_SRC} ${IMGUI_SRC}
    ${SW}/runtime/platform/ps4.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ps4_paths.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ps4_pad.c
    ${CMAKE_CURRENT_SOURCE_DIR}/shaderc_glslang.c
    ${VOLK}/volk.c
    ${PS4_RUNTIME_SOURCES})
target_include_directories(savagewind PRIVATE
    ${SW}/runtime ${SW}/runtime/nvn ${IMGUI} ${IMGUI}/backends ${SW}/third_party/stb ${CMAKE_CURRENT_BINARY_DIR}/inc
    ${SW}/third_party/smaa/Textures)
target_include_directories(savagewind SYSTEM PRIVATE
    ${SDL3_SRC}/include ${FFMPEG}/include ${GLSLANG}/include ${PS4_MESA_SRC}/include ${VOLK} ${SW}/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/shaderc)
target_compile_definitions(savagewind PRIVATE
    VK_NO_PROTOTYPES IMGUI_IMPL_VULKAN_USE_VOLK GAMELIB_ID="${GAMELIB_ID}" SAVAGEWIND_PS4_VERSION="${SW_VERSION}")
target_compile_options(savagewind PRIVATE
    -march=btver2 -fno-omit-frame-pointer -fno-strict-aliasing
    $<$<COMPILE_LANGUAGE:C>:-std=gnu17 -O2 -g -Wall -Wno-unused-function -Wno-format>
    $<$<COMPILE_LANGUAGE:CXX>:-O2 -g -std=c++17>)

# ------------------------------------------------------------------------------------------- the game code --
if (SW_GENERATED)
    file(GLOB GEN_SRC ${SW_GENERATED}/*.c ${SW_GENERATED}/*/*.c)
    if (NOT GEN_SRC)
        message(FATAL_ERROR "SW_GENERATED=${SW_GENERATED} has no generated code (make gen)")
    endif ()
    add_library(swgame OBJECT ${GEN_SRC} ${SW}/tools/gamelib_entry.c)
    target_include_directories(swgame PRIVATE ${SW}/runtime)
    target_compile_definitions(swgame PRIVATE GAMELIB_ID="${GAMELIB_ID}")
    target_compile_options(swgame PRIVATE ${GENFLAGS})
    target_sources(savagewind PRIVATE $<TARGET_OBJECTS:swgame>)
    message(STATUS "game code: ${SW_GENERATED} (${GAMELIB_ID})")
else ()
    target_sources(savagewind PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/no_game.c)
    target_compile_definitions(savagewind PRIVATE SW_PS4_NO_GAME)  # (runtime/main.c runs its console test)
    message(STATUS "game code: none (a runtime-only eboot)")
endif ()

# ------------------------------------------------------------------------------------------------------- link --
ps4_link_mesa(savagewind)
target_link_libraries(savagewind PRIVATE
    ${SDL3_LIB}
    ${FFMPEG}/lib/libavformat.a ${FFMPEG}/lib/libavcodec.a ${FFMPEG}/lib/libswscale.a ${FFMPEG}/lib/libavutil.a
    ${GLSLANG}/lib/libglslang.a ${GLSLANG}/lib/libglslang-default-resource-limits.a ${GLSLANG}/lib/libglslang.a
    z SceSysmodule SceSystemService SceUserService SceVideoOut ScePad SceAudioOut pthread m)
# The kernel's path calls only take absolute paths (ps4_paths.c).
target_link_options(savagewind PRIVATE
    "LINKER:--wrap=open,--wrap=stat,--wrap=lstat,--wrap=mkdir,--wrap=rmdir,--wrap=unlink,--wrap=rename"
    "LINKER:--wrap=access,--wrap=truncate,--wrap=chdir,--wrap=getcwd,--wrap=realpath")
ps4_add_eboot(savagewind)
