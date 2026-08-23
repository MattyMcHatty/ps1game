#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxpad.h>
#include <inline_c.h>
#include "spi.h"
#include "camera.h"
#include "render.h"
#include "delivery_area.h"
#include "player.h"
#include "vampire.h"
#include "crucifaxe.h"
#include "weapon.h"
#include "graveolver.h"
#include "sound.h"
#include "cdaudio.h"
#include "sml_med.h"
#include "particles.h"
#include "title.h"
#include "collision.h"
#include "crate.h"
#include "dining_table.h"
#include "save_point.h"
#include "dresser.h"
#include "grinder.h"
#include "grinder_puzzle.h"
#include "key.h"
#include "item_pickup.h"
#include "bullet_hit.h"
#include "door.h"
#include "demondog.h"
#include "zombie.h"
#include "spider.h"
#include "web.h"
#include "menu.h"
#include "hud.h"
#include "save_menu.h"
#include "savegame.h"
#include "debug_opts.h"
#include "kitchen_dining.h"
#include "reception.h"
#include "piano_room.h"
#include "piano_props.h"
#include "piano_puzzle.h"
#include "anzu_puzzle.h"
#include "anzu_tex.h"
#include "conservatory.h"
#include "hall_2f.h"
#include "master_bedroom.h"
#include "east_hall.h"
#include "east_hall_quake.h"
#include "library.h"
#include "library_destroyed.h"
#include "hadad_library.h"
#include "east_stairwell.h"
#include "attic_stairwell.h"
#include "attic_exit.h"
#include "garden_stairs.h"
#include "garden_courtyard.h"
#include "fountain_square.h"
#include "outside_catacombs.h"
#include "maze_one.h"
#include "maze_two.h"
#include "rear_gate.h"
#include "west_corridor.h"
#include "lightswitch_puzzle.h"
#include "exit_door_puzzle.h"
#include "chainlink_door.h"
#include "lever.h"
#include "trick_drawers.h"
#include "stove_puzzle.h"
#include "concrete_props.h"
#include "copper_pot.h"
#include "tentacle.h"
#include "rafflesia.h"
#include "mushroom.h"
#include "living_statue.h"
#include "hadad.h"
#include "rabisu.h"
#include "rabisu_boss.h"
#include "delivery_intro.h"
#include "world.h"
#include "fatdoor.h"
#include "door_anim.h"
#include "stair_anim.h"
#include "intro.h"

/* Title-screen background (TITLE_BG_* live in title.h). It is the framebuffer
   CLEAR colour, not a drawn tile: draw_title paints only the letters over it.
   Gameplay and the game-over screen both overwrite the clear colour (black /
   red), so anything that returns to the title has to put this back — see the
   STATE_TITLE entry hook at the bottom of the main loop. */

/* The front end: the title screen and the opening sequence that follows New
   Game. Both are "not in the world yet" — the hooks at the bottom of the main
   loop start the music and build the first room on the way OUT of this pair,
   not on the title -> intro step in the middle of it. */
#define IS_FRONTEND(s) ((s) == STATE_TITLE || (s) == STATE_INTRO)

GameState game_state   = STATE_TITLE;
GameState current_area = STATE_DELIVERY_AREA;  /* last playable area; menu returns here */
GameState pending_area = STATE_KITCHEN_DINING; /* area STATE_LOADING will switch to */
int       debug_mode   = 0;

/* Which of the two Hall 2F <-> Master Bedroom doors the player is currently
   walking through (1 = the west pair, 0 = the east pair). Set at the trigger,
   read by the STATE_LOADING branch to place the arrival spawn on the matching
   side. The rooms are linked in pairs, so one flag serves both directions. */
static int bedroom_door_west = 0;

/* Debug/game-over font stream (opened in main() after FntLoad). The HUD's log
   box renders with FntSort straight into the OT, so it needs no stream. */
static int gameover_fnt;

volatile uint8_t pad_buff[2][34];
volatile size_t  pad_buff_len[2];

void poll_cb(uint32_t port, const volatile uint8_t *buff, size_t rx_len) {
    pad_buff_len[port] = rx_len;
    if (rx_len)
        memcpy((void *)pad_buff[port], (void *)buff, rx_len);
}

void reset_game(RenderContext *ctx) {
    cam_x   = 1600;     /* back against the east wall (x=1800, radius 175) */
    cam_y   = 0;
    cam_vy  = 0;
    cam_z   = 0;        /* centred north-south in the first room */
    cam_rot = 3072;     /* facing west (-X), across the room */
    vampire_x  = 1200;
    vampire_y  = 0;
    vampire_vy = 0;
    vampire_z  = 1200;
    game_over     = 0;
    flash_timer   = 0;
    damage_timer  = 0;
    player_health = MAX_HEALTH;
    player_save_count = 0;   /* fresh playthrough: no saves recorded yet */
    game_flags        = 0;   /* ...and no puzzle has happened yet */
    swing_timer    = 0;
    vampire_kb_vx    = 0;
    vampire_kb_vz    = 0;
    vampire_health   = VAMPIRE_MAX_HEALTH;
    vampire_hit_timer = 0;
    reset_particles();
    bullet_hits_reset();
    webs_reset();
    player_poison_timer = 0;   /* status effects never survive a reset */
    kitchen_stove_reset();
    stove_puzzle_arm();      /* drop any in-progress cook, clear the board */
    piano_puzzle_arm();      /* ...and any in-progress piano key placement  */
    exit_door_puzzle_arm();  /* ...and any keystones left in the exit door  */
    cam_pitch = 0;           /* a puzzle camera never survives a reset */
    {
        int k;
        for (k = 0; k < PICKUP_MSG_COUNT; k++) pickup_log[k].live = 0;
    }
    sprint_stamina  = SPRINT_STAMINA_MAX;
    sprint_cooldown = 0;
    door_init();
    /* The two one-sided door locks (Hall 2F <-> Reception and West Corridor <->
       Reception) used to be cleared here. They are GameFlags now — see
       FLAG_HALL_2F_DOOR in src/player.h — so `game_flags = 0` above already
       starts a new game with both locked, and a LOADED game gets them back from
       SaveData.flags instead of being wrongly re-locked by this function. */
    menu_inventory_reset();      /* an empty inventory grid, in no arrangement */
    crates_reset();
    demon_dogs_reset();
    zombies_reset();
    spiders_reset();
    rafflesias_reset();
    mushrooms_reset();
    living_statues_reset();
    hadads_reset();
    hadad_library_reset(); /* ...and any half-played crawl under the shelving */
    east_hall_quake_reset();/* ...and any half-played collapse in the East Hall */
    rabisus_reset();
    rabisu_boss_reset();   /* forget any half-played boss encounter */
    delivery_intro_reset();/* ...and any half-played arrival sequence. This runs
                              BEFORE the arrival is armed on the New Game path
                              (see the frontend hook), so it never cancels the
                              one it is about to start. */
    fatdoors_reset();
    setRGB0(&ctx->buffers[0].draw_env, 0, 0, 0);
    setRGB0(&ctx->buffers[1].draw_env, 0, 0, 0);
}

/* Read one area's mesh into the shared room arena, evicting whatever room was
   in it. Called from STATE_LOADING for every area, and additionally from the
   title-exit path for the delivery area, which is the one room entered without
   a STATE_LOADING pass.

   Blocking CD access: legal here and nowhere else in the frame. See
   src/room_arena.h for why one arena is enough and why the outgoing room's mesh
   is safe to discard. A room missing from this switch loads no geometry and
   draws empty — add new rooms here as well as to draw_current_area(). */
static void load_area_geometry(GameState area) {
    switch (area) {
        case STATE_DELIVERY_AREA:    delivery_load_geometry();         break;
        case STATE_KITCHEN_DINING:   kitchen_load_geometry();          break;
        case STATE_RECEPTION:        reception_load_geometry();        break;
        case STATE_PIANO_ROOM:       piano_room_load_geometry();       break;
        case STATE_CONSERVATORY:     conservatory_load_geometry();     break;
        case STATE_2F_HALL:          hall_2f_load_geometry();          break;
        case STATE_MASTER_BEDROOM:   master_bedroom_load_geometry();   break;
        case STATE_EAST_HALL:        east_hall_load_geometry();        break;
        case STATE_LIBRARY:          library_load_geometry();          break;
        case STATE_LIBRARY_DESTROYED:library_destroyed_load_geometry();break;
        case STATE_EAST_STAIRWELL:   east_stairwell_load_geometry();   break;
        case STATE_ATTIC_STAIRWELL:  attic_stairwell_load_geometry();  break;
        case STATE_ATTIC_EXIT:       attic_exit_load_geometry();       break;
        case STATE_GARDEN_STAIRS:    garden_stairs_load_geometry();    break;
        case STATE_GARDEN_COURTYARD: garden_courtyard_load_geometry(); break;
        case STATE_FOUNTAIN_SQUARE:  fountain_square_load_geometry();  break;
        case STATE_OUTSIDE_CATACOMBS:outside_catacombs_load_geometry();break;
        case STATE_MAZE_ONE:         maze_one_load_geometry();         break;
        case STATE_MAZE_TWO:         maze_two_load_geometry();         break;
        case STATE_REAR_GATE:        rear_gate_load_geometry();        break;
        case STATE_WEST_CORRIDOR:    west_corridor_load_geometry();    break;
        default: break;   /* title, menu, transitions: no room to build */
    }
}

/* ---- Shared per-frame helpers, used by every playable area ---- */

/* Start's edge state for the inventory menu. Initialised "held" so a press left
   over from the front end is swallowed rather than opening the menu on the
   first frame of play. handle_menu_open_arm() re-asserts that: the opening
   sequence is skipped WITH Start, so the button is still down when the Delivery
   Area comes up. */
static int menu_start_prev = 1;
static void handle_menu_open_arm(void) { menu_start_prev = 1; }

/* Open the inventory menu on a fresh Start press, remembering the area. */
static void handle_menu_open(void) {
    if (!pad_buff_len[0]) return;
    PadResponse *pad = (PadResponse *)pad_buff[0];
    int start_held = (~pad->btn & PAD_START) ? 1 : 0;
    if (start_held && !menu_start_prev) {
        current_area = game_state;
        menu_open();
        game_state = STATE_MENU;
    }
    menu_start_prev = start_held;
}

/* Advance one area: player movement + the area's own geometry/entities,
   then the shared weapon and particle systems. */
