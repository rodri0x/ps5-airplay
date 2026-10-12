/* Starts a system app and runs display_host.elf inside it.  The PS5 interface
 * only steps aside for an app in the foreground, so this is what makes the
 * mirror visible outside games.
 *
 * The launch sequence is adapted from src/ps5/hbldr.c of
 * ps5-payload-dev/websrv, Copyright (C) John Törnblom, GPLv3.  The ptrace and
 * ELF-loading helpers in third_party/websrv are that project's, unmodified. */

#include "display_carrier.h"

#include "airplay_config.h"
#include "airplay_playout.h"
#include "receiver_log.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <time.h>

#include <sys/event.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/uio.h>
#include <sys/wait.h>

#include <ps5/kernel.h>

#include "elfldr.h"
#include "pt.h"

#define CARRIER_TITLE_ID "AIRP00002"
#define CARRIER_PATH "/system_ex/app/" CARRIER_TITLE_ID
/* A system-signed eboot the console will start; the process is taken over
 * before its main() runs. */
#define DONOR_EBOOT "/system_ex/app/NPXS40106/eboot.bin"
#define HOST_NAME "display_host.elf"

#define IOVEC_ENTRY(x) {(void *)(x), (x) ? strlen(x) + 1 : 0}
#define IOVEC_SIZE(x) (sizeof(x) / sizeof(struct iovec))

static const char param_json[] = "{\n"
  "  \"applicationCategoryType\": 0,\n"
  "  \"attribute\": 1,\n"
  "  \"attribute2\": 0,\n"
  "  \"attribute3\": 4,\n"
  "  \"titleId\": \"" CARRIER_TITLE_ID "\",\n"
  "  \"contentId\": \"IV9999-" CARRIER_TITLE_ID "_00-PS5AIRPLAYDISP00\",\n"
  "  \"localizedParameters\": {\n"
  "    \"defaultLanguage\": \"en-US\",\n"
  "    \"en-US\": {\n"
  "      \"titleName\": \"PS5 AirPlay\"\n"
  "    }\n"
  "  }\n"
  "}\n";

typedef struct app_launch_ctx {
    uint32_t structsize;
    uint32_t user_id;
    uint32_t app_opt;
    uint64_t crash_report;
    uint32_t check_flag;
} app_launch_ctx_t;

int sceUserServiceInitialize(void *);
int sceUserServiceGetForegroundUser(uint32_t *user_id);
int sceSystemServiceGetAppIdOfRunningBigApp(void);
int sceSystemServiceKillApp(int app_id, int how, int reason, int core_dump);
int sceSystemServiceLaunchApp(const char *title_id, char **argv, app_launch_ctx_t *ctx);

extern const uint8_t display_host_elf_data[];
extern const size_t display_host_elf_size;

enum { CARRIER_DOWN, CARRIER_STARTING, CARRIER_UP };

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int state = CARRIER_DOWN;
static int carrier_app_id = -1;
static int release_requested;
static int launch_result;

#define carrier_log(...) receiver_log("carrier", __VA_ARGS__)

static int remount_system_ex(void) {
    struct iovec iov[] = {
        IOVEC_ENTRY("from"),      IOVEC_ENTRY("/dev/ssd0.system_ex"),
        IOVEC_ENTRY("fspath"),    IOVEC_ENTRY("/system_ex"),
        IOVEC_ENTRY("fstype"),    IOVEC_ENTRY("exfatfs"),
        IOVEC_ENTRY("large"),     IOVEC_ENTRY("yes"),
        IOVEC_ENTRY("timezone"),  IOVEC_ENTRY("static"),
        IOVEC_ENTRY("async"),     IOVEC_ENTRY((const char *)NULL),
        IOVEC_ENTRY("ignoreacl"), IOVEC_ENTRY((const char *)NULL),
    };
    return nmount(iov, IOVEC_SIZE(iov), MNT_UPDATE) ? -1 : 0;
}

