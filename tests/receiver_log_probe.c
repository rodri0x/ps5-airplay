/* Read-only: sends the receiver's event log (AIRPLAY_LOG_PATH) over a temporary
 * TCP port.  Run it with elfldr, then read it with:
 *   nc <PS5_HOST> 18194
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define LOG_PORT 18194
#define LOG_PATH "/data/ps5-airplay/receiver.log"
#define LOG_PREVIOUS_PATH "/data/ps5-airplay/receiver.previous.log"

static void send_file(int client, const char *path, const char *title) {
    (void)send(client, title, strlen(title), 0);
    FILE *file = fopen(path, "rb");
    if (!file) {
        const char *missing = "missing\n";
        (void)send(client, missing, strlen(missing), 0);
        return;
    }
    char buffer[4096];
    size_t count;
    while ((count = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        size_t sent = 0;
        while (sent < count) {
            ssize_t written = send(client, buffer + sent, count - sent, 0);
            if (written <= 0) {
                break;
            }
            sent += (size_t)written;
        }
    }
    fclose(file);
}

int sceNetInit(void);

int main(void) {
    (void)sceNetInit();
    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) {
        return 1;
    }
    int reuse = 1;
    (void)setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(LOG_PORT);
    if (bind(server, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(server, 1) != 0) {
        close(server);
        return 1;
    }
    int client = accept(server, NULL, NULL);
    if (client >= 0) {
        send_file(client, LOG_PREVIOUS_PATH, "== previous session (receiver.previous.log)\n");
        send_file(client, LOG_PATH, "== current session (receiver.log)\n");
        close(client);
    }
    close(server);
    return 0;
}
