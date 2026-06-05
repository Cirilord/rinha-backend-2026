CC ?= gcc

TARGETARCH ?=
ifeq ($(TARGETARCH),)
UNAME_M := $(shell uname -m)
ifneq (,$(filter x86_64 amd64,$(UNAME_M)))
TARGETARCH := amd64
else ifneq (,$(filter aarch64 arm64,$(UNAME_M)))
TARGETARCH := arm64
else
TARGETARCH := arm64
endif
endif

CFLAGS_BASE := -Ofast -DNDEBUG -fomit-frame-pointer -flto -fno-plt -s -static -Wall -Wextra -std=c11 -pthread
ifeq ($(TARGETARCH),amd64)
CFLAGS_ARCH := -march=haswell -mtune=haswell
else
CFLAGS_ARCH :=
endif

CFLAGS ?= $(CFLAGS_BASE) $(CFLAGS_ARCH)

SERVER_SRCS := apps/server/src/main.c \
               apps/server/src/listener.c \
               apps/server/src/transaction_context.c \
               apps/server/src/utils.c \
               apps/server/src/x_score.c

LB_SRCS := apps/load-balancer/src/main.c \
           apps/load-balancer/src/env.c \
           apps/load-balancer/src/listener.c \
           apps/load-balancer/src/upstream.c \
           apps/load-balancer/src/utils.c

.PHONY: server load-balancer clean

server:
	$(CC) $(CFLAGS) -o server $(SERVER_SRCS)

load-balancer:
	$(CC) $(CFLAGS) -o load-balancer $(LB_SRCS)

clean:
	rm -f server load-balancer
