#include "audio_decoder.h"
#include "receiver_log.h"

#include "ps5_audio.h"

#include "ALACBitUtilities.h"
#include "ALACDecoder.h"
#include "aacdecoder_lib.h"

#include <cstdio>
#include <cstring>

static ALACDecoder *g_alac;
static int g_alac_ready;
static uint64_t g_alac_frames;
static uint64_t g_aac_unsupported_logged;
static HANDLE_AACDECODER g_aac;
static int g_aac_ready;

static const uint8_t k_alac_cookie[] = {
    0x00, 0x00, 0x00, 0x24, 0x61, 0x6c, 0x61, 0x63,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x60,
    0x00, 0x10, 0x28, 0x0a, 0x0e, 0x02, 0x00, 0xff,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xac, 0x44
};

static int ensure_alac(void) {
    if (g_alac_ready) return 0;
    g_alac = new ALACDecoder();
    if (!g_alac || g_alac->Init((void *)k_alac_cookie,
                                sizeof(k_alac_cookie)) != 0) {
        receiver_log("audio", "ALAC decoder initialization failed\n");
        delete g_alac;
        g_alac = nullptr;
        return -1;
    }
    if (ps5_audio_init() != 0) {
        delete g_alac;
        g_alac = nullptr;
        return -1;
    }
    g_alac_ready = 1;
    receiver_log("audio", "ALAC decoder ready: %u Hz, %u channels, %u samples/frame\n",
                 g_alac->mConfig.sampleRate, g_alac->mConfig.numChannels,
                 g_alac->mConfig.frameLength);
    return 0;
}

static int submit_alac(const uint8_t *payload, size_t length) {
    if (!payload || length <= 32 || payload[0] != 0x20) {
        return 0;
    }
    if (ensure_alac() != 0) return -1;

    uint8_t pcm[4096 * 2 * sizeof(int16_t)];
    uint32_t decoded_frames = 0;
    BitBuffer bits;
    BitBufferInit(&bits, (uint8_t *)payload, (uint32_t)length);
    const uint32_t frame_capacity = sizeof(pcm) / (2u * sizeof(int16_t));
    const int status = g_alac->Decode(&bits, pcm, frame_capacity, 2,
                                      &decoded_frames);
    if (status != 0 || decoded_frames == 0) {
        if ((g_alac_frames++ & 0x3fu) == 0) {
            receiver_log("audio", "ALAC Decode failed status=%d length=%zu\n",
                         status, length);
        }
        return -1;
    }
    g_alac_frames += decoded_frames;
    return ps5_audio_submit_s16((const int16_t *)pcm, decoded_frames,
                                 g_alac->mConfig.sampleRate,
                                 g_alac->mConfig.numChannels);
}

static int ensure_aac(void) {
    if (g_aac_ready) return 0;
    g_aac = aacDecoder_Open(TT_MP4_RAW, 1);
    if (!g_aac) {
        receiver_log("audio", "AAC decoder open failed\n");
        return -1;
    }
    UCHAR config_bytes[] = {0xf8, 0xe8, 0x50, 0x00};
    UCHAR *config[] = {config_bytes};
    const UINT config_length[] = {sizeof(config_bytes)};
    if (aacDecoder_ConfigRaw(g_aac, config, config_length) != AAC_DEC_OK) {
        receiver_log("audio", "AAC-ELD config f8e85000 rejected\n");
        aacDecoder_Close(g_aac);
        g_aac = nullptr;
        return -1;
    }
    if (ps5_audio_init() != 0) {
        aacDecoder_Close(g_aac);
        g_aac = nullptr;
        return -1;
    }
    g_aac_ready = 1;
    const CStreamInfo *info = aacDecoder_GetStreamInfo(g_aac);
    if (info) {
        receiver_log("audio", "AAC-ELD decoder ready: %d Hz, %d channels, %d samples/frame\n",
                     info->aacSampleRate, info->numChannels, info->aacSamplesPerFrame);
    }
    return 0;
}

static int submit_aac(const uint8_t *payload, size_t length) {
    if (!payload || length == 0 ||
        (length == 4 && payload[0] == 0x00 && payload[1] == 0x68 &&
         payload[2] == 0x34 && payload[3] == 0x00)) {
        return 0;
    }
    if (ensure_aac() != 0) return -1;
    UCHAR *input[] = {(UCHAR *)payload};
    const UINT input_size[] = {(UINT)length};
    UINT bytes_valid = (UINT)length;
    AAC_DECODER_ERROR status = aacDecoder_Fill(g_aac, input, input_size,
                                               &bytes_valid);
    if (status != AAC_DEC_OK && status != AAC_DEC_NOT_ENOUGH_BITS) {
        receiver_log("audio", "AAC-ELD fill failed: 0x%x\n", status);
        return -1;
    }
    INT_PCM pcm[2048 * 2];
    status = aacDecoder_DecodeFrame(g_aac, pcm,
                                    (INT)(sizeof(pcm) / sizeof(pcm[0])), 0);
    if (status != AAC_DEC_OK) {
        if ((g_aac_unsupported_logged++ & 0x3fu) == 0) {
            receiver_log("audio", "AAC-ELD decode failed: 0x%x\n", status);
        }
        return -1;
    }
    const CStreamInfo *info = aacDecoder_GetStreamInfo(g_aac);
    if (!info || info->numChannels <= 0 || info->aacSampleRate <= 0 ||
        info->frameSize <= 0) {
        return -1;
    }
    return ps5_audio_submit_s16((const int16_t *)pcm,
                                 (size_t)info->frameSize,
                                 (unsigned int)info->aacSampleRate,
                                 (unsigned int)info->numChannels);
}

extern "C" int audio_decoder_submit(const uint8_t *payload, size_t length,
                                     uint8_t codec, uint32_t timestamp) {
    (void)timestamp;
    switch (codec) {
        case 2:
            return submit_alac(payload, length);
        case 8:
            return submit_aac(payload, length);
        default:
            return -2;
    }
}

extern "C" void audio_decoder_cleanup(void) {
    delete g_alac;
    g_alac = nullptr;
    if (g_aac) {
        aacDecoder_Close(g_aac);
        g_aac = nullptr;
    }
    g_alac_ready = 0;
    g_aac_ready = 0;
    g_alac_frames = 0;
    g_aac_unsupported_logged = 0;
    ps5_audio_cleanup();
}
