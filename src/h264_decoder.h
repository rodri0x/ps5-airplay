#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int h264_decoder_submit(const uint8_t *annex_b, size_t length, int codec,
                        uint64_t timestamp_us);
void h264_decoder_cleanup(void);
void h264_decoder_set_presenting(int allowed);
void h264_decoder_stop(void);

#ifdef __cplusplus
}
#endif