static void update_current_area(GameState area) {
    /* Drawer puzzle owns the camera + input while active: no free movement,
       collision, doors or weapons — just the puzzle's own update. */
    if (area == STATE_2F_HALL && trick_drawers_puzzle_active()) {
        update_zombies();       /* enemies keep chasing/attacking during the puzzle */
        update_spiders();
        update_rabisus();
        update_rafflesias();    /* ...and the garden flowers keep gripping */
        update_mushrooms();     /* ...and the Mushroom Head keeps hunting  */
        update_living_statues();/* ...and the statue keeps closing in       */
        update_hadads();        /* ...and Hadad keeps walking his corridor  */
        webs_update();          /* ...and their webs keep flying */
        player_status_update();
        trick_drawers_update();
        return;
    }
    /* Same deal for the kitchen's stove puzzle — plus the flame update, since
       the cook step burns the stove for three seconds while it owns the view. */
    if (area == STATE_KITCHEN_DINING && stove_puzzle_active()) {
        update_zombies();
        update_spiders();
        update_rabisus();
        update_rafflesias();
        update_mushrooms();
        update_living_statues();
        update_hadads();
        webs_update();
        player_status_update();
        kitchen_stove_update();
        stove_puzzle_update();
        update_particles();
        return;
    }
    /* ...and the piano room's key puzzle. Nothing else lives in that room, so
       there is no enemy tick to keep running alongside it. */
    if (area == STATE_PIANO_ROOM && piano_puzzle_active()) {
        piano_puzzle_update();
        return;
    }
    /* ...and the same room's Anzu Tablet puzzle. */
    if (area == STATE_PIANO_ROOM && anzu_puzzle_active()) {
        anzu_puzzle_update();
        return;
    }
    /* The Attic Exit's lightswitch puzzle only takes the camera for its PAYOFF —
       the levers themselves are thrown in free play below. While the gate is
       winching up, nothing else in the room runs. */
    if (area == STATE_ATTIC_EXIT && lightswitch_puzzle_active()) {
        lightswitch_update();
        return;
    }
    /* The same room's exit-door puzzle DOES take the camera and input for its
       whole duration, like the stove board. Enemies keep running: the player is
       stood at the door the whole time.

       This also covers the QUAKE that follows pulling the four key stones back
       out of that door — it is a state of the same module precisely so that it
       inherits this branch and the overlay suppression further down, rather than
       needing a cutscene branch of its own like the boss and the delivery
       intro. See exit_door_puzzle.h. */
    if (area == STATE_ATTIC_EXIT && exit_door_puzzle_active()) {
        update_zombies();
        update_spiders();
        update_rabisus();
        update_tentacles();    /* the four guarding the north levers */
        update_rafflesias();
        update_mushrooms();
        update_living_statues();
        update_hadads();
        webs_update();
        player_status_update();
        exit_door_puzzle_update();
        return;
    }
    /* ...and the same quake again, this time in the East Hall, on the first
       arrival back out of the wrecked Library (east_hall_quake.h). Same shape as
       the branch above and for the same reason — update_camera, apply_collision
       and apply_height must NOT run, or the walls would push the shake around —
       but it needs a branch of its own because it is not a state of any puzzle
       this room hosts. The room's enemies keep running: the player is stood just
       inside the door the whole time, exactly as in the Attic Exit. */
    if (area == STATE_EAST_HALL && east_hall_quake_active()) {
        update_zombies();
        update_spiders();
        update_rabisus();
        player_status_update();
        east_hall_quake_update();
        return;
    }
    /* The Garden Courtyard's boss encounter takes the camera and all input for
       its reveal and for its death sequence — but NOT for the fight in between,
       which is ordinary free play and falls through to the branch below.
       update_rabisus still runs: the boss keeps facing the player throughout
       both cutscenes, and the director poses the body through it. */
    if (area == STATE_GARDEN_COURTYARD && rabisu_boss_cutscene()) {
        update_rabisus();
        player_status_update();
        rabisu_boss_update();
        update_particles();
        return;
    }
    /* ...and the Delivery Area's arrival sequence, on a New Game only. It drives
       the camera over the fence by hand, so update_camera and apply_height must
       not run: gravity would haul the jump straight back down onto the ground,
       and apply_collision would push the camera off the grass outside the yard
       the moment the sequence put it there. Nothing else in the room needs
       ticking — the yard starts with no enemies in it (world.c). */
    if (area == STATE_DELIVERY_AREA && delivery_intro_active()) {
        delivery_intro_update();
        return;
    }
    /* ...and the Library Destroyed's crawl under the collapsed shelving, which
       takes the camera and all input for its six seconds. Only the crawl — the
       rest of that encounter is free play with something walking at you and
       falls through to the branch below.

       >>> update_hadads IS DELIBERATELY NOT CALLED HERE. <<< Unlike the boss
       reveal, this cutscene is not something the player watches from safety:
       Hadad is mid-walk down the west aisle and the anchored player is standing
       right where he is heading, so ticking him would let a contact blow land on
       somebody who is supposedly under the floor and cannot be touched. Freezing
       him costs nothing, because the director TELEPORTS him to the single door
       the frame control comes back regardless of where the walk had got to
       (hadad_library.h). His music is a latch and simply keeps playing. */
    if (area == STATE_LIBRARY_DESTROYED && hadad_library_cutscene()) {
        player_status_update();
        hadad_library_update();
        update_particles();
        return;
    }
    update_camera();
    /* Menu open: the area still ticks (enemies move, gravity applies) but every
       player-driven level interaction — doors, stairs, the drawer puzzle, save
       point, prop examines — is locked out until the menu closes. */
    int lock = (game_state == STATE_MENU);
    if (area == STATE_KITCHEN_DINING) {
        apply_collision_kitchen_dining();
        apply_height();
        update_zombies();
        update_spiders();
        update_rabisus();
        sml_meds_update();
        kitchen_stove_update();
        if (!lock) stove_puzzle_update();   /* stove prompt + puzzle trigger */
        if (!lock && kitchen_door_triggered()) {
            pending_area = STATE_DELIVERY_AREA;
            door_anim_start(DOOR_PANEL_OUTER);
            game_state   = STATE_DOOR_ANIM;
            cdaudio_stop();   /* silence during the transition; delivery music
                                 starts when the delivery area finishes loading */
        } else if (!lock && to_reception_door_triggered()) {
            pending_area = STATE_RECEPTION;
            door_anim_start(DOOR_PANEL_INNER);
            game_state   = STATE_DOOR_ANIM;
            cdaudio_stop();   /* silence during the transition; reception music
                                 starts when reception finishes loading */
        }
    } else if (area == STATE_RECEPTION) {
        /* Placeholder room: walkable, flat-shaded. Walls-only collision (no
           kitchen props, which are global and would collide invisibly here). */
        save_points_update();
        apply_collision_reception();
        apply_height();
        item_pickups_update();
        if (!lock && save_point_triggered()) {
            /* Stand at the save point and press Circle to open the save flow.
               current_area is already STATE_RECEPTION, so the menu returns here. */
            save_menu_open();
            game_state = STATE_SAVE_MENU;
        } else if (!lock && reception_door_triggered()) {
            pending_area = STATE_KITCHEN_DINING;
            door_anim_start(DOOR_PANEL_INNER);   /* same interior double door */
            game_state   = STATE_DOOR_ANIM;
            cdaudio_stop();   /* silence during the transition; kitchen music
                                 resumes when the kitchen finishes loading */
        } else if (!lock && wdoor_triggered()) {
            pending_area = STATE_PIANO_ROOM;
            door_anim_start(DOOR_PANEL_WOOD);    /* single wooden door */
            game_state   = STATE_DOOR_ANIM;
            cdaudio_stop();
        } else if (!lock && cdoor_triggered()) {
            pending_area = STATE_CONSERVATORY;
            door_anim_start(DOOR_PANEL_WOOD);    /* single wooden door */
            game_state   = STATE_DOOR_ANIM;
            cdaudio_stop();
        } else if (!lock && hdoor_triggered()) {
            pending_area = STATE_2F_HALL;
            door_anim_start(DOOR_PANEL_WOOD);    /* single wooden door */
            game_state   = STATE_DOOR_ANIM;
            cdaudio_stop();
        } else if (!lock && edoor_triggered()) {
            /* Double door on the upper floor's east wall, into the East Hall. */
            pending_area = STATE_EAST_HALL;
            door_anim_start(DOOR_PANEL_INNER);   /* interior double door */
            game_state   = STATE_DOOR_ANIM;
            cdaudio_stop();
        } else if (!lock && ndoor_triggered()) {
            /* North-west door on the upper floor, into the West Corridor. A
               single wooden leaf, so the same transition as the ground-floor
               piano/conservatory doors. */
            pending_area = STATE_WEST_CORRIDOR;
            door_anim_start(DOOR_PANEL_WOOD);    /* single wooden door */
            game_state   = STATE_DOOR_ANIM;
            cdaudio_stop();
        }
    } else if (area == STATE_EAST_HALL) {
        /* Flat single-floor room; the shared wall collision routine is generic
           over current_collision_room (other rooms' props gate themselves out,
           and east_hall_init clears the two that don't). */
        apply_collision_reception();
        apply_height();
        update_zombies();      /* none placed yet, but keeps the room uniform */
        update_spiders();
        update_rabisus();
        item_pickups_update();
        if (!lock && east_hall_wdoor_triggered()) {
            pending_area = STATE_RECEPTION;
            door_anim_start(DOOR_PANEL_INNER);   /* same interior double door */
            game_state   = STATE_DOOR_ANIM;
            cdaudio_stop();
        } else if (!lock && east_hall_edoor_triggered()) {
            /* Double door at the hall's east end, into the library — or into
               the Library Destroyed once the keystones have come back out of the
               Attic Exit's door. One doorway, two rooms behind it; the rule is
               library_destroyed_active() and lives in one place.

               ...unless the ceiling came down on the far side of it on the way
               out of the wrecked Library (east_hall_quake.h). The door then
               keeps its sign — the player is meant to try it — and Circle only
               says why nothing happens. Handled HERE rather than inside
               east_hall_edoor_triggered() so the trigger keeps meaning "the
               player pressed O at this door" and the room this door leads to
               stays the one decision made in this file. */
            if (game_flag(FLAG_EAST_HALL_RUBBLE)) {
                show_pickup_msg_raw("There is rubble behind the door!");
            } else {
                pending_area = library_destroyed_active() ? STATE_LIBRARY_DESTROYED
                                                          : STATE_LIBRARY;
                door_anim_start(DOOR_PANEL_INNER);   /* interior double door */
                game_state   = STATE_DOOR_ANIM;
                cdaudio_stop();
            }
        } else if (!lock && east_hall_sdoor_triggered()) {
            /* Single wooden door in the south wall, onto the East Stairwell's
               west landing. */
            pending_area = STATE_EAST_STAIRWELL;
            door_anim_start(DOOR_PANEL_WOOD);    /* single wooden door */
            game_state   = STATE_DOOR_ANIM;
            cdaudio_stop();
        }
    } else if (area == STATE_LIBRARY) {
        /* Flat single-floor room; the shared wall collision routine is generic
           over current_collision_room (other rooms' props gate themselves out,
           and library_init clears the two that don't). */
        apply_collision_reception();
        apply_height();
        update_zombies();      /* one in the northern strip */
        update_spiders();      /* three on the reading room's ceiling */
        update_rabisus();
        item_pickups_update();
        if (!lock && library_wdoor_triggered()) {
            pending_area = STATE_EAST_HALL;
            door_anim_start(DOOR_PANEL_INNER);   /* same interior double door */
            game_state   = STATE_DOOR_ANIM;
            cdaudio_stop();
        } else if (!lock && library_sdoor_triggered()) {
            /* Single wooden door in the reading room's south wall, onto the
               East Stairwell's east landing. */
            pending_area = STATE_EAST_STAIRWELL;
            door_anim_start(DOOR_PANEL_WOOD);    /* single wooden door */
            game_state   = STATE_DOOR_ANIM;
            cdaudio_stop();
        }
    } else if (area == STATE_LIBRARY_DESTROYED) {
        /* The Library's replacement once FLAG_HADAD_TWO is set. Identical
           handling to the branch above — same flat single floor, same two doors
           to the same two rooms — plus the Hadad encounter this room now hosts.
           item_pickups_update stays: it is a no-op with none placed, and it is
           what a later addition needs. */
        apply_collision_reception();
        apply_height();
        update_hadads();          /* the U-shaped chase */
        hadad_library_update();   /* ...and the crawl gap's prompt watching for O */
        item_pickups_update();
        if (!lock && library_destroyed_wdoor_triggered()) {
            pending_area = STATE_EAST_HALL;
            door_anim_start(DOOR_PANEL_INNER);   /* same interior double door */
            game_state   = STATE_DOOR_ANIM;
            cdaudio_stop();
        } else if (!lock && !hadad_library_seals_sdoor() &&
                   library_destroyed_sdoor_triggered()) {
            pending_area = STATE_EAST_STAIRWELL;
            door_anim_start(DOOR_PANEL_WOOD);    /* single wooden door */
            game_state   = STATE_DOOR_ANIM;
            cdaudio_stop();
        }
    } else if (area == STATE_EAST_STAIRWELL) {
        /* Two disconnected landings, both flat at y=0; the shared wall collision
           routine is generic over current_collision_room (other rooms' props
           gate themselves out, and east_stairwell_init clears the two that
           don't). Only one of the two doors is reachable from a given arrival —
           the chain-link fences do not open. */
        apply_collision_reception();
        apply_height();
        update_zombies();      /* none placed yet, but keeps the room uniform */
        update_spiders();
        update_rabisus();
        item_pickups_update();
        sml_meds_update();     /* the east landing's medipac */
        if (!lock && east_stairwell_wdoor_triggered()) {
            pending_area = STATE_EAST_HALL;
            door_anim_start(DOOR_PANEL_WOOD);    /* same single wooden door */
            game_state   = STATE_DOOR_ANIM;
            cdaudio_stop();
        } else if (!lock && east_stairwell_edoor_triggered()) {
            /* The east landing's door into the library — same swap as the East
               Hall's: FLAG_HADAD_TWO decides which of the two rooms is there. */
            pending_area = library_destroyed_active() ? STATE_LIBRARY_DESTROYED
                                                      : STATE_LIBRARY;
            door_anim_start(DOOR_PANEL_WOOD);    /* same single wooden door */
            game_state   = STATE_DOOR_ANIM;
            cdaudio_stop();
        } else if (!lock && east_stairwell_stairs_triggered()) {
            /* Climb the east landing's stairs to the attic (stair-climb
               transition, the same one the conservatory <-> 2F hall pair use). */
            pending_area = STATE_ATTIC_STAIRWELL;
            stair_anim_start(STAIR_UP);
            game_state   = STATE_STAIR_ANIM;
            cdaudio_stop();
        }
    } else if (area == STATE_ATTIC_STAIRWELL) {
        /* Flat single-floor attic; the shared wall collision routine is generic
           over current_collision_room (other rooms' props gate themselves out,
           and attic_stairwell_init clears the two that don't). */
        apply_collision_reception();
        apply_height();
        update_zombies();      /* none placed yet, but keeps the room uniform */
        update_spiders();
        update_rabisus();
        item_pickups_update();
        if (!lock && attic_stairwell_stairs_triggered()) {
            /* Back down to the East Stairwell's east landing. */
            pending_area = STATE_EAST_STAIRWELL;
            stair_anim_start(STAIR_DOWN);
            game_state   = STATE_STAIR_ANIM;
            cdaudio_stop();
        } else if (!lock && attic_stairwell_exit_door_triggered()) {
            /* North through the west room's door into the Attic Exit. */
            pending_area = STATE_ATTIC_EXIT;
            door_anim_start(DOOR_PANEL_WOOD);
            game_state   = STATE_DOOR_ANIM;
            cdaudio_stop();
        }
    } else if (area == STATE_ATTIC_EXIT) {
        /* Flat single-floor room; the shared wall collision routine is generic
           over current_collision_room (other rooms' props gate themselves out,
           and attic_exit_init clears the two that don't). */
        apply_collision_reception();
        apply_height();
        update_zombies();      /* none placed yet, but keeps the room uniform */
        update_spiders();
        update_rabisus();
        update_tentacles();    /* two in front of each of the north-wall levers */
        item_pickups_update();
        if (!lock) lightswitch_update();   /* the four levers + their light cones */
        /* The north-wall exit door: its keystone board, or — once unlocked —
           the press that walks through it. */
        if (!lock && !lightswitch_puzzle_active()) exit_door_puzzle_update();
        /* A Circle that just tripped the payoff must not also be read as the
           exit door: the cutscene has already flung the camera across the room
           by the time this runs. Nor may one press work both doors at once —
           they are at opposite ends of the room, so only the range check keeps
           them apart, but the puzzle's own Circle edge state is separate. */
        if (!lock && exit_door_exit_triggered()) {
            /* North through the unlocked exit door, onto the Garden Stairs. Its
               own transition: both leaves of the exit door swing away from the
               camera before the player walks through. */
            pending_area = STATE_GARDEN_STAIRS;
            door_anim_start(DOOR_PANEL_EXIT);
            game_state   = STATE_DOOR_ANIM;
            cdaudio_stop();
        } else if (!lock && !lightswitch_puzzle_active() &&
                   !exit_door_puzzle_active() && attic_exit_door_triggered()) {
            /* South through the door, back into the Attic Stairwell. */
            pending_area = STATE_ATTIC_STAIRWELL;
            door_anim_start(DOOR_PANEL_WOOD);
            game_state   = STATE_DOOR_ANIM;
            cdaudio_stop();
        }
    } else if (area == STATE_GARDEN_STAIRS) {
        /* A five-flight switchback shaft. The shared wall collision routine is
           generic over current_collision_room, and apply_height picks the right
           landing/flight out of the stacked floor zones (see the ordering note
           in garden_stairs_floor_zones_init). */
        save_points_update();
        apply_collision_reception();
        apply_height();
        update_zombies();      /* none placed yet, but keeps the room uniform */
        update_spiders();
        update_rabisus();
        item_pickups_update();
        sml_meds_update();     /* the bottom landing's medipac */
        if (!lock && save_point_triggered()) {
            /* Save point on the top east landing; current_area is already
               STATE_GARDEN_STAIRS, so the menu returns here. */
            save_menu_open();
            game_state = STATE_SAVE_MENU;
        } else if (!lock && garden_stairs_door_triggered()) {
            /* South through the exit door, back into the Attic Exit. */
            pending_area = STATE_ATTIC_EXIT;
            door_anim_start(DOOR_PANEL_EXIT);
            game_state   = STATE_DOOR_ANIM;
            cdaudio_stop();
        } else if (!lock && garden_stairs_cg_door_triggered()) {
            /* West through the cage door at the bottom, out into the Garden
               Courtyard. Same two-leaf transition as the exit door above —
               it is the same kind of caged double gate. */
            pending_area = STATE_GARDEN_COURTYARD;
            door_anim_start(DOOR_PANEL_EXIT);
            game_state   = STATE_DOOR_ANIM;
            cdaudio_stop();
        }
    } else if (area == STATE_GARDEN_COURTYARD) {
        /* Flat-ish walled garden: three terraces a step apart, so the shared
           wall collision routine (generic over current_collision_room) and
           apply_height's non-overlapping floor zones handle it between them. */
        apply_collision_reception();
        apply_height();
        update_zombies();      /* none placed yet, but keeps the room uniform */
        update_spiders();
        update_rabisus();
        rabisu_boss_update();  /* arms the reveal, then watches for the kill */
        item_pickups_update();
        sml_meds_update();
        /* The gate is sealed for the whole encounter — reveal, fight and death.
           The seal is tested FIRST so the trigger is never even polled while it
           holds; garden_courtyard_door_arm() is called when the seal lifts (see
           rabisu_boss.c) so the Circle edge state is not left stale. */
        if (!lock && !rabisu_boss_seals_door() && garden_courtyard_door_triggered()) {
            /* East through the cage door, back onto the Garden Stairs. */
            pending_area = STATE_GARDEN_STAIRS;
            door_anim_start(DOOR_PANEL_EXIT);
            game_state   = STATE_DOOR_ANIM;
            cdaudio_stop();
        } else if (!lock && !rabisu_boss_seals_door() &&
                   garden_courtyard_ngate_triggered()) {
            /* North through the gate in the hedge, into Fountain Square. The one
               transition in the game that is not a door: DOOR_PANEL_GATE, a
               single iron leaf swinging over the length of SFX_GATE. */
            pending_area = STATE_FOUNTAIN_SQUARE;
            door_anim_start(DOOR_PANEL_GATE);
            game_state   = STATE_DOOR_ANIM;
            cdaudio_stop();
        }
    } else if (area == STATE_FOUNTAIN_SQUARE) {
        /* Flat parterre: one paved plane, so the shared wall collision routine
           (generic over current_collision_room) and a single floor zone are the
           whole of it. */
        apply_collision_reception();
        apply_height();
        update_zombies();      /* none placed — see world_seed_room's note on */
        update_spiders();      /* why this room has to stay empty            */
        update_rabisus();
        item_pickups_update();
        sml_meds_update();
        if (!lock && fountain_square_gate_triggered()) {
            /* South back through the same gate, into the Garden Courtyard. */
            pending_area = STATE_GARDEN_COURTYARD;
            door_anim_start(DOOR_PANEL_GATE);
            game_state   = STATE_DOOR_ANIM;
            cdaudio_stop();
        } else if (!lock && fountain_square_ngate_triggered()) {
            /* North through the gate in the far hedge, on to the Outside
               Catacombs. Same DOOR_PANEL_GATE transition as the south one —
               it is the same iron leaf. */
            pending_area = STATE_OUTSIDE_CATACOMBS;
            door_anim_start(DOOR_PANEL_GATE);
            game_state   = STATE_DOOR_ANIM;
            cdaudio_stop();
        } else if (!lock && fountain_square_egate_triggered()) {
            /* East through the gate in the side hedge, into Maze One. The same
               iron leaf again, so the same DOOR_PANEL_GATE transition. */
            pending_area = STATE_MAZE_ONE;
            door_anim_start(DOOR_PANEL_GATE);
            game_state   = STATE_DOOR_ANIM;
            cdaudio_stop();
        } else if (!lock && fountain_square_wgate_triggered()) {
            /* West through the gate in the opposite side hedge, into the Rear
               Gate — the last of this room's four leaves to lead anywhere. The
               same iron leaf once more, so the same DOOR_PANEL_GATE. */
            pending_area = STATE_REAR_GATE;
            door_anim_start(DOOR_PANEL_GATE);
            game_state   = STATE_DOOR_ANIM;
            cdaudio_stop();
        }
    } else if (area == STATE_REAR_GATE) {
        /* NOT a flat room, unlike every other one in the garden chain: the
           gravel path at the south end climbs 500 units to the double door in
           the brick wall. That costs nothing here, though — apply_height walks
           the room's floor-zone list and rear_gate_floor_zones_init puts a
           FLOOR_RAMP at the head of it, and apply_collision_reception is generic
           over current_collision_room and Y-aware regardless of the room's
           multi_level flag. So the two calls are the same two as everywhere. */
        apply_collision_reception();
        apply_height();
        update_zombies();      /* none placed — the room is seeded empty, but */
        update_spiders();      /* the updaters cost nothing on empty arrays   */
        update_rabisus();
        item_pickups_update();
        sml_meds_update();
        /* The corridor lever and the two grinders it drives. Takes `lock`
           itself rather than being wrapped in it: an open menu suppresses the
           PRESS, but grinders already on the move keep moving. */
        grinder_puzzle_update(lock);
        /* The plinth's inscription on the lawn. Takes `lock` itself, as the
           grinders do; it is a read with no state behind it, so there is
           nothing for an open menu to interrupt beyond the press. */
        rear_gate_plinth_update(lock);
        if (!lock && rear_gate_gate_triggered()) {
            /* East back through the same gate, into Fountain Square. The room's
               other two modelled gates (west and north) are drawn shut and lead
               nowhere, so this is the only way out. */
            pending_area = STATE_FOUNTAIN_SQUARE;
            door_anim_start(DOOR_PANEL_GATE);
            game_state   = STATE_DOOR_ANIM;
            cdaudio_stop();
        } else if (!lock && rear_gate_sdoor_triggered()) {
            /* South through the double door at the top of the ramp, into the
               West Corridor and back inside the house. Not a gate: this is a
               real door, and the user asked for the Delivery/Kitchen
               transition, which is DOOR_PANEL_OUTER. */
            pending_area = STATE_WEST_CORRIDOR;
            door_anim_start(DOOR_PANEL_OUTER);
            game_state   = STATE_DOOR_ANIM;
            cdaudio_stop();
        }
    } else if (area == STATE_WEST_CORRIDOR) {
        /* Flat single-floor room (all four collision floor planes are y=0), so
           the shared wall collision routine — generic over
           current_collision_room — and a single floor zone are the whole of
           it. */
        apply_collision_reception();
        apply_height();
        update_zombies();      /* none placed yet, but keeps the room uniform */
        update_spiders();
        update_rabisus();
        item_pickups_update();
        sml_meds_update();
        if (!lock && west_corridor_ndoor_triggered()) {
            /* North through the double door, out to the top of the Rear Gate's
               ramp. The Delivery/Kitchen transition, as on the other side. */
            pending_area = STATE_REAR_GATE;
            door_anim_start(DOOR_PANEL_OUTER);
            game_state   = STATE_DOOR_ANIM;
            cdaudio_stop();
        } else if (!lock && west_corridor_edoor_triggered()) {
            /* East through the single wooden door, back onto reception's upper
               floor. */
            pending_area = STATE_RECEPTION;
            door_anim_start(DOOR_PANEL_WOOD);
            game_state   = STATE_DOOR_ANIM;
            cdaudio_stop();
        }
    } else if (area == STATE_MAZE_ONE) {
        /* Flat maze: all twenty-seven collision floor planes are at y=0, so the
           shared wall collision routine (generic over current_collision_room)
           and a single floor zone are the whole of it. */
        apply_collision_reception();
        apply_height();
        update_zombies();      /* none placed — the room is seeded empty, but */
        update_spiders();      /* the updaters cost nothing on empty arrays   */
        update_rabisus();
        item_pickups_update();
        sml_meds_update();
        if (!lock && maze_one_gate_triggered()) {
            /* West back through the same gate, into Fountain Square. */
            pending_area = STATE_FOUNTAIN_SQUARE;
            door_anim_start(DOOR_PANEL_GATE);
            game_state   = STATE_DOOR_ANIM;
            cdaudio_stop();
        } else if (!lock && maze_one_ngate_triggered()) {
            /* North through the gate in the far hedge, on into Maze Two. The
               same iron leaf, so the same DOOR_PANEL_GATE transition. */
            pending_area = STATE_MAZE_TWO;
            door_anim_start(DOOR_PANEL_GATE);
            game_state   = STATE_DOOR_ANIM;
            cdaudio_stop();
        }
    } else if (area == STATE_MAZE_TWO) {
        /* Flat maze, exactly as Maze One: all seventeen collision floor planes
           are at y=0, so the shared wall collision routine (generic over
           current_collision_room) and a single floor zone are the whole of it. */
        apply_collision_reception();
        apply_height();
        update_zombies();      /* none placed — the room is seeded empty, but */
        update_spiders();      /* the updaters cost nothing on empty arrays   */
        update_rabisus();
        item_pickups_update();
        sml_meds_update();
        if (!lock && maze_two_gate_triggered()) {
            /* South back through the same gate, into Maze One. */
            pending_area = STATE_MAZE_ONE;
            door_anim_start(DOOR_PANEL_GATE);
            game_state   = STATE_DOOR_ANIM;
            cdaudio_stop();
        }
    } else if (area == STATE_OUTSIDE_CATACOMBS) {
        /* Flat ground throughout — all sixteen collision floor planes are at
           y=0 — so the shared wall collision routine (generic over
           current_collision_room) and a single floor zone are the whole of it. */
        apply_collision_reception();
        apply_height();
        update_zombies();      /* none placed — see world_seed_room's note on */
        update_spiders();      /* why this room has to stay empty            */
        update_rabisus();
        item_pickups_update();
        sml_meds_update();
        if (!lock && outside_catacombs_gate_triggered()) {
            /* South back through the same gate, into Fountain Square. */
            pending_area = STATE_FOUNTAIN_SQUARE;
            door_anim_start(DOOR_PANEL_GATE);
            game_state   = STATE_DOOR_ANIM;
            cdaudio_stop();
        }
    } else if (area == STATE_CONSERVATORY) {
        /* Flat single-floor room; the shared wall/prop collision routine is
           generic over current_collision_room (other rooms' props gate
           themselves out). */
        apply_collision_reception();
        apply_height();
        update_zombies();      /* the small-room zombie */
        update_spiders();
        update_rabisus();
        update_demon_dogs();   /* the three dogs at the far end of the hall */
        update_tentacles();    /* the two tentacles near the copper pot */
        sml_meds_update();     /* the small-room medipac */
        copper_pot_update();   /* proximity pickup of the copper pot */
        if (!lock && condoor_triggered()) {
            pending_area = STATE_RECEPTION;
            door_anim_start(DOOR_PANEL_WOOD);    /* same single wooden door */
            game_state   = STATE_DOOR_ANIM;
            cdaudio_stop();
        } else if (!lock && stairs_triggered()) {
            /* Ascend the stairs to the second-floor hall (stair-climb transition). */
            pending_area = STATE_2F_HALL;
            stair_anim_start(STAIR_UP);
            game_state   = STATE_STAIR_ANIM;
            cdaudio_stop();
        }
    } else if (area == STATE_2F_HALL) {
        /* Flat single-floor room; the shared wall collision routine is generic
           over current_collision_room (other rooms' props gate themselves out). */
        apply_collision_reception();
        apply_height();
        update_zombies();         /* the two hall zombies (enemies act even in menu) */
        update_spiders();
        update_rabisus();
        if (!lock) trick_drawers_update();   /* proximity "interact" prompt + puzzle trigger */
        if (!lock && hall_2f_stairs_triggered()) {
            /* Descend the stairs back to the conservatory (stair-climb transition). */
            pending_area = STATE_CONSERVATORY;
            stair_anim_start(STAIR_DOWN);
            game_state   = STATE_STAIR_ANIM;
            cdaudio_stop();
        } else if (!lock && hall_2f_edoor_triggered()) {
            /* Far-east door out to reception's 2nd floor. */
            pending_area = STATE_RECEPTION;
            door_anim_start(DOOR_PANEL_WOOD);
            game_state   = STATE_DOOR_ANIM;
            cdaudio_stop();
        } else if (!lock && hall_2f_bdoor_e_triggered()) {
            /* East south-wall door into the master bedroom (its east wing). */
            bedroom_door_west = 0;
            pending_area = STATE_MASTER_BEDROOM;
            door_anim_start(DOOR_PANEL_WOOD);
            game_state   = STATE_DOOR_ANIM;
            cdaudio_stop();
        } else if (!lock && hall_2f_bdoor_w_triggered()) {
            /* West south-wall door into the master bedroom (its west wing). */
            bedroom_door_west = 1;
            pending_area = STATE_MASTER_BEDROOM;
            door_anim_start(DOOR_PANEL_WOOD);
            game_state   = STATE_DOOR_ANIM;
            cdaudio_stop();
        }
    } else if (area == STATE_MASTER_BEDROOM) {
        /* Flat single-floor room; the shared wall collision routine is generic
           over current_collision_room (other rooms' props gate themselves out,
           and master_bedroom_init clears the two that don't). */
        apply_collision_reception();
        apply_height();
        update_zombies();      /* none placed yet, but keeps the room uniform */
        update_spiders();
        update_rabisus();
        item_pickups_update();  /* the box of rounds in front of the bed */
        if (!lock && master_bedroom_wdoor_triggered()) {
            bedroom_door_west = 1;
            pending_area = STATE_2F_HALL;
            door_anim_start(DOOR_PANEL_WOOD);
            game_state   = STATE_DOOR_ANIM;
            cdaudio_stop();
        } else if (!lock && master_bedroom_edoor_triggered()) {
            bedroom_door_west = 0;
            pending_area = STATE_2F_HALL;
            door_anim_start(DOOR_PANEL_WOOD);
            game_state   = STATE_DOOR_ANIM;
            cdaudio_stop();
        }
    } else if (area == STATE_PIANO_ROOM) {
        /* Flat single-floor room; the reception wall/prop collision routine is
           generic over current_collision_room (reception's props are all out of
           this room's bounds, so their collide calls are no-ops here). */
        apply_collision_reception();
        apply_height();
        /* The room had no collectibles until the Anzu Tablet started dropping
           the Yellow Key Stone in front of the piano — without this tick it
           would never bob or be pickable. */
        item_pickups_update();
        if (!lock) piano_puzzle_update();  /* examine prompt + puzzle trigger */
        if (!lock) anzu_puzzle_update();   /* Anzu frame prompt + puzzle trigger */
        if (!lock && pdoor_triggered()) {
            pending_area = STATE_RECEPTION;
            door_anim_start(DOOR_PANEL_WOOD);    /* same single wooden door */
            game_state   = STATE_DOOR_ANIM;
            cdaudio_stop();
        }
    } else {
        apply_collision();
        apply_height();
        update_demon_dogs();
        update_zombies();
        update_spiders();
        update_rabisus();
        crates_update();
        keys_update();
        sml_meds_update();
        if (!lock) door_update();   /* Circle-to-open front door */
    }
    weapons_update();
    update_particles();
    bullet_hits_update();
    /* Area-tagged like the webs, so this one call covers every room branch above
       and costs nothing in the rooms with no flowers in them. */
    update_rafflesias();
    /* Likewise area-tagged, so this one call covers every room branch above and
       costs nothing in the rooms with no mushrooms in them. */
    update_mushrooms();
    /* Likewise area-tagged. It is the only enemy that acts on the CAMERA'S
       facing rather than just the player's position, which is exactly why it
       has to be in the camera-locked puzzle branches above too: a puzzle points
       the camera at a board, so a statue behind the player is unwatched for the
       whole of it. */
    update_living_statues();
    /* Likewise area-tagged, and in the camera-locked branches above for the same
       reason the statue is: he keeps walking his corridor through a puzzle, and
       under flag one he is what stands between the player and the way back. */
    update_hadads();
    webs_update();            /* spider webs in flight (area-tagged, so free
                                 in rooms that have none) */
    player_status_update();   /* ticks the web's poison timer down */
}