static int copy_file(const char *from, const char *to, mode_t mode) {
    int in = open(from, O_RDONLY);
    if (in < 0) {
        return -1;
    }
    int out = open(to, O_CREAT | O_WRONLY | O_TRUNC, mode);
    if (out < 0) {
        close(in);
        return -1;
    }
    static uint8_t buffer[65536];
    int result = 0;
    for (;;) {
        ssize_t count = read(in, buffer, sizeof(buffer));
        if (count == 0) {
            break;
        }
        if (count < 0 || write(out, buffer, (size_t)count) != count) {
            result = -1;
            break;
        }
    }
    close(in);
    close(out);
    if (result != 0) {
        (void)unlink(to);
    }
    return result;
}

static int carrier_app_create_if_missing(void) {
    struct stat info;
    if (stat(CARRIER_PATH, &info) && mkdir(CARRIER_PATH, 0755)) {
        return -1;
    }
    if (stat(CARRIER_PATH "/sce_sys", &info) && mkdir(CARRIER_PATH "/sce_sys", 0755)) {
        return -1;
    }
    if (stat(CARRIER_PATH "/sce_sys/param.json", &info)) {
        int fd = open(CARRIER_PATH "/sce_sys/param.json", O_CREAT | O_WRONLY, 0644);
        if (fd < 0) {
            return -1;
        }
        const ssize_t size = (ssize_t)sizeof(param_json) - 1;
        const int failed = write(fd, param_json, (size_t)size) != size;
        close(fd);
        if (failed) {
            (void)unlink(CARRIER_PATH "/sce_sys/param.json");
            return -1;
        }
    }
    struct stat donor;
    if (stat(DONOR_EBOOT, &donor)) {
        carrier_log("donor eboot %s is missing", DONOR_EBOOT);
        return -1;
    }
    if (stat(CARRIER_PATH "/eboot.bin", &info) || info.st_size != donor.st_size) {
        if (copy_file(DONOR_EBOOT, CARRIER_PATH "/eboot.bin", 0755)) {
            return -1;
        }
    }
    return 0;
}

static pid_t find_pid(const char *name) {
    int mib[4] = {1, 14, 8, 0};
    pid_t mypid = getpid();
    pid_t pid = -1;
    size_t size = 0;
    if (sysctl(mib, 4, NULL, &size, NULL, 0)) {
        return -1;
    }
    uint8_t *buffer = malloc(size);
    if (!buffer) {
        return -1;
    }
    if (sysctl(mib, 4, buffer, &size, NULL, 0)) {
        free(buffer);
        return -1;
    }
    for (uint8_t *cursor = buffer; cursor < buffer + size;) {
        int record_size = *(int *)cursor;
        if (record_size <= 0) {
            break;
        }
        pid_t record_pid = *(pid_t *)&cursor[72];
        const char *thread_name = (const char *)&cursor[447];
        cursor += record_size;
        if (!strcmp(name, thread_name) && mypid != record_pid) {
            pid = record_pid;
        }
    }
    free(buffer);
    return pid;
}

static void *launch_thread(void *arg) {
    static char *argv[] = {(char *)HOST_NAME, NULL};
    app_launch_ctx_t ctx = {0};
    (void)arg;
    if (sceUserServiceGetForegroundUser(&ctx.user_id)) {
        launch_result = -1;
        return NULL;
    }
    launch_result = sceSystemServiceLaunchApp(CARRIER_TITLE_ID, argv, &ctx);
    return NULL;
}

