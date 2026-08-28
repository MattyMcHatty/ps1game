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
    src/delivery_intro.c
    src/player.c
    src/vampire.c
    src/crucifaxe.c
    src/weapon.c
    src/graveolver.c
    src/helluminator.c
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
    src/piano_puzzle.c
    src/anzu_tex.c
    src/anzu_puzzle.c
    src/concrete_props.c
    src/copper_pot.c
    src/tentacle.c
    src/rafflesia.c
    src/mushroom.c
    src/living_statue.c
    src/hadad.c
    src/conservatory_mesh_collision.c
    src/conservatory.c
    src/hall_2f_mesh_collision.c
    src/hall_2f.c
    src/master_bedroom_mesh_collision.c
    src/master_bedroom.c
    src/east_hall_mesh_collision.c
    src/east_hall.c
    src/east_hall_quake.c
    src/library_mesh_collision.c
    src/library.c
    src/library_destroyed_mesh_collision.c
    src/library_destroyed.c
    src/east_stairwell_mesh_collision.c
    src/east_stairwell.c
    src/attic_stairwell_mesh_collision.c
    src/attic_stairwell.c
    src/attic_exit_mesh_collision.c
    src/attic_exit.c
    src/garden_stairs_mesh_collision.c
    src/garden_stairs.c
    src/garden_courtyard_mesh_collision.c
    src/garden_courtyard.c
    src/fountain_square_mesh_collision.c
    src/fountain_square.c
    src/outside_catacombs_mesh_collision.c
    src/outside_catacombs.c
    src/maze_one_mesh_collision.c
    src/maze_one.c
    src/maze_two_mesh_collision.c
    src/maze_two.c
    src/keystone_maze_mesh_collision.c
    src/keystone_maze.c
    src/keystone_plinths.c
    src/chain_room_mesh_collision.c
    src/chain_room.c
    src/rear_gate_mesh_collision.c
    src/rear_gate.c
    src/west_corridor_mesh_collision.c
    src/west_corridor.c
    src/stables_mesh_collision.c
    src/greenhouse.c
    src/greenhouse_mesh_collision.c
    src/stables.c
    src/rabisu_boss.c
    src/hadad_library.c
    src/hadad_grinder.c
    src/reception_hadad.c
    src/chainlink_door.c
    src/lever.c
    src/lightswitch_puzzle.c
    src/exit_door_puzzle.c
    src/quake.c
    src/damage.c
    src/stair_anim.c
    src/trick_drawers.c
    src/stove_puzzle.c
    src/crate.c
    src/dining_table.c
    src/save_point.c
    src/dresser.c
    src/grinder.c
    src/grinder_puzzle.c
    src/texmgr.c
    src/room_arena.c
    src/key.c
    src/item_pickup.c
    src/bullet_hit.c
    src/door.c
    src/demondog.c
    src/zombie.c
    src/spider.c
    src/rabisu.c
    src/web.c
    src/sound.c
    src/cdaudio.c
    src/menu.c
    src/hud.c
    src/btn_glyph.c
    src/save_menu.c
    src/memcard.c
    src/savegame.c
    src/world.c
    src/fatdoor.c
    src/vines.c
    src/greenhouse_puzzle.c
    src/greenhouse_flood.c
    src/birdcage.c
    src/valve_handle.c
    src/door_anim.c
    src/intro.c
    src/debug_opts.c
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