/* Draw an area's world + entities only (player overlays come separately). */
static void draw_current_area(RenderContext *ctx, GameState area) {
    if (area == STATE_KITCHEN_DINING)
        kitchen_dining_draw(ctx);
    else if (area == STATE_RECEPTION)
        reception_draw(ctx);
    else if (area == STATE_PIANO_ROOM)
        piano_room_draw(ctx);
    else if (area == STATE_CONSERVATORY)
        conservatory_draw(ctx);
    else if (area == STATE_2F_HALL)
        hall_2f_draw(ctx);
    else if (area == STATE_MASTER_BEDROOM)
        master_bedroom_draw(ctx);
    else if (area == STATE_EAST_HALL)
        east_hall_draw(ctx);
    else if (area == STATE_LIBRARY)
        library_draw(ctx);
    else if (area == STATE_LIBRARY_DESTROYED)
        library_destroyed_draw(ctx);
    else if (area == STATE_EAST_STAIRWELL)
        east_stairwell_draw(ctx);
    else if (area == STATE_ATTIC_STAIRWELL)
        attic_stairwell_draw(ctx);
    else if (area == STATE_ATTIC_EXIT)
        attic_exit_draw(ctx);
    else if (area == STATE_GARDEN_STAIRS)
        garden_stairs_draw(ctx);
    else if (area == STATE_GARDEN_COURTYARD)
        garden_courtyard_draw(ctx);
    else if (area == STATE_FOUNTAIN_SQUARE)
        fountain_square_draw(ctx);
    else if (area == STATE_OUTSIDE_CATACOMBS)
        outside_catacombs_draw(ctx);
    else if (area == STATE_MAZE_ONE)
        maze_one_draw(ctx);
    else if (area == STATE_MAZE_TWO)
        maze_two_draw(ctx);
    else if (area == STATE_REAR_GATE)
        rear_gate_draw(ctx);
    else if (area == STATE_WEST_CORRIDOR)
        west_corridor_draw(ctx);
    else
        delivery_area_draw(ctx);
}

