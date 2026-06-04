PROJECT_NAME := rinha-backend

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

CFLAGS_BASE := -Ofast -DNDEBUG -fomit-frame-pointer -flto -fno-plt -s -static -std=c11 -Wall -Wextra
ifeq ($(TARGETARCH),amd64)
CFLAGS_ARCH := -march=haswell -mtune=haswell
else
CFLAGS_ARCH :=
endif

BUILD_ARGS := \
	--build-arg CC=$(CC) \
	--build-arg TARGETARCH=$(TARGETARCH) \
	--build-arg CFLAGS_BASE="$(CFLAGS_BASE)" \
	--build-arg CFLAGS_ARCH="$(CFLAGS_ARCH)"

COMPOSE ?= docker compose
COMPOSE_FILE ?= docker-compose.yml

.PHONY: build up down logs smoke test print-flags

build:
	$(COMPOSE) -f $(COMPOSE_FILE) build $(BUILD_ARGS)

up:
	$(MAKE) build COMPOSE_FILE=$(COMPOSE_FILE) TARGETARCH=$(TARGETARCH) CC=$(CC)
	$(COMPOSE) -f $(COMPOSE_FILE) up -d

down:
	$(COMPOSE) -f $(COMPOSE_FILE) down -v

logs:
	$(COMPOSE) -f $(COMPOSE_FILE) logs -f --tail=200

smoke:
	docker run --rm -i \
	  --network $(PROJECT_NAME)_rinha \
	  -v "$$PWD:/work" -w /work \
	  -e BASE_URL=http://load-balancer:9999 \
	  grafana/k6 run test/smoke.js

test:
	docker run --rm -i \
	  --network $(PROJECT_NAME)_rinha \
	  -v "$$PWD:/work" -w /work \
	  -e BASE_URL=http://load-balancer:9999 \
	  grafana/k6 run test/test.js

print-flags:
	@echo "TARGETARCH=$(TARGETARCH)"
	@echo "CC=$(CC)"
	@echo "CFLAGS_BASE=$(CFLAGS_BASE)"
	@echo "CFLAGS_ARCH=$(CFLAGS_ARCH)"
