/* TweetNaCl needs randombytes() to link; the host tests never call it. */
#include <stdlib.h>

void randombytes(unsigned char *buffer, unsigned long long length) {
    (void)buffer;
    (void)length;
    abort();
}
