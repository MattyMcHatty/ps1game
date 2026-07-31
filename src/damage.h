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
