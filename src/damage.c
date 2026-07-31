#include "damage.h"

int32_t damage_scale(int32_t base, DamageType type,
                     const Weakness *table, int count) {
    int i;
    for (i = 0; i < count; i++) {
        if (table[i].type != type) continue;
        int32_t out = (base * table[i].modifier) / 100;
        /* Never round a real hit down to nothing — see damage.h. */
        if (out < 1 && base > 0) out = 1;
        return out;
    }
    return base;   /* no entry for this type: unmodified */
}
