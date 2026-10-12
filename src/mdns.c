#include "mdns.h"
#include "receiver_log.h"

#include "airplay_config.h"

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MDNS_GROUP "224.0.0.251"
#define MDNS_MAX_PACKET 1500

static void put16(uint8_t **cursor, uint16_t value) {
    uint16_t network = htons(value);
    memcpy(*cursor, &network, sizeof(network));
    *cursor += sizeof(network);
}

static void put32(uint8_t **cursor, uint32_t value) {
    uint32_t network = htonl(value);
    memcpy(*cursor, &network, sizeof(network));
    *cursor += sizeof(network);
}

static int put_name(uint8_t **cursor, const char *name) {
    const char *start = name;
    while (*start) {
        const char *dot = strchr(start, '.');
        size_t length = dot ? (size_t)(dot - start) : strlen(start);
        if (length == 0 || length > 63) {
            return -1;
        }
        *(*cursor)++ = (uint8_t)length;
        memcpy(*cursor, start, length);
        *cursor += length;
        if (!dot) {
            break;
        }
        start = dot + 1;
    }
    *(*cursor)++ = 0;
    return 0;
}

static int put_txt(uint8_t **cursor, const char *text) {
    size_t length = strlen(text);
    if (length > 255) {
        return -1;
    }
    *(*cursor)++ = (uint8_t)length;
    memcpy(*cursor, text, length);
    *cursor += length;
    return 0;
}

static int add_record(uint8_t **cursor, const char *name, uint16_t type,
                      uint32_t address, uint16_t port, const char *target,
                      const char *const *txt, size_t txt_count, uint32_t ttl) {
    if (put_name(cursor, name) != 0) {
        return -1;
    }
    put16(cursor, type);
    put16(cursor, 1);
    put32(cursor, ttl);
    uint8_t *length_field = *cursor;
    put16(cursor, 0);
    uint8_t *rdata_start = *cursor;

    if (type == 12) {
        if (put_name(cursor, target) != 0) {
            return -1;
        }
    } else if (type == 33) {
        put16(cursor, 0);
        put16(cursor, 0);
        put16(cursor, port);
        if (put_name(cursor, target) != 0) {
            return -1;
        }
    } else if (type == 16) {
        for (size_t index = 0; index < txt_count; index++) {
            if (put_txt(cursor, txt[index]) != 0) {
                return -1;
            }
        }
    } else if (type == 1) {
        memcpy(*cursor, &address, sizeof(address));
        *cursor += sizeof(address);
    } else {
        return -1;
    }

    size_t rdlength = (size_t)(*cursor - rdata_start);
    if (rdlength > 65535) {
        return -1;
    }
    uint16_t network_length = htons((uint16_t)rdlength);
    memcpy(length_field, &network_length, sizeof(network_length));
    return 0;
}

static size_t build_response(const mdns_server_t *server, uint8_t *packet,
                             uint16_t id, uint32_t ttl) {
    char public_key_entry[80];
    (void)snprintf(public_key_entry, sizeof(public_key_entry), "pk=%s",
                   server->public_key);
    /* Status flag 0x8 is "PIN required". */
    const char *const txt[] = {
        "deviceid=" AIRPLAY_DEVICE_ID,
        "features=" AIRPLAY_FEATURES,
        server->pin_required ? "flags=" AIRPLAY_FLAGS_PIN : "flags=" AIRPLAY_FLAGS,
        "model=" AIRPLAY_MODEL,
        public_key_entry,
        "pi=" AIRPLAY_PI,
        server->pin_required ? "pw=true" : "pw=false",
        "srcvers=" AIRPLAY_SRCVERS,
        "vv=2",
    };
    char service[128];
    (void)snprintf(service, sizeof(service), "%s._airplay._tcp.local",
                   server->display_name);
    const char *airplay_type = "_airplay._tcp.local";
    const char *services_type = "_services._dns-sd._udp.local";

    uint8_t *cursor = packet;
    put16(&cursor, id);
    put16(&cursor, 0x8400);
    put16(&cursor, 0);
    put16(&cursor, 5);
    put16(&cursor, 0);
    put16(&cursor, 0);

    if (add_record(&cursor, services_type, 12, 0, 0, airplay_type, NULL, 0, ttl) != 0 ||
        add_record(&cursor, airplay_type, 12, 0, 0, service, NULL, 0, ttl) != 0 ||
        add_record(&cursor, service, 33, 0, AIRPLAY_RTSP_PORT, server->host_name, NULL, 0, ttl) != 0 ||
        add_record(&cursor, service, 16, 0, 0, NULL, txt, sizeof(txt) / sizeof(txt[0]), ttl) != 0 ||
        add_record(&cursor, server->host_name, 1, server->address, 0, NULL, NULL, 0, ttl) != 0) {
        return 0;
    }
    return (size_t)(cursor - packet);
}

