#include "installer_http.h"

#include "app_install.h"
#include "embedded_assets.h"
#include "installer_config.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int server_fd = -1;
static volatile int install_requested = 0;
static volatile int install_completed = 0;

static int send_all(int fd, const void *data, size_t size) {
    const uint8_t *bytes = data;
    size_t sent = 0;
    while (sent < size) {
        ssize_t count = send(fd, bytes + sent, size - sent, 0);
        if (count <= 0) {
            return -1;
        }
        sent += (size_t)count;
    }
    return 0;
}

static void respond(int client, int status, const char *type,
                    const uint8_t *body, size_t body_size) {
    char header[512];
    int header_size = snprintf(header, sizeof(header),
                               "HTTP/1.1 %d %s\r\n"
                               "Content-Type: %s\r\n"
                               "Content-Length: %zu\r\n"
                               "Cache-Control: no-store\r\n"
                               "Connection: close\r\n\r\n",
                               status, status == 200 ? "OK" : "Not Found",
                               type, body_size);
    if (header_size > 0 && (size_t)header_size < sizeof(header)) {
        (void)send_all(client, header, (size_t)header_size);
        (void)send_all(client, body, body_size);
    }
}

static void handle_request(int client) {
    char request[4096];
    ssize_t count = recv(client, request, sizeof(request) - 1, 0);
    if (count <= 0) {
        close(client);
        return;
    }
    request[count] = '\0';
    char path[512] = {0};
    if (sscanf(request, "GET %511s", path) != 1) {
        close(client);
        return;
    }
    char *query = strchr(path, '?');
    if (query) {
        *query = '\0';
    }

    if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
        respond(client, 200, "text/html; charset=utf-8", installer_html_data,
                installer_html_size);
    } else if (strcmp(path, "/cache.appcache") == 0) {
        respond(client, 200, "text/cache-manifest", cache_manifest_data,
                cache_manifest_size);
    } else if (strcmp(path, "/app/index.html") == 0) {
        respond(client, 200, "text/html; charset=utf-8", app_html_data,
                app_html_size);
    } else if (strcmp(path, "/status") == 0) {
        const char *status = app_install_status();
        respond(client, 200, "text/plain; charset=utf-8", (const uint8_t *)status,
                strlen(status));
    } else if (strcmp(path, "/install") == 0) {
        if (install_completed || app_install_payload_and_shortcut() == 0) {
            static const uint8_t body[] = "installed\n";
            install_requested = 1;
            install_completed = 1;
            respond(client, 200, "text/plain; charset=utf-8", body, sizeof(body) - 1);
        } else {
            static const uint8_t body[] = "install failed\n";
            respond(client, 500, "text/plain; charset=utf-8", body, sizeof(body) - 1);
        }
    } else {
        static const uint8_t body[] = "not found\n";
        respond(client, 404, "text/plain; charset=utf-8", body, sizeof(body) - 1);
    }
    close(client);
}

void installer_http_mark_installed(void) {
    install_completed = 1;
}

int installer_http_init(void) {
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket(installer HTTP)");
        return -1;
    }
    int reuse = 1;
    (void)setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(AIRPLAY_INSTALLER_PORT);
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(server_fd, 4) != 0) {
        perror("bind/listen(installer HTTP)");
        close(server_fd);
        server_fd = -1;
        return -1;
    }
    return 0;
}

void installer_http_close(void) {
    if (server_fd >= 0) {
        close(server_fd);
        server_fd = -1;
    }
}

int installer_http_run(void) {
    return installer_http_run_for_ms(-1);
}

int installer_http_run_for_ms(int timeout_ms) {
    struct timespec start;
    if (timeout_ms >= 0) {
        (void)clock_gettime(CLOCK_MONOTONIC, &start);
    }
    while (!install_requested && server_fd >= 0) {
        struct pollfd descriptor = {.fd = server_fd, .events = POLLIN, .revents = 0};
        int wait_ms = 1000;
        if (timeout_ms >= 0) {
            struct timespec now;
            (void)clock_gettime(CLOCK_MONOTONIC, &now);
            int elapsed = (int)((now.tv_sec - start.tv_sec) * 1000 +
                                (now.tv_nsec - start.tv_nsec) / 1000000);
            int remaining = timeout_ms - elapsed;
            if (remaining <= 0) {
                break;
            }
            if (remaining < wait_ms) {
                wait_ms = remaining;
            }
        }
        int ready = poll(&descriptor, 1, wait_ms);
        if (ready <= 0 || (descriptor.revents & POLLIN) == 0) {
            continue;
        }
        struct sockaddr_in remote;
        socklen_t remote_size = sizeof(remote);
        int client = accept(server_fd, (struct sockaddr *)&remote, &remote_size);
        if (client >= 0) {
            handle_request(client);
        }
    }
    return install_requested ? 0 : -1;
}
