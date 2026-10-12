#pragma once

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/event.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PS5_VideoBuf {
    void    *data;
    uint64_t junk0[3];
} PS5_VideoBuf;

typedef struct PS5_VideoAttr {
    uint8_t junk0[80];
} PS5_VideoAttr;

#define PS5_VIDEO_OUT_USER_SYSTEM   0xFF
#define PS5_VIDEO_OUT_BUS_MAIN      0

/* 32-bit BGRA, GPU-tiled; value taken from ps5-payload-dev/SDL. */
#define PS5_VIDEO_ATTR_BGRA_TILED   0x8000000022000000ULL

int  sceVideoOutOpen(int userId, int busType, int index, const void *param);
void sceVideoOutClose(int handle);
int  sceVideoOutSetFlipRate(int handle, int rate);
int  sceVideoOutSubmitFlip(int handle, int bufIndex, uint32_t flipMode, int64_t flipArg);
int  sceVideoOutAddFlipEvent(struct kevent *eq, int handle, void *udata);
int  sceVideoOutDeleteFlipEvent(struct kevent *eq, int handle);
void sceVideoOutSetBufferAttribute2(PS5_VideoAttr *attr, uint64_t pixelFormat,
                                    uint32_t tilingMode, uint32_t width,
                                    uint32_t height, uint64_t a,
                                    uint32_t b, uint64_t c);
int  sceVideoOutRegisterBuffers2(int handle, int set, int index,
                                 PS5_VideoBuf *bufs, int count,
                                 PS5_VideoAttr *attr, int category, void *unk);

int sceKernelAllocateMainDirectMemory(size_t len, size_t alignment,
                                      int memoryType, intptr_t *physAddrOut);
int sceKernelMapDirectMemory(void **addrOut, size_t len, int prot, int flags,
                             intptr_t physAddr, size_t alignment);
int sceKernelReleaseDirectMemory(intptr_t physAddr, size_t len);

int sceKernelCreateEqueue(struct kevent **eq, const char *name);
int sceKernelWaitEqueue(struct kevent *eq, struct kevent *out, int num,
                        int *count, unsigned int *timeout);
int sceKernelDeleteEqueue(struct kevent *eq);

int sceUserServiceInitialize(const void *params);
int sceUserServiceTerminate(void);
int sceUserServiceGetInitialUser(int *userId);

int sceSystemServiceHideSplashScreen(void);

#define SCE_PAD_PORT_TYPE_STANDARD 0
int scePadInit(void);
int scePadOpen(int userId, int type, int index, const void *param);
int scePadClose(int handle);
int scePadReadState(int handle, void *data);

#define SCE_AUDIO_OUT_PORT_TYPE_MAIN          0
#define SCE_AUDIO_OUT_PARAM_FORMAT_S16_MONO   0
#define SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO 1
#define SCE_AUDIO_OUT_PARAM_FORMAT_S16_8CH    2
#define SCE_AUDIO_OUT_VOLUME_0DB              32768
int sceAudioOutInit(void);
int sceAudioOutOpen(int userId, int portType, int index,
                    uint32_t len, uint32_t freq, uint32_t param);
int sceAudioOutClose(int handle);
int sceAudioOutOutput(int handle, const void *ptr);
int sceAudioOutSetVolume(int handle, int flag, int *vol);

#ifdef __cplusplus
}
#endif
