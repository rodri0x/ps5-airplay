#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void ps5_notify(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif
