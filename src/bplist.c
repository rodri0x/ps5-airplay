#include "bplist.h"

#include <string.h>

#define BP_MAX_OBJECTS 48
#define BP_MAX_CHILDREN 16

typedef struct {
    uint8_t type;
    const uint8_t *data;
    size_t data_length;
    uint64_t number;
    int children[BP_MAX_CHILDREN];
    size_t child_count;
    int dict_keys[BP_MAX_CHILDREN];
    int dict_values[BP_MAX_CHILDREN];
    size_t dict_count;
} bp_object_t;

typedef struct {
    bp_object_t objects[BP_MAX_OBJECTS];
    size_t count;
} bp_writer_t;

static uint64_t read_be(const uint8_t *p, size_t n) {
    uint64_t value = 0;
    for (size_t i = 0; i < n; i++) {
        value = (value << 8) | p[i];
    }
    return value;
}

static int plist_layout(const uint8_t *p, size_t length, uint8_t *offset_size,
                        uint8_t *ref_size, uint64_t *object_count,
                        uint64_t *top_object, uint64_t *offset_table) {
    if (!p || length < 40 || memcmp(p, "bplist00", 8) != 0) {
        return -1;
    }
    const uint8_t *trailer = p + length - 32;
    *offset_size = trailer[6];
    *ref_size = trailer[7];
    *object_count = read_be(trailer + 8, 8);
    *top_object = read_be(trailer + 16, 8);
    *offset_table = read_be(trailer + 24, 8);
    if (*offset_size == 0 || *ref_size == 0 || *object_count == 0 ||
        *object_count > 4096 || *offset_table >= length ||
        *offset_table + *object_count * *offset_size > length ||
        *top_object >= *object_count) {
        return -1;
    }
    return 0;
}

static int object_offset(const uint8_t *p, size_t length, uint8_t offset_size,
                         uint64_t offset_table, uint64_t index,
                         uint64_t *offset) {
    const uint64_t at = offset_table + index * offset_size;
    if (at + offset_size > length) {
        return -1;
    }
    *offset = read_be(p + at, offset_size);
    if (*offset >= length) {
        return -1;
    }
    return 0;
}

static int object_length(const uint8_t *p, size_t length, uint64_t offset,
                         uint8_t info, size_t *header_size, uint64_t *count) {
    *header_size = 1;
    if (info != 0x0f) {
        *count = info;
        return 0;
    }
    if (offset + 2 > length || (p[offset + 1] >> 4) != 0x1) {
        return -1;
    }
    const uint8_t exponent = p[offset + 1] & 0x0f;
    if (exponent > 3) {
        return -1;
    }
    const size_t bytes = (size_t)1u << exponent;
    if (offset + 2 + bytes > length) {
        return -1;
    }
    *header_size = 1 + 1 + bytes;
    *count = read_be(p + offset + 2, bytes);
    return 0;
}