static pid_t carrier_app_launch(void) {
    struct kevent event;
    pid_t parent = find_pid("SceSysCore.elf");
    if (parent < 0) {
        carrier_log("SceSysCore.elf not found");
        return -1;
    }
    int queue = kqueue();
    if (queue < 0) {
        return -1;
    }
    EV_SET(&event, parent, EVFILT_PROC, EV_ADD | EV_ENABLE | EV_CLEAR,
           NOTE_FORK | NOTE_EXEC | NOTE_TRACK, 0, 0);
    if (kevent(queue, &event, 1, NULL, 0, NULL) < 0) {
        carrier_log("kevent register failed, errno %d", errno);
        close(queue);
        return -1;
    }

    pthread_t thread;
    launch_result = 0;
    if (pthread_create(&thread, NULL, launch_thread, NULL) != 0) {
        close(queue);
        return -1;
    }
    pthread_detach(thread);

    pid_t child = -1;
    for (int attempt = 0; attempt < 20 && child < 0; attempt++) {
        struct timespec timeout = {.tv_sec = 1, .tv_nsec = 0};
        int count = kevent(queue, NULL, 0, &event, 1, &timeout);
        if (count < 0) {
            break;
        }
        if (count == 1 && (event.fflags & NOTE_CHILD)) {
            child = (pid_t)event.ident;
        }
    }
    close(queue);
    if (child < 0) {
        carrier_log("the system did not start %s (LaunchApp -> 0x%x)",
                    CARRIER_TITLE_ID, launch_result);
        return -1;
    }

    if (pt_attach(child)) {
        carrier_log("pt_attach(%d) failed, errno %d", (int)child, errno);
        return -1;
    }
    if (pt_follow_exec(child) < 0 || pt_continue(child, SIGCONT) < 0 ||
        pt_await_exec(child)) {
        carrier_log("following exec of pid %d failed, errno %d", (int)child, errno);
        pt_detach(child, SIGKILL);
        return -1;
    }
    return child;
}

static int carrier_app_replace(pid_t pid, uint8_t *elf) {
    uint8_t int3instr = 0xcc;
    uint8_t orginstr;
    intptr_t brkpoint;

    if (pt_syscall(pid, 599)) {
        carrier_log("sys_dynlib_process_needed_and_relocate failed");
    }
    elfldr_set_heap_size(pid, -1);

    if (!(brkpoint = kernel_dynlib_entry_addr(pid, 0))) {
        carrier_log("kernel_dynlib_entry_addr failed");
        return -1;
    }
    brkpoint += 58;
    if (kernel_mprotect(pid, brkpoint, PAGE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC)) {
        carrier_log("kernel_mprotect failed");
        return -1;
    }
    if (pt_copyout(pid, brkpoint, &orginstr, sizeof(orginstr)) ||
        pt_copyin(pid, &int3instr, brkpoint, sizeof(int3instr))) {
        carrier_log("setting the breakpoint failed, errno %d", errno);
        return -1;
    }
    if (pt_continue(pid, SIGCONT) || waitpid(pid, NULL, 0) == -1) {
        carrier_log("running to the breakpoint failed, errno %d", errno);
        return -1;
    }
    if (pt_copyin(pid, &orginstr, brkpoint, sizeof(orginstr))) {
        carrier_log("removing the breakpoint failed, errno %d", errno);
        return -1;
    }

    elfldr_raise_privileges(pid);
    elfldr_set_procname(pid, HOST_NAME);
    if (elfldr_exec(pid, elf)) {
        carrier_log("elfldr_exec failed");
        return -1;
    }
    return 0;
}

