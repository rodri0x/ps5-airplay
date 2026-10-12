#include "notify.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef struct notify_request {
    char useless1[45];
    char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int device, notify_request_t *request,
                                     size_t size, int unused);

void ps5_notify(const char *fmt, ...) {
    notify_request_t request;
    va_list args;
    memset(&request, 0, sizeof(request));
    va_start(args, fmt);
    (void)vsnprintf(request.message, sizeof(request.message), fmt, args);
    va_end(args);
    (void)sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
}
