#include "ps5_video.h"
#include "receiver_log.h"
#include "ps5_sce.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ps5_log(...) receiver_log("video", __VA_ARGS__)

#include "ps5_tilemap.inc"

#define FB_COUNT   2
#define DMEM_ALIGN 0x20000
#define DMEM_TYPE  3
#define MAP_PROT   0x33
#define FLIP_VSYNC 1

static int           s_handle = -1;
static int           s_width, s_height;
static int           s_tiles_w, s_tiles_h;
static size_t        s_buf_pixels;
static intptr_t      s_paddr;
static size_t        s_memsize;
static PS5_VideoBuf  s_vbuf[FB_COUNT];
static struct kevent *s_eq;
static int           s_back;
static int64_t       s_frame_id;
static int           s_flip_pending;
static int          *s_map_x;
static int          *s_map_y;
static int           s_map_dst_w = -1, s_map_dst_h = -1;
static int           s_map_src_w = -1, s_map_src_h = -1;
static uint32_t      *s_linear;
static size_t         s_linear_pixels;

static size_t round_up_sz(size_t v, size_t m) { return (v + m - 1) / m * m; }

/* 512x128 tiles (swizzle table from ps5-payload-dev/SDL).  A row of tiles is
 * one display width wide (1920 px = 3.75 tiles), not a whole number of tiles. */
static size_t tile_base(int tx, int ty)
{
    return (size_t)tx * PS5_TILE_SIZE + (size_t)ty * (size_t)s_width * PS5_TILE_HEIGHT;
}

static void swizzle_into(uint32_t *dst, const uint32_t *src)
{
    const int W = s_width, H = s_height;
    for (int y = 0; y < H; y++) {
        const int ty = y / PS5_TILE_HEIGHT;
        const uint16_t *trow = PS5_tilemap[y % PS5_TILE_HEIGHT];
        const uint32_t *srow = src + (size_t)y * W;
        for (int x = 0; x < W; x++) {
            const int tx = x / PS5_TILE_WIDTH;
            const size_t base = tile_base(tx, ty);
            dst[base + trow[x % PS5_TILE_WIDTH]] = srow[x];
        }
    }
}

/* The buffer drawn next stays on screen until the pending flip takes place;
 * drawing into it earlier tears the picture. */
static void wait_for_flip(void)
{
    if (s_flip_pending) {
        struct kevent ev;
        int n = 0;
        sceKernelWaitEqueue(s_eq, &ev, 1, &n, NULL);
        s_flip_pending = 0;
    }
}

static int flip(void)
{
    wait_for_flip();
    int rc = sceVideoOutSubmitFlip(s_handle, s_back, FLIP_VSYNC, s_frame_id);
    if (rc) {
        ps5_log("video: sceVideoOutSubmitFlip failed: 0x%x (%s)\n", rc, strerror(errno));
        return -1;
    }
    s_flip_pending = 1;
    s_frame_id++;
    s_back = (s_back + 1) % FB_COUNT;
    return 0;
}