static int carrier_app_start(void) {
    static int user_service_ready;
    if (!user_service_ready) {
        (void)sceUserServiceInitialize(NULL);
        user_service_ready = 1;
    }
    if (carrier_app_create_if_missing()) {
        if (remount_system_ex()) {
            carrier_log("remounting /system_ex failed, errno %d", errno);
            return -1;
        }
        if (carrier_app_create_if_missing()) {
            carrier_log("creating %s failed, errno %d", CARRIER_PATH, errno);
            return -1;
        }
        carrier_log("created %s", CARRIER_PATH);
    }

    uint8_t *elf = malloc(display_host_elf_size);
    if (!elf) {
        return -1;
    }
    memcpy(elf, display_host_elf_data, display_host_elf_size);

    pid_t pid = carrier_app_launch();
    if (pid < 0) {
        free(elf);
        return -1;
    }
    if (carrier_app_replace(pid, elf) < 0) {
        pt_detach(pid, SIGKILL);
        free(elf);
        return -1;
    }
    free(elf);

    int app_id = -1;
    for (int attempt = 0; attempt < 30 && app_id <= 0; attempt++) {
        app_id = sceSystemServiceGetAppIdOfRunningBigApp();
        if (app_id <= 0) {
            usleep(100000);
        }
    }
    carrier_log("display host started: pid %d, app id 0x%x", (int)pid, app_id);
    usleep(1000000);
    return app_id > 0 ? app_id : -1;
}

static void *start_thread(void *arg) {
    (void)arg;
    int app_id = carrier_app_start();
    pthread_mutex_lock(&lock);
    if (app_id > 0) {
        state = CARRIER_UP;
        carrier_app_id = app_id;
    } else {
        state = CARRIER_DOWN;
    }
    const int release = release_requested;
    pthread_mutex_unlock(&lock);
    if (release) {
        display_carrier_release();
    }
    return NULL;
}

void display_carrier_heartbeat(void) {
    static int fd = -1;
    static struct timespec last;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return;
    }
    const long elapsed_ms = (long)(now.tv_sec - last.tv_sec) * 1000 +
                            (now.tv_nsec - last.tv_nsec) / 1000000;
    if (elapsed_ms < 500) {
        return;
    }
    last = now;
    if (fd < 0) {
        fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) {
            return;
        }
        (void)fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    }
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(AIRPLAY_HOST_AUDIO_PORT);
    const int delay_ms = airplay_playout_delay_ms();
    const uint8_t beat[4] = {'A', 'P', (uint8_t)delay_ms, (uint8_t)(delay_ms >> 8)};
    (void)sendto(fd, beat, sizeof(beat), 0, (const struct sockaddr *)&address,
                 sizeof(address));
}

void display_carrier_request(void) {
    pthread_mutex_lock(&lock);
    if (state != CARRIER_DOWN) {
        pthread_mutex_unlock(&lock);
        return;
    }
    const int running = sceSystemServiceGetAppIdOfRunningBigApp();
    if (running > 0) {
        carrier_log("app 0x%x is running; it holds the screen, nothing to start", running);
        pthread_mutex_unlock(&lock);
        return;
    }
    state = CARRIER_STARTING;
    release_requested = 0;
    pthread_mutex_unlock(&lock);

    pthread_t thread;
    if (pthread_create(&thread, NULL, start_thread, NULL) != 0) {
        pthread_mutex_lock(&lock);
        state = CARRIER_DOWN;
        pthread_mutex_unlock(&lock);
        return;
    }
    pthread_detach(thread);
}

int display_carrier_starting(void) {
    pthread_mutex_lock(&lock);
    const int starting = state == CARRIER_STARTING;
    pthread_mutex_unlock(&lock);
    return starting;
}

void display_carrier_release(void) {
    pthread_mutex_lock(&lock);
    if (state == CARRIER_STARTING) {
        release_requested = 1;
        pthread_mutex_unlock(&lock);
        return;
    }
    if (state != CARRIER_UP) {
        pthread_mutex_unlock(&lock);
        return;
    }
    const int app_id = carrier_app_id;
    state = CARRIER_DOWN;
    carrier_app_id = -1;
    pthread_mutex_unlock(&lock);

    const int running = sceSystemServiceGetAppIdOfRunningBigApp();
    if (running != app_id) {
        carrier_log("display host 0x%x is no longer the running app (now 0x%x)",
                    app_id, running);
        return;
    }
    const int result = sceSystemServiceKillApp(app_id, -1, 0, 0);
    carrier_log("closed the display host 0x%x -> 0x%x", app_id, result);
}
