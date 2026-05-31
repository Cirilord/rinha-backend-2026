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

CFLAGS_BASE := -Wall -Wextra -std=c11
ifeq ($(TARGETARCH),amd64)
CFLAGS_SERVER_ARCH := -mavx2 -mfma -march=haswell
CFLAGS_LB_ARCH :=
else
CFLAGS_SERVER_ARCH :=
CFLAGS_LB_ARCH :=
endif

CFLAGS_SERVER ?= $(CFLAGS_BASE) -O3 $(CFLAGS_SERVER_ARCH) -fno-plt
CFLAGS_XSCORE ?= $(CFLAGS_BASE) -O2 $(CFLAGS_SERVER_ARCH) -fno-plt
CFLAGS_LB ?= $(CFLAGS_BASE) -O3 $(CFLAGS_LB_ARCH) -fno-plt

SERVER_COMMON_SRCS := apps/server/src/main.c \
                      apps/server/src/server.c \
                      apps/server/src/responses.c \
                      apps/server/src/transaction_context.c
SERVER_XSCORE_SRC := apps/server/src/detector.c
SERVER_COMMON_OBJS := $(SERVER_COMMON_SRCS:.c=.o)
SERVER_XSCORE_OBJ := $(SERVER_XSCORE_SRC:.c=.o)
SERVER_OBJS := $(SERVER_COMMON_OBJS) $(SERVER_XSCORE_OBJ)

LB_SRCS := apps/load-balancer/src/main.c \
           apps/load-balancer/src/envs.c \
           apps/load-balancer/src/server.c
LB_OBJS := $(LB_SRCS:.c=.o)

.PHONY: server load-balancer clean

server: $(SERVER_OBJS)
	$(CC) -o server $(SERVER_OBJS)

$(SERVER_COMMON_OBJS): %.o: %.c
	$(CC) $(CFLAGS_SERVER) -c $< -o $@

$(SERVER_XSCORE_OBJ): $(SERVER_XSCORE_SRC)
	$(CC) $(CFLAGS_XSCORE) -c $< -o $@

load-balancer: $(LB_OBJS)
	$(CC) -o load-balancer $(LB_OBJS)

$(LB_OBJS): %.o: %.c
	$(CC) $(CFLAGS_LB) -c $< -o $@

clean:
	rm -f server load-balancer $(SERVER_OBJS) $(LB_OBJS)
