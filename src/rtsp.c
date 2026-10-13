#include "rtsp.h"
#include "receiver_log.h"

#include "airplay_config.h"
#include "airplay_pairing.h"
#include "bplist.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define RTSP_MAX_BODY (96u * 1024u)

typedef struct {
    char method[16];
    char path[128];
    char cseq[32];
    const uint8_t *body;
    size_t body_length;
    size_t message_length;
} rtsp_request_t;

static const char *reason_phrase(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 454: return "Session Not Found";
        case 470: return "Connection Authorization Required";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        default: return "Error";
    }
}

static int make_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int send_all(int fd, const void *data, size_t length) {
    const uint8_t *bytes = data;
    size_t sent = 0;
    while (sent < length) {
        ssize_t count = send(fd, bytes + sent, length - sent, 0);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return -1;
        }
        sent += (size_t)count;
    }
    return 0;
}

static const char *find_header_end(const uint8_t *data, size_t length,
                                   size_t *delimiter_length) {
    if (length < 4) {
        return NULL;
    }
    for (size_t i = 0; i + 4 <= length; i++) {
        if (data[i] == '\r' && data[i + 1] == '\n' &&
            data[i + 2] == '\r' && data[i + 3] == '\n') {
            *delimiter_length = 4;
            return (const char *)(data + i);
        }
    }
    for (size_t i = 0; i + 2 <= length; i++) {
        if (data[i] == '\n' && data[i + 1] == '\n') {
            *delimiter_length = 2;
            return (const char *)(data + i);
        }
    }
    return NULL;
}

static int header_value(const uint8_t *header, size_t header_length,
                        const char *name, char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return -1;
    }
    out[0] = '\0';
    const size_t name_length = strlen(name);
    size_t line_start = 0;
    while (line_start < header_length) {
        size_t line_end = line_start;
        while (line_end < header_length && header[line_end] != '\n') {
            line_end++;
        }
        size_t content_end = line_end;
        if (content_end > line_start && header[content_end - 1] == '\r') {
            content_end--;
        }
        if (content_end - line_start > name_length &&
            strncasecmp((const char *)(header + line_start), name, name_length) == 0 &&
            header[line_start + name_length] == ':') {
            size_t value_start = line_start + name_length + 1;
            while (value_start < content_end &&
                   (header[value_start] == ' ' || header[value_start] == '\t')) {
                value_start++;
            }
            size_t value_length = content_end - value_start;
            if (value_length >= out_size) {
                value_length = out_size - 1;
            }
            memcpy(out, header + value_start, value_length);
            out[value_length] = '\0';
            return 1;
        }
        line_start = line_end < header_length ? line_end + 1 : header_length;
    }
    return 0;
}

static int parse_request(const uint8_t *data, size_t length,
                         rtsp_request_t *request) {
    memset(request, 0, sizeof(*request));
    size_t delimiter_length = 0;
    const char *delimiter = find_header_end(data, length, &delimiter_length);
    if (!delimiter) {
        return 0;
    }
    const size_t header_length = (size_t)(delimiter - (const char *)data);
    const uint8_t *line_end = memchr(data, '\n', header_length);
    if (!line_end) {
        return -1;
    }
    char start_line[256];
    size_t start_length = (size_t)(line_end - data);
    if (start_length > 0 && data[start_length - 1] == '\r') {
        start_length--;
    }
    if (start_length == 0 || start_length >= sizeof(start_line)) {
        return -1;
    }
    memcpy(start_line, data, start_length);
    start_line[start_length] = '\0';
    if (sscanf(start_line, "%15s %127s", request->method, request->path) != 2) {
        return -1;
    }
    char value[64];
    if (header_value(data, header_length, "CSeq", request->cseq,
                     sizeof(request->cseq)) <= 0) {
        snprintf(request->cseq, sizeof(request->cseq), "1");
    }
    size_t content_length = 0;
    int header_result = header_value(data, header_length, "Content-Length",
                                     value, sizeof(value));
    if (header_result > 0) {
        char *end = NULL;
        unsigned long parsed = strtoul(value, &end, 10);
        if (!end || *end != '\0' || parsed > RTSP_MAX_BODY) {
            return -1;
        }
        content_length = (size_t)parsed;
    }
    const size_t total = header_length + delimiter_length + content_length;
    if (total > length) {
        return 0;
    }
    request->body = data + header_length + delimiter_length;
    request->body_length = content_length;
    request->message_length = total;
    return 1;
}

