#pragma once

int installer_http_init(void);
void installer_http_close(void);
int installer_http_run(void);
int installer_http_run_for_ms(int timeout_ms);
void installer_http_mark_installed(void);
