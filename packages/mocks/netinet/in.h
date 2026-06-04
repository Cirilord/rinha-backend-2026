#pragma once

#ifdef __APPLE__
#include <stdint.h>

#ifndef __SA_FAMILY_T_DEFINED
#define __SA_FAMILY_T_DEFINED
typedef uint8_t sa_family_t;
#endif

#ifndef __IN_PORT_T_DEFINED
#define __IN_PORT_T_DEFINED
typedef uint16_t in_port_t;
#endif

#ifndef __IN_ADDR_T_DEFINED
#define __IN_ADDR_T_DEFINED
typedef uint32_t in_addr_t;
#endif

struct in_addr {
  in_addr_t s_addr;
};

struct sockaddr_in {
  uint8_t sin_len;
  sa_family_t sin_family;
  in_port_t sin_port;
  struct in_addr sin_addr;
  unsigned char sin_zero[8];
};

#ifndef IPPROTO_TCP
#define IPPROTO_TCP 6
#endif

#ifndef AF_INET
#define AF_INET 2
#endif

#ifndef INADDR_ANY
#define INADDR_ANY ((in_addr_t)0x00000000U)
#endif
#endif
