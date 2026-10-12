#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int  ps5_video_init(int width, int height);

void ps5_video_get_size(int *width, int *height);

int  ps5_video_present(const uint32_t *abgr_linear);

int ps5_video_present_i420(const uint8_t *y, int y_stride,
                           const uint8_t *u, int u_stride,
                           const uint8_t *v, int v_stride,
                           int width, int height);

int  ps5_video_fill(uint32_t bgra);

void ps5_video_cleanup(void);

#ifdef __cplusplus
}
#endif
