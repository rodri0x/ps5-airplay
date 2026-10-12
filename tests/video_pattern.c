/* Diagnostic: shows a fixed test pattern on the PS5 video output for two
 * minutes, without any AirPlay input.  A photo of the TV shows whether the
 * scan-out layout is right: straight lines and an intact frame mean the tiling
 * matches; sheared lines or offset blocks mean it does not.
 *
 *   build/video_pattern.elf -> loaded through elfldr like the receiver.
 *   Stop it with the receiver's kill probe or let it finish after 120 s.
 */
#include "ps5_video.h"

#include <stdint.h>
#include <time.h>

#define PATTERN_W 1920
#define PATTERN_H 1080
#define PATTERN_SECONDS 120

/* ps5_video_present() takes A<<24 | B<<16 | G<<8 | R. */
#define RGB(r, g, b) (0xff000000u | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r))

static uint32_t frame[PATTERN_W * PATTERN_H];

static void draw_pattern(void)
{
    static const uint32_t bars[8] = {
        RGB(255, 255, 255),
        RGB(255, 255, 0),
        RGB(0, 255, 255),
        RGB(0, 255, 0),
        RGB(255, 0, 255),
        RGB(255, 0, 0),
        RGB(0, 0, 255),
        RGB(40, 40, 40),
    };
    for (int y = 0; y < PATTERN_H; y++) {
        /* Diagonal from the top-left to the bottom-right corner. */
        const int diagonal_x = (y * (PATTERN_W - 1)) / (PATTERN_H - 1);
        for (int x = 0; x < PATTERN_W; x++) {
            uint32_t color = bars[x / (PATTERN_W / 8)];
            if (y % 120 < 3)
                color = RGB(255, 255, 255);
            if (x >= diagonal_x - 2 && x <= diagonal_x + 2)
                color = RGB(255, 255, 255);
            if (x < 12 || x >= PATTERN_W - 12 || y < 12 || y >= PATTERN_H - 12)
                color = RGB(255, 255, 255);
            frame[(size_t)y * PATTERN_W + x] = color;
        }
    }
}

int main(void)
{
    if (ps5_video_init(PATTERN_W, PATTERN_H) != 0)
        return 1;
    draw_pattern();
    const time_t started = time(NULL);
    while (time(NULL) - started < PATTERN_SECONDS) {
        if (ps5_video_present(frame) != 0)
            break;
    }
    ps5_video_cleanup();
    return 0;
}