/* Player overlays shared by every area: blood particles, the swingable weapon,
   the poison wash and the debug collision view. NOT the HUD — that is drawn by
   the caller, because the menu and save screens want these overlays but must
   not show the HUD (see src/hud.h for the full visibility rules). */
static void draw_player_systems(RenderContext *ctx) {
    draw_particles(ctx);
    bullet_hits_draw(ctx);  /* brief impact sprites (world-space billboards) */
#ifdef DEBUG_COLLISION
    /* Debug world-space overlays run HERE, right after the bullet-hit sprites and
       BEFORE weapons_draw: at this point the GTE still holds the scene's camera
       view matrix (same as bullet_hits_draw relies on). weapons_draw replaces it
       with the held weapon's view-space matrix, which is why these must precede
       it — running them after left every projected vertex in the wrong space and
       nothing showed. */
    debug_draw_walls(ctx);       /* purple: walls + props that block a shot */
    graveolver_debug_draw(ctx);  /* yellow: the gun's hit cone              */
#endif
    weapons_draw(ctx);
    player_draw_status_overlay(ctx);   /* poison wash: over the scene, under the HUD */
#ifdef DEBUG_COLLISION
    debug_draw_coords(ctx);      /* 2D panel — screen space, so it goes last */
#endif
}

/* Debug overlay: held items + the scrolling compass.
   Uses FntSort (sorts the font into the render OT, drawn by the single
   DrawOTagEnv in flip_buffers) rather than FntFlush. FntFlush draws IMMEDIATELY
   on its own — which races/hangs the GPU when it overlays a heavy scene still
   being drawn (Reception's ~951 prims), freezing the game while the music plays.
   FntSort has no immediate draw, so it composes safely with any scene.
   Coordinates are already shown by debug_draw_coords (the top panel), so they
   are not repeated here. */
