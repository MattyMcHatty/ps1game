#ifndef DAMAGE_H
#define DAMAGE_H

#include <stdint.h>

/*
 * Damage types and enemy weaknesses.
 *
 * Every ammo type carries a DamageType (see ammo_info[] in player.h), and every
 * enemy owns a small table of the damage types it is WEAK (or resistant) to.
 * The gun looks the shot's damage type up in the target's table and scales the
 * base damage by the modifier it finds; a type with no entry does base damage.
 *
 * Modifiers are PERCENTAGES, so 200 = double damage and 300 = triple. Percent
 * rather than a whole multiplier so partial values (150 = 1.5x) and resistances
 * (50 = half) can be added later without reworking the tables.
 *
 * ADDING A WEAKNESS: append an entry to the enemy's table in its .c file, e.g.
 *
 *     static const Weakness zombie_weakness[] = {
 *         { DMG_FLAME, 200 },   \* fire rounds hurt twice as much *\
 *         { DMG_HOLY,  150 },   \* ...and a second entry is just another line *\
 *     };
 *
 * The tables are sized with WEAKNESS_COUNT, so nothing else needs touching.
 *
 * Melee does NOT go through this table: the crucifaxe has no damage type, and
 * giving it one would silently change every existing swing.
 */

typedef enum {
    DMG_KINETIC = 0,   /* plain lead — Standard Rounds */
    DMG_FLAME,         /* fire — Flame Rounds          */
    /* Blessed fire — the Helluminator, which burns holy anointing oil. It is
       NOT an AmmoType: the Grave-olver's cylinder can never hold oil, so this
       type reaches an enemy only from the lantern (see helluminator.h).

       ZOMBIES ARE 3x WEAK TO IT ({ DMG_HOLY, 300 } in zombie.c) and nothing
       else is, which is exactly why it has its own type rather than reusing
       DMG_FLAME: that one line raised the lantern's tick against the walking
       dead from 1 to 3 without touching what Flame Rounds do to them. Every
       other enemy's table still falls through to the unmodified base, so the
       lantern does its stated 1 hp/sec to all of them.

       A weakness is not the only way an enemy can answer holy fire. The Living
       Statue takes the plain 1 but is the one enemy the lantern can damage at
       all while it stalks — a STATE gate, not a multiplier, and it lives in
       living_statue_burn rather than in any table here. */
    DMG_HOLY,
    MAX_DAMAGE_TYPES
} DamageType;

typedef struct {
    DamageType type;
    int32_t    modifier;   /* percent: 100 = normal, 200 = double, 50 = resist */
} Weakness;

/* Number of entries in a statically-declared weakness table. */
#define WEAKNESS_COUNT(tbl) ((int)(sizeof(tbl) / sizeof((tbl)[0])))

/* Scale `base` by the target's weakness to `type`. Returns at least 1 for any
   positive base, so a heavy resistance can never make a hit do nothing at all
   (an enemy that ignored a whole ammo type would read as a broken gun). */
int32_t damage_scale(int32_t base, DamageType type,
                     const Weakness *table, int count);

#endif
