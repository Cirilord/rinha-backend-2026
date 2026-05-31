#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "envs.h"

bool parse_port(int *port_out) {
  if (port_out == NULL) {
    return false;
  }

  const char *port_env = getenv("PORT");
  if (port_env == NULL || *port_env == '\0') {
    return false;
  }

  errno = 0;
  char *end = NULL;
  long port = strtol(port_env, &end, 10);
  if (errno != 0 || *end != '\0' || port < 1 || port > 65535) {
    return false;
  }

  *port_out = (int)port;
  return true;
}

bool parse_worker_sockets(char worker_sockets[MAX_WORKERS][MAX_SOCKET_PATH],
                          size_t *worker_count_out) {
  if (worker_sockets == NULL || worker_count_out == NULL) {
    return false;
  }

  const char *env = getenv("WORKER_SOCKETS");
  if (env == NULL || *env == '\0') {
    return false;
  }

  char buffer[1024];
  strncpy(buffer, env, sizeof(buffer) - 1);
  buffer[sizeof(buffer) - 1] = '\0';

  size_t count = 0;
  char *save = NULL;
  char *token = strtok_r(buffer, ",", &save);
  while (token != NULL) {
    while (*token == ' ' || *token == '\t') {
      token++;
    }

    char *end = token + strlen(token);
    while (end > token &&
           (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r')) {
      *--end = '\0';
    }

    size_t len = strlen(token);
    if (len == 0 || len >= MAX_SOCKET_PATH) {
      return false;
    }

    if (count >= MAX_WORKERS) {
      return false;
    }

    memcpy(worker_sockets[count], token, len + 1);

    count++;
    token = strtok_r(NULL, ",", &save);
  }

  if (count == 0) {
    return false;
  }

  *worker_count_out = count;
  return true;
}
