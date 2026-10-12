#pragma once

#include <stddef.h>
#include <stdint.h>

typedef enum {
    BP_ITEM_NONE = 0,
    BP_ITEM_BOOL,
    BP_ITEM_UINT,
    BP_ITEM_DATA,
    BP_ITEM_STRING,
    BP_ITEM_ARRAY,
    BP_ITEM_DICT
} bp_item_type_t;

typedef struct {
    bp_item_type_t type;
    uint64_t uint_value;
    const uint8_t *data;
    size_t data_length;
    const char *string;
    size_t string_length;
    int bool_value;
} bp_item_t;

int bplist_find(const uint8_t *plist, size_t plist_length,
                const char *key, bp_item_t *item);

int bplist_streams(const uint8_t *plist, size_t plist_length,
                   int *want_mirror, int *want_audio,
                   uint64_t *stream_connection_id);

size_t bplist_build_info(uint8_t *out, size_t capacity,
                         const char *name, const char *device_id,
                         uint64_t features, const char *model,
                         const char *source_version);

size_t bplist_build_data_dict(uint8_t *out, size_t capacity, const char *const *names,
                              const uint8_t *const *values, const size_t *lengths,
                              size_t count);

size_t bplist_build_setup(uint8_t *out, size_t capacity,
                          int timing_port, int event_port,
                          int mirror_port, int audio_data_port,
                          int audio_control_port, int want_mirror,
                          int want_audio);
