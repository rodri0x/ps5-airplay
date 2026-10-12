#include "update_check.h"

#include "airplay_config.h"
#include "notify.h"
#include "receiver_log.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define RELEASES_URL "https://api.github.com/repos/" AIRPLAY_UPDATE_REPO "/releases/latest"
#define RELEASES_PAGE "github.com/" AIRPLAY_UPDATE_REPO "/releases"

int update_check_compare(const char *a, const char *b) {
    if (*a == 'v' || *a == 'V') a++;
    if (*b == 'v' || *b == 'V') b++;
    while (*a || *b) {
        char *a_end = (char *)a;
        char *b_end = (char *)b;
        const long a_part = strtol(a, &a_end, 10);
        const long b_part = strtol(b, &b_end, 10);
        if (a_part != b_part) {
            return a_part < b_part ? -1 : 1;
        }
        a = *a_end == '.' ? a_end + 1 : a_end;
        b = *b_end == '.' ? b_end + 1 : b_end;
        if (a == a_end && b == b_end) {
            break;
        }
    }
    return 0;
}

#ifndef UPDATE_CHECK_HOST_BUILD

int sceNetPoolCreate(const char *name, int size, int flags);
int sceNetPoolDestroy(int pool);
int sceSslInit(size_t pool_size);
int sceSslTerm(int context);
int sceHttp2Init(int net_pool, int ssl_context, size_t pool_size, int max_requests);
int sceHttp2Term(int context);
int sceHttp2CreateTemplate(int context, const char *agent, int http_version, int auto_proxy);
int sceHttp2DeleteTemplate(int template_id);
int sceHttp2SetSslCallback(int id,
                           int (*callback)(int ssl_context, unsigned int verify_error,
                                           void *const certificates[], int count,
                                           void *user),
                           void *user);
int sceHttp2CreateRequestWithURL(int template_id, const char *method, const char *url,
                                 uint64_t content_length);
int sceHttp2DeleteRequest(int request);
int sceHttp2SendRequest(int request, const void *body, size_t size);
int sceHttp2GetStatusCode(int request, int *status);
int sceHttp2ReadData(int request, void *data, size_t size);

/* Certificates are not verified: the console's store does not know GitHub's
 * authority, and jailbroken consoles often sit behind a DNS with its own.  The
 * answer only feeds a notification; it must never be used to fetch or run
 * anything. */
static int accept_certificate(int ssl_context, unsigned int verify_error,
                              void *const certificates[], int count, void *user) {
    (void)ssl_context;
    (void)verify_error;
    (void)certificates;
    (void)count;
    (void)user;
    return 0;
}

/* Fetches the URL into `body`.  Returns the HTTP status, or -1. */
static int https_get(const char *url, char *body, size_t capacity) {
    int status = -1;
    int net = -1, ssl = -1, http = -1, template_id = -1, request = -1;
    int result = 0;
    const char *step = "sceNetPoolCreate";
    size_t used = 0;
    body[0] = '\0';
    if ((result = net = sceNetPoolCreate("airplay-update", 32 * 1024, 0)) >= 0 &&
        (step = "sceSslInit", result = ssl = sceSslInit(256 * 1024)) >= 0 &&
        (step = "sceHttp2Init", result = http = sceHttp2Init(net, ssl, 256 * 1024, 1)) >= 0 &&
        (step = "sceHttp2CreateTemplate",
         result = template_id = sceHttp2CreateTemplate(http, "ps5-airplay/" AIRPLAY_VERSION, 3, 1)) >= 0 &&
        (step = "sceHttp2SetSslCallback",
         result = sceHttp2SetSslCallback(template_id, accept_certificate, NULL)) >= 0 &&
        (step = "sceHttp2CreateRequestWithURL",
         result = request = sceHttp2CreateRequestWithURL(template_id, "GET", url, 0)) >= 0 &&
        (step = "sceHttp2SendRequest", result = sceHttp2SendRequest(request, NULL, 0)) >= 0 &&
        (step = "sceHttp2GetStatusCode", result = sceHttp2GetStatusCode(request, &status)) >= 0) {
        int count;
        while (used + 1 < capacity &&
               (count = sceHttp2ReadData(request, body + used, capacity - 1 - used)) > 0) {
            used += (size_t)count;
        }
        body[used] = '\0';
    } else {
        receiver_log("update", "%s failed: 0x%x", step, result);
        status = -1;
    }
    if (request >= 0) (void)sceHttp2DeleteRequest(request);
    if (template_id >= 0) (void)sceHttp2DeleteTemplate(template_id);
    if (http >= 0) (void)sceHttp2Term(http);
    if (ssl >= 0) (void)sceSslTerm(ssl);
    if (net >= 0) (void)sceNetPoolDestroy(net);
    return status;
}

static int latest_tag(const char *json, char *tag, size_t size) {
    const char *key = strstr(json, "\"tag_name\"");
    const char *start = key ? strchr(key + 10, '"') : NULL;
    const char *end = start ? strchr(start + 1, '"') : NULL;
    if (!end || (size_t)(end - start - 1) >= size || end - start - 1 <= 0) {
        return -1;
    }
    memcpy(tag, start + 1, (size_t)(end - start - 1));
    tag[end - start - 1] = '\0';
    return 0;
}

static void *check_thread(void *arg) {
    static char body[16384];
    char tag[32];
    (void)arg;
    const int status = https_get(RELEASES_URL, body, sizeof(body));
    if (status == 200 && latest_tag(body, tag, sizeof(tag)) == 0) {
        if (update_check_compare(tag, AIRPLAY_VERSION) > 0) {
            receiver_log("update", "running %s, %s is available", AIRPLAY_VERSION, tag);
            ps5_notify("PS5 AirPlay %s\nUpdate available: %s\n%s", AIRPLAY_VERSION,
                       tag[0] == 'v' ? tag + 1 : tag, RELEASES_PAGE);
        } else {
            receiver_log("update", "running %s, latest release is %s", AIRPLAY_VERSION, tag);
            ps5_notify("PS5 AirPlay %s\nUp to date", AIRPLAY_VERSION);
        }
    } else {
        receiver_log("update", "running %s, the update check failed (HTTP %d)",
                     AIRPLAY_VERSION, status);
        ps5_notify("PS5 AirPlay %s\nCould not check for updates", AIRPLAY_VERSION);
    }
    return NULL;
}

void update_check_start(void) {
    pthread_t thread;
    if (pthread_create(&thread, NULL, check_thread, NULL) == 0) {
        (void)pthread_detach(thread);
    }
}

#endif
