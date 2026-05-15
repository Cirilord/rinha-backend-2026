#include "env.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>

bool env_read_int(const char *name, bool required, int *out_value) {
  if (!name || !out_value) {
    return false;
  }

  const char *raw = getenv(name);
  if (!raw || *raw == '\0') {
    return !required;
  }

  errno = 0;
  char *end = NULL;
  long parsed = strtol(raw, &end, 10);
  if (errno != 0 || end == raw || *end != '\0') {
    return false;
  }
  if (parsed < INT_MIN || parsed > INT_MAX) {
    return false;
  }

  *out_value = (int)parsed;
  return true;
}
