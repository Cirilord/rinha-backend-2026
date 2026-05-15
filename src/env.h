#ifndef ENV_H
#define ENV_H

#include <stdbool.h>

bool env_read_int(const char *name, bool required, int *out_value);

#endif
