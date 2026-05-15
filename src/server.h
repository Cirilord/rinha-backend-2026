#ifndef SERVER_H
#define SERVER_H

#include <stdbool.h>

bool create_server(int port, int *out_server_fd);

#endif