static void log_printable(const char *label, const uint8_t *data, size_t length,
                          size_t limit) {
    char text[700];
    size_t used = 0;
    for (size_t i = 0; i < length && used + 4 < sizeof(text) && used < limit; i++) {
        unsigned char c = data[i];
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            text[used++] = '|';
        } else if (c >= 0x20 && c < 0x7f) {
            text[used++] = (char)c;
        } else {
            used += (size_t)snprintf(text + used, sizeof(text) - used, "\\x%02x", c);
        }
    }
    text[used] = '\0';
    receiver_log("rtsp", "%s: %s", label, text);
}

static void log_request_headers(const uint8_t *data, size_t length) {
    log_printable("request headers", data, length, 600);
}

static void log_plist_names(const char *label, const uint8_t *data, size_t length) {
    char names[400];
    size_t used = 0;
    int count = 0;
    size_t i = 0;
    while (i < length && count < 40) {
        size_t run = 0;
        while (i + run < length && data[i + run] >= 0x20 && data[i + run] < 0x7f) {
            run++;
        }
        if (run >= 3 && used + run + 2 < sizeof(names)) {
            memcpy(names + used, data + i, run);
            used += run;
            names[used++] = ' ';
            count++;
        }
        i += run ? run : 1;
    }
    names[used] = '\0';
    receiver_log("rtsp", "%s (%zu bytes): %s", label, length, names);
}

static int respond(int fd, const rtsp_request_t *request, int status,
                   const char *extra_headers, const void *body, size_t body_length,
                   int close_after) {
    char header[2048];
    if (strcmp(request->path, "/feedback") != 0) {
        receiver_log("rtsp", "%s %s body=%zu -> %d", request->method, request->path,
                     request->body_length, status);
    }
    int written = snprintf(header, sizeof(header),
                           "RTSP/1.0 %d %s\r\n"
                           "CSeq: %s\r\n"
                           "Server: AirTunes/220.68\r\n"
                           "%s"
                           "Content-Length: %zu\r\n"
                           "Connection: %s\r\n\r\n",
                           status, reason_phrase(status), request->cseq,
                           extra_headers ? extra_headers : "", body_length,
                           close_after ? "close" : "keep-alive");
    if (written <= 0 || (size_t)written >= sizeof(header) ||
        send_all(fd, header, (size_t)written) != 0) {
        return -1;
    }
    return body_length == 0 || send_all(fd, body, body_length) == 0 ? 0 : -1;
}

static uint64_t feature_number(void) {
    return strtoull(AIRPLAY_FEATURES, NULL, 0);
}

static int handle_fp_setup(rtsp_server_t *server, const rtsp_request_t *request,
                           int fd) {
    receiver_log("fairplay", "fp-setup: %zu bytes, byte4=0x%02x", request->body_length,
                 request->body_length > 4 ? request->body[4] : 0);
    uint8_t response[142];
    size_t response_length = 0;
    if (request->body_length == 16) {
        if (airplay_fairplay_setup(server->fairplay, request->body, response) != 0) {
            return respond(fd, request, 400, NULL, NULL, 0, 0);
        }
        response_length = sizeof(response);
    } else if (request->body_length == 164) {
        uint8_t handshake[32];
        if (airplay_fairplay_handshake(server->fairplay, request->body, handshake) != 0) {
            return respond(fd, request, 400, NULL, NULL, 0, 0);
        }
        memcpy(response, handshake, sizeof(handshake));
        response_length = sizeof(handshake);
    } else {
        return respond(fd, request, 400, NULL, NULL, 0, 0);
    }
    return respond(fd, request, 200,
                   "Content-Type: application/octet-stream\r\n",
                   response, response_length, 0);
}

static int handle_pair_setup(rtsp_server_t *server, const rtsp_request_t *request,
                             int fd) {
    uint8_t response[32];
    if (!server->identity || request->body_length != 32 ||
        airplay_pairing_setup(server->identity, &server->pairing, request->body,
                              response) != 0) {
        return respond(fd, request, 400, NULL, NULL, 0, 0);
    }
    return respond(fd, request, 200, "Content-Type: application/octet-stream\r\n",
                   response, sizeof(response), 0);
}

static int passcode_required(const rtsp_server_t *server) {
    return server->passcode[0] != '\0';
}

#define PAIRING_WRONG_CODES_ALLOWED 5u
#define PAIRING_PAUSE_MS 60000u

