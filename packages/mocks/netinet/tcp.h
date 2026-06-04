#pragma once

#ifdef __APPLE__
#ifndef TCP_NODELAY
#define TCP_NODELAY 0x01
#endif

#ifndef TCP_QUICKACK
#define TCP_QUICKACK 0x0c
#endif

#ifndef TCP_DEFER_ACCEPT
#define TCP_DEFER_ACCEPT 0x09
#endif
#endif
