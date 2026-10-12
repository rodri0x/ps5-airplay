#pragma once

#define AIRPLAY_NAME "#NODISCNOBUY"
#define AIRPLAY_HOSTNAME "ps5-airplay.local"
#ifndef AIRPLAY_VERSION
#define AIRPLAY_VERSION "dev"
#endif
#define AIRPLAY_UPDATE_REPO "rodri0x/ps5-airplay"
#define AIRPLAY_RTSP_PORT 7000
#define AIRPLAY_MDNS_PORT 5353
#define AIRPLAY_CONTROL_PORT 18182
#define AIRPLAY_PLAYOUT_DELAY_MS 120
#define AIRPLAY_HOST_AUDIO_PORT 18185
#define AIRPLAY_DATA_DIR "/data/ps5-airplay"
#define AIRPLAY_SETTINGS_PATH "/data/ps5-airplay/settings.json"

#define AIRPLAY_MODEL "AppleTV3,2"
#define AIRPLAY_SRCVERS "220.68"
#define AIRPLAY_FEATURES "0x5A7FFEE6,0x0"
#define AIRPLAY_FLAGS "0x84"
#define AIRPLAY_FLAGS_PIN "0x8c"
#define AIRPLAY_PI "2e388006-13ba-4041-9a67-25dd4a43d536"
#define AIRPLAY_DEVICE_ID "02:00:00:13:42:60"
#define AIRPLAY_IDENTITY_PATH AIRPLAY_DATA_DIR "/identity.key"
#define AIRPLAY_PAIRED_PATH AIRPLAY_DATA_DIR "/paired_devices.bin"
#define AIRPLAY_LOG_PATH AIRPLAY_DATA_DIR "/receiver.log"
#define AIRPLAY_LOG_PREVIOUS_PATH AIRPLAY_DATA_DIR "/receiver.previous.log"