static uint64_t monotonic_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static int respond_data_dict(int fd, const rtsp_request_t *request, const char *const *names,
                             const uint8_t *const *values, const size_t *lengths,
                             size_t count) {
    uint8_t body[1024];
    const size_t length = bplist_build_data_dict(body, sizeof(body), names, values,
                                                 lengths, count);
    if (length == 0) {
        return respond(fd, request, 500, NULL, NULL, 0, 0);
    }
    return respond(fd, request, 200, "Content-Type: application/x-apple-binary-plist\r\n",
                   body, length, 0);
}

static int handle_pair_setup_pin(rtsp_server_t *server, const rtsp_request_t *request,
                                 int fd) {
    if (!passcode_required(server) || !server->identity) {
        return respond(fd, request, 470, NULL, NULL, 0, 0);
    }
    bp_item_t method, user, pk, proof, epk, tag;
    const int has_user =
        bplist_find(request->body, request->body_length, "user", &user) == 1 &&
        user.type == BP_ITEM_STRING;
    const int has_method =
        bplist_find(request->body, request->body_length, "method", &method) == 1 &&
        method.type == BP_ITEM_STRING;
    if (monotonic_ms() < server->pairing_paused_until_ms) {
        receiver_log("pair", "passcode pairing is paused after too many wrong codes");
        return respond(fd, request, 470, NULL, NULL, 0, 0);
    }
    if (has_user && has_method) {
        char name[sizeof(server->srp.username)];
        uint8_t random[AIRPLAY_SRP_SALT_SIZE + AIRPLAY_SRP_PRIVATE_SIZE];
        if (method.string_length != 3 || memcmp(method.string, "pin", 3) != 0 ||
            user.string_length == 0 || user.string_length >= sizeof(name) ||
            airplay_random_bytes(random, sizeof(random)) != 0) {
            return respond(fd, request, 470, NULL, NULL, 0, 0);
        }
        memcpy(name, user.string, user.string_length);
        name[user.string_length] = '\0';
        if (airplay_srp_start(&server->srp, name, server->passcode, random) != 0) {
            return respond(fd, request, 470, NULL, NULL, 0, 0);
        }
        receiver_log("pair", "passcode pairing started by %s", name);
        const char *const names[] = {"pk", "salt"};
        const uint8_t *const values[] = {server->srp.public_key, server->srp.salt};
        const size_t lengths[] = {server->srp.public_key_length, sizeof(server->srp.salt)};
        return respond_data_dict(fd, request, names, values, lengths, 2);
    }
    if (bplist_find(request->body, request->body_length, "proof", &proof) == 1 &&
        proof.type == BP_ITEM_DATA && proof.data_length >= AIRPLAY_SRP_PROOF_SIZE &&
        bplist_find(request->body, request->body_length, "pk", &pk) == 1 &&
        pk.type == BP_ITEM_DATA) {
        uint8_t reply[AIRPLAY_SRP_PROOF_SIZE];
        if (airplay_srp_prove(&server->srp, pk.data, pk.data_length, proof.data, reply) != 0) {
            receiver_log("pair", "passcode pairing refused: wrong code");
            if (++server->wrong_codes >= PAIRING_WRONG_CODES_ALLOWED) {
                server->wrong_codes = 0;
                server->pairing_paused_until_ms = monotonic_ms() + PAIRING_PAUSE_MS;
            }
            return respond(fd, request, 470, NULL, NULL, 0, 0);
        }
        const char *const names[] = {"proof"};
        const uint8_t *const values[] = {reply};
        const size_t lengths[] = {sizeof(reply)};
        return respond_data_dict(fd, request, names, values, lengths, 1);
    }
    if (bplist_find(request->body, request->body_length, "epk", &epk) == 1 &&
        epk.type == BP_ITEM_DATA && epk.data_length == 32 &&
        bplist_find(request->body, request->body_length, "authTag", &tag) == 1 &&
        tag.type == BP_ITEM_DATA && tag.data_length == 16) {
        uint8_t client_key[32], reply_epk[32], reply_tag[16];
        if (airplay_srp_exchange_keys(&server->srp, epk.data, tag.data,
                                      server->identity->public_key, client_key,
                                      reply_epk, reply_tag) != 0) {
            receiver_log("pair", "passcode pairing refused: key exchange did not verify");
            return respond(fd, request, 470, NULL, NULL, 0, 0);
        }
        server->wrong_codes = 0;
        airplay_paired_add(&server->paired, client_key, AIRPLAY_PAIRED_PATH);
        receiver_log("pair", "device paired with the passcode (%zu paired)",
                     server->paired.count);
        const char *const names[] = {"epk", "authTag"};
        const uint8_t *const values[] = {reply_epk, reply_tag};
        const size_t lengths[] = {sizeof(reply_epk), sizeof(reply_tag)};
        return respond_data_dict(fd, request, names, values, lengths, 2);
    }
    return respond(fd, request, 470, NULL, NULL, 0, 0);
}

