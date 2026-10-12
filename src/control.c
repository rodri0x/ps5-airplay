#include "control.h"
#include "airplay_playout.h"
#include "receiver_log.h"

#include "airplay_config.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define CONTROL_MAX_REQUEST 8192

static int send_all(int fd, const char *data, size_t length) {
    size_t sent = 0;
    while (sent < length) {
        ssize_t count = send(fd, data + sent, length - sent, 0);
        if (count <= 0) {
            return -1;
        }
        sent += (size_t)count;
    }
    return 0;
}

static const char *reason_phrase(int status) {
    switch (status) {
        case 200: return "OK";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 422: return "Unprocessable Entity";
        default: return "Internal Server Error";
    }
}

static void respond(int client, int status, const char *type,
                    const char *body, size_t body_size) {
    char header[768];
    int header_size = snprintf(
        header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n\r\n",
        status, reason_phrase(status), type, body_size);
    if (header_size > 0 && (size_t)header_size < sizeof(header)) {
        (void)send_all(client, header, (size_t)header_size);
        (void)send_all(client, body, body_size);
    }
}

static size_t json_escape(char *out, size_t out_size, const char *value) {
    size_t used = 0;
    for (const unsigned char *cursor = (const unsigned char *)value;
         *cursor && used + 1 < out_size; cursor++) {
        const char *replacement = NULL;
        char escaped[3] = {0};
        if (*cursor == '\\' || *cursor == '"') {
            escaped[0] = '\\';
            escaped[1] = (char)*cursor;
            replacement = escaped;
        } else if (*cursor < 0x20) {
            replacement = " ";
        }
        if (replacement) {
            size_t length = strlen(replacement);
            if (used + length >= out_size) {
                break;
            }
            memcpy(out + used, replacement, length);
            used += length;
        } else {
            out[used++] = (char)*cursor;
        }
    }
    out[used] = '\0';
    return used;
}

static int find_json_string(const char *json, const char *key,
                            char *out, size_t out_size) {
    char needle[64];
    int needle_size = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (needle_size <= 0 || (size_t)needle_size >= sizeof(needle)) {
        return -1;
    }
    const char *cursor = strstr(json, needle);
    if (!cursor) {
        return 0;
    }
    cursor = strchr(cursor + needle_size, ':');
    if (!cursor) {
        return -1;
    }
    cursor++;
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n') {
        cursor++;
    }
    if (*cursor++ != '"') {
        return -1;
    }
    size_t used = 0;
    while (*cursor && *cursor != '"') {
        unsigned char value = (unsigned char)*cursor++;
        if (value == '\\' && *cursor) {
            value = (unsigned char)*cursor++;
            if (value != '\\' && value != '"') {
                return -1;
            }
        }
        if (used + 1 >= out_size) {
            return -1;
        }
        out[used++] = (char)value;
    }
    if (*cursor != '"') {
        return -1;
    }
    out[used] = '\0';
    return 1;
}

static int find_json_bool(const char *json, const char *key, int *value) {
    char needle[64];
    int needle_size = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (needle_size <= 0 || (size_t)needle_size >= sizeof(needle)) {
        return -1;
    }
    const char *cursor = strstr(json, needle);
    if (!cursor) {
        return 0;
    }
    cursor = strchr(cursor + needle_size, ':');
    if (!cursor) {
        return -1;
    }
    cursor++;
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n') {
        cursor++;
    }
    if (strncmp(cursor, "true", 4) == 0) {
        *value = 1;
        return 1;
    }
    if (strncmp(cursor, "false", 5) == 0) {
        *value = 0;
        return 1;
    }
    return -1;
}

static void copy_bounded(char *out, size_t out_size, const char *value) {
    if (out_size == 0) {
        return;
    }
    snprintf(out, out_size, "%s", value);
}

void airplay_settings_defaults(airplay_settings_t *settings) {
    memset(settings, 0, sizeof(*settings));
    copy_bounded(settings->name, sizeof(settings->name), AIRPLAY_NAME);
    copy_bounded(settings->mode, sizeof(settings->mode), AIRPLAY_MODE_DEFAULT);
    settings->enabled = 1;
}

static int valid_passcode(const char *value) {
    if (value[0] == '\0') {
        return 1;
    }
    for (int i = 0; i < 4; i++) {
        if (value[i] < '0' || value[i] > '9') {
            return 0;
        }
    }
    return value[4] == '\0';
}

static int read_settings_file(char *buffer, size_t buffer_size) {
    FILE *file = fopen(AIRPLAY_SETTINGS_PATH, "rb");
    if (!file) {
        return -1;
    }
    size_t length = fread(buffer, 1, buffer_size - 1, file);
    int error = ferror(file);
    (void)fclose(file);
    if (error) {
        return -1;
    }
    buffer[length] = '\0';
    return 0;
}

