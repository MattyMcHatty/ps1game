#ifndef KEY_H
#define KEY_H

#include <stdint.h>
#include "render.h"

/* Each entry is a distinct one-off key. Add new key types here. */
typedef enum {
    KEY_FRONT_DOOR = 0,
    MAX_KEY_TYPES
} KeyType;

/* Display name for each KeyType — index matches the enum value. */
extern const char * const key_type_names[MAX_KEY_TYPES];

#define MAX_KEYS          4
#define KEY_PICKUP_RADIUS 200

typedef struct {
    int32_t x, y, z;
    int32_t spin_angle;
    int32_t active;
    KeyType key_type;
} KeyPickup;

extern KeyPickup keys[MAX_KEYS];
extern int       key_count;

void keys_init(void);
void key_spawn(int32_t x, int32_t y, int32_t z, KeyType type);
void keys_update(void);
void keys_draw(RenderContext *ctx);
void keys_reset(void);

#endif