static int handle_pair_verify(rtsp_server_t *server, const rtsp_request_t *request,
                              int fd, int *close_after) {
    if (!server->identity || request->body_length != 68) {
        return respond(fd, request, 400, NULL, NULL, 0, 0);
    }
    if (request->body[0] == 1) {
        /* An empty reply sends the sender to its passcode prompt. */
        if (passcode_required(server) &&
            !airplay_paired_contains(&server->paired, request->body + 36)) {
            receiver_log("pair", "pair-verify from a device that has not entered the passcode");
            return respond(fd, request, 200, NULL, NULL, 0, 0);
        }
        uint8_t response[96];
        if (airplay_pairing_verify_start(server->identity, &server->pairing,
                                         request->body, response) != 0) {
            return respond(fd, request, 400, NULL, NULL, 0, 0);
        }
        return respond(fd, request, 200, "Content-Type: application/octet-stream\r\n",
                       response, sizeof(response), 0);
    }
    if (request->body[0] == 0) {
        if (airplay_pairing_verify_finish(&server->pairing, request->body) != 0) {
            receiver_log("pair", "pair-verify step 2 rejected: client signature does not verify");
            *close_after = 1;
            return respond(fd, request, 470, NULL, NULL, 0, 1);
        }
        receiver_log("rtsp", "pair-verify completed\n");
        return respond(fd, request, 200, "Content-Type: application/octet-stream\r\n",
                       NULL, 0, 0);
    }
    return respond(fd, request, 400, NULL, NULL, 0, 0);
}

static void setup_audio_key(rtsp_server_t *server, const rtsp_request_t *request) {
    bp_item_t ekey;
    if (bplist_find(request->body, request->body_length, "ekey", &ekey) != 1 ||
        ekey.type != BP_ITEM_DATA || ekey.data_length != 72) {
        return;
    }
    uint8_t audio_key[16];
    if (airplay_fairplay_decrypt(server->fairplay, ekey.data, audio_key) == 0) {
        if (airplay_pairing_hash_key(&server->pairing, audio_key)) {
            receiver_log("rtsp", "audio key hashed with pair-verify secret\n");
        }
        airplay_mirror_set_audio_key(&server->mirror, audio_key);
        bp_item_t eiv;
        uint8_t audio_iv[16] = {0};
        if (bplist_find(request->body, request->body_length, "eiv", &eiv) == 1 &&
            eiv.type == BP_ITEM_DATA && eiv.data_length == sizeof(audio_iv)) {
            memcpy(audio_iv, eiv.data, sizeof(audio_iv));
        }
        airplay_audio_set_crypto(&server->audio, audio_key, audio_iv);
        server->setup_keys_ready = 1;
        receiver_log("rtsp", "SETUP audio/video key established\n");
    } else {
        receiver_log("rtsp", "SETUP ekey received before fp-setup\n");
    }
}

