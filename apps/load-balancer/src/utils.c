#define _POSIX_C_SOURCE 200809L

#include "utils.h"

#include <errno.h>
#include <time.h>

void sleep_ms(long ms) {
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (ms % 1000) * 1000000L;

  while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
    ;
}
