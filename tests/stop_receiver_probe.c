/* Development-only cleanup for the named PS5 AirPlay receiver. */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>

int sceNetInit(void);

int main(void) {
    (void)sceNetInit();
    int mib[4] = {1, 14, 8, 0};
    size_t buffer_size = 0;
    if (sysctl(mib, 4, NULL, &buffer_size, NULL, 0) != 0) return 1;
    uint8_t *buffer = malloc(buffer_size);
    if (!buffer || sysctl(mib, 4, buffer, &buffer_size, NULL, 0) != 0) {
        free(buffer);
        return 1;
    }
    pid_t own_pid = getpid();
    int stopped = 0;
    for (uint8_t *cursor = buffer; cursor < buffer + buffer_size;) {
        int record_size = *(int *)cursor;
        if (record_size <= 0 || cursor + record_size > buffer + buffer_size) break;
        pid_t pid = *(pid_t *)(cursor + 72);
        const char *name = (const char *)(cursor + 447);
        if (pid != own_pid && strcmp(name, "ps5airplay.elf") == 0 &&
            kill(pid, SIGTERM) == 0) {
            stopped++;
        }
        cursor += record_size;
    }
    free(buffer);

    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) return 1;
    int reuse = 1;
    (void)setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(18188);
    if (bind(server, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(server, 1) != 0) {
        close(server);
        return 1;
    }
    int client = accept(server, NULL, NULL);
    if (client >= 0) {
        char report[64];
        int length = snprintf(report, sizeof(report), "stopped=%d\n", stopped);
        if (length > 0) (void)send(client, report, (size_t)length, 0);
        close(client);
    }
    close(server);
    return 0;
}