void airplay_settings_load(airplay_settings_t *settings) {
    char json[2048];
    if (read_settings_file(json, sizeof(json)) != 0) {
        return;
    }
    char value[64];
    int result = find_json_string(json, "name", value, sizeof(value));
    if (result > 0 && value[0] != '\0') {
        copy_bounded(settings->name, sizeof(settings->name), value);
    }
    result = find_json_string(json, "passcode", value, sizeof(value));
    if (result > 0 && valid_passcode(value)) {
        copy_bounded(settings->passcode, sizeof(settings->passcode), value);
    }
    result = find_json_string(json, "mode", value, sizeof(value));
    if (result > 0 && airplay_playout_mode_valid(value)) {
        copy_bounded(settings->mode, sizeof(settings->mode), value);
    }
    int enabled = settings->enabled;
    result = find_json_bool(json, "enabled", &enabled);
    if (result > 0) {
        settings->enabled = enabled;
    }
}

int airplay_settings_save(const airplay_settings_t *settings) {
    (void)mkdir(AIRPLAY_DATA_DIR, 0755);
    FILE *file = fopen(AIRPLAY_SETTINGS_PATH, "wb");
    if (!file) {
        receiver_log("api", "cannot save settings: %s\n", strerror(errno));
        return -1;
    }
    char name[128];
    (void)json_escape(name, sizeof(name), settings->name);
    int written = fprintf(file,
                          "{\"name\":\"%s\",\"enabled\":%s,\"passcode\":\"%s\",\"mode\":\"%s\"}\n",
                          name, settings->enabled ? "true" : "false", settings->passcode,
                          settings->mode);
    int close_error = fclose(file);
    return written > 0 && close_error == 0 ? 0 : -1;
}

static int apply_settings(control_server_t *server, const char *json) {
    airplay_settings_t next = *server->settings;
    char value[64];
    int result = find_json_string(json, "name", value, sizeof(value));
    if (result < 0) {
        return 400;
    }
    if (result > 0) {
        if (value[0] == '\0' || strlen(value) >= sizeof(next.name)) {
            return 422;
        }
        copy_bounded(next.name, sizeof(next.name), value);
    }
    result = find_json_string(json, "passcode", value, sizeof(value));
    if (result < 0) {
        return 400;
    }
    if (result > 0) {
        if (!valid_passcode(value)) {
            return 422;
        }
        copy_bounded(next.passcode, sizeof(next.passcode), value);
    }
    result = find_json_string(json, "mode", value, sizeof(value));
    if (result < 0) {
        return 400;
    }
    if (result > 0) {
        if (!airplay_playout_mode_valid(value)) {
            return 422;
        }
        copy_bounded(next.mode, sizeof(next.mode), value);
    }
    int enabled = next.enabled;
    result = find_json_bool(json, "enabled", &enabled);
    if (result < 0) {
        return 400;
    }
    if (result > 0) {
        next.enabled = enabled;
    }
    if (strcmp(next.name, server->settings->name) != 0 &&
        mdns_server_set_name(server->mdns, next.name) != 0) {
        return 422;
    }
    if (next.enabled != server->settings->enabled) {
        (void)mdns_server_set_enabled(server->mdns, next.enabled);
        (void)rtsp_server_set_enabled(server->rtsp, next.enabled);
    }
    const int passcode_changed = strcmp(next.passcode, server->settings->passcode) != 0;
    *server->settings = next;
    airplay_playout_set_mode(next.mode);
    if (passcode_changed) {
        rtsp_server_set_passcode(server->rtsp, next.passcode);
        (void)mdns_server_set_pin_required(server->mdns, next.passcode[0] != '\0');
    }
    (void)airplay_settings_save(server->settings);
    return 200;
}

static int make_status_json(const control_server_t *server, char *out,
                            size_t out_size) {
    char name[128];
    (void)json_escape(name, sizeof(name), mdns_server_name(server->mdns));
    int length = snprintf(
        out, out_size,
        "{\"name\":\"%s\",\"enabled\":%s,\"passcodeSet\":%s,\"mode\":\"%s\","
        "\"version\":\"" AIRPLAY_VERSION "\",\"rtspPort\":%d,\"controlPort\":%d}",
        name, server->settings->enabled ? "true" : "false",
        server->settings->passcode[0] ? "true" : "false",
        server->settings->mode, AIRPLAY_RTSP_PORT, AIRPLAY_CONTROL_PORT);
    return length > 0 && (size_t)length < out_size ? length : -1;
}

static int content_length(const char *request) {
    const char *header = strstr(request, "Content-Length:");
    if (!header) {
        return 0;
    }
    header += strlen("Content-Length:");
    while (*header == ' ' || *header == '\t') {
        header++;
    }
    long length = strtol(header, NULL, 10);
    return length >= 0 && length <= CONTROL_MAX_REQUEST ? (int)length : -1;
}

