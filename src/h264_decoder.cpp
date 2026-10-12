#include "h264_decoder.h"

#include "airplay_playout.h"
#include "ps5_video.h"
#include "receiver_log.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <time.h>

/* Frames are shown airplay_playout_delay_ms() after their timestamp on the
 * sender's clock.  The two clocks have different origins, so frames are placed
 * relative to the quickest recent trip (smallest arrival minus timestamp). */
#define QUEUE_CAPACITY 16
#define LATE_MS 60.0
#define OFFSET_WINDOW_MS 4000.0

static AVCodecContext *g_context;
static AVFrame *g_frame;
static AVFrame *g_queue[QUEUE_CAPACITY];
static int g_queue_first;
static int g_queue_count;
static AVPacket *g_packet;
static int g_submits;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_wake = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_idle = PTHREAD_COND_INITIALIZER;
static pthread_t g_presenter;
static int g_presenter_running;
static int g_presenter_quit;
static int g_presenting_allowed;
static int g_presenter_busy;
static int g_video_ready;
static double g_retry_after_ms;

static double g_offset_current;
static double g_offset_previous;
static double g_offset_window_started_ms;
static int g_offset_known;

static int g_decoded;
static int g_shown;
static int g_dropped;
static int g_late;
static double g_worst_arrival_ms;
static double g_decode_ms;
static double g_present_ms;
static double g_report_started_ms;

static double now_ms()
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec * 1000.0 + (double)now.tv_nsec / 1e6;
}

static void queue_clear()
{
    for (int i = 0; i < QUEUE_CAPACITY; i++) {
        if (g_queue[i])
            av_frame_unref(g_queue[i]);
    }
    g_queue_first = 0;
    g_queue_count = 0;
}

static void *presenter_thread(void *);

static int ensure_decoder()
{
    if (g_context)
        return 0;
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        receiver_log("video", "libavcodec has no H.264 decoder");
        return -1;
    }
    g_context = avcodec_alloc_context3(codec);
    g_frame = av_frame_alloc();
    g_packet = av_packet_alloc();
    bool allocated = g_context && g_frame && g_packet;
    for (int i = 0; i < QUEUE_CAPACITY; i++) {
        g_queue[i] = av_frame_alloc();
        allocated = allocated && g_queue[i];
    }
    if (!allocated) {
        receiver_log("video", "decoder allocation failed");
        h264_decoder_cleanup();
        return -1;
    }
    g_context->thread_count = 1;
    if (avcodec_open2(g_context, codec, nullptr) != 0) {
        receiver_log("video", "avcodec_open2 failed");
        h264_decoder_cleanup();
        return -1;
    }
    g_presenter_quit = 0;
    if (pthread_create(&g_presenter, nullptr, presenter_thread, nullptr) != 0) {
        receiver_log("video", "presenter thread could not be started");
        h264_decoder_cleanup();
        return -1;
    }
    g_presenter_running = 1;
    receiver_log("video", "FFmpeg H.264 decoder ready");
    return 0;
}

static double note_arrival(double timestamp_ms, double arrival_ms)
{
    const double trip = arrival_ms - timestamp_ms;
    if (!g_offset_known || trip < g_offset_current - 2000.0 || trip > g_offset_current + 2000.0) {
        g_offset_current = g_offset_previous = trip;
        g_offset_window_started_ms = arrival_ms;
        g_offset_known = 1;
        return 0.0;
    }
    if (arrival_ms - g_offset_window_started_ms > OFFSET_WINDOW_MS) {
        g_offset_previous = g_offset_current;
        g_offset_current = trip;
        g_offset_window_started_ms = arrival_ms;
    } else if (trip < g_offset_current) {
        g_offset_current = trip;
    }
    const double quickest = g_offset_current < g_offset_previous ? g_offset_current
                                                                  : g_offset_previous;
    return trip - quickest;
}

static double due_ms(const AVFrame *frame)
{
    if (!g_offset_known || frame->pts == AV_NOPTS_VALUE || frame->pts <= 0)
        return 0.0;
    const double quickest = g_offset_current < g_offset_previous ? g_offset_current
                                                                  : g_offset_previous;
    return (double)frame->pts / 1000.0 + quickest + airplay_playout_delay_ms();
}

