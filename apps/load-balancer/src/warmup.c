#define _POSIX_C_SOURCE 200809L

#include "warmup.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#if !defined(__linux__)
#include "../../../packages/mocks/sys/socket.h"
#endif
#include <sys/types.h>
#include <unistd.h>

#include "utils.h"

#define E2E_WARMUP_ATTEMPTS 50
#define E2E_WARMUP_POST_REQUESTS 8192

static const char LB_READY_REQUEST[] = "GET /ready ";
static volatile sig_atomic_t e2e_warmup_done = 0;

void warmup_mark_done(void) { e2e_warmup_done = 1; }

bool warmup_handle_ready_gate(int client_fd) {
  if (e2e_warmup_done) {
    return false;
  }

  char peek[sizeof(LB_READY_REQUEST) - 1];
  ssize_t npeek = recv(client_fd, peek, sizeof(peek), MSG_PEEK | MSG_DONTWAIT);
  if (npeek < (ssize_t)sizeof(LB_READY_REQUEST) - 1 ||
      memcmp(peek, LB_READY_REQUEST, sizeof(LB_READY_REQUEST) - 1) != 0) {
    return false;
  }

  const char *resp = "HTTP/1.1 503 Service Unavailable\r\n"
                     "Content-Length: 10\r\n"
                     "Connection: close\r\n\r\n"
                     "warming up";
  (void)send(client_fd, resp, strlen(resp), MSG_NOSIGNAL | MSG_DONTWAIT);
  return true;
}

static bool run_e2e_warmup_request(int port, const char *request, size_t request_len) {
  int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    return false;
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  addr.sin_addr.s_addr = htonl(0x7f000001u);

  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(fd);
    return false;
  }

  size_t sent = 0;
  while (sent < request_len) {
    ssize_t nwritten = send(fd, request + sent, request_len - sent, MSG_NOSIGNAL);
    if (nwritten > 0) {
      sent += (size_t)nwritten;
      continue;
    }
    if (nwritten < 0 && errno == EINTR) {
      continue;
    }
    close(fd);
    return false;
  }

  (void)shutdown(fd, SHUT_WR);

  char response[256];
  size_t used = 0;
  while (used + 1 < sizeof(response)) {
    ssize_t nread = read(fd, response + used, sizeof(response) - used - 1);
    if (nread > 0) {
      used += (size_t)nread;
      if (used >= 12) {
        break;
      }
      continue;
    }
    if (nread == 0) {
      break;
    }
    if (errno == EINTR) {
      continue;
    }
    close(fd);
    return false;
  }

  response[used] = '\0';
  close(fd);
  return used >= 12 && strncmp(response, "HTTP/1.1 200", 12) == 0;
}

void spawn_e2e_warmup(int port) {
  static const char fraud_body[] =
    "{\"id\":\"tx-e2e-warmup\","
    "\"transaction\":{\"amount\":384.88,\"installments\":3,\"requested_at\":"
    "\"2026-03-11T20:23:35Z\"},"
    "\"customer\":{\"avg_amount\":769.76,\"tx_count_24h\":3,\"known_"
    "merchants\":[\"MERC-009\",\"MERC-001\",\"MERC-001\"]},"
    "\"merchant\":{\"id\":\"MERC-001\",\"mcc\":\"5912\",\"avg_amount\":298.95},"
    "\"terminal\":{\"is_online\":false,\"card_present\":true,\"km_from_"
    "home\":13.7090520965},"
    "\"last_transaction\":{\"timestamp\":\"2026-03-11T14:58:35Z\",\"km_from_"
    "current\":18.8626479774}}";

  pid_t pid = fork();
  if (pid != 0) {
    return;
  }

  pid_t parent_pid = getppid();
  char fraud_request[2048];
  int fraud_request_len = snprintf(fraud_request, sizeof(fraud_request),
                                   "POST /fraud-score HTTP/1.1\r\n"
                                   "Host: 127.0.0.1\r\n"
                                   "Content-Type: application/json\r\n"
                                   "Content-Length: %zu\r\n"
                                   "Connection: close\r\n\r\n%s",
                                   sizeof(fraud_body) - 1, fraud_body);
  if (fraud_request_len <= 0 || (size_t)fraud_request_len >= sizeof(fraud_request)) {
    _exit(1);
  }

  sleep_ms(100);
  for (int attempt = 0; attempt < E2E_WARMUP_ATTEMPTS; attempt++) {
    bool warmed = true;
    for (int i = 0; i < E2E_WARMUP_POST_REQUESTS; i++) {
      if (!run_e2e_warmup_request(port, fraud_request, (size_t)fraud_request_len)) {
        warmed = false;
        break;
      }
    }

    if (warmed) {
      fprintf(stderr, "INFO: e2e warmup complete\n");
      if (parent_pid > 0) {
        kill(parent_pid, SIGUSR1);
      }
      _exit(0);
    }

    sleep_ms(100);
  }

  fprintf(stderr, "WARN: e2e warmup did not complete successfully\n");
  if (parent_pid > 0) {
    kill(parent_pid, SIGUSR1);
  }
  _exit(1);
}