static int object_view(const uint8_t *p, size_t length, uint8_t offset_size,
                       uint8_t ref_size, uint64_t offset_table,
                       uint64_t index, bp_item_t *item) {
    uint64_t offset = 0;
    if (object_offset(p, length, offset_size, offset_table, index, &offset) != 0) {
        return -1;
    }
    const uint8_t marker = p[offset];
    const uint8_t type = marker >> 4;
    const uint8_t info = marker & 0x0f;
    size_t header = 0;
    uint64_t count = 0;
    if (object_length(p, length, offset, info, &header, &count) != 0) {
        return -1;
    }
    if (count > length || offset + header > length) {
        return -1;
    }
    memset(item, 0, sizeof(*item));
    if (type == 0x0) {
        if (info == 0x8 || info == 0x9) {
            item->type = BP_ITEM_BOOL;
            item->bool_value = info == 0x9;
            return 0;
        }
        item->type = BP_ITEM_NONE;
        return 0;
    }
    if (type == 0x1) {
        if (info > 3 || offset + header + ((size_t)1u << info) > length) {
            return -1;
        }
        item->type = BP_ITEM_UINT;
        item->uint_value = read_be(p + offset + header, (size_t)1u << info);
        return 0;
    }
    if (type == 0x4) {
        if (offset + header + count > length) {
            return -1;
        }
        item->type = BP_ITEM_DATA;
        item->data = p + offset + header;
        item->data_length = (size_t)count;
        return 0;
    }
    if (type == 0x5 || type == 0x6) {
        if (count > SIZE_MAX / (type == 0x6 ? 2u : 1u) ||
            offset + header + count * (type == 0x6 ? 2u : 1u) > length) {
            return -1;
        }
        item->type = BP_ITEM_STRING;
        item->string = (const char *)(p + offset + header);
        item->string_length = (size_t)count;
        return 0;
    }
    if (type == 0xa) {
        if (count > (length - offset - header) / ref_size) {
            return -1;
        }
        item->type = BP_ITEM_ARRAY;
        return 0;
    }
    if (type == 0xd) {
        if (count > (length - offset - header) / (2u * ref_size)) {
            return -1;
        }
        item->type = BP_ITEM_DICT;
        return 0;
    }
    return -1;
}

static int find_object(const uint8_t *p, size_t length, uint8_t offset_size,
                       uint8_t ref_size, uint64_t offset_table,
                       uint64_t index, const char *key, int depth,
                       bp_item_t *result) {
    if (depth > 12) {
        return 0;
    }
    uint64_t offset = 0;
    if (object_offset(p, length, offset_size, offset_table, index, &offset) != 0) {
        return 0;
    }
    const uint8_t marker = p[offset];
    const uint8_t type = marker >> 4;
    const uint8_t info = marker & 0x0f;
    size_t header = 0;
    uint64_t count = 0;
    if (object_length(p, length, offset, info, &header, &count) != 0) {
        return 0;
    }
    if (type == 0xd) {
        const uint64_t keys_at = offset + header;
        const uint64_t values_at = keys_at + count * ref_size;
        for (uint64_t i = 0; i < count; i++) {
            if (keys_at + (i + 1) * ref_size > length ||
                values_at + (i + 1) * ref_size > length) {
                return 0;
            }
            const uint64_t key_index = read_be(p + keys_at + i * ref_size, ref_size);
            const uint64_t value_index = read_be(p + values_at + i * ref_size, ref_size);
            bp_item_t key_item;
            if (object_view(p, length, offset_size, ref_size, offset_table,
                            key_index, &key_item) != 0) {
                return 0;
            }
            if (key_item.type == BP_ITEM_STRING && key_item.string_length == strlen(key) &&
                memcmp(key_item.string, key, key_item.string_length) == 0) {
                return object_view(p, length, offset_size, ref_size, offset_table,
                                   value_index, result) == 0;
            }
            if (find_object(p, length, offset_size, ref_size, offset_table,
                            value_index, key, depth + 1, result)) {
                return 1;
            }
        }
        return 0;
    }
    if (type == 0xa) {
        const uint64_t refs_at = offset + header;
        for (uint64_t i = 0; i < count; i++) {
            if (refs_at + (i + 1) * ref_size > length) {
                return 0;
            }
            const uint64_t child = read_be(p + refs_at + i * ref_size, ref_size);
            if (find_object(p, length, offset_size, ref_size, offset_table,
                            child, key, depth + 1, result)) {
                return 1;
            }
        }
    }
    return 0;
}

int bplist_find(const uint8_t *plist, size_t plist_length,
                const char *key, bp_item_t *item) {
    uint8_t offset_size = 0, ref_size = 0;
    uint64_t count = 0, top = 0, offset_table = 0;
    if (!key || !item || plist_layout(plist, plist_length, &offset_size, &ref_size,
                                      &count, &top, &offset_table) != 0) {
        return 0;
    }
    return find_object(plist, plist_length, offset_size, ref_size, offset_table,
                       top, key, 0, item);
}

