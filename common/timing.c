#include "timing.h"

#include <errno.h>
#include <time.h>

int sleep_milliseconds(unsigned int milliseconds)
{
    struct timespec remaining;

    remaining.tv_sec = (time_t)(milliseconds / 1000U);
    remaining.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;

    while (nanosleep(&remaining, &remaining) == -1) {
        if (errno != EINTR) {
            return -1;
        }
    }
    return 0;
}