static int handle_setup(rtsp_server_t *server, const rtsp_request_t *request,
                        int fd) {
    log_plist_names("SETUP plist", request->body, request->body_length);
    int want_mirror = 0;
    int want_audio = 0;
    uint64_t stream_id = 0;
    (void)bplist_streams(request->body, request->body_length, &want_mirror,
                         &want_audio, &stream_id);
    setup_audio_key(server, request);
    bp_item_t remote_timing_port;
    if (server->remote_address_valid &&
        bplist_find(request->body, request->body_length, "timingPort",
                    &remote_timing_port) == 1 &&
        remote_timing_port.type == BP_ITEM_UINT &&
        remote_timing_port.uint_value > 0 &&
        remote_timing_port.uint_value <= 65535) {
        airplay_timing_set_peer(&server->timing, &server->remote_address,
                                (uint16_t)remote_timing_port.uint_value);
    }
    if (want_audio) {
        bp_item_t codec;
        bp_item_t spf;
        uint64_t codec_number = 8;
        uint64_t spf_number = 480;
        if (bplist_find(request->body, request->body_length, "ct", &codec) == 1 &&
            codec.type == BP_ITEM_UINT) codec_number = codec.uint_value;
        if (bplist_find(request->body, request->body_length, "spf", &spf) == 1 &&
            spf.type == BP_ITEM_UINT) spf_number = spf.uint_value;
        airplay_audio_set_format(&server->audio, (uint8_t)codec_number,
                                 (uint16_t)spf_number);
    }
    if (want_mirror) {
        if (airplay_mirror_start(&server->mirror, stream_id) != 0) {
            return respond(fd, request, 503, NULL, NULL, 0, 0);
        }
        server->stream_ready = 1;
    }
    const int mirror_port = airplay_mirror_port(&server->mirror);
    const int audio_data_port = airplay_audio_data_port(&server->audio);
    const int audio_control_port = airplay_audio_control_port(&server->audio);
    uint8_t response[4096];
    size_t response_length = bplist_build_setup(
        response, sizeof(response), airplay_timing_port(&server->timing), 0,
        want_mirror ? mirror_port : 0,
        want_audio ? audio_data_port : 0,
        want_audio ? audio_control_port : 0, want_mirror, want_audio);
    if (response_length == 0) {
        return respond(fd, request, 500, NULL, NULL, 0, 0);
    }
    receiver_log("rtsp", "SETUP streams mirror=%d audio=%d port=%d\n",
            want_mirror, want_audio, want_mirror ? mirror_port : audio_data_port);
    return respond(fd, request, 200,
                   "Session: 1\r\nContent-Type: application/x-apple-binary-plist\r\n",
                   response, response_length, 0);
}

static int handle_request(rtsp_server_t *server, const rtsp_request_t *request,
                          int fd, int *close_after) {
    if (!server->enabled) {
        static const char disabled[] = "PS5 AirPlay disabled\n";
        return respond(fd, request, 503, "Content-Type: text/plain\r\n",
                       disabled, sizeof(disabled) - 1, 0);
    }
    char path[128];
    snprintf(path, sizeof(path), "%s", request->path);
    char *query = strchr(path, '?');
    if (query) {
        *query = '\0';
    }
    if (strcmp(request->method, "OPTIONS") == 0) {
        return respond(fd, request, 200,
                       "Public: OPTIONS, GET, POST, ANNOUNCE, SETUP, RECORD, FLUSH, TEARDOWN, GET_PARAMETER, SET_PARAMETER\r\n",
                       NULL, 0, 0);
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(path, "/info") == 0) {
        uint8_t response[8192];
        size_t response_length = bplist_build_info(
            response, sizeof(response), server->name, AIRPLAY_DEVICE_ID,
            feature_number(), AIRPLAY_MODEL, AIRPLAY_SRCVERS);
        if (response_length == 0) {
            return respond(fd, request, 500, NULL, NULL, 0, 0);
        }
        return respond(fd, request, 200,
                       "Content-Type: application/x-apple-binary-plist\r\n",
                       response, response_length, 0);
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(path, "/pair-setup") == 0) {
        return handle_pair_setup(server, request, fd);
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(path, "/pair-verify") == 0) {
        return handle_pair_verify(server, request, fd, close_after);
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(path, "/pair-pin-start") == 0) {
        receiver_log("pair", "the sender is asking its user for the passcode");
        return respond(fd, request, passcode_required(server) ? 200 : 470, NULL, NULL, 0, 0);
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(path, "/pair-setup-pin") == 0) {
        return handle_pair_setup_pin(server, request, fd);
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(path, "/feedback") == 0) {
        return respond(fd, request, 200, NULL, NULL, 0, 0);
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(path, "/fp-setup") == 0) {
        return handle_fp_setup(server, request, fd);
    }
    if (strcmp(request->method, "SETUP") == 0) {
        if (passcode_required(server) && !server->pairing.verified) {
            receiver_log("pair", "SETUP refused: the passcode has not been entered");
            return respond(fd, request, 470, NULL, NULL, 0, 0);
        }
        return handle_setup(server, request, fd);
    }
    if (strcmp(request->method, "ANNOUNCE") == 0 ||
        strcmp(request->method, "RECORD") == 0 ||
        strcmp(request->method, "GET_PARAMETER") == 0 ||
        strcmp(request->method, "SET_PARAMETER") == 0 ||
        strcmp(request->method, "FLUSH") == 0) {
        const char *headers = NULL;
        if (strcmp(request->method, "RECORD") == 0) {
            headers = "Session: 1\r\nAudio-Latency: 11025\r\nAudio-Jack-Status: connected; type=analog\r\n";
        } else if (strcmp(request->method, "GET_PARAMETER") == 0) {
            static const char volume[] = "volume: 0.0\r\n";
            return respond(fd, request, 200,
                           "Session: 1\r\nContent-Type: text/parameters\r\n",
                           volume, sizeof(volume) - 1, 0);
        }
        return respond(fd, request, 200, headers ? headers : "Session: 1\r\n",
                       NULL, 0, 0);
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(path, "/audioMode") == 0) {
        bp_item_t mode;
        if (bplist_find(request->body, request->body_length, "audioMode", &mode) == 1 &&
            mode.type == BP_ITEM_STRING) {
            receiver_log("rtsp", "audioMode %.*s", (int)mode.string_length, mode.string);
        }
        return respond(fd, request, 200, NULL, NULL, 0, 0);
    }
    if (strcmp(request->method, "TEARDOWN") == 0) {
        /* Stream types: 96 audio, 110 mirror.  No list ends the session. */
        int ends_mirror = 0, ends_audio = 0;
        uint64_t ignored = 0;
        (void)bplist_streams(request->body, request->body_length, &ends_mirror,
                             &ends_audio, &ignored);
        const int audio_only = ends_audio && !ends_mirror;
        receiver_log("rtsp", "TEARDOWN mirror=%d audio=%d", ends_mirror, ends_audio);
        if (audio_only) {
            return respond(fd, request, 200, NULL, NULL, 0, 0);
        }
        airplay_mirror_stop_stream(&server->mirror);
        if (server->mirror_end_callback) {
            server->mirror_end_callback(server->mirror_end_opaque);
        }
        *close_after = 1;
        return respond(fd, request, 200, NULL, NULL, 0, 1);
    }
    static const char unsupported[] = "Unsupported AirPlay request\n";
    return respond(fd, request, 501, "Content-Type: text/plain\r\n",
                   unsupported, sizeof(unsupported) - 1, 0);
}