static int find_stream_type(const uint8_t *plist, size_t length, uint64_t object,
                            uint8_t offset_size, uint8_t ref_size,
                            uint64_t offset_table, int *mirror, int *audio,
                            uint64_t *connection_id, int depth) {
    if (depth > 16) {
        return 0;
    }
    uint64_t at = 0;
    if (object_offset(plist, length, offset_size, offset_table, object, &at) != 0) {
        return 0;
    }
    const uint8_t marker = plist[at];
    const uint8_t type = marker >> 4;
    const uint8_t info = marker & 0x0f;
    size_t header = 0;
    uint64_t count = 0;
    if (object_length(plist, length, at, info, &header, &count) != 0) {
        return 0;
    }
    if (type == 0xd) {
        uint64_t type_value = UINT64_MAX;
        uint64_t id_value = 0;
        const uint64_t keys_at = at + header;
        const uint64_t values_at = keys_at + count * ref_size;
        for (uint64_t i = 0; i < count; i++) {
            uint64_t ki = read_be(plist + keys_at + i * ref_size, ref_size);
            uint64_t vi = read_be(plist + values_at + i * ref_size, ref_size);
            bp_item_t k;
            if (object_view(plist, length, offset_size, ref_size, offset_table,
                            ki, &k) != 0 || k.type != BP_ITEM_STRING) {
                continue;
            }
            bp_item_t v;
            if (object_view(plist, length, offset_size, ref_size, offset_table,
                            vi, &v) != 0) {
                continue;
            }
            if (k.string_length == 4 && memcmp(k.string, "type", 4) == 0 &&
                v.type == BP_ITEM_UINT) {
                type_value = v.uint_value;
            } else if (k.string_length == 18 &&
                       memcmp(k.string, "streamConnectionID", 18) == 0 &&
                       v.type == BP_ITEM_UINT) {
                id_value = v.uint_value;
            } else if (find_stream_type(plist, length, vi, offset_size, ref_size,
                                        offset_table, mirror, audio, connection_id,
                                        depth + 1)) {
            }
        }
        if (type_value == 110) {
            *mirror = 1;
            *connection_id = id_value;
            return 1;
        }
        if (type_value == 96) {
            *audio = 1;
            return 1;
        }
        return 0;
    }
    if (type == 0xa) {
        const uint64_t refs_at = at + header;
        for (uint64_t i = 0; i < count; i++) {
            uint64_t child = read_be(plist + refs_at + i * ref_size, ref_size);
            (void)find_stream_type(plist, length, child, offset_size, ref_size,
                                   offset_table, mirror, audio, connection_id,
                                   depth + 1);
        }
    }
    return 0;
}

int bplist_streams(const uint8_t *plist, size_t plist_length,
                   int *want_mirror, int *want_audio,
                   uint64_t *stream_connection_id) {
    uint8_t offset_size = 0, ref_size = 0;
    uint64_t count = 0, top = 0, offset_table = 0;
    if (!want_mirror || !want_audio || !stream_connection_id ||
        plist_layout(plist, plist_length, &offset_size, &ref_size, &count, &top,
                     &offset_table) != 0) {
        return -1;
    }
    *want_mirror = 0;
    *want_audio = 0;
    *stream_connection_id = 0;
    (void)find_stream_type(plist, plist_length, top, offset_size, ref_size,
                           offset_table, want_mirror, want_audio,
                           stream_connection_id, 0);
    return 0;
}

static int add_object(bp_writer_t *writer, uint8_t type) {
    if (writer->count >= BP_MAX_OBJECTS) {
        return -1;
    }
    const int index = (int)writer->count++;
    memset(&writer->objects[index], 0, sizeof(writer->objects[index]));
    writer->objects[index].type = type;
    return index;
}