extern "C" int h264_decoder_submit(const uint8_t *annex_b, size_t length, int codec,
                                   uint64_t timestamp_us)
{
    if (!annex_b || length == 0)
        return -1;
    if (codec != 264)
        return -2;
    if (ensure_decoder() != 0)
        return -1;

    g_submits++;
    if (av_new_packet(g_packet, static_cast<int>(length)) < 0)
        return -1;
    std::memcpy(g_packet->data, annex_b, length);
    g_packet->pts = timestamp_us ? (int64_t)timestamp_us : AV_NOPTS_VALUE;

    const double started = now_ms();
    const int sent = avcodec_send_packet(g_context, g_packet);
    av_packet_unref(g_packet);
    if (sent < 0 && sent != AVERROR(EAGAIN)) {
        receiver_log("video", "frame %d rejected by the decoder: %d", g_submits, sent);
        return -1;
    }

    for (;;) {
        const int got = avcodec_receive_frame(g_context, g_frame);
        if (got == AVERROR(EAGAIN) || got == AVERROR_EOF)
            break;
        if (got < 0) {
            receiver_log("video", "decoder returned %d", got);
            return -1;
        }
        if (g_frame->format != AV_PIX_FMT_YUV420P && g_frame->format != AV_PIX_FMT_YUVJ420P) {
            receiver_log("video", "unsupported pixel format %d", g_frame->format);
            av_frame_unref(g_frame);
            continue;
        }
        pthread_mutex_lock(&g_lock);
        g_decoded++;
        if (g_frame->pts != AV_NOPTS_VALUE && g_frame->pts > 0) {
            const double behind = note_arrival((double)g_frame->pts / 1000.0, started);
            if (behind > g_worst_arrival_ms)
                g_worst_arrival_ms = behind;
        }
        if (g_queue_count == QUEUE_CAPACITY) {
            av_frame_unref(g_queue[g_queue_first]);
            g_queue_first = (g_queue_first + 1) % QUEUE_CAPACITY;
            g_queue_count--;
            g_dropped++;
        }
        AVFrame *slot = g_queue[(g_queue_first + g_queue_count) % QUEUE_CAPACITY];
        av_frame_move_ref(slot, g_frame);
        g_queue_count++;
        pthread_cond_signal(&g_wake);
        pthread_mutex_unlock(&g_lock);
    }
    pthread_mutex_lock(&g_lock);
    g_decode_ms += now_ms() - started;
    pthread_mutex_unlock(&g_lock);
    return 0;
}

static void report_if_due(int width, int height)
{
    const double now = now_ms();
    if (g_report_started_ms == 0.0) {
        g_report_started_ms = now;
        return;
    }
    const double elapsed = now - g_report_started_ms;
    if (elapsed < 10000.0)
        return;
    receiver_log("video",
                 "%dx%d, last %.0f s: %d decoded (%.1f fps), %d shown (%.1f fps), "
                 "%d dropped (queue full), %d skipped (late); worst arrival %.0f ms behind; "
                 "decode %.1f ms, present %.1f ms per frame",
                 width, height, elapsed / 1000.0, g_decoded, g_decoded * 1000.0 / elapsed,
                 g_shown, g_shown * 1000.0 / elapsed, g_dropped, g_late, g_worst_arrival_ms,
                 g_decoded ? g_decode_ms / g_decoded : 0.0,
                 g_shown ? g_present_ms / g_shown : 0.0);
    g_report_started_ms = now;
    g_decoded = g_shown = g_dropped = g_late = 0;
    g_decode_ms = g_present_ms = g_worst_arrival_ms = 0.0;
}