int ps5_video_init(int width, int height)
{
    if (width  <= 0) width  = 1920;
    if (height <= 0) height = 1080;
    s_width = width;
    s_height = height;

    s_tiles_w = (width  + PS5_TILE_WIDTH  - 1) / PS5_TILE_WIDTH;
    s_tiles_h = (height + PS5_TILE_HEIGHT - 1) / PS5_TILE_HEIGHT;
    s_buf_pixels = (size_t)s_tiles_w * s_tiles_h * PS5_TILE_SIZE;

    int hr = sceSystemServiceHideSplashScreen();
    ps5_log("video: HideSplashScreen -> 0x%x\n", hr);

    size_t one = round_up_sz(s_buf_pixels * 4, DMEM_ALIGN);
    s_memsize = one * FB_COUNT;

    if (sceKernelAllocateMainDirectMemory(s_memsize, DMEM_ALIGN, DMEM_TYPE, &s_paddr)) {
        ps5_log("video: AllocateMainDirectMemory(%zu) failed: %s\n", s_memsize, strerror(errno));
        ps5_video_cleanup();
        return -1;
    }

    void *vaddr = NULL;
    if (sceKernelMapDirectMemory(&vaddr, s_memsize, MAP_PROT, 0, s_paddr, DMEM_ALIGN)) {
        ps5_log("video: MapDirectMemory failed: %s\n", strerror(errno));
        ps5_video_cleanup();
        return -1;
    }

    for (int i = 0; i < FB_COUNT; i++) {
        s_vbuf[i].data = (uint8_t *)vaddr + (size_t)i * one;
        memset(s_vbuf[i].data, 0, one);
    }

    if (sceKernelCreateEqueue(&s_eq, "mlps5 flip")) {
        ps5_log("video: sceKernelCreateEqueue failed: %s\n", strerror(errno));
        ps5_video_cleanup();
        return -1;
    }

    s_handle = sceVideoOutOpen(PS5_VIDEO_OUT_USER_SYSTEM, PS5_VIDEO_OUT_BUS_MAIN, 0, NULL);
    if (s_handle < 0) {
        ps5_log("video: sceVideoOutOpen failed: 0x%x (%s)\n", s_handle, strerror(errno));
        ps5_video_cleanup();
        return -1;
    }
    /* The console only registers buffers with the default pitch (the display
     * width); an explicit pitch or a tile-aligned width is refused. */
    PS5_VideoAttr attr;
    memset(&attr, 0, sizeof(attr));
    sceVideoOutSetBufferAttribute2(&attr, PS5_VIDEO_ATTR_BGRA_TILED, 0,
                                   (uint32_t)s_width, (uint32_t)s_height, 0, 0, 0);
    int rc = sceVideoOutRegisterBuffers2(s_handle, 0, 0, s_vbuf, FB_COUNT, &attr, 0, NULL);
    if (rc) {
        ps5_log("video: sceVideoOutRegisterBuffers2 failed: 0x%x (%s)\n", rc, strerror(errno));
        ps5_video_cleanup();
        return -1;
    }

    if (sceVideoOutAddFlipEvent(s_eq, s_handle, NULL)) {
        ps5_log("video: sceVideoOutAddFlipEvent failed: %s\n", strerror(errno));
        ps5_video_cleanup();
        return -1;
    }

    sceVideoOutSetFlipRate(s_handle, 0);

    ps5_log("video: init OK\n");
    return 0;
}

void ps5_video_get_size(int *width, int *height)
{
    if (width)  *width  = s_width;
    if (height) *height = s_height;
}

int ps5_video_present(const uint32_t *abgr_linear)
{
    if (s_handle < 0 || !abgr_linear)
        return -1;
    wait_for_flip();
    swizzle_into((uint32_t *)s_vbuf[s_back].data, abgr_linear);
    return flip();
}

static uint8_t clamp8(int value)
{
    if (value < 0) return 0;
    if (value > 255) return 255;
    return (uint8_t)value;
}

static int build_maps(int src_width, int src_height)
{
    if (s_map_x && s_map_y && s_map_dst_w == s_width && s_map_dst_h == s_height &&
        s_map_src_w == src_width && s_map_src_h == src_height)
        return 0;
    int *map_x = realloc(s_map_x, sizeof(int) * (size_t)s_width);
    if (!map_x)
        return -1;
    s_map_x = map_x;
    int *map_y = realloc(s_map_y, sizeof(int) * (size_t)s_height);
    if (!map_y)
        return -1;
    s_map_y = map_y;

    int dest_w, dest_h;
    if ((long long)src_width * s_height <= (long long)src_height * s_width) {
        dest_h = s_height;
        dest_w = (int)((long long)src_width * s_height / src_height);
    } else {
        dest_w = s_width;
        dest_h = (int)((long long)src_height * s_width / src_width);
    }
    if (dest_w < 1) dest_w = 1;
    if (dest_h < 1) dest_h = 1;
    const int dest_x = (s_width - dest_w) / 2;
    const int dest_y = (s_height - dest_h) / 2;
    for (int dx = 0; dx < s_width; dx++)
        map_x[dx] = (dx < dest_x || dx >= dest_x + dest_w)
                        ? -1
                        : ((dx - dest_x) * src_width) / dest_w;
    for (int dy = 0; dy < s_height; dy++)
        map_y[dy] = (dy < dest_y || dy >= dest_y + dest_h)
                        ? -1
                        : ((dy - dest_y) * src_height) / dest_h;

    s_map_dst_w = s_width;
    s_map_dst_h = s_height;
    s_map_src_w = src_width;
    s_map_src_h = src_height;
    return 0;
}

static int y_term[256], r_v_term[256], g_u_term[256], g_v_term[256], b_u_term[256];
static uint8_t clip_table[2048];
static int tables_ready;