static void draw_debug_overlay(RenderContext *ctx) {
    if (!debug_mode) return;
    uint32_t *ot = ctx->buffers[ctx->active_buffer].ot;
    int k;

    /* Performance readout (right side): effective FPS, vblanks/frame (1 = full
       rate), GPU vs cpu bound, and packet bytes queued (~scene load; est. prims
       at ~28 B each). */
    {
        /* NOTE ON "GPU-bound": perf_gpu_busy is sampled before the DrawSync, so
           it reports only that the GPU was still working through the previous
           frame — true of any non-trivial scene, at 60fps as well as 30. Treat
           it as "the GPU is doing something", NOT as proof that fill is the
           bottleneck. The isolation switches below are what actually answer
           that, by removing one part of the scene at a time. */
        static const char *exp_name[5] = {
            "", "no mesh", "no frustum", "cull 2000", "cull 1800"
        };
        char pbuf[64];
        int vb  = perf_frame_vblanks < 1 ? 1 : perf_frame_vblanks;
        int fps = 60 / vb;   /* assumes 60Hz NTSC; VB is region-independent */
        int exp = DEBUG_EXPERIMENT();
        /* U/D/G are HBLANKS — 262 to a 60Hz frame, so they read almost as
           percentages of one. U = game logic, D = queueing the scene (CPU, and
           where the culls' own cost lands), G = time spent WAITING for the GPU.
           G near zero means the CPU is the wall however busy the GPU looked;
           G large means fill is. See the note in render.h.
           The prim estimate divides by 40, POLY_FT4's real size — the old 28
           overstated every count in this room by 40%. */
        snprintf(pbuf, sizeof(pbuf),
                 "FPS%d VB%d\n%dB ~%dp\nU%d D%d G%d\nT%d %s",
                 fps, vb,
                 perf_packet_bytes, perf_packet_bytes / 40,
                 perf_ticks_update, perf_ticks_draw, perf_ticks_gpu,
                 perf_ticks_now(), exp_name[exp]);
        ctx->next_packet = FntSort(&ot[1], ctx->next_packet, 196, 140, pbuf);
    }

    /* Everything below is the AUTHORING overlay (level 2+). It is deliberately
       NOT drawn at level 1: it costs ~250 primitives a frame and was making the
       perf readout above measure itself. See the Select comment in camera.c. */
    if (debug_mode != 2 && debug_mode != 3) return;

    /* Scrolling compass tape, just below the coordinate panel.
       80 chars = 360deg, 10 chars per 45deg; N->NE->E->SE->S->SW->W->NW matches
       increasing cam_rot (CW). */
    {
        static const char tape[] =
            "N         NE        E         SE        S         SW        W         NW        ";
        int rot   = ((cam_rot % 4096) + 4096) % 4096;
        int pos   = rot * 80 / 4096;
        int start = ((pos - 20) % 80 + 80) % 80;
        char cbuf[41];
        for (k = 0; k < 40; k++)
            cbuf[k] = tape[(start + k) % 80];
        cbuf[40] = '\0';
        ctx->next_packet = FntSort(&ot[1], ctx->next_packet, 0, 32, cbuf);
    }

    /* Held keys down the left side. */
    {
        int y = 44;
        for (k = 0; k < MAX_KEY_TYPES; k++)
            if (player_keys & (1 << k)) {
                ctx->next_packet = FntSort(&ot[1], ctx->next_packet, 4, y,
                                           key_type_names[k]);
                y += 10;
            }
    }

    /* Camera coordinates, bottom-left (as before). */
    {
        char cbuf[32];
        snprintf(cbuf, sizeof(cbuf), "X:%d\nY:%d\nZ:%d", cam_x, cam_y, cam_z);
        ctx->next_packet = FntSort(&ot[1], ctx->next_packet, 4, 210, cbuf);
    }
}

/* Put the red LOADING screen on the television NOW, part-way through a frame,
   instead of at the bottom of it.

   A frame's drawing is not displayed until the flip at the END of the loop, so
   anything that blocks inside a frame — a title-screen Load Game reading a room
   off the disc — leaves the PREVIOUS frame up for the whole wait. Two draws and
   two flips fill BOTH framebuffers, so whichever one is being displayed holds
   the loading screen before the reads begin; it is the same trick, for the same
   reason, as the pair of flips main() does at boot. The third draw is for the
   frame we are in the middle of: without it the loop's own flip at the bottom
   would put an empty buffer up and the screen would blink to the clear colour
   mid-load. */
static void show_loading_screen_now(RenderContext *ctx) {
    draw_loading_screen(ctx);
    flip_buffers(ctx);
    draw_loading_screen(ctx);
    flip_buffers(ctx);
    draw_loading_screen(ctx);
}

/* Game-over / restart screen (area-agnostic). */
static void draw_lose_screen(RenderContext *ctx) {
    if (flash_timer > 0) {
        flash_timer--;
        uint8_t r = (flash_timer / 6) % 2 == 0 ? 200 : 0;
        setRGB0(&ctx->buffers[ctx->active_buffer].draw_env, r, 0, 0);
    } else if (pad_buff_len[0] &&
               (~((PadResponse *)pad_buff[0])->btn & PAD_START)) {
        reset_game(ctx);
        game_state = STATE_TITLE;
    } else {
        setRGB0(&ctx->buffers[ctx->active_buffer].draw_env, 80, 0, 0);
        FntPrint(gameover_fnt, "          GAME OVER\n    PRESS START TO RESTART");
        FntFlush(gameover_fnt);
    }
}