static void close_client(rtsp_server_t *server) {
    if (server->client_fd >= 0) {
        close(server->client_fd);
        server->client_fd = -1;
        airplay_mirror_stop_stream(&server->mirror);
        if (server->mirror_end_callback) {
            server->mirror_end_callback(server->mirror_end_opaque);
        }
    }
    server->rx_used = 0;
    server->remote_address_valid = 0;
    airplay_timing_clear_peer(&server->timing);
}

static int process_client(rtsp_server_t *server) {
    for (;;) {
        if (server->rx_used == sizeof(server->rx)) {
            receiver_log("rtsp", "request buffer full (%zu bytes), closing", server->rx_used);
            return -1;
        }
        ssize_t count = recv(server->client_fd, server->rx + server->rx_used,
                             sizeof(server->rx) - server->rx_used, 0);
        if (count == 0) {
            receiver_log("rtsp", "client closed the connection (%zu bytes pending)",
                         server->rx_used);
            return -1;
        }
        if (count < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            receiver_log("rtsp", "recv failed, errno %d", errno);
            return -1;
        }
        server->rx_used += (size_t)count;
    }

    for (;;) {
        rtsp_request_t request;
        int parsed = parse_request(server->rx, server->rx_used, &request);
        if (parsed < 0) {
            log_printable("malformed request", server->rx, server->rx_used, 300);
            return -1;
        }
        if (parsed == 0) {
            break;
        }
        if (strcmp(request.path, "/feedback") != 0) {
            log_request_headers(server->rx, request.message_length - request.body_length);
        }
        int close_after = 0;
        server->last_request_ms = monotonic_ms();
        int handled = handle_request(server, &request, server->client_fd, &close_after);
        if (handled != 0 || close_after) {
            receiver_log("rtsp", "closing connection after %s %s: handler=%d close=%d",
                         request.method, request.path, handled, close_after);
            return -1;
        }
        const size_t remaining = server->rx_used - request.message_length;
        memmove(server->rx, server->rx + request.message_length, remaining);
        server->rx_used = remaining;
    }
    return 0;
}

#define ACTIVE_CLIENT_QUIET_MS 8000u

static int open_listener(rtsp_server_t *server) {
    if (server->listen_fd >= 0) {
        close(server->listen_fd);
        server->listen_fd = -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        receiver_log("rtsp", "TCP listen socket failed, errno %d", errno);
        return -1;
    }
    int reuse = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(AIRPLAY_RTSP_PORT);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        receiver_log("rtsp", "init failed: bind TCP/%d, errno %d", AIRPLAY_RTSP_PORT, errno);
        close(fd);
        return -1;
    }
    if (listen(fd, 4) != 0 || make_nonblocking(fd) != 0) {
        receiver_log("rtsp", "listen on TCP/%d failed, errno %d", AIRPLAY_RTSP_PORT, errno);
        close(fd);
        return -1;
    }
    server->listen_fd = fd;
    return 0;
}