static void *presenter_thread(void *)
{
    AVFrame *frame = av_frame_alloc();
    if (!frame)
        return nullptr;
    pthread_mutex_lock(&g_lock);
    for (;;) {
        while (!g_presenter_quit && (!g_presenting_allowed || g_queue_count == 0))
            pthread_cond_wait(&g_wake, &g_lock);
        if (g_presenter_quit)
            break;
        const double wait_ms = due_ms(g_queue[g_queue_first]) - now_ms();
        if (wait_ms > 1.0) {
            const double step_ms = wait_ms < 20.0 ? wait_ms : 20.0;
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_nsec += (long)(step_ms * 1e6);
            if (until.tv_nsec >= 1000000000L) {
                until.tv_sec += 1;
                until.tv_nsec -= 1000000000L;
            }
            (void)pthread_cond_timedwait(&g_wake, &g_lock, &until);
            continue;
        }
        if (wait_ms < -LATE_MS && g_queue_count > 1) {
            av_frame_unref(g_queue[g_queue_first]);
            g_queue_first = (g_queue_first + 1) % QUEUE_CAPACITY;
            g_queue_count--;
            g_late++;
            continue;
        }
        av_frame_move_ref(frame, g_queue[g_queue_first]);
        g_queue_first = (g_queue_first + 1) % QUEUE_CAPACITY;
        g_queue_count--;
        g_presenter_busy = 1;
        pthread_mutex_unlock(&g_lock);

        const int width = frame->width, height = frame->height;
        int shown = 0;
        double spent = 0.0;
        if (!g_video_ready && now_ms() >= g_retry_after_ms) {
            if (ps5_video_init(1920, 1080) == 0) {
                g_video_ready = 1;
                receiver_log("video", "showing %dx%d on the PS5 1920x1080 output", width, height);
            } else {
                receiver_log("video", "PS5 video-out initialization failed, retrying in 2 s");
                g_retry_after_ms = now_ms() + 2000.0;
            }
        }
        if (g_video_ready) {
            const double started = now_ms();
            (void)ps5_video_present_i420(frame->data[0], frame->linesize[0],
                                         frame->data[1], frame->linesize[1],
                                         frame->data[2], frame->linesize[2],
                                         width, height);
            spent = now_ms() - started;
            shown = 1;
        }
        av_frame_unref(frame);

        pthread_mutex_lock(&g_lock);
        g_presenter_busy = 0;
        pthread_cond_broadcast(&g_idle);
        if (shown) {
            g_shown++;
            g_present_ms += spent;
            report_if_due(width, height);
        }
    }
    pthread_mutex_unlock(&g_lock);
    av_frame_free(&frame);
    return nullptr;
}

extern "C" void h264_decoder_set_presenting(int allowed)
{
    pthread_mutex_lock(&g_lock);
    if (g_presenting_allowed != (allowed != 0)) {
        g_presenting_allowed = allowed != 0;
        pthread_cond_signal(&g_wake);
    }
    pthread_mutex_unlock(&g_lock);
}

extern "C" void h264_decoder_stop(void)
{
    pthread_mutex_lock(&g_lock);
    g_presenting_allowed = 0;
    queue_clear();
    while (g_presenter_busy)
        pthread_cond_wait(&g_idle, &g_lock);
    g_report_started_ms = 0.0;
    g_decoded = g_shown = g_dropped = g_late = 0;
    g_decode_ms = g_present_ms = g_worst_arrival_ms = 0.0;
    g_offset_known = 0;
    const int release = g_video_ready;
    g_video_ready = 0;
    pthread_mutex_unlock(&g_lock);

    if (g_context)
        avcodec_flush_buffers(g_context);
    if (release) {
        ps5_video_cleanup();
        receiver_log("video", "stream ended: PS5 video output released");
    }
}

extern "C" void h264_decoder_cleanup(void)
{
    if (g_presenter_running) {
        pthread_mutex_lock(&g_lock);
        g_presenter_quit = 1;
        pthread_cond_signal(&g_wake);
        pthread_mutex_unlock(&g_lock);
        (void)pthread_join(g_presenter, nullptr);
        g_presenter_running = 0;
    }
    if (g_context) {
        avcodec_free_context(&g_context);
    }
    av_frame_free(&g_frame);
    for (int i = 0; i < QUEUE_CAPACITY; i++)
        av_frame_free(&g_queue[i]);
    g_queue_first = 0;
    g_queue_count = 0;
    av_packet_free(&g_packet);
}