int main(int argc, const char **argv) {
    ResetGraph(0);

    RenderContext ctx;
    setup_context(&ctx, SCREEN_XRES, SCREEN_YRES,
                  TITLE_BG_R, TITLE_BG_G, TITLE_BG_B);

    perf_timer_init();   /* root counter 1: the debug meter's section timers */
    InitGeom();
    gte_SetGeomScreen(256);
    gte_SetGeomOffset(160, 120);

    SPI_Init(&poll_cb);

    /* Show loading screen while CD assets initialise.
       Two flips fill both double-buffer framebuffers before blocking reads begin. */
    draw_loading_screen(&ctx);
    flip_buffers(&ctx);
    draw_loading_screen(&ctx);
    flip_buffers(&ctx);

    /* ROOM GEOMETRY IS NOT LOADED HERE. Each room's mesh is read into the shared
       arena when the player walks into it (<room>_load_geometry, called from the
       STATE_LOADING branch below) — see src/room_arena.h. What is left in these
       calls is the texture side: texmgr registrations that keep a RAM copy so the
       entry-time VRAM upload stays a pure LoadImage with no CD read.

       LoadImage itself is only safe before the main render loop starts, which is
       why the texture work still happens here and not on entry. */
    delivery_area_init();
    kitchen_load_assets();     /* kitchen textures (geometry streams on entry) */
    reception_load_assets();   /* reception's unique textures -> RAM */
    piano_room_load_assets();  /* piano room textures (prpl_wlppr streamed) */
    piano_props_load_assets(); /* piano + bookcase props (streamed textures) */
    conservatory_load_assets();/* conservatory streamed textures */
    hall_2f_load_assets();     /* 2F hall streamed textures */
    master_bedroom_load_assets();/* master bedroom streamed textures */
    east_hall_load_assets();   /* east hall texture slots (all owned elsewhere) */
    library_load_assets();     /* library texture slots (all owned elsewhere) */
    library_destroyed_load_assets();/* same seven slots, likewise all owned elsewhere */
    east_stairwell_load_assets();/* east stairwell streamed textures */
    attic_stairwell_load_assets();/* attic stairwell streamed textures */
    attic_exit_load_assets();  /* attic exit streamed textures */
    exit_door_puzzle_load_assets();/* the exit door's fixed magenta stone icon */
    garden_stairs_load_assets();/* garden stairs streamed textures */
    garden_courtyard_load_assets();/* garden courtyard texture slots */
    fountain_square_load_assets(); /* fountain square texture slots  */
    outside_catacombs_load_assets();/* outside catacombs texture slots */
    maze_one_load_assets();    /* maze one texture slots (owns only PIPE) */
    maze_two_load_assets();    /* maze two texture slots (owns only PLINTH) */
    rear_gate_load_assets();   /* rear gate texture slots (owns the two retargets,
                                  PLNTHRG and DBLDRRG) */
    west_corridor_load_assets();/* west corridor texture slots — TIM_SLOT only; it
                                   owns no texture and registers nothing */
    chainlink_doors_load_assets();/* chainlink gate prop geometry + texture header */
    levers_load_assets();      /* wall lever prop geometry (flat-shaded, no texture) */
    trick_drawers_load_assets();/* 2F hall chest-of-drawers prop + texture */
    concrete_props_load_assets();/* concrete block/chair props + shared texture */
    copper_pot_load_assets();  /* copper pot collectible (texture deferred, key slot) */
    fatdoors_load_assets();    /* kitchen entryway doors (texture + geometry) */
    fatdoors_init();
    tentacles_load_assets();   /* tentacle enemy sprites (resident) */
    tentacles_init();          /* conservatory + attic exit tentacles */
    intro_load_assets();       /* opening sequence's mansion still (texture) */
    door_anim_load_assets();   /* level-transition door panel (texture) */
    stair_anim_load_assets();  /* conservatory<->2F stair-climb transition (upstairs tex) */
    collision_init();
    floor_zones_init();
    crates_init();
    dining_tables_init();      /* static kitchen props (loads DINTABLE.SMD) */
    save_points_init();        /* reusable save-point prop (loads SAVEPT.SMD) */
    dresser_load_assets();     /* reusable dresser prop: geometry + preload its
                                  streamed texture (uploaded on reception entry) */
    grinder_load_assets();     /* reusable grinder prop: geometry + its texture,
                                  which owns its VRAM outright and so goes up
                                  here ONCE — there is no per-entry upload */
    keys_init();
    sml_meds_init();
    anzu_tex_load();               /* the six Anzu tiles (LoadImage: startup only) */
    item_pickups_load_textures();  /* Grave-olver + rounds sprites (LoadImage: startup only) */
    bullet_hits_load_texture();    /* bullet-impact sprite (resident, all levels) */
    door_init();
    demon_dogs_init();
    zombies_load_textures();   /* LoadImage at startup only (see TEXTURING_NOTES) */
    zombies_init();            /* capture spawn defaults (none placed yet) */
    spiders_load_textures();   /* same rule: CD read at startup only. The two body
                                  sprites go through texmgr because the Rafflesia
                                  time-shares their VRAM slots — see below. */
    spiders_init();
    rafflesias_load_assets();  /* garden flower sprites: REGISTERED here, uploaded
                                  on entry to the Outside Catacombs (they sit in
                                  the spiders' two VRAM slots; see rafflesia.h) */
    rafflesias_init();         /* the three Outside Catacombs beds */
    mushrooms_load_textures(); /* startup CD read only, as above. The four
                                  sprites OWN their VRAM slots (no time-share,
                                  so nothing to re-upload on a transition) —
                                  see mushroom.h */
    mushrooms_init();
    living_statues_load_textures(); /* startup CD read only, as above. Both
                                  sprites OWN their VRAM slots in the 96-row
                                  band under the HUD — see living_statue.h */
    living_statues_init();
    hadads_load_textures();    /* startup CD read only, as above. All THREE
                                  sprites own their VRAM slots in the same
                                  96-row band under the HUD — see hadad.h */
    hadads_init();
    rabisus_load_assets();     /* boss MODEL, not sprites: one CD read for
                                  RABISU.SMD. Startup-only for the same reason —
                                  CD access is unsafe once the render loop runs.
                                  It owns no VRAM, so there is no upload step. */
    rabisus_init();            /* array starts empty; world_enter places it */
    weapons_init();
    sound_init();
    cdaudio_init();

    FntLoad(960, 0);
    gameover_fnt = FntOpen(40,  104, 240, 32, 0, 128);

    /* menu_init and title_init open their own font streams, so call them
       after FntLoad above. */
    hud_load_texture();        /* resident in VRAM for every room; startup-only */
    menu_init();
    title_init();

    GameState prev_state = STATE_TITLE;

    for (;;) {
        /* Look mode (hold Circle) only belongs to free roam, where game_state is
           the area itself. Every other state either takes the camera or freezes
           the game on a frame where Circle may still be down — title, loading,
           the door/stair transitions, the save flow — and none of them run
           update_camera to unwind the offset themselves, so drop it here.
           STATE_MENU is exempt: update_camera still runs and cancels it there. */
        if (game_state != current_area && game_state != STATE_MENU)
            camera_look_cancel();

        if (game_state == STATE_TITLE) {
            update_title();
            /* update_title may have routed us OUT of the title on this very
               frame — a Load Game, or a debug level-select jump. Both are about
               to block for seconds building the saved room, either in the
               STATE_LOADING branch on the next frame or in the frontend hook at
               the bottom of THIS one, and neither of them flips until it has
               finished. This is therefore the last chance to put anything on
               screen before the wait, so the title's own draw is REPLACED by the
               loading screen rather than followed by it — otherwise the player
               sits looking at a title with its menu already gone (which is
               exactly what they were).
               New Game is deliberately not included: it goes to STATE_INTRO,
               which opens on a white flash and reads nothing off the disc. */
            if (game_state == STATE_TITLE)
                draw_title(&ctx);
            else if (game_state == STATE_LOADING ||
                     game_state == STATE_DELIVERY_AREA)
                show_loading_screen_now(&ctx);
        } else if (game_state == STATE_INTRO) {
            /* Opening sequence (src/intro.c). It owns the clear colour for its
               whole run — the title's purple fading to black — and stops the
               music itself before it finishes, because the hook below reads the
               Delivery Area's mesh off the disc the moment we leave here. */
            intro_update();
            intro_draw(&ctx);
            if (intro_finished()) {
                handle_menu_open_arm();   /* Start may still be down from a skip */
                game_state = STATE_DELIVERY_AREA;
            }
        } else if (game_state == STATE_MENU) {
            /* The area keeps running in the background (enemies move, gravity
               applies); update_camera and weapon input self-disable while in
               STATE_MENU, so the player is effectively frozen. */
            if (game_over) {
                game_state = current_area; /* death closes menu, shows game over */
            } else {
                update_current_area(current_area);
                draw_current_area(&ctx, current_area);
                draw_player_systems(&ctx);   /* no HUD: the menu is open */
                menu_update();
                menu_draw(&ctx);
            }
        } else if (game_state == STATE_SAVE_MENU) {
            if (game_over) {
                game_state = current_area;   /* death cancels the save flow */
            } else {
                /* Frozen room backdrop (drawn, not updated: player, gravity and
                   enemies all hold still during the save flow) + the menu on top.
                   save_menu_update() defers the blocking card write by one frame
                   so the "SAVING" screen is already on-screen when it runs. */
                draw_current_area(&ctx, current_area);
                draw_player_systems(&ctx);   /* no HUD: the save flow is a menu */
                save_menu_update();
                save_menu_draw(&ctx);
            }
        } else if (game_state == STATE_LOADING) {
            /* Switch areas. This is where the incoming room is actually BUILT:
               its geometry is read off the disc, its textures are pushed into
               VRAM, and its sound bank is swapped in. The door animation has
               already faded to black and has five seconds of runway behind it,
               so a few hundred milliseconds of blocking CD access here is
               invisible — which is the whole reason the geometry is allowed to
               live on the disc instead of in RAM.

               Everything here runs on EVERY path into a room: a door, a stair
               climb, a title-screen Load Game, a debug level-select jump. Keying
               any of it off a door trigger instead would leave the other paths
               entering a room that was never built. */

            /* Idle the GPU first so the previous frame's async DrawOTagEnv is
               finished, then do the loads while quiescent (no draw is kicked
               until flip_buffers at the loop bottom). */
            DrawSync(0);

            /* Geometry: read the incoming room's mesh into the shared arena,
               evicting the one the player just left. room_arena_load brackets
               its own CdRead with cdaudio_suspend/resume — a data read issued
               while CD-DA streams hangs the drive
               (tools/TEXTURE_STREAMING_DEBUG.txt). */
            load_area_geometry(pending_area);

            /* Per-room SOUND streaming, and the one thing here that DOES touch
               the drive. The Rabisu's reveal clips are too big to keep in SPU
               RAM alongside the monster effects, so the sets timeshare one
               region (see sound.h). Three-way now:

                 GARDEN COURTYARD  -> BOSS.   The fight needs EMERGE/DMNSPEAK,
                                     and they leave too little spare for the
                                     flower's four, so no rafflesia can sound
                                     in that room. It is the one exception.
                 FOUNTAIN SQUARE   -> GARDEN. Both of these used to be on the
                 OUTSIDE CATACOMBS    boss bank purely to reach SFX_GATE, which
                                      cost them every monster sound in the game
                                      — Fountain Square was seeded empty for
                                      exactly that reason. The garden bank
                                      carries its own copy of the gate, so they
                                      get monster sounds back.
                 EVERYTHING ELSE   -> HOUSE. Including the Garden Stairs, kept
                                     there deliberately so house monsters stay
                                     placeable on it.

               sound_bank_select suspends
               CD-DA around its own reads and returns immediately when the right
               bank is already in, so this is safe to run unconditionally on
               every transition — including a title-screen load or a debug
               level-select jump, both of which arrive through here. Keyed on
               pending_area rather than on a door trigger for exactly that
               reason: every path into a room passes this line. */
            sound_bank_select(
                (pending_area == STATE_GARDEN_COURTYARD) ? SND_BANK_BOSS :
                (pending_area == STATE_FOUNTAIN_SQUARE ||
                 pending_area == STATE_OUTSIDE_CATACOMBS ||
                 pending_area == STATE_MAZE_ONE ||
                 pending_area == STATE_MAZE_TWO ||
                 pending_area == STATE_REAR_GATE) ? SND_BANK_GARDEN :
                SND_BANK_HOUSE);

            /* Upload reception's unique textures into VRAM from their resident RAM
               copies. Pure LoadImage, no CD access (the bytes were preloaded at
               startup), so no CD-DA suspend is needed and the drive never hangs. */
            if (pending_area == STATE_RECEPTION) {
                reception_upload_textures();
                dresser_upload_texture();   /* dresser prop texture -> kchn_wl slot */
            } else if (pending_area == STATE_PIANO_ROOM) {
                piano_room_upload_textures();   /* prpl_wlppr -> stove slot,
                                                   props -> stn_stl/kchn_tile */
            } else if (pending_area == STATE_CONSERVATORY) {
                conservatory_upload_textures(); /* 6 streamed slots (see module) */
            } else if (pending_area == STATE_2F_HALL) {
                hall_2f_upload_textures();       /* 4 streamed slots (see module) */
            } else if (pending_area == STATE_MASTER_BEDROOM) {
                master_bedroom_upload_textures();/* red_crpt + bed + dresser */
            } else if (pending_area == STATE_EAST_HALL) {
                east_hall_upload_textures();     /* cncrte + dresser */
            } else if (pending_area == STATE_LIBRARY) {
                library_upload_textures();       /* cncrte + prpl_wlppr + bookshelf */
            } else if (pending_area == STATE_LIBRARY_DESTROYED) {
                library_destroyed_upload_textures();/* the same three, same slots */
            } else if (pending_area == STATE_EAST_STAIRWELL) {
                east_stairwell_upload_textures();/* upstairs + chnlnk + cncrte */
            } else if (pending_area == STATE_ATTIC_STAIRWELL) {
                attic_stairwell_upload_textures();/* trck_clue + cncrte + upstairs
                                                     + strs + con_tile */
            } else if (pending_area == STATE_ATTIC_EXIT) {
                attic_exit_upload_textures();    /* xt_dr_lckd + chnlnk */
            } else if (pending_area == STATE_GARDEN_STAIRS) {
                garden_stairs_upload_textures(); /* xt_dr_cg + xt_dr_outr
                                                    + brick_wall + chnlnk */
            } else if (pending_area == STATE_GARDEN_COURTYARD) {
                garden_courtyard_upload_textures(); /* the same slot set — it
                                                       delegates to the above */
            } else if (pending_area == STATE_FOUNTAIN_SQUARE) {
                fountain_square_upload_textures();  /* ditto, plus fountain +
                                                       drain over brick_wall
                                                       and xt_dr_cg */
            } else if (pending_area == STATE_OUTSIDE_CATACOMBS) {
                outside_catacombs_upload_textures();/* ditto, plus con_tile and
                                                       the tablet + flower bed
                                                       over xt_dr_cg,
                                                       brick_wall and chnlnk */
            } else if (pending_area == STATE_MAZE_ONE) {
                maze_one_upload_textures();         /* ditto, plus the drain and
                                                       flower bed through their
                                                       owners' NARROW uploads,
                                                       and its own pipe over
                                                       brick_wall */
            } else if (pending_area == STATE_MAZE_TWO) {
                maze_two_upload_textures();         /* ditto, plus the flower bed
                                                       and Maze One's pipe through
                                                       NARROW uploads, and its own
                                                       plinth over xt_dr_cg */
            } else if (pending_area == STATE_REAR_GATE) {
                rear_gate_upload_textures();        /* ditto, plus the drain
                                                       through Fountain Square's
                                                       NARROW upload, and its own
                                                       retargeted plinth and
                                                       double door over chnlnk
                                                       and frnt_dr */
            } else if (pending_area == STATE_WEST_CORRIDOR) {
                west_corridor_upload_textures();    /* owns nothing: three NARROW
                                                       uploads on the kitchen's
                                                       and the piano room's RAM
                                                       copies (double_door,
                                                       red_crpt, prpl_wlppr) */
            } else if (pending_area == STATE_DELIVERY_AREA) {
                /* The conservatory streams over gravel/fence/brick/double_door
                   — restore them. Unconditional for the same reason as the
                   kitchen's restore below. */
                delivery_restore_textures();
            } else if (pending_area == STATE_KITCHEN_DINING) {
                /* Reception overwrites the kitchen's stn_stl/kchn_tile/red_crpt
                   slots (plus kchn_wl via the dresser), and the piano room
                   overwrites stove/stn_stl/kchn_tile — restore them all. Done
                   unconditionally: a title-screen load into the kitchen can
                   arrive from ANY current_area, and a redundant re-upload from
                   RAM is harmless (pure LoadImage, GPU idled above). */
                kitchen_restore_textures();
            }
            /* Once the copper pot is owned, keep its texture resident in the
               (spent) key slot on every room load so the menu icon is correct
               in any room — not just the conservatory that streams it for the
               world sprite. Harmless re-upload when the destination is the
               conservatory (already done above). */
            if (copper_pot_owned())
                copper_pot_upload_texture();

            /* THE SHARED ENEMY-SPRITE SLOTS. The spiders' two 128x128 sprites
               and the Rafflesia's live at the same VRAM x320/x384, y128 — VRAM
               had no 128-row hole left for a second pair, and the two enemies
               never share a room (spiders are house-interior, the flowers are
               garden). So exactly one pair is streamed in on every room entry,
               unconditionally, from their resident RAM copies: pure LoadImage,
               GPU already idled above. Keyed on pending_area so a title-screen
               load or a debug level-select jump gets it right too. */
            if (pending_area == STATE_OUTSIDE_CATACOMBS ||
                pending_area == STATE_MAZE_ONE ||
                pending_area == STATE_MAZE_TWO)
                rafflesias_upload_textures();
            else
                spiders_upload_textures();
            {
                TILE *bg = (TILE *)ctx.next_packet;
                setTile(bg);
                setXY0(bg, 0, 0);
                setWH(bg, SCREEN_XRES, SCREEN_YRES);
                setRGB0(bg, 0, 0, 0);
                addPrim(&ctx.buffers[ctx.active_buffer].ot[OT_LENGTH - 1], bg);
                ctx.next_packet += sizeof(TILE);
            }
            /* Snapshot the room we're leaving so its progress (defeated enemies,
               smashed crates, collected pickups, door state) persists. */
            world_leave(current_area);
            /* Back to the default wall standoff before the room sets itself up:
               a room that wants more says so in its own _init(), and one that
               says nothing must not inherit the last room's value. */
            collision_set_wall_radius(0);
            if (pending_area == STATE_KITCHEN_DINING) {
                kitchen_dining_init();
                /* Coming back from reception: spawn at the kitchen's "to
                   reception" door (far west wall), facing east into the kitchen,
                   instead of the default delivery-side spawn. */
                if (current_area == STATE_RECEPTION) {
                    cam_x   = -3100;
                    cam_y   = -149;
                    cam_vy  = 0;
                    cam_z   = -26;
                    cam_rot = 1024;   /* face +X, into the kitchen */
                }
                /* Start the music on arrival, from every direction — including
                   from the delivery area, which plays this same track. The two
                   used to share it across the transition without stopping, but
                   streaming the room's mesh suspends CD-DA mid-track, so what
                   the player heard was the score pausing over the load. Cheaper
                   to be consistent with every other door than to hide the seam. */
                cdaudio_play(CDAUDIO_MUSIC_TRACK, 1);
                kitchen_door_arm();
            } else if (pending_area == STATE_RECEPTION) {
                reception_init();
                /* Coming back from the piano room or conservatory: spawn at the
                   matching west-wall door, facing east into the room, instead
                   of the default east-door spawn. */
                if (current_area == STATE_PIANO_ROOM) {
                    cam_x   = -1290;
                    cam_y   = -189;
                    cam_vy  = 0;
                    cam_z   = -756;
                    cam_rot = 1024;   /* face +X, into reception */
                } else if (current_area == STATE_CONSERVATORY) {
                    cam_x   = -1290;
                    cam_y   = -189;
                    cam_vy  = 0;
                    cam_z   = 757;
                    cam_rot = 1024;   /* face +X, into reception */
                } else if (current_area == STATE_2F_HALL) {
                    /* Arrive at the 2nd-floor south door (upper floor y=-600),
                       facing +X into the room. apply_height settles cam_y. */
                    cam_x   = -1290;
                    cam_y   = -789;   /* upper-floor standing eye (-600 - 189) */
                    cam_vy  = 0;
                    cam_z   = -750;
                    cam_rot = 1024;   /* face +X, into reception */
                    hdoor_arm();      /* don't re-trigger on the held Circle */
                } else if (current_area == STATE_EAST_HALL) {
                    /* Arrive at the 2nd-floor EAST double door (upper floor
                       y=-600), facing -X back into the room. */
                    cam_x   = 1290;
                    cam_y   = -789;   /* upper-floor standing eye (-600 - 189) */
                    cam_vy  = 0;
                    cam_z   = 1071;
                    cam_rot = 3072;   /* face -X, into reception */
                    edoor_arm();      /* don't re-trigger on the held Circle */
                } else if (current_area == STATE_WEST_CORRIDOR) {
                    /* Arrive at the 2nd-floor NORTH-WEST door (upper floor
                       y=-600), facing +X into the room. The helper sets the
                       camera and arms that door itself. */
                    reception_spawn_northwest();
                }
                cdaudio_play(CDAUDIO_RECEPTION_TRACK, 1);   /* reception music */
            } else if (pending_area == STATE_PIANO_ROOM) {
                piano_room_init();
                /* This room's music depends on a saved flag, so it is chosen
                   AFTER savegame_apply_pending() below rather than here — see
                   the note there. */
            } else if (pending_area == STATE_CONSERVATORY) {
                conservatory_init();
                /* Coming back down from the 2F hall: spawn at the bottom of the
                   conservatory stairs (south-west corner), facing north (+Z)
                   into the room, instead of the default east-door spawn. */
                if (current_area == STATE_2F_HALL) {
                    cam_x   = -2450;
                    cam_y   = -189;
                    cam_vy  = 0;
                    cam_z   = -230;
                    cam_rot = 0;      /* face +Z, into the conservatory */
                    stairs_arm();     /* don't re-trigger the ascend on a held Circle */
                }
                cdaudio_play(CDAUDIO_PIANO_TRACK, 1);   /* shares the piano room music */
            } else if (pending_area == STATE_2F_HALL) {
                hall_2f_init();
                /* Coming in from reception's 2nd-floor door: spawn just inside the
                   hall's east door, facing west (-X) into the corridor, instead of
                   the default stairwell-top spawn. */
                if (current_area == STATE_RECEPTION) {
                    cam_x   = -120;
                    cam_y   = -189;
                    cam_vy  = 0;
                    cam_z   = -320;
                    cam_rot = 3072;   /* face -X, into the hall */
                    hall_2f_edoor_arm();
                } else if (current_area == STATE_MASTER_BEDROOM) {
                    /* Coming back up out of the bedroom: stand just north of
                       whichever south-wall door was used, facing into the
                       corridor (the spawn helper arms all three interactions). */
                    if (bedroom_door_west) hall_2f_spawn_bdoor_w();
                    else                   hall_2f_spawn_bdoor_e();
                }
                cdaudio_play(CDAUDIO_PIANO_TRACK, 1);   /* shares the piano room music */
            } else if (pending_area == STATE_MASTER_BEDROOM) {
                master_bedroom_init();
                /* master_bedroom_init defaults to the east-wing door; override
                   when the player came through the west one. */
                if (bedroom_door_west)
                    master_bedroom_spawn_west();
                cdaudio_play(CDAUDIO_PIANO_TRACK, 1);   /* shares the 2F music */
            } else if (pending_area == STATE_EAST_HALL) {
                east_hall_init();   /* defaults to the west (reception) door */
                /* Coming back out of the library, arrive at the east door; out
                   of the stairwell, at the south offshoot's door. Either version
                   of the library lands on the same east door — they share the
                   doorway, only what is behind it changes. */
                if (current_area == STATE_LIBRARY ||
                    current_area == STATE_LIBRARY_DESTROYED)
                    east_hall_spawn_east();
                else if (current_area == STATE_EAST_STAIRWELL)
                    east_hall_spawn_south();
                /* Out of the WRECKED library specifically: the house shakes and
                   the door behind the player is buried (east_hall_quake.h). The
                   module owns the rest of the rule — the two flags, and whether
                   it has already happened — so all this line decides is where
                   the player came from. Armed rather than started, so the base
                   to shake around is taken a frame later, after everything below
                   has finished placing the player.

                   A title-screen Load Game cannot reach this: that path sets
                   current_area to STATE_DELIVERY_AREA before the loading branch
                   runs, so the saved flags installed further down can never
                   arrive after an arm and undo it. */
                east_hall_quake_reset();
                if (current_area == STATE_LIBRARY_DESTROYED)
                    east_hall_quake_arm_on_entry();
                cdaudio_play(CDAUDIO_RECEPTION_TRACK, 1);  /* shares reception's music */
            } else if (pending_area == STATE_LIBRARY) {
                library_init();     /* defaults to the west (east hall) door */
                /* Coming back out of the stairwell, arrive at the south door. */
                if (current_area == STATE_EAST_STAIRWELL)
                    library_spawn_south();
                cdaudio_play(CDAUDIO_RECEPTION_TRACK, 1);  /* shares the east hall's music */
            } else if (pending_area == STATE_LIBRARY_DESTROYED) {
                library_destroyed_init();  /* defaults to the west (east hall) door */
                /* Coming back out of the stairwell, arrive at the south door. */
                if (current_area == STATE_EAST_STAIRWELL)
                    library_destroyed_spawn_south();
                /* NO music here — the collapsed Library is silent on entry, and
                   the only thing that ever sounds in it is Hadad's stalker track
                   once he appears (update_hadads brings that up over this).
                   STOPPED rather than merely not started, for the Garden Stairs'
                   reason: a title-screen load or a debug level-select jump does
                   not pass through the door transition's own cdaudio_stop and
                   would otherwise arrive with the previous room's music running. */
                cdaudio_stop();
            } else if (pending_area == STATE_EAST_STAIRWELL) {
                east_stairwell_init();  /* defaults to the west landing */
                /* The two landings are separate rooms in practice: the Library
                   door lands on the east one, the East Hall door on the west,
                   and the attic stairs land at the foot of the east landing's
                   stair alcove. */
                if (current_area == STATE_LIBRARY ||
                    current_area == STATE_LIBRARY_DESTROYED)
                    east_stairwell_spawn_east();
                else if (current_area == STATE_ATTIC_STAIRWELL)
                    east_stairwell_spawn_stairs();
                cdaudio_play(CDAUDIO_RECEPTION_TRACK, 1);  /* shares the east hall's music */
            } else if (pending_area == STATE_ATTIC_STAIRWELL) {
                attic_stairwell_init(); /* defaults to the stair head */
                /* Coming back out of the Attic Exit, arrive at the west room's
                   north door instead. */
                if (current_area == STATE_ATTIC_EXIT)
                    attic_stairwell_spawn_exit_door();
                cdaudio_play(CDAUDIO_PIANO_TRACK, 1);      /* shares the conservatory's music */
            } else if (pending_area == STATE_ATTIC_EXIT) {
                attic_exit_init();  /* defaults to the south-wall door */
                /* Coming back down from the Garden Stairs, arrive at the north
                   exit door instead. */
                if (current_area == STATE_GARDEN_STAIRS)
                    attic_exit_spawn_north();
                cdaudio_play(CDAUDIO_PIANO_TRACK, 1);      /* shares the conservatory's music */
            } else if (pending_area == STATE_GARDEN_STAIRS) {
                garden_stairs_init();  /* defaults to the exit door, at the TOP
                                          of the stairs */
                /* Coming back up out of the Garden Courtyard, arrive at the cage
                   door on the BOTTOM landing instead. */
                if (current_area == STATE_GARDEN_COURTYARD)
                    garden_stairs_spawn_bottom();
                /* NO music here — the stairway is silent by design. Stopped
                   rather than merely not started, so a title-screen load or a
                   debug level-select jump (neither of which passes through the
                   door transition's cdaudio_stop) also arrives in silence. */
                cdaudio_stop();
            } else if (pending_area == STATE_GARDEN_COURTYARD) {
                garden_courtyard_init();  /* defaults to the east cage door */
                /* Coming back south out of Fountain Square, arrive at the gate
                   in the north hedge instead, down on the lawn. */
                if (current_area == STATE_FOUNTAIN_SQUARE)
                    garden_courtyard_spawn_north();
                /* NO music here. The courtyard's track is the BOSS's track, and
                   src/rabisu_boss.c starts it at the moment the reveal's lights
                   die back — not on walking in. The garden is silent from the
                   cage door until then, and silent again once the boss is
                   destroyed, which is why this is a cdaudio_stop rather than
                   simply omitting the play: a debug level-select jump straight
                   into this room does not pass through the door transition's
                   own stop. */
                cdaudio_stop();
            } else if (pending_area == STATE_FOUNTAIN_SQUARE) {
                fountain_square_init();  /* defaults to the south gate, back
                                            into the courtyard */
                /* Coming back south out of the Outside Catacombs, arrive at the
                   gate in the north hedge instead; coming back west out of Maze
                   One, at the gate in the east hedge; coming back east out of
                   the Rear Gate, at the gate in the west hedge. Each spawn arms
                   all FOUR gates, so whichever branch runs the other three are
                   safe. */
                if (current_area == STATE_OUTSIDE_CATACOMBS)
                    fountain_square_spawn_north();
                else if (current_area == STATE_MAZE_ONE)
                    fountain_square_spawn_east();
                else if (current_area == STATE_REAR_GATE)
                    fountain_square_spawn_west();
                /* Music of its own, unlike the rest of the garden — the stairs
                   and the courtyard are silent by design, and the courtyard's
                   track belongs to the boss. Played HERE rather than on the door
                   trigger so every route in gets it: the gate, a title-screen
                   load and a debug level-select jump all pass through this
                   branch. The transition's own cdaudio_stop has already killed
                   whatever was playing next door. */
                cdaudio_play(CDAUDIO_FOUNTAIN_TRACK, 1);
            } else if (pending_area == STATE_OUTSIDE_CATACOMBS) {
                outside_catacombs_init();  /* only one gate is connected: the
                                              south one, back into the square */
                /* The SAME track as Fountain Square next door, restarted on
                   arrival. The two could in principle have shared it across the
                   gate without a stop, but streaming this room's mesh suspends
                   CD-DA mid-track and what the player would hear is the score
                   pausing over the load — the kitchen/delivery pair settled that
                   argument already. Played HERE rather than on the gate trigger
                   so every route in gets it: the gate, a title-screen load and a
                   debug level-select jump all pass through this branch. */
                cdaudio_play(CDAUDIO_FOUNTAIN_TRACK, 1);
            } else if (pending_area == STATE_MAZE_ONE) {
                maze_one_init();  /* defaults to the west gate, back into the
                                     square */
                /* Coming back south out of Maze Two, arrive at the gate in the
                   north hedge instead. Either spawn arms BOTH gates, so
                   whichever branch runs the other is safe. */
                if (current_area == STATE_MAZE_TWO)
                    maze_one_spawn_north();
                /* The SAME track as Fountain Square through the gate, restarted
                   on arrival, exactly as the Outside Catacombs does it. Sharing
                   it across the gate without a stop is not an option: streaming
                   this room's 117 KB mesh — the largest on the disc — suspends
                   CD-DA for longer than any other transition in the game, and
                   what the player would hear is the score pausing over the load.
                   Played HERE rather than on the gate trigger so every route in
                   gets it: the gate, a title-screen load and a debug
                   level-select jump all pass through this branch. */
                cdaudio_play(CDAUDIO_FOUNTAIN_TRACK, 1);
            } else if (pending_area == STATE_MAZE_TWO) {
                maze_two_init();  /* only one gate is connected: the south one,
                                     back into Maze One. No spawn override
                                     needed — there is nowhere else to arrive
                                     from. */
                /* THE SAME TRACK as Maze One through the gate — the user asked
                   for it explicitly, and it is what the rest of the garden runs
                   on anyway. Restarted on arrival rather than carried across:
                   streaming this room's 107 KB mesh suspends CD-DA for nearly as
                   long as Maze One's does, and what the player would hear is the
                   score pausing over the load. Played HERE rather than on the
                   gate trigger so every route in gets it: the gate, a
                   title-screen load and a debug level-select jump all pass
                   through this branch. */
                cdaudio_play(CDAUDIO_FOUNTAIN_TRACK, 1);
            } else if (pending_area == STATE_REAR_GATE) {
                rear_gate_init();  /* defaults to the east gate, back into
                                      Fountain Square. */
                /* Coming back out of the West Corridor, arrive at the south
                   door instead — on the ramp, not down on the lawn. Either
                   spawn arms the gate, the plinth and the south door, so
                   whichever branch runs the others are safe. */
                if (current_area == STATE_WEST_CORRIDOR)
                    rear_gate_spawn_south();
                /* NO MUSIC HERE — the user asked for this room to be silent.
                   Stopped rather than merely not started, exactly as the Garden
                   Stairs and the Garden Courtyard do it: a title-screen load or
                   a debug level-select jump does not pass through the gate
                   transition's own cdaudio_stop, and without this line either
                   would carry Fountain Square's track in with it.

                   >>> THE ROOM IS SILENT; HADAD IS NOT. <<< update_hadads owns a
                   CD track of its own and brings it up the moment he is active
                   in here, so this stop is the room's BASELINE rather than its
                   whole story. It has to run first either way — arriving with
                   the square's track still going and then layering the stalk on
                   top is not something cdaudio can do. See hadad.h. */
                cdaudio_stop();
            } else if (pending_area == STATE_WEST_CORRIDOR) {
                west_corridor_init();  /* defaults to the east door, back into
                                          Reception */
                /* Coming south out of the Rear Gate, arrive at the north
                   double door instead. Either spawn arms BOTH doors, so
                   whichever branch runs the other is safe. */
                if (current_area == STATE_REAR_GATE)
                    west_corridor_spawn_north();
                /* NO MUSIC HERE — the user asked for this room to be silent.
                   Stopped rather than merely not started, exactly as the Rear
                   Gate and the Garden Stairs do it: a title-screen load or a
                   debug level-select jump does not pass through the door
                   transition's own cdaudio_stop, and without this line either
                   would carry Reception's track in with it. */
                cdaudio_stop();
            } else {
                /* Return to the delivery area: restore its collision/floor and
                   place the player just inside the front door, facing in, armed
                   so a held Circle won't bounce them straight back. */
                collision_init();
                floor_zones_init();
                cam_x   = DOOR_X + 150;   /* delivery side of the door */
                cam_y   = DOOR_Y;         /* upper-floor eye level */
                cam_vy  = 0;
                cam_z   = DOOR_Z;
                cam_rot = 1024;           /* face +X, into the delivery area */
                door_arm();
                cdaudio_play(CDAUDIO_MUSIC_TRACK, 1);   /* same track as the
                                                           kitchen, restarted on
                                                           arrival — see there */
            }
            /* Restore the entered room's entities into the live arrays. */
            world_enter(pending_area);
            current_area = pending_area;
            game_state   = pending_area;
            /* If this transition came from a title-screen Load Game, overwrite
               the room's default spawn/player state with the saved one (no-op
               otherwise). Must run after the area init above, which sets its
               own spawn position. */
            savegame_apply_pending();
            /* The debug menu's grants, latched so this fires only on the jump
               that armed it and not on every door transition.

               >>> BEFORE THE RE-DERIVE BLOCK, NOT AFTER. <<< These are not only
               inventory cheats any more: DBG_HADAD_FLAG_ONE also solves the exit
               door puzzle and retires the stove and the Anzu tablets (see
               debug_opts.c), and every one of those is a flag the block below
               re-reads. Granted afterwards they landed a frame too late — the
               room had already placed its props and picked its door art from the
               pre-grant flags. */
            debug_opts_apply_grants();
            /* game_flags has only just been installed, so anything a room
               DERIVES from a saved flag has to be re-derived here: the area
               init above ran while the flags still held the pre-load values,
               which on a "Load Game" straight into this room is the previous
               playthrough's. */
            if (pending_area == STATE_PIANO_ROOM) {
                anzu_puzzle_place();   /* re-reads FLAG_ANZU_SOLVED */
                if (game_flag(FLAG_ANZU_SOLVED)) piano_props_tablets_hide();
                cdaudio_play(game_flag(FLAG_ANZU_SOLVED) ? CDAUDIO_ANZU_TRACK
                                                         : CDAUDIO_PIANO_TRACK, 1);
            }
            if (pending_area == STATE_ATTIC_EXIT)
                attic_exit_apply_flags();   /* re-reads FLAG_LIGHTS_SOLVED, and
                                               the two behind the door's art */
            if (pending_area == STATE_EAST_HALL)
                east_hall_apply_flags();    /* re-reads FLAG_HADAD_TWO: the hall's
                                               monsters die with the Library */
        } else if (game_state == STATE_DOOR_ANIM) {
            /* RE-style door transition: a black screen with the door swinging
               open, then a fade to black. Draws nothing of the live room — when
               it finishes we hand off to STATE_LOADING, which switches areas. */
            door_anim_update();
            door_anim_draw(&ctx);
            if (door_anim_finished())
                game_state = STATE_LOADING;
        } else if (game_state == STATE_STAIR_ANIM) {
            /* Stair-climb transition (conservatory <-> 2F hall): shows the
               upstairs texture and lurches up/down three times with footstep
               sounds, then fades to black and hands off to STATE_LOADING. */
            stair_anim_update();
            stair_anim_draw(&ctx);
            if (stair_anim_finished())
                game_state = STATE_LOADING;
        } else if (game_state == STATE_DELIVERY_AREA ||
                   game_state == STATE_KITCHEN_DINING ||
                   game_state == STATE_RECEPTION ||
                   game_state == STATE_PIANO_ROOM ||
                   game_state == STATE_CONSERVATORY ||
                   game_state == STATE_2F_HALL ||
                   game_state == STATE_MASTER_BEDROOM ||
                   game_state == STATE_EAST_HALL ||
                   game_state == STATE_LIBRARY ||
                   game_state == STATE_LIBRARY_DESTROYED ||
                   game_state == STATE_EAST_STAIRWELL ||
                   game_state == STATE_ATTIC_STAIRWELL ||
                   game_state == STATE_ATTIC_EXIT ||
                   game_state == STATE_GARDEN_STAIRS ||
                   game_state == STATE_GARDEN_COURTYARD ||
                   game_state == STATE_FOUNTAIN_SQUARE ||
                   game_state == STATE_OUTSIDE_CATACOMBS ||
                   game_state == STATE_MAZE_ONE ||
                   game_state == STATE_MAZE_TWO ||
                   game_state == STATE_REAR_GATE ||
                   game_state == STATE_WEST_CORRIDOR) {
            if (game_over) {
                draw_lose_screen(&ctx);
            } else {
                /* Capture the area first: handle_menu_open may switch game_state
                   to STATE_MENU mid-frame, but the rest of this frame must keep
                   using the real area so the correct collision runs (otherwise
                   the kitchen would fall through to delivery-area collision and
                   its back-face push would catapult the player). */
                GameState area = game_state;
                /* While a camera-locked puzzle (drawers / stove / piano) owns
                   the screen, suppress the Start menu and the player weapon/HUD
                   overlays. */
                int puzzle = (area == STATE_2F_HALL && trick_drawers_puzzle_active()) ||
                             (area == STATE_KITCHEN_DINING && stove_puzzle_active()) ||
                             (area == STATE_PIANO_ROOM && piano_puzzle_active()) ||
                             (area == STATE_PIANO_ROOM && anzu_puzzle_active()) ||
                             (area == STATE_ATTIC_EXIT && lightswitch_puzzle_active()) ||
                             (area == STATE_ATTIC_EXIT && exit_door_puzzle_active()) ||
                             /* The East Hall's collapse. A cutscene by shape,
                                but it belongs in THIS list and not the one
                                below: the quake's whole payload is a log line
                                ("You feel the ground shake violently") and only
                                `puzzle` keeps the log box up. The Attic Exit's
                                quake gets the same treatment by riding inside
                                exit_door_puzzle_active() on the line above. */
                             (area == STATE_EAST_HALL && east_hall_quake_active());
                int cutscene = (area == STATE_GARDEN_COURTYARD && rabisu_boss_cutscene()) ||
                               (area == STATE_DELIVERY_AREA && delivery_intro_active()) ||
                               (area == STATE_LIBRARY_DESTROYED && hadad_library_cutscene());
                if (!puzzle && !cutscene) handle_menu_open();
                {
                    int t0 = perf_ticks_now();
                    update_current_area(area);
                    perf_ticks_update = (perf_ticks_now() - t0) & 0xFFFF;
                }
                {
                    int t0 = perf_ticks_now();
                    draw_current_area(&ctx, area);
                    perf_ticks_draw = (perf_ticks_now() - t0) & 0xFFFF;
                }
                /* The HUD belongs to the player having the camera. While a
                   puzzle or cutscene owns it, the panel goes with it. */
                if (!puzzle && !cutscene) {
                    draw_player_systems(&ctx);
                    hud_draw(&ctx);
                } else if (puzzle) {
                    /* The camera is not the player's: no bars, no weapon box.
                       The log box alone stays, because puzzles post lines the
                       player has to read. A CUTSCENE gets nothing at all — log
                       lines no longer expire on their own, so leaving the box up
                       would park stale text over the Rabisu reveal and death. */
                    hud_draw_log_only(&ctx);
                }
                draw_debug_overlay(&ctx);
            }
        }

        /* CD-DA music: start once when leaving the title for gameplay, stop
           when returning to the title. In-game area transitions do their CD work
           inside STATE_LOADING, which suspends playback around it. */
        if (IS_FRONTEND(prev_state) && !IS_FRONTEND(game_state)) {
            if (game_state == STATE_DELIVERY_AREA) {
                /* This path enters delivery directly (no STATE_LOADING pass), so
                   everything that branch would have done has to happen here.
                   Geometry first: the arena may hold any room at all — the player
                   could have quit to the title from the Garden Courtyard — and
                   delivery's own mesh has not been resident since startup stopped
                   loading it. Music is not playing yet on this path, so the read
                   is uncontended. */
                delivery_load_geometry();
                /* Then the slots the conservatory may have streamed over in a
                   previous session segment. GPU idled first (pure LoadImage from
                   RAM, same rule as the loading branch). */
                DrawSync(0);
                delivery_restore_textures();
                /* Same reason, for the sound banks: a session that reached the
                   Garden Courtyard and then went back to the title would
                   otherwise start the new game with the boss bank still in and
                   every monster in the house silent. */
                sound_bank_select(SND_BANK_HOUSE);
                /* COLLISION, for the same "the arena may hold any room at all"
                   reason as the geometry above. These two install the delivery
                   area's wall list and floor zones; every other room's _init
                   overwrites both, so a session that reached any other room and
                   came back to the title would otherwise re-enter delivery
                   walking on the LAST room's walls and floor heights. It only
                   looked right on a cold boot because main()'s startup inits
                   happen to leave delivery's data in place.
                   The standoff is reset first for the same reason main.c resets
                   it before every room init: the garden rooms raise it, and
                   delivery wants the default back. */
                collision_set_wall_radius(0);
                collision_init();
                floor_zones_init();
                reset_game(&ctx);   /* fresh-start spawn/state */

                /* THE ARRIVAL SEQUENCE (src/delivery_intro.h): the camera walks
                   up to the yard's east fence from the grass outside and vaults
                   it into the starting spot. Armed HERE, after reset_game, which
                   would otherwise stamp the normal spawn back over the camera —
                   and gated on prev_state so it fires on a NEW GAME only. A
                   title-screen Load Game reaches this same block straight from
                   STATE_TITLE and must drop the player in unceremoniously. */
                if (prev_state == STATE_INTRO) delivery_intro_start();
            }
            /* The room we are "coming from" is now the fresh start, not
               whatever room the previous session ended in. STATE_LOADING reads
               current_area to pick arrival spawns and to snapshot the outgoing
               room, and enemies read it for their per-room behaviour, so a
               stale value here mis-places the player on the first door
               transition of the new game and writes delivery's entities into
               another room's slot. */
            current_area = STATE_DELIVERY_AREA;
            world_new_game();       /* reset rooms; capture the fresh starting room */
            /* Loading into the delivery area enters it directly (no
               STATE_LOADING pass), so apply the staged save here, after
               reset_game's defaults. STATE_LOADING targets apply theirs at the
               end of the loading branch instead; no-op when not loading. */
            if (game_state != STATE_LOADING) {
                savegame_apply_pending();
                /* Delivery is entered directly, so its debug grants land here —
                   after reset_game() above, which clears the inventory. */
                debug_opts_apply_grants();
                /* Ownership is only known after the save/grants above, and
                   delivery_restore_textures() has just put the key back in the
                   slot the two share. Same rule as the STATE_LOADING branch:
                   once the pot is owned it stays resident so the menu icon is
                   right in every room. */
                if (copper_pot_owned()) {
                    DrawSync(0);
                    copper_pot_upload_texture();
                }
            }
            cdaudio_play(CDAUDIO_MUSIC_TRACK, 1);
        }
        if (!IS_FRONTEND(prev_state) && game_state == STATE_TITLE) {
            cdaudio_stop();
            /* Put the opening sequence's voice back in the shared SPU region,
               the way it was at boot. Doing it HERE — while the player is
               looking at the title with the drive idle — is what keeps the
               white flash instant on New Game: intro_start()'s own request is
               then always a no-op. (The read is safe because cdaudio_stop above
               has just freed the drive.) */
            sound_bank_select(SND_BANK_INTRO);
            /* Put the title's purple back. The screen we are coming from owns
               the clear colour and left its own in place — gameplay's black,
               or the game-over screen's red — and draw_title paints only the
               title letters, so without this the title comes up in whatever
               colour the last screen happened to be using. Both buffers, since
               either may be the next one drawn into. */
            setRGB0(&ctx.buffers[0].draw_env, TITLE_BG_R, TITLE_BG_G, TITLE_BG_B);
            setRGB0(&ctx.buffers[1].draw_env, TITLE_BG_R, TITLE_BG_G, TITLE_BG_B);
        }
        cdaudio_update();
        prev_state = game_state;

        flip_buffers(&ctx);
    }

    return 0;
}