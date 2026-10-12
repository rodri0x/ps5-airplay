#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int audio_decoder_submit(const uint8_t *payload, size_t length, uint8_t codec,
                         uint32_t timestamp);
void audio_decoder_cleanup(void);

#ifdef __cplusplus
}
#endif
