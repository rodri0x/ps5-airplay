#include "airplay_config.h"
#include "receiver_log.h"
#include "airplay_pairing.h"
#include "audio_decoder.h"
#include "h264_decoder.h"
#include "control.h"
#include "airplay_playout.h"
#include "display_carrier.h"
#include "mdns.h"
#include "ps5_video.h"
#include "rtsp.h"
#include "update_check.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

int sceNetInit(void);

static volatile sig_atomic_t running = 1;

static void stop_handler(int signal_number) {
    (void)signal_number;
    running = 0;
}

static void set_process_name(void) {
    (void)syscall(0x1d0, -1, "ps5airplay.elf");
}

static int video_active;
static double last_video_ms;

static double now_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec * 1000.0 + (double)now.tv_nsec / 1e6;
}

static void video_callback(void *opaque, const uint8_t *annex_b,
                           size_t length, int codec, uint64_t timestamp_us) {
    (void)opaque;
    if (!video_active) {
        display_carrier_request();
    }
    video_active = 1;
    last_video_ms = now_ms();
    int result = h264_decoder_submit(annex_b, length, codec, timestamp_us);
    if (result < 0 && result != -2) {
        receiver_log("main", "video decoder submit failed: %d\n", result);
    }
}

static void end_video_session(void) {
    if (!video_active) {
        return;
    }
    h264_decoder_stop();
    video_active = 0;
    display_carrier_release();
}

static void mirror_end_callback(void *opaque) {
    (void)opaque;
    end_video_session();
}

static volatile unsigned long loop_passes;

#define LOOP_STUCK_SECONDS 15

/* A main loop stuck in a console call keeps the ports taken; exiting lets the
 * shortcut start the receiver again. */
static void *loop_watchdog(void *arg) {
    (void)arg;
    unsigned long seen = loop_passes;
    int quiet = 0;
    for (;;) {
        sleep(1);
        if (loop_passes != seen) {
            seen = loop_passes;
            quiet = 0;
        } else if (++quiet >= LOOP_STUCK_SECONDS) {
            receiver_log("main", "the main loop has not moved for %d s; exiting", quiet);
            receiver_log_flush();
            _exit(3);
        }
    }
    return NULL;
}

static void audio_callback(void *opaque, const uint8_t *payload, size_t length,
                           uint8_t codec, uint32_t timestamp) {
    (void)opaque;
    int result = audio_decoder_submit(payload, length, codec, timestamp);
    if (result < 0 && result != -2) {
        receiver_log("main", "audio decoder submit failed: %d\n", result);
    }
}

