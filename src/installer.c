#include "app_install.h"
#include "installer_http.h"
#include "notify.h"

#include "airplay_config.h"
#include "installer_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>

int sceSystemServiceLaunchWebBrowser(const char *uri);
int sceNetCtlInit(void);
int sceUserServiceInitialize(void *param);
int sceUserServiceTerminate(void);

static void set_process_name(void) {
    (void)syscall(0x1d0, -1, "ps5airplay-installer.elf");
}

int main(void) {
    set_process_name();
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("[installer] PS5 AirPlay installer starting\n");

    (void)sceNetCtlInit();

    if (installer_http_init() != 0) {
        return EXIT_FAILURE;
    }

    int user_priority = 256;
    (void)sceUserServiceInitialize(&user_priority);
    char url[160];
    (void)snprintf(url, sizeof(url), "http://127.0.0.1:%d/?v=%s",
                   AIRPLAY_INSTALLER_PORT, AIRPLAY_INSTALLER_VERSION);
    /* The shortcut loads its page from WebKit's AppCache, which only a WebKit
     * window can fill. */
    int launch_error = sceSystemServiceLaunchWebBrowser(url);
    if (launch_error != 0) {
        fprintf(stderr, "[installer] sceSystemServiceLaunchWebBrowser: 0x%x\n",
                launch_error);
        fprintf(stderr, "[installer] continuing; the URL can be opened manually\n");
    }
    ps5_notify("PS5 AirPlay: installing the shortcut");
    int install_result = app_install_payload_and_shortcut();
    if (install_result == 0) {
        installer_http_mark_installed();
        printf("[installer] registration verified: %s\n", app_install_status());
    } else {
        fprintf(stderr, "[installer] homescreen registration failed: %s\n",
                app_install_status());
    }

    int cache_result = installer_http_run_for_ms(120000);
    (void)sceUserServiceTerminate();
    installer_http_close();
    int result = cache_result;
    if (result == 0) {
        ps5_notify("PS5 AirPlay " AIRPLAY_VERSION " is ready. Open it from the home screen.");
    } else {
        ps5_notify("PS5 AirPlay: the offline page did not finish caching");
    }
    printf("[installer] finished with status %d\n", result);
    return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
