#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Looks up the latest release in the background and shows a system
 * notification with the running version and whether a newer one exists. */
void update_check_start(void);

/* Compares dotted versions ("0.10" is newer than "0.9"); a leading "v" is
 * ignored.  Returns <0, 0 or >0. */
int update_check_compare(const char *a, const char *b);

#ifdef __cplusplus
}
#endif
