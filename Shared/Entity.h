#pragma once

#include <stdint.h>

typedef uint16_t EntityIdx;
// must be multiple of 8 and not 0
#define RR_MAX_ENTITY_COUNT (512)
#define RR_NULL_ENTITY (0)

#define RR_FOR_EACH_COMPONENT \
    XX(ai,          1)        \
    XX(arena,       2)        \
    XX(flower,      3)        \
    XX(health,      4)        \
    XX(mob,         5)        \
    XX(petal,       6)        \
    XX(physical,    7)        \
    XX(player_info, 8)        \
    XX(relations,   9)        \
    XX(projectile, 10)        \
    XX(centipede,  11)
