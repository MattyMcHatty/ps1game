#ifndef SML_MED_H
#define SML_MED_H

#include <stdint.h>
#include "render.h"

#define MAX_SML_MEDS          8
#define SML_MED_PICKUP_RADIUS 200
/* Vertical reach, paired with the radius above. Needed because the Garden
   Stairs stacks landings over one XZ footprint: without it, walking the landing
   two flights up collects the medipac on the floor below. Both bounds are
   tight-ish, so do not retune casually — see the note in sml_meds_update(). */
#define SML_MED_PICKUP_HEIGHT 300

typedef struct {
    int32_t x, y, z;
    int32_t bob_angle;
    int32_t active;
} SmlMed;

extern SmlMed sml_meds[MAX_SML_MEDS];
extern int    sml_med_count;

void sml_meds_init(void);
void sml_med_spawn(int32_t x, int32_t y, int32_t z);
void sml_meds_update(void);
void sml_meds_draw(RenderContext *ctx);
void sml_meds_reset(void);

#endif
