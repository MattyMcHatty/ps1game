#include <stdint.h>
#include "area_bank.h"
#include "texmgr.h"
#include "cdaudio.h"

/* Every module that holds a prop model. Kept in the one place on purpose: a
   prop added to the game with a kept buffer and no row in prop_banks[] below is
   4 KB that every area pays for and most of them never see. */
#include "concrete_props.h"
#include "dining_table.h"
#include "piano_props.h"
#include "trick_drawers.h"
#include "valve_handle.h"
#include "vines.h"
#include "chainlink_door.h"
#include "dresser.h"
#include "fatdoor.h"
#include "grinder.h"
#include "lever.h"

/* ---- Which bank each room's textures live in ------------------------------
   A switch rather than a table indexed by GameState, because GameState also
   holds the non-room states (the title, the menu, the transitions) and a table
   would silently give one of those a bank the day somebody appended an enum
   value in the wrong place. The default is MANSION, which is where the game
   starts and the only safe wrong answer. */
TexBank area_bank_of(GameState area) {
    switch (area) {
    /* ---- THE MANSION ------------------------------------------------- */
    case STATE_DELIVERY_AREA:
    case STATE_KITCHEN_DINING:
    case STATE_RECEPTION:
    case STATE_PIANO_ROOM:
    case STATE_CONSERVATORY:
    case STATE_2F_HALL:
    case STATE_MASTER_BEDROOM:
    case STATE_EAST_HALL:
    case STATE_LIBRARY:
    case STATE_LIBRARY_DESTROYED:
    case STATE_EAST_STAIRWELL:
    case STATE_ATTIC_STAIRWELL:
    case STATE_ATTIC_EXIT:
    /* The West Corridor is a GARDEN-chapter room by the debug menu's reckoning
       and a HOUSE room by its art: its uploader runs the kitchen's and the
       piano room's and nothing of the garden's. The bank follows the ART. */
    case STATE_WEST_CORRIDOR:
        return TEXBANK_MANSION;

    /* ---- THE GARDEN --------------------------------------------------- */
    /* The Garden Stairs are reached from the Attic Exit, so this boundary is
       the one the player crosses first and the one most likely to be blamed
       for a longer load. It is the right place for it even so: the stairs draw
       the courtyard's set, not the attic's. */
    case STATE_GARDEN_STAIRS:
    case STATE_FOUNTAIN_SQUARE:
    case STATE_OUTSIDE_CATACOMBS:
    case STATE_MAZE_ONE:
    case STATE_MAZE_TWO:
    case STATE_KEYSTONE_MAZE:
    case STATE_CHAIN_ROOM:
    case STATE_THE_HATCH:
    case STATE_REAR_GATE:
        return TEXBANK_GARDEN;

    /* ---- THE RABISU'S ROOM -------------------------------------------- */
    /* One room, and it is NOT a sealed pocket — it sits between the Garden
       Stairs and Fountain Square and the player walks through it both ways.
       It gets its own bank because of what is READ there and nowhere else:
       104 KB of boss model and clip, plus SND_BANK_BOSS, whose EMERGE clip is
       a 70 KB malloc and the largest single transient in the program. This
       cuts what is resident at that door from 424 KB to 206 KB. */
    case STATE_GARDEN_COURTYARD:
        return TEXBANK_RABISU;

    /* ---- THE WEST GARDEN ---------------------------------------------- */
    case STATE_STABLES:
    case STATE_GREENHOUSE:
        return TEXBANK_WEST_GARDEN;

    /* ---- ASAG'S ARENA, AND IT IS EMPTY --------------------------------- */
    /* Not a stub: that room streams every texture it draws into a scratch
       buffer it frees again (src/asag_arena.h) and registers none at all. So
       selecting this bank frees everything and reads nothing, and the tightest
       door in the game — where 90 KB of boss model is read — goes from about
       92 KB of contiguous heap to the whole of it. */
    case STATE_ASAG_ARENA:
        return TEXBANK_ASAG;

    /* ---- CHAPTER 3 ----------------------------------------------------- */
    case STATE_CATACOMBS_ENTRY:
    /* The Up Down Maze draws COBBLE and CTCMBDR and nothing else, both of them
       the Catacombs Entry's own registrations, so its bank is that room's by
       construction rather than by geography. py tools/check_tex_banks.py walks
       up_down_maze_upload_textures() to the two narrow uploaders and proves it.  */
    case STATE_UP_DOWN_MAZE:
    /* The Incinerator Room is the Up Down Maze's case again, and for the same
       reason rather than because it is next door: it draws COBBLE and CTCMBDR
       and nothing else, both of them the Catacombs Entry's own registrations.
       py tools/check_tex_banks.py walks incinerator_room_upload_textures() to
       the same two narrow uploaders and proves it. */
    case STATE_INCINERATOR_ROOM:
    /* The Tomb is the third room in a row to be here by CONSTRUCTION rather
       than by geography: it draws COBBLE, LOCULUS and CTCMBDR and nothing else,
       all three of them the Catacombs Entry's own registrations. py
       tools/check_tex_banks.py walks tomb_upload_textures() to that room's
       three narrow uploaders and proves it. It is the first room besides the
       burial hall to draw the loculus, which is why the third of those
       uploaders exists at all. */
    case STATE_ROOM_OF_ARMS:
    case STATE_TOMB:
        return TEXBANK_CATACOMBS;

    default:
        return TEXBANK_MANSION;
    }
}

