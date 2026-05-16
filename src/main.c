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
      char out[2048];
      TransactionContext ctx = transaction_context_new();

      if (find_value(body, "id", out, sizeof(out))) {
        ctx.id = strdup(out);
      } else {
        (void)write(client_fd, bad_request_response->data, bad_request_response->len);
        close(client_fd);
        continue;
      }

      if (find_value(body, "transaction.amount", out, sizeof(out))) {
        (void)to_double(out, &ctx.transaction.amount);
      } else {
        (void)write(client_fd, bad_request_response->data, bad_request_response->len);
        close(client_fd);
        continue;
      }
      if (find_value(body, "transaction.installments", out, sizeof(out))) {
        (void)to_int(out, &ctx.transaction.installments);
      } else {
        (void)write(client_fd, bad_request_response->data, bad_request_response->len);
        close(client_fd);
        continue;
      }
      if (find_value(body, "transaction.requested_at", out, sizeof(out))) {
        (void)to_epoch_time(out, &ctx.transaction.requested_at);
      } else {
        (void)write(client_fd, bad_request_response->data, bad_request_response->len);
        close(client_fd);
        continue;
      }

      if (find_value(body, "customer.avg_amount", out, sizeof(out))) {
        (void)to_double(out, &ctx.customer.avg_amount);
      } else {
        (void)write(client_fd, bad_request_response->data, bad_request_response->len);
        close(client_fd);
        continue;
      }
      if (find_value(body, "customer.tx_count_24h", out, sizeof(out))) {
        (void)to_int(out, &ctx.customer.tx_count_24h);
      } else {
        (void)write(client_fd, bad_request_response->data, bad_request_response->len);
        close(client_fd);
        continue;
      }
      if (find_value(body, "customer.known_merchants", out, sizeof(out))) {
        const char sep = '\x1F';
        size_t count = 0;
        char *p = out;
        while (*p) {
          if (*p == sep) count++;
          p++;
        }
        count = (*out == '\0') ? 0 : (count + 1);
        ctx.customer.known_merchants_count = count;
        if (count > 0) {
          ctx.customer.known_merchants = (char **)malloc(sizeof(char *) * count);
          if (!ctx.customer.known_merchants) {
            (void)write(client_fd, bad_request_response->data, bad_request_response->len);
            close(client_fd);
            continue;
          }

          size_t idx = 0;
          char *start = out;
          p = out;
          while (1) {
            if (*p == sep || *p == '\0') {
              char saved = *p;
              *p = '\0';
              ctx.customer.known_merchants[idx++] = strdup(start);
              if (saved == '\0') break;
              *p = saved;
              start = p + 1;
            }
            p++;
          }
        }
      } else {
        (void)write(client_fd, bad_request_response->data, bad_request_response->len);
        close(client_fd);
        continue;
      }

      if (find_value(body, "merchant.id", out, sizeof(out))) {
        ctx.merchant.id = strdup(out);
      } else {
        (void)write(client_fd, bad_request_response->data, bad_request_response->len);
        close(client_fd);
        continue;
      }
      if (find_value(body, "merchant.mcc", out, sizeof(out))) {
        ctx.merchant.mcc = strdup(out);
      } else {
        (void)write(client_fd, bad_request_response->data, bad_request_response->len);
        close(client_fd);
        continue;
      }
      if (find_value(body, "merchant.avg_amount", out, sizeof(out))) {
        (void)to_double(out, &ctx.merchant.avg_amount);
      } else {
        (void)write(client_fd, bad_request_response->data, bad_request_response->len);
        close(client_fd);
        continue;
      }

      if (find_value(body, "terminal.is_online", out, sizeof(out))) {
        (void)to_bool(out, &ctx.terminal.is_online);
      } else {
        (void)write(client_fd, bad_request_response->data, bad_request_response->len);
        close(client_fd);
        continue;
      }
      if (find_value(body, "terminal.card_present", out, sizeof(out))) {
        (void)to_bool(out, &ctx.terminal.card_present);
      } else {
        (void)write(client_fd, bad_request_response->data, bad_request_response->len);
        close(client_fd);
        continue;
      }
      if (find_value(body, "terminal.km_from_home", out, sizeof(out))) {
        (void)to_double(out, &ctx.terminal.km_from_home);
      } else {
        (void)write(client_fd, bad_request_response->data, bad_request_response->len);
        close(client_fd);
        continue;
      }

      if (find_value(body, "last_transaction.timestamp", out, sizeof(out))) {
        ctx.last_transaction = (LastTransaction *)malloc(sizeof(LastTransaction));
        if (ctx.last_transaction) {
          (void)to_epoch_time(out, &ctx.last_transaction->timestamp);
          if (find_value(body, "last_transaction.km_from_current", out, sizeof(out))) {
            (void)to_double(out, &ctx.last_transaction->km_from_current);
          }
        }
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
