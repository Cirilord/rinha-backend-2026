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

CFLAGS_BASE := -O3 -Wall -Wextra -std=c11
ifeq ($(TARGETARCH),amd64)
CFLAGS_ARCH := -mavx2 -mfma -march=haswell
else
CFLAGS_ARCH :=
endif

CFLAGS ?= $(CFLAGS_BASE) $(CFLAGS_ARCH)

SERVER_SRCS := apps/server/src/main.c \
               apps/server/src/server.c \
               apps/server/src/responses.c \
               apps/server/src/transaction_context.c \
               apps/server/src/x-score.c

LB_SRCS := apps/load-balancer/src/main.c

.PHONY: server load-balancer clean

server:
	$(CC) $(CFLAGS) -o server $(SERVER_SRCS)

load-balancer:
	$(CC) $(CFLAGS) -o load-balancer $(LB_SRCS)

clean:
	rm -f server load-balancer
