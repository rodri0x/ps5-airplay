#include "airplay_playout.h"

#include <string.h>

static const struct {
    const char *name;
    int delay_ms;
} modes[] = {
    {AIRPLAY_MODE_QUALITY, 200},
    {AIRPLAY_MODE_BALANCED, 120},
    {AIRPLAY_MODE_PERFORMANCE, 60},
};

static volatile int current_delay_ms = 120;

static int find_mode(const char *mode) {
    for (size_t i = 0; mode && i < sizeof(modes) / sizeof(modes[0]); i++) {
        if (strcmp(mode, modes[i].name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

int airplay_playout_mode_valid(const char *mode) {
    return find_mode(mode) >= 0;
}

void airplay_playout_set_mode(const char *mode) {
    int index = find_mode(mode);
    if (index < 0) {
        index = find_mode(AIRPLAY_MODE_DEFAULT);
    }
    current_delay_ms = modes[index].delay_ms;
}

int airplay_playout_delay_ms(void) {
    return current_delay_ms;
}
