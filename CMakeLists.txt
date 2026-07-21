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
    src/delivery_area.c
    src/player.c
    src/vampire.c
    src/crucifaxe.c
    src/weapon.c
    src/graveolver.c
    src/sml_med.c
    src/particles.c
    src/title.c
    src/spi.c
    src/collision.c
    src/delivery_area_mesh_collision.c
    src/kitchen_dining_mesh_collision.c
    src/kitchen_dining.c
    src/reception_mesh_collision.c
    src/reception.c
    src/piano_room_mesh_collision.c
    src/piano_room.c
    src/piano_props.c
    src/concrete_props.c
    src/copper_pot.c
    src/tentacle.c
    src/conservatory_mesh_collision.c
    src/conservatory.c
    src/crate.c
    src/dining_table.c
    src/save_point.c
    src/dresser.c
    src/texmgr.c
    src/key.c
    src/item_pickup.c
    src/bullet_hit.c
    src/door.c
    src/demondog.c
    src/zombie.c
    src/sound.c
    src/cdaudio.c
    src/menu.c
    src/btn_glyph.c
    src/save_menu.c
    src/memcard.c
    src/savegame.c
    src/world.c
    src/fatdoor.c
    src/door_anim.c
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

# Convert the ELF to a raw PS-EXE (HORROR2.BIN) that the disc boots.
# Without this the disc keeps packing a stale executable and code changes
# never reach the game.
add_custom_command(TARGET horror_game POST_BUILD
    COMMAND "${PSN00BSDK}/bin/elf2x.exe"
            "$<TARGET_FILE:horror_game>"
            "${CMAKE_BINARY_DIR}/HORROR2.BIN"
    COMMENT "Converting ELF to PS-EXE (HORROR2.BIN)"
)