static void init_tables(void)
{
    if (tables_ready)
        return;
    for (int i = 0; i < 256; i++) {
        y_term[i] = 298 * (i - 16);
        r_v_term[i] = 409 * (i - 128);
        g_u_term[i] = -100 * (i - 128);
        g_v_term[i] = -208 * (i - 128);
        b_u_term[i] = 516 * (i - 128);
    }
    for (int v = -1024; v < 1024; v++)
        clip_table[v + 1024] = clamp8(v);
    tables_ready = 1;
}

int ps5_video_present_i420(const uint8_t *y, int y_stride,
                           const uint8_t *u, int u_stride,
                           const uint8_t *v, int v_stride,
                           int width, int height)
{
    if (s_handle < 0 || !y || !u || !v || width <= 0 || height <= 0 ||
        y_stride < width || u_stride < (width + 1) / 2 ||
        v_stride < (width + 1) / 2)
        return -1;
    if (build_maps(width, height) != 0)
        return -1;
    init_tables();

    wait_for_flip();
    uint32_t *dst = (uint32_t *)s_vbuf[s_back].data;
    for (int ty = 0; ty < s_tiles_h; ty++) {
        for (int ly = 0; ly < PS5_TILE_HEIGHT; ly++) {
            const int dy = ty * PS5_TILE_HEIGHT + ly;
            if (dy >= s_height)
                break;
            const int sy = s_map_y[dy];
            const uint16_t *trow = PS5_tilemap[ly];
            const uint8_t *yrow = sy < 0 ? NULL : y + (size_t)sy * y_stride;
            const uint8_t *urow = sy < 0 ? NULL : u + (size_t)(sy / 2) * u_stride;
            const uint8_t *vrow = sy < 0 ? NULL : v + (size_t)(sy / 2) * v_stride;
            for (int tx = 0; tx < s_tiles_w; tx++) {
                const size_t base = tile_base(tx, ty);
                for (int lx = 0; lx < PS5_TILE_WIDTH; lx++) {
                    const int dx = tx * PS5_TILE_WIDTH + lx;
                    if (dx >= s_width)
                        break;
                    const int sx = sy < 0 ? -1 : s_map_x[dx];
                    uint32_t pixel;
                    if (sx < 0) {
                        pixel = 0xff000000u;
                    } else {
                        const int yy = yrow[sx];
                        const int uu = urow[sx >> 1];
                        const int vv = vrow[sx >> 1];
                        const int yt = y_term[yy];
                        const int red = (yt + r_v_term[vv] + 128) >> 8;
                        const int green = (yt + g_u_term[uu] + g_v_term[vv] + 128) >> 8;
                        const int blue = (yt + b_u_term[uu] + 128) >> 8;
                        pixel = 0xff000000u | ((uint32_t)clip_table[blue + 1024] << 16) |
                                ((uint32_t)clip_table[green + 1024] << 8) |
                                clip_table[red + 1024];
                    }
                    dst[base + trow[lx]] = pixel;
                }
            }
        }
    }
    return flip();
}

int ps5_video_fill(uint32_t bgra)
{
    if (s_handle < 0)
        return -1;
    wait_for_flip();
    uint32_t *dst = (uint32_t *)s_vbuf[s_back].data;
    for (size_t i = 0; i < s_buf_pixels; i++)
        dst[i] = bgra;
    return flip();
}

void ps5_video_cleanup(void)
{
    if (s_eq && s_handle >= 0)
        sceVideoOutDeleteFlipEvent(s_eq, s_handle);
    if (s_eq) {
        sceKernelDeleteEqueue(s_eq);
        s_eq = NULL;
    }
    if (s_handle >= 0) {
        sceVideoOutClose(s_handle);
        s_handle = -1;
    }
    if (s_paddr) {
        sceKernelReleaseDirectMemory(s_paddr, s_memsize);
        s_paddr = 0;
        s_memsize = 0;
    }
    for (int i = 0; i < FB_COUNT; i++)
        s_vbuf[i].data = NULL;
    free(s_linear);
    s_linear = NULL;
    s_linear_pixels = 0;
    free(s_map_x);
    free(s_map_y);
    s_map_x = NULL;
    s_map_y = NULL;
    s_map_dst_w = s_map_dst_h = s_map_src_w = s_map_src_h = -1;
    s_flip_pending = 0;
}