int area_is_catacombs(GameState area) {
    return area == STATE_CATACOMBS_ENTRY ||
           area == STATE_UP_DOWN_MAZE ||
           area == STATE_INCINERATOR_ROOM ||
           area == STATE_TOMB ||
           area == STATE_ROOM_OF_ARMS;
}

/* ---- The prop models -------------------------------------------------------
   43 KB of .smd read at startup. Every one of them is NULL-safe and leaves its
   module's SMD pointer NULL, which every draw and collide in those files
   already tests, so a prop whose bank is out draws nothing rather than
   following a dangling pointer.

   >>> THE MASKS HERE ARE DELIBERATELY COARSE, AND THAT IS NOT LAZINESS. <<<
   Every one of them is "anywhere a Chapter 1 or 2 room can be", which is what
   these props already cost before banking existed. Narrowing them to the rooms
   each prop is actually PLACED in would save perhaps 30 KB in the mansion's
   bank — and the placement sets are spread across room inits, puzzle modules
   and world_seed_room(), so getting one wrong means a prop that is silently
   invisible and un-collidable in a room it belongs in. That is a worse bug
   than 30 KB in a bank with 480 KB of headroom, and it is not checkable the
   way the TEXTURE masks are (tools/check_tex_banks.py has an uploader call
   graph to walk; placement has no equivalent). Narrow them when something
   needs the space, with the placements verified one at a time.

   What they DO buy as written: the two pockets. Asag's arena and the Catacombs
   free all 43 KB, which is exactly where it matters. */
#define PROP_CHAPTERS_1_2 (TEXBANK_MANSION | TEXBANK_GARDEN | \
                           TEXBANK_RABISU  | TEXBANK_WEST_GARDEN)

static const struct {
    TexBank banks;
    void  (*free_fn)(void);
    void  (*reload_fn)(void);   /* NULL = the room's own init re-reads it */
} prop_banks[] = {
    { PROP_CHAPTERS_1_2, concrete_props_free_assets,   concrete_props_reload_assets   },
    /* dining_tables has no reload: its geometry is read by dining_tables_init(),
       which runs on every kitchen entry, so it restores itself. */
    { PROP_CHAPTERS_1_2, dining_tables_free_assets,    NULL                           },
    { PROP_CHAPTERS_1_2, piano_props_free_assets,      piano_props_reload_assets      },
    { PROP_CHAPTERS_1_2, trick_drawers_free_assets,    trick_drawers_reload_assets    },
    { PROP_CHAPTERS_1_2, valve_handles_free_assets,    valve_handles_reload_assets    },
    { PROP_CHAPTERS_1_2, vines_free_assets,            vines_reload_assets            },
    { PROP_CHAPTERS_1_2, chainlink_doors_free_assets,  chainlink_doors_reload_assets  },
    { PROP_CHAPTERS_1_2, dresser_free_assets,          dresser_reload_assets          },
    { PROP_CHAPTERS_1_2, fatdoors_free_assets,         fatdoors_reload_assets         },
    { PROP_CHAPTERS_1_2, grinder_free_assets,          grinder_reload_assets          },
    { PROP_CHAPTERS_1_2, levers_free_assets,           levers_reload_assets           },
};
#define PROP_COUNT ((int)(sizeof prop_banks / sizeof prop_banks[0]))

/* One bit per row above: 1 = that prop's model is currently held. They all are
   at boot, because main()'s startup block reads them. */
static uint32_t props_held = (1u << PROP_COUNT) - 1u;

/* >>> THE PROP RELOADS ARE GEOMETRY ONLY. <<< Their *_load_assets()
   counterparts also register textures, and calling those again would add a
   SECOND texmgr entry for each — a second RAM copy and one more against
   TEXMGR_MAX, which fails silently when it runs out (see the head of
   src/texmgr.c). The registrations come back through texmgr_bank_select(),
   which re-reads entries that already exist rather than making new ones.

   They are raw CdReads — their originals run at startup, where nothing is
   streaming, and do not suspend CD-DA for themselves — so they are bracketed
   here. texmgr_bank_select does its own. */
static void props_sync(TexBank bank) {
    int i;
    uint32_t want = 0;
    for (i = 0; i < PROP_COUNT; i++)
        if (prop_banks[i].banks & bank) want |= (1u << i);
    if (want == props_held) return;

    /* Free before load, the same rule texmgr_bank_select follows and for the
       same reason: the heap top is the stack. */
    for (i = 0; i < PROP_COUNT; i++)
        if ((props_held & (1u << i)) && !(want & (1u << i)))
            prop_banks[i].free_fn();

    cdaudio_suspend();
    for (i = 0; i < PROP_COUNT; i++)
        if (!(props_held & (1u << i)) && (want & (1u << i)) && prop_banks[i].reload_fn)
            prop_banks[i].reload_fn();
    cdaudio_resume();

    props_held = want;
}

void area_bank_sync(GameState area) {
    TexBank bank = area_bank_of(area);
    /* Props first: they are the smaller free and doing them ahead of the
       texture bank means the texture reads start against a slightly emptier
       heap. Neither ordering is load-bearing — both halves free before they
       load — but if one of them is going to be the peak it should be the one
       that is measured, which is the texture bank. */
    props_sync(bank);
    texmgr_bank_select(bank);
}