static void handle_client(control_server_t *server, int client) {
    char request[CONTROL_MAX_REQUEST + 1];
    size_t used = 0;
    size_t header_end = 0;
    int wanted_body = 0;
    while (used < CONTROL_MAX_REQUEST) {
        ssize_t count = recv(client, request + used, CONTROL_MAX_REQUEST - used, 0);
        if (count <= 0) {
            break;
        }
        used += (size_t)count;
        request[used] = '\0';
        char *end = strstr(request, "\r\n\r\n");
        if (!end) {
            end = strstr(request, "\n\n");
        }
        if (end) {
            header_end = (size_t)(end - request) + (end[0] == '\r' ? 4 : 2);
            wanted_body = content_length(request);
            if (wanted_body < 0 || used >= header_end + (size_t)wanted_body) {
                break;
            }
            if (used >= header_end + (size_t)wanted_body) {
                break;
            }
        }
    }
    request[used] = '\0';
    if (header_end == 0) {
        static const char body[] = "bad request\n";
        respond(client, 400, "text/plain; charset=utf-8", body, sizeof(body) - 1);
        return;
    }

    char method[16] = {0};
    char path[256] = {0};
    if (sscanf(request, "%15s %255s", method, path) != 2) {
        static const char body[] = "bad request\n";
        respond(client, 400, "text/plain; charset=utf-8", body, sizeof(body) - 1);
        return;
    }
    char *query = strchr(path, '?');
    if (query) {
        *query = '\0';
    }
    if (strcmp(method, "OPTIONS") == 0) {
        respond(client, 204, "text/plain", "", 0);
        return;
    }
    if (strcmp(method, "GET") == 0 &&
        (strcmp(path, "/api/status") == 0 || strcmp(path, "/api/settings") == 0)) {
        char body[1024];
        int length = make_status_json(server, body, sizeof(body));
        if (length < 0) {
            respond(client, 500, "text/plain; charset=utf-8", "status failed\n", 15);
        } else {
            respond(client, 200, "application/json; charset=utf-8", body, (size_t)length);
        }
        return;
    }
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/settings") == 0) {
        const char *body_start = request + header_end;
        size_t available = used - header_end;
        if (wanted_body < 0 || (size_t)wanted_body > available) {
            static const char body[] = "request body incomplete\n";
            respond(client, 400, "text/plain; charset=utf-8", body, sizeof(body) - 1);
            return;
        }
        char body[1024];
        size_t body_size = (size_t)wanted_body < sizeof(body) - 1 ?
                           (size_t)wanted_body : sizeof(body) - 1;
        memcpy(body, body_start, body_size);
        body[body_size] = '\0';
        int status = apply_settings(server, body);
        receiver_log("api", "POST /api/settings -> %d body=%.120s", status, body);
        if (status != 200) {
            static const char error_body[] = "invalid settings\n";
            respond(client, status, "text/plain; charset=utf-8", error_body,
                    sizeof(error_body) - 1);
            return;
        }
        char response[1024];
        int length = make_status_json(server, response, sizeof(response));
        respond(client, 200, "application/json; charset=utf-8", response,
                length > 0 ? (size_t)length : 0);
        return;
    }
    static const char body[] = "not found\n";
    respond(client, 404, "text/plain; charset=utf-8", body, sizeof(body) - 1);
}

static int open_control_listener(control_server_t *server) {
    if (server->listen_fd >= 0) {
        close(server->listen_fd);
        server->listen_fd = -1;
    }
    server->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->listen_fd < 0) {
        perror("socket(AirPlay control)");
        return -1;
    }
    int reuse = 1;
    (void)setsockopt(server->listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(AIRPLAY_CONTROL_PORT);
    if (bind(server->listen_fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(server->listen_fd, 4) != 0) {
        perror("bind/listen(AirPlay control)");
        close(server->listen_fd);
        server->listen_fd = -1;
        return -1;
    }
    return 0;
}

int control_server_init(control_server_t *server, airplay_settings_t *settings,
                        mdns_server_t *mdns, rtsp_server_t *rtsp) {
    memset(server, 0, sizeof(*server));
    server->listen_fd = -1;
    server->settings = settings;
    server->mdns = mdns;
    server->rtsp = rtsp;
    return open_control_listener(server);
}

void control_server_close(control_server_t *server) {
    if (server->listen_fd >= 0) {
        close(server->listen_fd);
        server->listen_fd = -1;
    }
}

int control_server_poll(control_server_t *server, int timeout_ms) {
    if (server->rtsp && server->listener_generation != server->rtsp->listener_generation) {
        server->listener_generation = server->rtsp->listener_generation;
        if (open_control_listener(server) == 0) {
            receiver_log("api", "control listener reopened on 127.0.0.1:%d", AIRPLAY_CONTROL_PORT);
        }
    }
    if (server->listen_fd < 0) {
        return -1;
    }
    struct pollfd descriptor = {.fd = server->listen_fd, .events = POLLIN, .revents = 0};
    int ready = poll(&descriptor, 1, timeout_ms);
    if (ready <= 0 || (descriptor.revents & POLLIN) == 0) {
        return ready;
    }
    struct sockaddr_in remote;
    socklen_t remote_length = sizeof(remote);
    int client = accept(server->listen_fd, (struct sockaddr *)&remote, &remote_length);
    if (client < 0) {
        return -1;
    }
    handle_client(server, client);
    close(client);
    return 1;
}
