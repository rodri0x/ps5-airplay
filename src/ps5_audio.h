#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int ps5_audio_init(void);
void ps5_audio_cleanup(void);
int ps5_audio_submit_s16(const int16_t *samples, size_t frames,
                         unsigned int sample_rate, unsigned int channels);
void ps5_audio_flush(void);

#ifdef __cplusplus
}
#endif