int main(void) {
    receiver_log_start();
    receiver_log("main", "PS5 AirPlay %s, built %s %s", AIRPLAY_VERSION, __DATE__, __TIME__);
    receiver_install_crash_logger();
    setvbuf(stdout, NULL, _IOLBF, 0);
    set_process_name();
    signal(SIGINT, stop_handler);
    signal(SIGTERM, stop_handler);
    signal(SIGHUP, stop_handler);

    int network_error = sceNetInit();
    if (network_error != 0) {
        receiver_log("main", "sceNetInit returned 0x%x; continuing\n", network_error);
    }

    airplay_settings_t settings;
    airplay_settings_defaults(&settings);
    airplay_settings_load(&settings);
    airplay_playout_set_mode(settings.mode);

    (void)mkdir(AIRPLAY_DATA_DIR, 0755);
    airplay_identity_t identity;
    int identity_ready = airplay_identity_load(&identity, AIRPLAY_IDENTITY_PATH) == 0;
    if (!identity_ready) {
        receiver_log("main", "identity unavailable; pair-setup will be refused\n");
    }

    mdns_server_t mdns;
    if (mdns_server_init(&mdns) != 0) {
        receiver_log("main", "mDNS initialization failed\n");
        return EXIT_FAILURE;
    }
    if (mdns_server_set_name(&mdns, settings.name) != 0) {
        receiver_log("main", "invalid saved receiver name; using default\n");
        airplay_settings_defaults(&settings);
    }
    if (identity_ready) {
        char public_key[65];
        airplay_identity_public_hex(&identity, public_key);
        (void)mdns_server_set_public_key(&mdns, public_key);
    }
    (void)mdns_server_set_enabled(&mdns, settings.enabled);
    rtsp_server_t rtsp;
    if (rtsp_server_init(&rtsp) != 0) {
        mdns_server_close(&mdns);
        receiver_log("main", "RTSP initialization failed\n");
        return EXIT_FAILURE;
    }
    if (identity_ready) {
        rtsp_server_set_identity(&rtsp, &identity);
    }
    rtsp_server_set_video_callback(&rtsp, video_callback, NULL);
    rtsp_server_set_audio_callback(&rtsp, audio_callback, NULL);
    rtsp_server_set_mirror_end_callback(&rtsp, mirror_end_callback, NULL);
    rtsp_server_set_name(&rtsp, mdns_server_name(&mdns));
    rtsp_server_set_passcode(&rtsp, settings.passcode);
    (void)mdns_server_set_pin_required(&mdns, settings.passcode[0] != '\0');
    (void)rtsp_server_set_enabled(&rtsp, settings.enabled);
    control_server_t control;
    if (control_server_init(&control, &settings, &mdns, &rtsp) != 0) {
        rtsp_server_close(&rtsp);
        mdns_server_close(&mdns);
        receiver_log("main", "control server initialization failed\n");
        return EXIT_FAILURE;
    }

    receiver_log("main", "PS5 AirPlay receiver\n");
    receiver_log("main", "name %s, IPv4 %s, mDNS _airplay._tcp.local, RTSP %d, control %d\n",
           mdns_server_name(&mdns), mdns.address_text, AIRPLAY_RTSP_PORT,
           AIRPLAY_CONTROL_PORT);
    if (settings.enabled) {
        (void)mdns_server_announce(&mdns);
    }
    update_check_start();

    time_t last_announcement = time(NULL);
    double pass_started_ms = now_ms();
    double longest_pass_ms = 0.0;
    double pass_report_ms = pass_started_ms;
    pthread_t watchdog;
    if (pthread_create(&watchdog, NULL, loop_watchdog, NULL) == 0) {
        (void)pthread_detach(watchdog);
    }
    while (running) {
        loop_passes++;
        const double pass_now_ms = now_ms();
        if (video_active && pass_now_ms - pass_started_ms > longest_pass_ms) {
            longest_pass_ms = pass_now_ms - pass_started_ms;
        }
        pass_started_ms = pass_now_ms;
        if (pass_now_ms - pass_report_ms > 10000.0) {
            if (video_active) {
                receiver_log("main", "longest loop pass in the last 10 s: %.0f ms", longest_pass_ms);
            }
            longest_pass_ms = 0.0;
            pass_report_ms = pass_now_ms;
        }
        const int streaming = video_active;
        (void)control_server_poll(&control, streaming ? 0 : 50);
        (void)mdns_server_poll(&mdns, streaming ? 0 : 50);
        (void)rtsp_server_poll(&rtsp, streaming ? 5 : 50);
        display_carrier_heartbeat();
        h264_decoder_set_presenting(video_active && !display_carrier_starting());
        if (video_active && now_ms() - last_video_ms > 60000.0) {
            end_video_session();
        }
        time_t now = time(NULL);
        if (settings.enabled && now - last_announcement >= 5) {
            (void)mdns_server_announce(&mdns);
            last_announcement = now;
        }
    }

    end_video_session();
    control_server_close(&control);
    rtsp_server_close(&rtsp);
    mdns_server_close(&mdns);
    audio_decoder_cleanup();
    h264_decoder_cleanup();
    receiver_log("main", "stopped\n");
    receiver_log_flush();
    return EXIT_SUCCESS;
}
