cmake_minimum_required(VERSION 3.21)

set(PSN00BSDK "C:/Users/virtu/Documents/PSn00bSDK")

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR mips)

set(CMAKE_C_COMPILER "${PSN00BSDK}/bin/mipsel-none-elf-gcc.exe")
set(CMAKE_ASM_COMPILER "${PSN00BSDK}/bin/mipsel-none-elf-gcc.exe")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-nostdlib")

# This stops CMake from trying to test the compiler like a normal Windows app
set(CMAKE_C_COMPILER_WORKS 1)
set(CMAKE_ASM_COMPILER_WORKS 1)

project(horror_game LANGUAGES C ASM)

set(PSN00BSDK_LIBS "${PSN00BSDK}/lib/libpsn00b")
set(PSN00BSDK_INCLUDE "${PSN00BSDK}/include")

add_executable(horror_game
    src/main.c
    src/camera.c
    src/render.c
    src/level.c
    src/player.c
    src/vampire.c
    src/bat.c
    src/sml_med.c
    src/particles.c
    src/title.c
    src/spi.c
    src/collision.c
    src/level1_mesh_collision.c
    src/crate.c
    src/key.c
    src/door.c
    src/demondog.c
)

target_include_directories(horror_game PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${PSN00BSDK_INCLUDE}/libpsn00b"
    "${PSN00BSDK}/lib/gcc/mipsel-none-elf/12.3.0/include"
    "${PSN00BSDK}/lib/gcc/mipsel-none-elf/12.3.0/include-fixed"
)

target_compile_options(horror_game PRIVATE
    -march=r3000 -mabi=32 -mfp32
    -fno-builtin -fdata-sections -ffunction-sections
    -O2 -G0
    -isystem "${PSN00BSDK}/lib/gcc/mipsel-none-elf/12.3.0/include"
)

target_link_options(horror_game PRIVATE
    -T "${PSN00BSDK_LIBS}/ldscripts/exe.ld"
    -march=r3000 -mabi=32 -mfp32
    -nostdlib -G0
    -Wl,--gc-sections
)

target_link_libraries(horror_game PRIVATE
    "${PSN00BSDK_LIBS}/release/libpsxgpu_exe_nogprel.a"
    "${PSN00BSDK_LIBS}/release/libpsxgte_exe_nogprel.a"
    "${PSN00BSDK_LIBS}/release/libpsxetc_exe_nogprel.a"
    "${PSN00BSDK_LIBS}/release/libpsxspu_exe_nogprel.a"
    "${PSN00BSDK_LIBS}/release/libpsxapi_exe_nogprel.a"
    "${PSN00BSDK_LIBS}/release/libpsxcd_exe_nogprel.a"
    "${PSN00BSDK_LIBS}/release/libsmd_exe_nogprel.a"
    "${PSN00BSDK_LIBS}/release/libc_exe_nogprel.a"
)

set_target_properties(horror_game PROPERTIES
    OUTPUT_NAME "HORROR"
    SUFFIX ".exe"
)