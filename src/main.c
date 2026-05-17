#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>

#include "env.h"
#include "responses.h"
#include "server.h"
#include "transaction_context.h"
#include "utils.h"
#include "x-score.h"

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);

  int workers = 0;
  if (!env_read_int("WORKERS", true, &workers) || workers <= 0) {
    fprintf(stderr, "WORKERS env var must be a positive integer\n");
    return 1;
  }

  int port = 0;
  if (!env_read_int("PORT", true, &port) || port <= 0 || port > 65535) {
    fprintf(stderr, "PORT env var must be an integer between 1 and 65535\n");
    return 1;
  }

  int server_fd = -1;
  if (!create_server(port, &server_fd)) {
    fprintf(stderr, "failed to create server on port %d\n", port);
    return 1;
  }
  printf("server listening on 0.0.0.0:%d\n", port);

  XScoreIndexView xscore;
  if (!x_score_open("resources/references.idx", &xscore)) {
    fprintf(stderr, "failed to load x-score index from resources/references.idx\n");
    close(server_fd);
    return 1;
  }
  printf(
      "x-score index loaded: count=%u dims=%d partitions=%u\n",
      xscore.count,
      xscore.header ? xscore.header->dims : -1,
      xscore.partition_count);

  for (int i = 1; i < workers; i++) {
    pid_t pid = fork();
    if (pid < 0) {
      perror("fork");
      close(server_fd);
      x_score_close(&xscore);
      return 1;
    }
    if (pid == 0) {
      break;
    }
  }
  printf("worker pid=%d ready on port=%d\n", getpid(), port);

  const Response *response = &RESPONSE_OK;
  const Response *ready_response = &RESPONSE_READY;
  const Response *not_found_response = &RESPONSE_NOT_FOUND;
  const Response *bad_request_response = &RESPONSE_BAD_REQUEST;

  while (1) {
    int client_fd = accept(server_fd, NULL, NULL);
    if (client_fd < 0) {
      continue;
    }

    char buffer[4096];
    ssize_t nread = read(client_fd, buffer, sizeof(buffer) - 1);
    if (nread <= 0) {
      close(client_fd);
      continue;
    }
    buffer[nread] = '\0';

    char method[8] = {0};
    char url[2048] = {0};
    char pathname[2048] = {0};

    if (sscanf(buffer, "%7s %2047s", method, url) != 2) {
      (void)write(client_fd, not_found_response->data, not_found_response->len);
      close(client_fd);
      continue;
    }

    if (!extract_pathname(url, pathname, sizeof(pathname))) {
      (void)write(client_fd, not_found_response->data, not_found_response->len);
      close(client_fd);
      continue;
    }

    if (strcmp(method, "GET") == 0 && strcmp(pathname, "/ready") == 0) {
      (void)write(client_fd, ready_response->data, ready_response->len);
    } else if (strcmp(method, "POST") == 0 && strcmp(pathname, "/fraud-score") == 0) {
      const char *body = strstr(buffer, "\r\n\r\n");
      if (!body || *(body + 4) == '\0') {
        (void)write(client_fd, bad_request_response->data, bad_request_response->len);
        close(client_fd);
        continue;
      }

      body += 4;
      TransactionContext ctx = transaction_context_new();
      if (!ctx.from_body(&ctx, body)) {
        (void)write(client_fd, bad_request_response->data, bad_request_response->len);
        ctx.destroy(&ctx);
        close(client_fd);
        continue;
      }

      double vector[14];
      ctx.to_vector(&ctx, vector);
      ctx.destroy(&ctx);

      uint8_t fraud_count = x_score_predict_fraud_count(&xscore, vector);
      if (fraud_count >= FRAUD_RESPONSES_LEN) {
        fraud_count = FRAUD_RESPONSES_LEN - 1;
      }
      const Response *fraud_response = &FRAUD_RESPONSES[fraud_count];
      (void)write(client_fd, fraud_response->data, fraud_response->len);
    } else if (strcmp(method, "GET") == 0 && strcmp(pathname, "/") == 0) {
      (void)write(client_fd, response->data, response->len);
    } else {
      (void)write(client_fd, not_found_response->data, not_found_response->len);
    }

    close(client_fd);
  }

  close(server_fd);
  x_score_close(&xscore);
  return 0;
}
