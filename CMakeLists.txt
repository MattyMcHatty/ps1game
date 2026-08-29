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
    src/the_hatch_mesh_collision.c
    src/the_hatch.c
    src/hatch_doors.c
    src/hatch_puzzle.c
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
    src/cull_arena.c
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
    src/valve_puzzle.c
    src/door_anim.c
    src/intro.c
    src/debug_opts.c
)

# ---- SIZE-OPTIMISE THE COLD MODULES -----------------------------------------
# >>> THIS IS A HEAP FIX, NOT A TIDY-UP. <<< The heap top IS the stack (see
# tools/HEAP_BUDGET.txt): everything from the end of BSS to 0x801FFFF8 is heap,
# the BIOS stack grows down into the same region, and the measured boot cliff is
# about 234 KB of "free at rest". Every kilobyte of .text pushes _end up and
# eats that margin directly. Adding the Valve Puzzle cost 10 KB of code and took
# the build from 236 KB to 226 KB -- straight through the cliff, into the
# unaligned-JR-inside-CdReadSync crash that tools/DIAGNOSING_A_BOOT_CRASH.txt
# describes. This is what paid it back.
#
# WHAT IS IN THE LIST AND WHY. Every file here is code that does NOT run inside
# a per-frame render or AI loop:
#
#   the puzzles           each one owns the screen while it runs. The room is
#                         frozen or nearly so, and the whole module executes a
#                         few hundred instructions a frame.
#   the menus and saves   menu, save menu, savegame, memcard, title, intro: all
#                         of them run with the world stopped.
#   the transitions       door/stair animations, CD audio, the room arena: once
#                         per room change.
#   world.c               room swaps and the save delta, once per transition.
#   the scripted beats    the quake, the two Hadad set-pieces, the boss cutscene
#                         driver, the delivery intro.
#
# WHAT IS DELIBERATELY NOT IN IT: every room's own *_draw, the collision and
# floor code, the enemies, the weapons, particles, render.c and camera.c. Those
# are the hot loops tools/DIAGNOSING_FRAME_RATE.txt is about, they stay at -O2,
# and NOTHING here should be moved into this list to save a few more bytes
# without measuring U/D/G first -- that document exists because optimising this
# engine by reasoning alone made it 56% worse.
#
# Measured on this build: 231168 -> 247856 bytes free at rest (226 KB -> 242 KB),
# i.e. 16.2 KB reclaimed. That is 6 KB BETTER than the tree stood at before the
# Valve Puzzle was added (236 KB) and 8 KB clear of the cliff -- the first real
# headroom this heap has had since the Greenhouse. Do not spend it casually.
set_source_files_properties(
    src/menu.c
    src/save_menu.c
    src/savegame.c
    src/memcard.c
    src/title.c
    src/intro.c
    src/debug_opts.c
    src/world.c
    src/room_arena.c
    src/cull_arena.c
    src/texmgr.c
    src/cdaudio.c
    src/spi.c
    src/door_anim.c
    src/stair_anim.c
    src/stove_puzzle.c
    src/piano_puzzle.c
    src/anzu_puzzle.c
    src/keystone_plinths.c
    src/exit_door_puzzle.c
    src/lightswitch_puzzle.c
    src/trick_drawers.c
    src/grinder_puzzle.c
    src/greenhouse_puzzle.c
    src/greenhouse_flood.c
    src/valve_puzzle.c
    src/hatch_puzzle.c
    src/birdcage.c
    src/delivery_intro.c
    src/east_hall_quake.c
    src/reception_hadad.c
    src/hadad_grinder.c
    src/hadad_library.c
    src/rabisu_boss.c
    PROPERTIES COMPILE_OPTIONS "-Os"
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