#include "app_install.h"

#include "embedded_assets.h"
#include "installer_config.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>

#include <ps5/kernel.h>

#include "notify.h"

int sceAppInstUtilInitialize(void);
int sceAppInstUtilTerminate(void);
int sceAppInstUtilAppInstallAll(void *reserved);
int sceAppInstUtilAppUnInstall(const char *title_id);

#define AIRPLAY_REJECTED_TITLE_ID "P5AP00001"

static char install_status[256] = "result=pending";

#define RECEIVER_PROCESS_NAME "ps5airplay.elf"

static int mkdir_p(const char *path, mode_t mode) {
    char buffer[256];
    size_t length = strlen(path);
    if (length == 0 || length >= sizeof(buffer)) {
        return -1;
    }
    memcpy(buffer, path, length + 1);
    if (buffer[length - 1] == '/') {
        buffer[length - 1] = '\0';
    }
    for (char *cursor = buffer + 1; *cursor; cursor++) {
        if (*cursor != '/') {
            continue;
        }
        *cursor = '\0';
        if (mkdir(buffer, mode) != 0 && errno != EEXIST) {
            *cursor = '/';
            return -1;
        }
        *cursor = '/';
    }
    if (mkdir(buffer, mode) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

static int write_asset(const char *path, const uint8_t *data, size_t size) {
    FILE *file = fopen(path, "wb");
    if (!file) {
        fprintf(stderr, "[installer] fopen(%s): %s\n", path, strerror(errno));
        return -1;
    }
    size_t written = fwrite(data, 1, size, file);
    int close_error = fclose(file);
    if (written != size || close_error != 0) {
        fprintf(stderr, "[installer] write(%s) failed\n", path);
        return -1;
    }
    return 0;
}

static void publish_status(const char *status) {
    (void)snprintf(install_status, sizeof(install_status), "%s", status);
    if (strncmp(status, "result=failed", 13) == 0) {
        ps5_notify("PS5 AirPlay: install failed");
    }
    FILE *file = fopen(AIRPLAY_INSTALL_STATUS_PATH, "wb");
    if (!file) {
        return;
    }
    (void)fwrite(status, 1, strlen(status), file);
    (void)fwrite("\n", 1, 1, file);
    (void)fclose(file);
}

static int needs_update(const char *path, const uint8_t *expected_data,
                        size_t expected_size) {
    struct stat status;
    if (stat(path, &status) != 0 || (size_t)status.st_size != expected_size) {
        return 1;
    }

    FILE *file = fopen(path, "rb");
    if (!file) {
        return 1;
    }
    uint8_t *contents = malloc(expected_size);
    if (!contents) {
        fclose(file);
        return 1;
    }
    size_t read_size = fread(contents, 1, expected_size, file);
    fclose(file);
    int different = read_size != expected_size ||
                    memcmp(contents, expected_data, expected_size) != 0;
    free(contents);
    return different;
}

static int install_title_directory(void) {
    typedef int (*install_title_dir_fn)(const char *, const char *, void *);
    install_title_dir_fn install_title_dir = NULL;
    uint32_t handle = 0;
    if (kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx", &handle) == 0) {
        install_title_dir = (install_title_dir_fn)kernel_dynlib_resolve(
            -1, handle, "Wudg3Xe3heE");
    }
    if (install_title_dir) {
        return install_title_dir(AIRPLAY_TITLE_ID, "/user/app/", NULL);
    }
    return sceAppInstUtilAppInstallAll(NULL);
}

static const char *install_method(void) {
    uint32_t handle = 0;
    if (kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx", &handle) == 0 &&
        kernel_dynlib_resolve(-1, handle, "Wudg3Xe3heE") != 0) {
        return "title-dir";
    }
    return "install-all";
}

static int stop_previous_receivers(void) {
    int mib[4] = {1, 14, 8, 0};
    size_t size = 0;
    if (sysctl(mib, 4, NULL, &size, NULL, 0) != 0 || size == 0) {
        return 0;
    }
    uint8_t *buffer = malloc(size);
    if (!buffer) {
        return 0;
    }
    if (sysctl(mib, 4, buffer, &size, NULL, 0) != 0) {
        free(buffer);
        return 0;
    }
    pid_t own_pid = getpid();
    pid_t targets[32];
    int count = 0;
    for (uint8_t *cursor = buffer; cursor < buffer + size;) {
        int record_size = *(int *)cursor;
        if (record_size <= 0 || cursor + record_size > buffer + size) {
            break;
        }
        pid_t pid = *(pid_t *)(cursor + 72);
        const char *name = (const char *)(cursor + 447);
        if (pid != own_pid && count < 32 && strcmp(name, RECEIVER_PROCESS_NAME) == 0) {
            targets[count++] = pid;
        }
        cursor += record_size;
    }
    free(buffer);

    for (int i = 0; i < count; i++) {
        (void)kill(targets[i], SIGTERM);
    }
    for (int wait = 0; wait < 30; wait++) {
        int alive = 0;
        for (int i = 0; i < count; i++) {
            alive += kill(targets[i], 0) == 0;
        }
        if (alive == 0) {
            break;
        }
        usleep(100000);
    }
    for (int i = 0; i < count; i++) {
        if (kill(targets[i], 0) == 0) {
            (void)kill(targets[i], SIGKILL);
        }
    }
    if (count > 0) {
        ps5_notify("PS5 AirPlay: stopped the running receiver");
    }
    return count;
}

static int remove_existing_shortcut(void) {
    char app_dir[256];
    char sce_sys_dir[256];
    char param_path[256];
    char icon_path[256];
    snprintf(app_dir, sizeof(app_dir), "/user/app/%s", AIRPLAY_TITLE_ID);
    snprintf(sce_sys_dir, sizeof(sce_sys_dir), "%s/sce_sys", app_dir);
    snprintf(param_path, sizeof(param_path), "%s/param.json", sce_sys_dir);
    snprintf(icon_path, sizeof(icon_path), "%s/icon0.png", sce_sys_dir);

    int error = sceAppInstUtilAppUnInstall(AIRPLAY_TITLE_ID);
    (void)unlink(param_path);
    (void)unlink(icon_path);
    (void)rmdir(sce_sys_dir);
    (void)rmdir(app_dir);
    printf("[installer] removed existing shortcut %s (uninstall 0x%x)\n",
           AIRPLAY_TITLE_ID, error);
    return error;
}

static void remove_rejected_title(void) {
    char app_dir[64];
    char sce_sys_dir[80];
    char param_path[96];
    char icon_path[96];
    snprintf(app_dir, sizeof(app_dir), "/user/app/%s", AIRPLAY_REJECTED_TITLE_ID);
    snprintf(sce_sys_dir, sizeof(sce_sys_dir), "%s/sce_sys", app_dir);
    snprintf(param_path, sizeof(param_path), "%s/param.json", sce_sys_dir);
    snprintf(icon_path, sizeof(icon_path), "%s/icon0.png", sce_sys_dir);

    struct stat status;
    if (stat(app_dir, &status) != 0) {
        return;
    }
    (void)unlink(param_path);
    (void)unlink(icon_path);
    (void)rmdir(sce_sys_dir);
    (void)rmdir(app_dir);
    printf("[installer] removed rejected title directory %s\n", app_dir);
}

static int write_library_copy(void) {
    if (mkdir_p(AIRPLAY_LIBRARY_DIR, 0755) != 0) {
        return -1;
    }
    return write_asset(AIRPLAY_LIBRARY_ELF_PATH, airplay_elf_data, airplay_elf_size);
}

/* AppInstallAll returns 0 for a title it then skips; the homescreen database
 * is what shows whether the title was registered. */
static int title_in_homescreen_db(const char *title_id) {
    static uint8_t chunk[65536];
    size_t id_size = strlen(title_id);
    size_t carried = 0;
    int found = 0;

    FILE *file = fopen(AIRPLAY_HOMESCREEN_DB, "rb");
    if (!file) {
        return -1;
    }
    for (;;) {
        size_t read_size = fread(chunk + carried, 1, sizeof(chunk) - carried, file);
        size_t total = carried + read_size;
        for (size_t offset = 0; offset + id_size <= total; offset++) {
            if (memcmp(chunk + offset, title_id, id_size) == 0) {
                found = 1;
                break;
            }
        }
        if (found || read_size == 0) {
            break;
        }
        carried = total < id_size - 1 ? total : id_size - 1;
        memmove(chunk, chunk + total - carried, carried);
    }
    fclose(file);
    return found;
}

static int wait_for_homescreen_entry(const char *title_id) {
    int found = 0;
    for (int attempt = 0; attempt < 10; attempt++) {
        found = title_in_homescreen_db(title_id);
        if (found == 1) {
            return 1;
        }
        usleep(500000);
    }
    return found;
}

const char *app_install_status(void) {
    return install_status;
}

int app_install_payload_and_shortcut(void) {
    char app_dir[256];
    char sce_sys_dir[256];
    char param_path[256];
    char icon_path[256];
    char status[256];
    snprintf(app_dir, sizeof(app_dir), "/user/app/%s", AIRPLAY_TITLE_ID);
    snprintf(sce_sys_dir, sizeof(sce_sys_dir), "%s/sce_sys", app_dir);
    snprintf(param_path, sizeof(param_path), "%s/param.json", sce_sys_dir);
    snprintf(icon_path, sizeof(icon_path), "%s/icon0.png", sce_sys_dir);

    int receivers_stopped = stop_previous_receivers();
    if (mkdir_p(AIRPLAY_DATA_DIR, 0755) != 0 ||
        write_asset(AIRPLAY_ELF_PATH, airplay_elf_data, airplay_elf_size) != 0) {
        fprintf(stderr, "[installer] mkdir failed: %s\n", strerror(errno));
        publish_status("result=failed|stage=payload-write");
        return -1;
    }
    const char *library = write_library_copy() == 0 ? "ok" : "failed";
    remove_rejected_title();

    struct stat title_status;
    int update_needed = stat(app_dir, &title_status) != 0 ||
                        needs_update(param_path, param_json_data, param_json_size) ||
                        needs_update(icon_path, icon0_png_data, icon0_png_size);
    if (!update_needed) {
        printf("[installer] %s assets are current; re-registering title\n",
               AIRPLAY_APP_NAME);
    }

    int error = sceAppInstUtilInitialize();
    if (error != 0) {
        fprintf(stderr, "[installer] sceAppInstUtilInitialize: 0x%x\n", error);
        (void)snprintf(status, sizeof(status),
                       "result=failed|stage=appinst-init|error=0x%x", error);
        publish_status(status);
        return -1;
    }

    int uninstall_error = remove_existing_shortcut();
    if (mkdir_p(sce_sys_dir, 0755) != 0 ||
        write_asset(param_path, param_json_data, param_json_size) != 0 ||
        write_asset(icon_path, icon0_png_data, icon0_png_size) != 0) {
        publish_status("result=failed|stage=metadata-write");
        (void)sceAppInstUtilTerminate();
        return -1;
    }

    error = install_title_directory();
    int terminate_error = sceAppInstUtilTerminate();
    const char *method = install_method();
    if (error != 0 || terminate_error != 0) {
        (void)snprintf(status, sizeof(status),
                       "result=failed|stage=appinst-call|method=%s|install=0x%x|terminate=0x%x",
                       method, error, terminate_error);
        publish_status(status);
        fprintf(stderr, "[installer] app install: 0x%x, terminate: 0x%x\n",
                error, terminate_error);
        return -1;
    }

    int registered = wait_for_homescreen_entry(AIRPLAY_TITLE_ID);
    if (registered != 1) {
        (void)snprintf(status, sizeof(status),
                       "result=failed|stage=homescreen-db|title=%s|found=%s",
                       AIRPLAY_TITLE_ID, registered == 0 ? "no" : "unreadable");
        publish_status(status);
        fprintf(stderr, "[installer] %s is not in the homescreen database\n",
                AIRPLAY_TITLE_ID);
        return -1;
    }

    (void)snprintf(status, sizeof(status),
                   "result=registered|title=%s|method=%s|found=yes|library=%s|"
                   "receivers_stopped=%d|uninstall=0x%x",
                   AIRPLAY_TITLE_ID, method, library, receivers_stopped, uninstall_error);
    publish_status(status);
    ps5_notify("PS5 AirPlay shortcut installed");
    return 0;
}
