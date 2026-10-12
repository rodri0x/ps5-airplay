#include "bplist.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    uint8_t info[8192];
    size_t info_length = bplist_build_info(
        info, sizeof(info), "PS5 AirPlay", "02:00:00:13:42:60",
        0x527FFEE6u, "AppleTV3,2", "220.68");
    FILE *dump = fopen("build/info.bplist", "wb");
    assert(dump != NULL);
    assert(fwrite(info, 1, info_length, dump) == info_length);
    fclose(dump);
    assert(info_length > 40);
    bp_item_t item;
    assert(bplist_find(info, info_length, "name", &item) == 1);
    assert(item.type == BP_ITEM_STRING && item.string_length == strlen("PS5 AirPlay"));
    assert(memcmp(item.string, "PS5 AirPlay", item.string_length) == 0);
    assert(bplist_find(info, info_length, "features", &item) == 1);
    assert(item.type == BP_ITEM_UINT && item.uint_value == 0x527FFEE6u);

    uint8_t setup[4096];
    size_t setup_length = bplist_build_setup(setup, sizeof(setup), 0, 0,
                                              46321, 0, 0, 1, 0);
    assert(setup_length > 40);
    int mirror = 0;
    int audio = 0;
    uint64_t stream_id = 0;
    assert(bplist_streams(setup, setup_length, &mirror, &audio, &stream_id) == 0);
    assert(mirror == 1 && audio == 0);
    puts("bplist roundtrip: ok");
    return 0;
}