static int add_string(bp_writer_t *writer, const char *value) {
    const int index = add_object(writer, 0x5);
    if (index >= 0) {
        writer->objects[index].data = (const uint8_t *)value;
        writer->objects[index].data_length = strlen(value);
    }
    return index;
}

static int add_data(bp_writer_t *writer, const uint8_t *value, size_t length) {
    const int index = add_object(writer, 0x4);
    if (index >= 0) {
        writer->objects[index].data = value;
        writer->objects[index].data_length = length;
    }
    return index;
}

static int add_uint(bp_writer_t *writer, uint64_t value) {
    const int index = add_object(writer, 0x1);
    if (index >= 0) {
        writer->objects[index].number = value;
    }
    return index;
}

static int add_bool(bp_writer_t *writer, int value) {
    const int index = add_object(writer, value ? 0x9 : 0x8);
    return index;
}

static int add_array(bp_writer_t *writer, const int *children, size_t count) {
    if (count > BP_MAX_CHILDREN) {
        return -1;
    }
    const int index = add_object(writer, 0xa);
    if (index >= 0) {
        writer->objects[index].child_count = count;
        for (size_t i = 0; i < count; i++) {
            writer->objects[index].children[i] = children[i];
        }
    }
    return index;
}

static int add_dict(bp_writer_t *writer, const int *keys, const int *values,
                    size_t count) {
    if (count > BP_MAX_CHILDREN) {
        return -1;
    }
    const int index = add_object(writer, 0xd);
    if (index >= 0) {
        writer->objects[index].dict_count = count;
        for (size_t i = 0; i < count; i++) {
            writer->objects[index].dict_keys[i] = keys[i];
            writer->objects[index].dict_values[i] = values[i];
        }
    }
    return index;
}

static size_t put_be(uint8_t *out, uint64_t value, size_t bytes) {
    for (size_t i = 0; i < bytes; i++) {
        out[bytes - i - 1] = (uint8_t)(value & 0xffu);
        value >>= 8;
    }
    return bytes;
}

static size_t object_encoded_size(const bp_object_t *object) {
    size_t count = object->data_length;
    if (object->type == 0x1) {
        count = object->number <= 0xff ? 1 : object->number <= 0xffff ? 2 :
                object->number <= 0xffffffffu ? 4 : 8;
        return 1 + count;
    }
    if (object->type == 0x8 || object->type == 0x9) {
        return 1;
    }
    if (object->type == 0xa) {
        return 1 + object->child_count;
    }
    if (object->type == 0xd) {
        return 1 + object->dict_count * 2;
    }
    return count < 15 ? 1 + count : count <= 255 ? 3 + count :
           count <= 65535 ? 4 + count : 0;
}

static int write_count_marker(uint8_t *out, uint8_t type, size_t count,
                              size_t *written) {
    if (count < 15) {
        out[0] = (uint8_t)(type | count);
        *written = 1;
        return 0;
    }
    if (count <= 255) {
        out[0] = (uint8_t)(type | 0x0f);
        out[1] = 0x10;
        out[2] = (uint8_t)count;
        *written = 3;
        return 0;
    }
    if (count <= 65535) {
        out[0] = (uint8_t)(type | 0x0f);
        out[1] = 0x11;
        out[2] = (uint8_t)(count >> 8);
        out[3] = (uint8_t)count;
        *written = 4;
        return 0;
    }
    return -1;
}

