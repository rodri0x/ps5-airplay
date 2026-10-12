#include "airplay_timing.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int make_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static uint64_t ntp_now(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
    const uint64_t seconds = (uint64_t)ts.tv_sec + 2208988800ull;
    const uint64_t fraction = ((uint64_t)ts.tv_nsec << 32) / 1000000000ull;
    return (seconds << 32) | fraction;
}

static void write_be64(uint8_t *bytes, uint64_t value) {
    for (int i = 7; i >= 0; i--) {
        bytes[i] = (uint8_t)value;
        value >>= 8;
    }
}

int airplay_timing_init(airplay_timing_t *timing) {
    if (!timing) return -1;
    memset(timing, 0, sizeof(*timing));
    timing->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (timing->fd < 0) return -1;
    int reuse = 1;
    (void)setsockopt(timing->fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(0);
    if (bind(timing->fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        make_nonblocking(timing->fd) != 0) {
        close(timing->fd);
        timing->fd = -1;
        return -1;
    }
    return 0;
}

void airplay_timing_close(airplay_timing_t *timing) {
    if (!timing) return;
    if (timing->fd >= 0) close(timing->fd);
    timing->fd = -1;
    timing->peer_valid = 0;
    timing->peer_port = 0;
}

void airplay_timing_set_peer(airplay_timing_t *timing,
                             const struct sockaddr_in *peer,
                             uint16_t remote_port) {
    if (!timing || !peer || timing->fd < 0 || remote_port == 0) return;
    timing->peer = *peer;
    timing->peer.sin_port = htons(remote_port);
    timing->peer_port = remote_port;
    timing->peer_valid = 1;
    timing->next_probe_ms = 0;
}

void airplay_timing_clear_peer(airplay_timing_t *timing) {
    if (!timing) return;
    timing->peer_valid = 0;
    timing->peer_port = 0;
    timing->next_probe_ms = 0;
}

static void send_probe(airplay_timing_t *timing) {
    if (!timing->peer_valid) return;
    uint8_t packet[32] = {0x80, 0xd2, 0x00, 0x07};
    write_be64(packet + 24, ntp_now());
    (void)sendto(timing->fd, packet, sizeof(packet), 0,
                 (const struct sockaddr *)&timing->peer, sizeof(timing->peer));
}

static void answer_probe(airplay_timing_t *timing, const uint8_t *request,
                         size_t length, const struct sockaddr_in *from) {
    if (length < 32 || !from || request[1] != 0xd2) return;
    uint8_t response[32] = {0x80, 0xd3, 0x00, 0x07};
    memcpy(response + 8, request + 24, 8);
    write_be64(response + 16, ntp_now());
    write_be64(response + 24, ntp_now());
    (void)sendto(timing->fd, response, sizeof(response), 0,
                 (const struct sockaddr *)from, sizeof(*from));
}

int airplay_timing_poll(airplay_timing_t *timing, uint64_t now_ms) {
    if (!timing || timing->fd < 0) return -1;
    if (timing->peer_valid && (timing->next_probe_ms == 0 ||
                               now_ms >= timing->next_probe_ms)) {
        send_probe(timing);
        timing->next_probe_ms = now_ms + 3000u;
    }
    for (;;) {
        uint8_t packet[256];
        struct sockaddr_in from;
        socklen_t from_length = sizeof(from);
        ssize_t length = recvfrom(timing->fd, packet, sizeof(packet), 0,
                                  (struct sockaddr *)&from, &from_length);
        if (length < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            return -1;
        }
        if (length < 8) continue;
        timing->packets++;
        if (packet[1] == 0xd2) answer_probe(timing, packet, (size_t)length, &from);
    }
    return 0;
}

int airplay_timing_port(const airplay_timing_t *timing) {
    if (!timing || timing->fd < 0) return -1;
    struct sockaddr_in address;
    socklen_t length = sizeof(address);
    if (getsockname(timing->fd, (struct sockaddr *)&address, &length) != 0) return -1;
    return ntohs(address.sin_port);
}
