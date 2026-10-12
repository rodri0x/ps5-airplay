#include "receiver_log.h"

#include "airplay_config.h"

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define RECEIVER_LOG_LIMIT (512L * 1024L)

static struct timespec log_origin;
static int log_started = 0;
static int log_full = 0;

#define LOG_PENDING_CAPACITY (64u * 1024u)

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t log_has_lines = PTHREAD_COND_INITIALIZER;
static pthread_cond_t log_drained = PTHREAD_COND_INITIALIZER;
static pthread_once_t log_writer_once = PTHREAD_ONCE_INIT;
static char log_pending[LOG_PENDING_CAPACITY];
static size_t log_pending_used;
static unsigned int log_lines_lost;
static int log_writing;
static int log_writer_running;

static double seconds_since_start(void) {
    struct timespec now;
    if (!log_started || clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0.0;
    }
    return (double)(now.tv_sec - log_origin.tv_sec) +
           (double)(now.tv_nsec - log_origin.tv_nsec) / 1e9;
}

static void utc_clock(char *text, size_t size) {
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
        snprintf(text, size, "--:--:--.---Z");
        return;
    }
    time_t seconds = now.tv_sec;
    long day_seconds = (long)(seconds % 86400);
    snprintf(text, size, "%02ld:%02ld:%02ld.%03ldZ", day_seconds / 3600,
             (day_seconds / 60) % 60, day_seconds % 60, now.tv_nsec / 1000000);
}

static void write_lines(const char *lines, size_t length) {
    FILE *file = fopen(AIRPLAY_LOG_PATH, "ab");
    if (!file) {
        return;
    }
    if (fseek(file, 0, SEEK_END) == 0 && ftell(file) < RECEIVER_LOG_LIMIT) {
        (void)fwrite(lines, 1, length, file);
    } else if (!log_full) {
        log_full = 1;
        (void)fputs("log limit reached\n", file);
    }
    (void)fclose(file);
}

static void *log_writer(void *arg) {
    static char batch[LOG_PENDING_CAPACITY];
    (void)arg;
    pthread_mutex_lock(&log_lock);
    for (;;) {
        while (log_pending_used == 0) {
            pthread_cond_wait(&log_has_lines, &log_lock);
        }
        const size_t length = log_pending_used;
        memcpy(batch, log_pending, length);
        log_pending_used = 0;
        log_writing = 1;
        pthread_mutex_unlock(&log_lock);
        write_lines(batch, length);
        pthread_mutex_lock(&log_lock);
        log_writing = 0;
        pthread_cond_broadcast(&log_drained);
    }
    return NULL;
}

static void start_log_writer(void) {
    pthread_t thread;
    if (pthread_create(&thread, NULL, log_writer, NULL) == 0) {
        (void)pthread_detach(thread);
        log_writer_running = 1;
    }
}

void receiver_log_start(void) {
    (void)rename(AIRPLAY_LOG_PATH, AIRPLAY_LOG_PREVIOUS_PATH);
    FILE *file = fopen(AIRPLAY_LOG_PATH, "wb");
    if (file) {
        (void)fclose(file);
    }
    (void)clock_gettime(CLOCK_MONOTONIC, &log_origin);
    log_started = 1;
    log_full = 0;
    receiver_log("main", "log started, file %s", AIRPLAY_LOG_PATH);
}

void receiver_log(const char *area, const char *format, ...) {
    char text[1024];
    va_list args;
    va_start(args, format);
    int written = vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (written < 0) {
        return;
    }
    size_t length = strlen(text);
    while (length > 0 && (text[length - 1] == '\n' || text[length - 1] == '\r')) {
        text[--length] = '\0';
    }
    char clock[32];
    char line[1100];
    utc_clock(clock, sizeof(clock));
    const int line_length = snprintf(line, sizeof(line), "%s %9.3f %-6s %s\n", clock,
                                     seconds_since_start(), area, text);
    if (line_length <= 0) {
        return;
    }
    const size_t size = (size_t)line_length < sizeof(line) ? (size_t)line_length
                                                           : sizeof(line) - 1;

    (void)pthread_once(&log_writer_once, start_log_writer);
    if (!log_writer_running) {
        write_lines(line, size);
        return;
    }
    pthread_mutex_lock(&log_lock);
    if (log_pending_used + size <= sizeof(log_pending)) {
        if (log_lines_lost != 0 && log_pending_used + size + 48 <= sizeof(log_pending)) {
            log_pending_used += (size_t)snprintf(log_pending + log_pending_used, 48,
                                                 "(%u log lines lost)\n", log_lines_lost);
            log_lines_lost = 0;
        }
        memcpy(log_pending + log_pending_used, line, size);
        log_pending_used += size;
        pthread_cond_signal(&log_has_lines);
    } else {
        log_lines_lost++;
    }
    pthread_mutex_unlock(&log_lock);
}

void receiver_log_flush(void) {
    if (!log_writer_running) {
        return;
    }
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 2;
    pthread_mutex_lock(&log_lock);
    while (log_pending_used != 0 || log_writing) {
        if (pthread_cond_timedwait(&log_drained, &log_lock, &until) != 0) {
            break;
        }
    }
    pthread_mutex_unlock(&log_lock);
}

static void crash_handler(int signo, siginfo_t *info, void *context) {
    (void)context;
    char line[160];
    void *address = info ? info->si_addr : NULL;
    int length = snprintf(line, sizeof(line), "crash: fatal signal %d at address %p\n",
                          signo, address);
    int fd = open(AIRPLAY_LOG_PATH, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd >= 0 && length > 0) {
        if (log_pending_used > 0 && log_pending_used <= sizeof(log_pending)) {
            (void)write(fd, log_pending, log_pending_used);
        }
        (void)write(fd, line, (size_t)length);
        (void)close(fd);
    }
    _exit(128 + signo);
}

void receiver_install_crash_logger(void) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = crash_handler;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    static const int fatal[] = {SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE};
    for (size_t i = 0; i < sizeof(fatal) / sizeof(fatal[0]); i++) {
        (void)sigaction(fatal[i], &action, NULL);
    }
}