static int listener_accepts(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    (void)make_nonblocking(fd);
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(AIRPLAY_RTSP_PORT);
    int result = connect(fd, (struct sockaddr *)&address, sizeof(address));
    if (result != 0 && errno == EINPROGRESS) {
        struct pollfd wait = {.fd = fd, .events = POLLOUT, .revents = 0};
        int error = 0;
        socklen_t length = sizeof(error);
        if (poll(&wait, 1, 200) == 1 &&
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0) {
            result = error == 0 ? 0 : -1;
            errno = error;
        } else {
            close(fd);
            return -1;
        }
    }
    const int refused = result != 0 && errno == ECONNREFUSED;
    close(fd);
    return result == 0 ? 1 : refused ? 0 : -1;
}

#define LISTENER_CHECK_INTERVAL_MS 10000u

/* Rest mode leaves the TCP listeners dead with nothing reported on the
 * sockets, so while idle the receiver connects to its own port and reopens the
 * listeners when that is refused. */
static void check_listener(rtsp_server_t *server, uint64_t now_ms) {
    if (server->client_fd >= 0 || now_ms < server->next_listener_check_ms) {
        return;
    }
    server->next_listener_check_ms = now_ms + LISTENER_CHECK_INTERVAL_MS;
    const int accepts = listener_accepts();
    if (accepts == 1) {
        server->self_check_pending = 1;
        return;
    }
    if (accepts != 0) {
        return;
    }
    receiver_log("rtsp", "TCP/%d stopped accepting connections (rest mode?); reopening the listeners",
                 AIRPLAY_RTSP_PORT);
    if (open_listener(server) == 0) {
        receiver_log("rtsp", "RTSP listening again on TCP/%d", AIRPLAY_RTSP_PORT);
    }
    if (airplay_mirror_reopen_listener(&server->mirror) != 0) {
        receiver_log("rtsp", "mirror listener could not be reopened, errno %d", errno);
    }
    server->listener_generation++;
}

int rtsp_server_init(rtsp_server_t *server) {
    if (!server) {
        return -1;
    }
    memset(server, 0, sizeof(*server));
    server->listen_fd = -1;
    server->client_fd = -1;
    server->enabled = 1;
    rtsp_server_set_name(server, AIRPLAY_NAME);
    server->fairplay = airplay_fairplay_create();
    if (!server->fairplay) {
        receiver_log("rtsp", "init failed: fairplay state");
    }
    const int mirror_result = airplay_mirror_init(&server->mirror);
    const int mirror_errno = errno;
    const int audio_result = airplay_audio_init(&server->audio);
    const int audio_errno = errno;
    const int timing_result = airplay_timing_init(&server->timing);
    const int timing_errno = errno;
    if (mirror_result != 0) {
        receiver_log("rtsp", "init failed: mirror listener, errno %d", mirror_errno);
    }
    if (audio_result != 0) {
        receiver_log("rtsp", "init failed: audio sockets, errno %d", audio_errno);
    }
    if (timing_result != 0) {
        receiver_log("rtsp", "init failed: timing socket, errno %d", timing_errno);
    }
    if (!server->fairplay || mirror_result != 0 || audio_result != 0 || timing_result != 0) {
        airplay_fairplay_destroy(server->fairplay);
        airplay_mirror_close(&server->mirror);
        airplay_audio_close(&server->audio);
        airplay_timing_close(&server->timing);
        return -1;
    }
    if (open_listener(server) != 0) {
        rtsp_server_close(server);
        return -1;
    }
    receiver_log("rtsp", "RTSP persistent server listening on TCP/%d\n",
            AIRPLAY_RTSP_PORT);
    return 0;
}

void rtsp_server_close(rtsp_server_t *server) {
    if (!server) {
        return;
    }
    close_client(server);
    if (server->listen_fd >= 0) {
        close(server->listen_fd);
        server->listen_fd = -1;
    }
    airplay_mirror_close(&server->mirror);
    airplay_audio_close(&server->audio);
    airplay_timing_close(&server->timing);
    airplay_fairplay_destroy(server->fairplay);
    server->fairplay = NULL;
}

