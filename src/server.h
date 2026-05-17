#ifndef SERVER_H
#define SERVER_H

#include <stdbool.h>

bool create_unix_server(const char *socket_path, int *out_server_fd);

#endif
