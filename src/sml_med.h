#ifndef SML_MED_H
#define SML_MED_H

#include <stdint.h>
#include "render.h"

#define MAX_SML_MEDS          8
#define SML_MED_PICKUP_RADIUS 200

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
