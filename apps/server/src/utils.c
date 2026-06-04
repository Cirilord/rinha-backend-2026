#include <stdio.h>
#include <stdlib.h>

#include "utils.h"

void fatal(const char *message) {
  perror(message);
  exit(1);
}