static int choose_ipv4(mdns_server_t *server) {
    struct ifaddrs *interfaces = NULL;
    if (getifaddrs(&interfaces) != 0) {
        return -1;
    }
    int result = -1;
    for (struct ifaddrs *item = interfaces; item; item = item->ifa_next) {
        if (!item->ifa_addr || item->ifa_addr->sa_family != AF_INET ||
            (item->ifa_flags & IFF_LOOPBACK) != 0) {
            continue;
        }
        const struct sockaddr_in *address = (const struct sockaddr_in *)item->ifa_addr;
        server->address = address->sin_addr.s_addr;
        if (!inet_ntop(AF_INET, &address->sin_addr, server->address_text,
                       sizeof(server->address_text))) {
            continue;
        }
        result = 0;
        break;
    }
    freeifaddrs(interfaces);
    return result;
}

int mdns_server_init(mdns_server_t *server) {
    memset(server, 0, sizeof(*server));
    server->fd = -1;
    server->enabled = 1;
    if (choose_ipv4(server) != 0) {
        receiver_log("mdns", "no non-loopback IPv4 interface\n");
        return -1;
    }
    snprintf(server->host_name, sizeof(server->host_name), "%s", AIRPLAY_HOSTNAME);
    snprintf(server->display_name, sizeof(server->display_name), "%s", AIRPLAY_NAME);
    server->public_key[0] = '\0';

    server->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (server->fd < 0) {
        perror("socket(mDNS)");
        return -1;
    }
    int reuse = 1;
    (void)setsockopt(server->fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
    (void)setsockopt(server->fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif

    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(AIRPLAY_MDNS_PORT);
    if (bind(server->fd, (struct sockaddr *)&local, sizeof(local)) != 0) {
        receiver_log("mdns", "bind UDP/5353 failed, errno %d: queries will NOT be received", errno);
        close(server->fd);
        server->fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (server->fd < 0) {
            receiver_log("mdns", "socket fallback failed, errno %d", errno);
            return -1;
        }
    } else {
        receiver_log("mdns", "bound UDP/5353, receiving mDNS queries");
    }

    struct ip_mreq membership;
    memset(&membership, 0, sizeof(membership));
    membership.imr_multiaddr.s_addr = inet_addr(MDNS_GROUP);
    membership.imr_interface.s_addr = server->address;
    if (setsockopt(server->fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership,
                   sizeof(membership)) != 0) {
        receiver_log("mdns", "IP_ADD_MEMBERSHIP failed, errno %d", errno);
        close(server->fd);
        server->fd = -1;
        return -1;
    }
    unsigned char ttl = 255;
    (void)setsockopt(server->fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    (void)setsockopt(server->fd, IPPROTO_IP, IP_MULTICAST_IF,
                     &server->address, sizeof(server->address));
    return 0;
}

void mdns_server_close(mdns_server_t *server) {
    if (server->fd >= 0) {
        close(server->fd);
        server->fd = -1;
    }
}

int mdns_server_announce(mdns_server_t *server) {
    if (server->fd < 0 || !server->enabled) {
        return -1;
    }
    uint8_t packet[MDNS_MAX_PACKET];
    size_t length = build_response(server, packet, 0, 120);
    if (length == 0) {
        return -1;
    }
    struct sockaddr_in destination;
    memset(&destination, 0, sizeof(destination));
    destination.sin_family = AF_INET;
    destination.sin_addr.s_addr = inet_addr(MDNS_GROUP);
    destination.sin_port = htons(AIRPLAY_MDNS_PORT);
    ssize_t sent = sendto(server->fd, packet, length, 0,
                          (struct sockaddr *)&destination, sizeof(destination));
    return sent == (ssize_t)length ? 0 : -1;
}

static int mdns_server_send_goodbye(mdns_server_t *server) {
    if (server->fd < 0) {
        return -1;
    }
    uint8_t packet[MDNS_MAX_PACKET];
    size_t length = build_response(server, packet, 0, 0);
    if (length == 0) {
        return -1;
    }
    struct sockaddr_in destination;
    memset(&destination, 0, sizeof(destination));
    destination.sin_family = AF_INET;
    destination.sin_addr.s_addr = inet_addr(MDNS_GROUP);
    destination.sin_port = htons(AIRPLAY_MDNS_PORT);
    ssize_t sent = sendto(server->fd, packet, length, 0,
                          (struct sockaddr *)&destination, sizeof(destination));
    return sent == (ssize_t)length ? 0 : -1;
}

int mdns_server_set_enabled(mdns_server_t *server, int enabled) {
    enabled = enabled != 0;
    if (server->enabled == enabled) {
        return 0;
    }
    if (!enabled) {
        (void)mdns_server_send_goodbye(server);
    }
    server->enabled = enabled;
    if (enabled) {
        return mdns_server_announce(server);
    }
    return 0;
}

int mdns_server_set_name(mdns_server_t *server, const char *name) {
    if (!name || !*name || strlen(name) >= sizeof(server->display_name)) {
        return -1;
    }
    for (const unsigned char *cursor = (const unsigned char *)name; *cursor; cursor++) {
        if (*cursor < 0x20 || *cursor == '.' || *cursor == '/') {
            return -1;
        }
    }
    snprintf(server->display_name, sizeof(server->display_name), "%s", name);
    return server->enabled ? mdns_server_announce(server) : 0;
}

int mdns_server_set_public_key(mdns_server_t *server, const char *hex) {
    if (!hex || strlen(hex) != 64) {
        return -1;
    }
    snprintf(server->public_key, sizeof(server->public_key), "%s", hex);
    return server->enabled ? mdns_server_announce(server) : 0;
}

int mdns_server_set_pin_required(mdns_server_t *server, int required) {
    server->pin_required = required != 0;
    return server->enabled ? mdns_server_announce(server) : 0;
}

const char *mdns_server_name(const mdns_server_t *server) {
    return server->display_name;
}

static int packet_mentions(const uint8_t *packet, size_t length, const char *needle) {
    const size_t needle_length = strlen(needle);
    for (size_t i = 0; i + needle_length <= length; i++) {
        if (memcmp(packet + i, needle, needle_length) == 0) {
            return 1;
        }
    }
    return 0;
}

int mdns_server_poll(mdns_server_t *server, int timeout_ms) {
    if (server->fd < 0) {
        return -1;
    }
    struct pollfd descriptor = {.fd = server->fd, .events = POLLIN, .revents = 0};
    int ready = poll(&descriptor, 1, timeout_ms);
    if (ready <= 0 || (descriptor.revents & POLLIN) == 0) {
        return ready;
    }
    uint8_t query[MDNS_MAX_PACKET];
    struct sockaddr_in source;
    socklen_t source_length = sizeof(source);
    ssize_t received = recvfrom(server->fd, query, sizeof(query), 0,
                                 (struct sockaddr *)&source, &source_length);
    const int is_response = received >= 12 && (query[2] & 0x80) != 0;
    if (received >= 12 && !is_response && source.sin_addr.s_addr != server->address) {
        static int queries_logged = 0;
        if (queries_logged < 60 && received > 0) {
            if (packet_mentions(query, (size_t)received, "_airplay")) {
                queries_logged++;
                receiver_log("mdns", "AirPlay browse query from %s", inet_ntoa(source.sin_addr));
            } else if (packet_mentions(query, (size_t)received, "ps5-airplay")) {
                queries_logged++;
                receiver_log("mdns", "host name query for ps5-airplay.local from %s",
                             inet_ntoa(source.sin_addr));
            }
        }
        uint16_t id;
        memcpy(&id, query, sizeof(id));
        id = ntohs(id);
        (void)mdns_server_announce(server);
    }
    return 1;
}
