#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void receiver_log_start(void);
void receiver_install_crash_logger(void);
void receiver_log(const char *area, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
void receiver_log_flush(void);

#ifdef __cplusplus
}
#endif
