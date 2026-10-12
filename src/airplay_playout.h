#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define AIRPLAY_MODE_QUALITY "quality"
#define AIRPLAY_MODE_BALANCED "balanced"
#define AIRPLAY_MODE_PERFORMANCE "performance"
#define AIRPLAY_MODE_DEFAULT AIRPLAY_MODE_BALANCED

int airplay_playout_mode_valid(const char *mode);
void airplay_playout_set_mode(const char *mode);
int airplay_playout_delay_ms(void);

#ifdef __cplusplus
}
#endif
