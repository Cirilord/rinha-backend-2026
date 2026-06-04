#include "env.h"

#include <stdlib.h>
#include <string.h>

#define UPSTREAM_SOCKETS_ENV "UPSTREAM_SOCKETS"

int get_upstream_paths(const char *paths[MAX_UPSTREAM_COUNT]) {
  static char sockets_buf[256];
  static const char *parsed_paths[MAX_UPSTREAM_COUNT];

  const char *raw_paths = getenv(UPSTREAM_SOCKETS_ENV);
  if (raw_paths == NULL || *raw_paths == '\0') {
    return 0;
  }

  memset(sockets_buf, 0, sizeof(sockets_buf));
  strncpy(sockets_buf, raw_paths, sizeof(sockets_buf) - 1);

  memset(parsed_paths, 0, sizeof(parsed_paths));

  int count = 0;
  char *cursor = sockets_buf;
  while (*cursor != '\0' && count < MAX_UPSTREAM_COUNT) {
    parsed_paths[count++] = cursor;

    char *comma = strchr(cursor, ',');
    if (comma == NULL) {
      break;
    }

    *comma = '\0';
    cursor = comma + 1;
  }

  for (int i = 0; i < count; i++) {
    paths[i] = parsed_paths[i];
  }

  return count;
}