int rtsp_server_poll(rtsp_server_t *server, int timeout_ms) {
    if (!server || server->listen_fd < 0) {
        return -1;
    }
    struct pollfd descriptors[2];
    nfds_t count = 0;
    descriptors[count++] = (struct pollfd){.fd = server->listen_fd, .events = POLLIN};
    if (server->client_fd >= 0) {
        descriptors[count++] = (struct pollfd){.fd = server->client_fd,
                                               .events = POLLIN | POLLERR | POLLHUP};
    }
    int ready = poll(descriptors, count, timeout_ms);
    if (ready < 0 && errno == EINTR) {
        ready = 0;
    }
    if (ready > 0 && (descriptors[0].revents & POLLIN) != 0) {
        struct sockaddr_in remote;
        socklen_t remote_length = sizeof(remote);
        int client = accept(server->listen_fd, (struct sockaddr *)&remote, &remote_length);
        if (client >= 0 && server->self_check_pending &&
            remote.sin_addr.s_addr == htonl(INADDR_LOOPBACK)) {
            server->self_check_pending = 0;
            close(client);
        } else if (client >= 0 && server->client_fd >= 0 &&
                   monotonic_ms() - server->last_request_ms < ACTIVE_CLIENT_QUIET_MS) {
            receiver_log("rtsp", "busy: connection from %s refused, a session is active",
                         inet_ntoa(remote.sin_addr));
            close(client);
        } else if (client >= 0) {
            (void)make_nonblocking(client);
            close_client(server);
            server->client_fd = client;
            server->last_request_ms = monotonic_ms();
            server->remote_address = remote;
            server->remote_address_valid = 1;
            server->connections++;
            airplay_pairing_session_reset(&server->pairing);
            memset(&server->srp, 0, sizeof(server->srp));
            receiver_log("rtsp", "client connected from %s:%d", inet_ntoa(remote.sin_addr), ntohs(remote.sin_port));
        }
    }
    if (server->client_fd >= 0 && count == 2 &&
        (descriptors[1].revents & (POLLIN | POLLERR | POLLHUP)) != 0) {
        if (process_client(server) != 0) {
            receiver_log("rtsp", "RTSP client disconnected\n");
            close_client(server);
        }
    }
    (void)airplay_mirror_poll(&server->mirror);
    (void)airplay_audio_poll(&server->audio);
    struct timespec now;
    uint64_t now_ms = 0;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
        now_ms = (uint64_t)now.tv_sec * 1000u +
                 (uint64_t)now.tv_nsec / 1000000u;
    }
    (void)airplay_timing_poll(&server->timing, now_ms);
    check_listener(server, now_ms);
    return ready;
}

void rtsp_server_set_identity(rtsp_server_t *server,
                              const airplay_identity_t *identity) {
    if (server) {
        server->identity = identity;
    }
}

void rtsp_server_set_name(rtsp_server_t *server, const char *name) {
    if (!server || !name) {
        return;
    }
    snprintf(server->name, sizeof(server->name), "%s", name);
}

void rtsp_server_set_passcode(rtsp_server_t *server, const char *passcode) {
    if (!server) {
        return;
    }
    snprintf(server->passcode, sizeof(server->passcode), "%s", passcode ? passcode : "");
    airplay_paired_load(&server->paired, server->passcode, AIRPLAY_PAIRED_PATH);
    memset(&server->srp, 0, sizeof(server->srp));
    receiver_log("pair", "passcode %s, %zu paired device(s)",
                 server->passcode[0] ? "required" : "off", server->paired.count);
}

int rtsp_server_set_enabled(rtsp_server_t *server, int enabled) {
    if (!server) {
        return -1;
    }
    server->enabled = enabled != 0;
    return 0;
}

void rtsp_server_set_video_callback(rtsp_server_t *server,
                                    rtsp_video_callback_t callback, void *opaque) {
    if (!server) {
        return;
    }
    airplay_mirror_set_video_callback(&server->mirror, callback, opaque);
}

void rtsp_server_set_mirror_end_callback(rtsp_server_t *server,
                                         rtsp_mirror_end_callback_t callback,
                                         void *opaque) {
    if (!server) {
        return;
    }
    server->mirror_end_callback = callback;
    server->mirror_end_opaque = opaque;
}

void rtsp_server_set_audio_callback(rtsp_server_t *server,
                                    rtsp_audio_callback_t callback, void *opaque) {
    if (!server) {
        return;
    }
    airplay_audio_set_callback(&server->audio, callback, opaque);
}