static size_t writer_finish(bp_writer_t *writer, int top, uint8_t *out,
                            size_t capacity) {
    if (writer->count == 0 || writer->count > 255 || top < 0 ||
        (size_t)top >= writer->count) {
        return 0;
    }
    const size_t ref_size = 1;
    size_t offset_size = 1;
    size_t table_at = 8;
    for (size_t i = 0; i < writer->count; i++) {
        table_at += object_encoded_size(&writer->objects[i]);
    }
    if (table_at > 0xff) offset_size = table_at <= 0xffff ? 2 : 4;
    const size_t total = table_at + writer->count * offset_size + 32;
    if (total > capacity) {
        return 0;
    }
    memset(out, 0, total);
    memcpy(out, "bplist00", 8);
    size_t at = 8;
    uint64_t offsets[BP_MAX_OBJECTS];
    for (size_t i = 0; i < writer->count; i++) {
        offsets[i] = at;
        bp_object_t *object = &writer->objects[i];
        const size_t count = object->type == 0x1 ?
            (object->number <= 0xff ? 1 : object->number <= 0xffff ? 2 :
             object->number <= 0xffffffffu ? 4 : 8) :
            (object->type == 0xa ? object->child_count :
             object->type == 0xd ? object->dict_count : object->data_length);
        size_t header_written = 0;
        if (object->type == 0x8 || object->type == 0x9) {
            out[at++] = object->type;
        } else if (object->type == 0x1) {
            const size_t exponent = count == 1 ? 0 : count == 2 ? 1 :
                                    count == 4 ? 2 : count == 8 ? 3 : 99;
            if (exponent > 3) {
                return 0;
            }
            out[at++] = (uint8_t)(0x10u | exponent);
        } else if (write_count_marker(out + at, object->type << 4, count,
                                      &header_written) != 0) {
            return 0;
        } else {
            at += header_written;
        }
        if (object->type == 0x1) {
            at += put_be(out + at, object->number, count);
        } else if (object->type == 0x4 || object->type == 0x5) {
            memcpy(out + at, object->data, count);
            at += count;
        } else if (object->type == 0xa) {
            for (size_t j = 0; j < count; j++) out[at++] = (uint8_t)object->children[j];
        } else if (object->type == 0xd) {
            for (size_t j = 0; j < count; j++) out[at++] = (uint8_t)object->dict_keys[j];
            for (size_t j = 0; j < count; j++) out[at++] = (uint8_t)object->dict_values[j];
        }
    }
    const size_t offset_table = at;
    for (size_t i = 0; i < writer->count; i++) {
        put_be(out + at, offsets[i], offset_size);
        at += offset_size;
    }
    uint8_t *trailer = out + at;
    trailer[6] = (uint8_t)offset_size;
    trailer[7] = (uint8_t)ref_size;
    put_be(trailer + 8, writer->count, 8);
    put_be(trailer + 16, (uint64_t)top, 8);
    put_be(trailer + 24, offset_table, 8);
    return at + 32;
}

static int build_display(bp_writer_t *writer) {
    const int k_width = add_string(writer, "width");
    const int k_height = add_string(writer, "height");
    const int k_pixels_w = add_string(writer, "widthPixels");
    const int k_pixels_h = add_string(writer, "heightPixels");
    const int k_maxfps = add_string(writer, "maxFPS");
    const int k_refresh = add_string(writer, "refreshRate");
    const int k_overscan = add_string(writer, "overscanned");
    const int k_features = add_string(writer, "features");
    const int k_uuid = add_string(writer, "uuid");
    const int v_width = add_uint(writer, 1920);
    const int v_height = add_uint(writer, 1080);
    const int v_maxfps = add_uint(writer, 60);
    const int v_refresh = add_uint(writer, 1);
    const int v_overscan = add_bool(writer, 0);
    const int v_features = add_uint(writer, 14);
    const int v_uuid = add_string(writer, "e0ff8a27-6738-3d56-8a16-cc53aacee925");
    const int display_keys[] = {k_width, k_height, k_pixels_w, k_pixels_h,
                                k_maxfps, k_refresh, k_overscan, k_features, k_uuid};
    const int display_values[] = {v_width, v_height, v_width, v_height, v_maxfps,
                                  v_refresh, v_overscan, v_features, v_uuid};
    const int display = add_dict(writer, display_keys, display_values,
                                 sizeof(display_keys) / sizeof(display_keys[0]));
    return add_array(writer, &display, 1);
}

