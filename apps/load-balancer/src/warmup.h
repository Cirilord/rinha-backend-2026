#ifndef LOAD_BALANCER_WARMUP_H
#define LOAD_BALANCER_WARMUP_H

#include <stdbool.h>

void warmup_mark_done(void);
bool warmup_handle_ready_gate(int client_fd);
void spawn_e2e_warmup(int port);

#endif