size_t bplist_build_info(uint8_t *out, size_t capacity, const char *name,
                         const char *device_id, uint64_t features,
                         const char *model, const char *source_version) {
    bp_writer_t writer;
    memset(&writer, 0, sizeof(writer));
    const int keys[] = {
        add_string(&writer, "deviceID"), add_string(&writer, "macAddress"),
        add_string(&writer, "name"), add_string(&writer, "model"),
        add_string(&writer, "sourceVersion"), add_string(&writer, "protovers"),
        add_string(&writer, "osBuildVersion"), add_string(&writer, "pi"),
        add_string(&writer, "features"), add_string(&writer, "vv"),
        add_string(&writer, "statusFlags"), add_string(&writer, "displays")
    };
    const int values[] = {
        add_string(&writer, device_id), add_string(&writer, device_id),
        add_string(&writer, name), add_string(&writer, model),
        add_string(&writer, source_version), add_string(&writer, "1.0"),
        add_string(&writer, "12B435"),
        add_string(&writer, "2e388006-13ba-4041-9a67-25dd4a43d536"),
        add_uint(&writer, features), add_uint(&writer, 2), add_uint(&writer, 68),
        build_display(&writer)
    };
    const int root = add_dict(&writer, keys, values,
                              sizeof(keys) / sizeof(keys[0]));
    return writer_finish(&writer, root, out, capacity);
}

size_t bplist_build_setup(uint8_t *out, size_t capacity, int timing_port,
                          int event_port, int mirror_port, int audio_data_port,
                          int audio_control_port, int want_mirror, int want_audio) {
    bp_writer_t writer;
    memset(&writer, 0, sizeof(writer));
    int root_keys[3];
    int root_values[3];
    size_t root_count = 0;
    root_keys[root_count] = add_string(&writer, "timingPort");
    root_values[root_count++] = add_uint(&writer, (uint64_t)timing_port);
    root_keys[root_count] = add_string(&writer, "eventPort");
    root_values[root_count++] = add_uint(&writer, (uint64_t)event_port);
    int stream_nodes[2];
    size_t stream_count = 0;
    if (want_mirror) {
        const int keys[] = {add_string(&writer, "dataPort"), add_string(&writer, "type")};
        const int values[] = {add_uint(&writer, (uint64_t)mirror_port), add_uint(&writer, 110)};
        stream_nodes[stream_count++] = add_dict(&writer, keys, values, 2);
    }
    if (want_audio) {
        const int keys[] = {add_string(&writer, "dataPort"), add_string(&writer, "controlPort"),
                            add_string(&writer, "type")};
        const int values[] = {add_uint(&writer, (uint64_t)audio_data_port),
                              add_uint(&writer, (uint64_t)audio_control_port), add_uint(&writer, 96)};
        stream_nodes[stream_count++] = add_dict(&writer, keys, values, 3);
    }
    if (stream_count) {
        root_keys[root_count] = add_string(&writer, "streams");
        root_values[root_count++] = add_array(&writer, stream_nodes, stream_count);
    }
    const int root = add_dict(&writer, root_keys, root_values, root_count);
    return writer_finish(&writer, root, out, capacity);
}

size_t bplist_build_data_dict(uint8_t *out, size_t capacity, const char *const *names,
                              const uint8_t *const *values, const size_t *lengths,
                              size_t count) {
    bp_writer_t writer;
    memset(&writer, 0, sizeof(writer));
    int keys[4];
    int items[4];
    if (count > sizeof(keys) / sizeof(keys[0])) {
        return 0;
    }
    for (size_t i = 0; i < count; i++) {
        keys[i] = add_string(&writer, names[i]);
        items[i] = add_data(&writer, values[i], lengths[i]);
    }
    const int root = add_dict(&writer, keys, items, count);
    return writer_finish(&writer, root, out, capacity);
}
