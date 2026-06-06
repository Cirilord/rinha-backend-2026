# rinha-backend-2026

`C` implementation of a backend with:

- a TCP `load-balancer`
- an HTTP `server`
- connection handoff via `SCM_RIGHTS`
- vector search over a custom index
- a strong focus on low latency and high Rinha score

## Overview

The project is split into two binaries:

- `apps/load-balancer`
  accepts HTTP connections on `:9999` and forwards sockets to the API instances
- `apps/server`
  receives sockets through a Unix Domain Socket, handles `GET /ready` and `POST /fraud-score`, runs the index search, and returns a prebuilt response

The system runs with:

- `1` load balancer
- `2` API instances
- a shared `tmpfs` for Unix sockets

## Architecture

### 1. Load Balancer

Main files:

- [apps/load-balancer/src/main.c](/Users/cirilo/Desktop/test-load-balancer/apps/load-balancer/src/main.c)
- [apps/load-balancer/src/upstream.c](/Users/cirilo/Desktop/test-load-balancer/apps/load-balancer/src/upstream.c)
- [apps/load-balancer/src/listener.c](/Users/cirilo/Desktop/test-load-balancer/apps/load-balancer/src/listener.c)
- [apps/load-balancer/src/env.c](/Users/cirilo/Desktop/test-load-balancer/apps/load-balancer/src/env.c)

The load balancer:

- opens a TCP listener on `:9999`
- accepts connections in batches with `accept4`
- applies `TCP_NODELAY` and `TCP_QUICKACK`
- uses round robin across upstreams
- sends the `client_fd` to the API with `sendmsg(..., SCM_RIGHTS)`

Important details:

- it uses `AF_UNIX + SOCK_SEQPACKET` for the internal channel
- it uses `MSG_DONTWAIT` when sending the `fd`, with a blocking fallback
- it uses `poll()` to sleep while the listener is idle
- it reads internal socket paths from the `UPSTREAM_SOCKETS` environment variable

### 2. Server

Main files:

- [apps/server/src/main.c](/Users/cirilo/Desktop/test-load-balancer/apps/server/src/main.c)
- [apps/server/src/listener.c](/Users/cirilo/Desktop/test-load-balancer/apps/server/src/listener.c)
- [apps/server/src/transaction_context.c](/Users/cirilo/Desktop/test-load-balancer/apps/server/src/transaction_context.c)
- [apps/server/src/x_score.c](/Users/cirilo/Desktop/test-load-balancer/apps/server/src/x_score.c)

The server:

- creates one Unix listener per instance
- accepts a single connection from the load balancer
- receives `client_fd`s through `recvmsg(..., SCM_RIGHTS)`
- distributes those `fd`s across `2` workers
- lets each worker process its own clients with a dedicated `epoll` instance

Current concurrency model:

- `2` worker threads per API
- one queue per worker
- round robin in the main dispatcher
- each worker manages its own sockets through `epoll`

### 3. HTTP Parsing

The HTTP parser is intentionally simple and narrow.

Supported routes:

- `GET /ready`
- `POST /fraud-score`

Any other valid route returns:

- `404 Not Found`

Parsing includes:

- header end detection through `\r\n\r\n`
- `Content-Length` parsing
- max-size validation
- correct body consumption before sending the response

### 4. Transaction Context

Files:

- [apps/server/src/transaction_context.h](/Users/cirilo/Desktop/test-load-balancer/apps/server/src/transaction_context.h)
- [apps/server/src/transaction_context.c](/Users/cirilo/Desktop/test-load-balancer/apps/server/src/transaction_context.c)

Responsibilities:

- parse the JSON body of `POST /fraud-score`
- map the fields into a `transaction_context`
- convert that context into the numeric vector used by the scorer

Request flow:

1. parse the body
2. call `transaction_context__to_vector(...)`
3. call the scorer
4. destroy the context

## Index and Search

Files:

- [apps/server/src/x_score.h](/Users/cirilo/Desktop/test-load-balancer/apps/server/src/x_score.h)
- [apps/server/src/x_score.c](/Users/cirilo/Desktop/test-load-balancer/apps/server/src/x_score.c)
- [scripts/build_references_idx.py](/Users/cirilo/Desktop/test-load-balancer/scripts/build_references_idx.py)
- [resources/references.json.gz](/Users/cirilo/Desktop/test-load-balancer/resources/references.json.gz)

The index is built at image build time from `references.json.gz`.

Generated artifacts:

- `/resources/refs.bin`
- `/resources/kdtree.bin`

Scorer characteristics:

- `k-NN` search with `k = 5`
- primary-key partitioning
- a custom KD-tree
- SIMD-friendly leaf layout
- prebuilt responses for `fraud_score`

Possible outputs:

- `0.0`
- `0.2`
- `0.4`
- `0.6`
- `0.8`
- `1.0`

Decision rule:

- count how many of the `top 5` neighbors are fraud
- choose the matching response

### SIMD

The scorer uses architecture-specific vectorized paths:

- `AVX2` on `amd64`
- a compatible fallback path in the local environment

## Current Tuning

Tuning currently enabled:

- `SO_SNDBUF` on the LB Unix socket
- `SO_RCVBUF` and `SO_SNDBUF` on the API `control_fd`
- larger backlog on the internal Unix listener
- `EPOLL_BUSY_POLL_*`
- `EPOLL_IDLE_US`
- `SOCK_CLOEXEC`
- `MSG_NOSIGNAL`
- `TCP_NODELAY`
- `TCP_QUICKACK`
- batched `accept` on the load balancer

Current compose sizing:

- `load-balancer`: `0.20 CPU`, `20MB`
- `api1`: `0.40 CPU`, `165MB`
- `api2`: `0.40 CPU`, `165MB`

## Build

Files:

- [Makefile](/Users/cirilo/Desktop/test-load-balancer/Makefile)
- [apps/load-balancer/Dockerfile](/Users/cirilo/Desktop/test-load-balancer/apps/load-balancer/Dockerfile)
- [apps/server/Dockerfile](/Users/cirilo/Desktop/test-load-balancer/apps/server/Dockerfile)

`Makefile` targets:

- `make load-balancer`
- `make server`
- `make clean`

The `Makefile`:

- detects `TARGETARCH`
- uses `-Ofast`
- uses `-pthread`
- adds `-march=haswell -mtune=haswell` on `amd64`

The `Dockerfile`s:

- invoke the `Makefile`
- generate static binaries
- build the final image on `scratch`

## Docker Compose

Files:

- [docker-compose.yml](/Users/cirilo/Desktop/test-load-balancer/docker-compose.yml)
- [docker-compose.submission.yml](/Users/cirilo/Desktop/test-load-balancer/docker-compose.submission.yml)

The main compose file:

- starts the local environment
- publishes `9999`
- uses `tmpfs` for `/tmp`
- disables container logs

The submission compose file:

- isolates the `linux/amd64` submission configuration
- keeps the same service layout

## How To Run

### Start the environment

```bash
docker compose up -d --build
```

### Stop the environment

```bash
docker compose down
```

### Health check

```bash
curl http://127.0.0.1:9999/ready
```

## Tests

Files:

- [test/test.js](/Users/cirilo/Desktop/test-load-balancer/test/test.js)
- [test/smoke.js](/Users/cirilo/Desktop/test-load-balancer/test/smoke.js)

If you have `k6` installed:

```bash
k6 run test/test.js
```

Via Docker:

```bash
docker run --rm --network host -v "$PWD":/work -w /work grafana/k6 run test/test.js
```

Smoke test:

```bash
docker run --rm --network host -v "$PWD":/work -w /work grafana/k6 run test/smoke.js
```

Results are written to:

- [test/results.json](/Users/cirilo/Desktop/test-load-balancer/test/results.json)

## Development Mocks on macOS

Files:

- [packages/mocks/sys/socket.h](/Users/cirilo/Desktop/test-load-balancer/packages/mocks/sys/socket.h)
- `packages/mocks/netinet/*.h`

These mocks exist to:

- satisfy `clangd`
- avoid editor errors on macOS
- keep Linux-like constants visible to the code

They are not used in the real Linux submission build.

## Current Status

The project is currently optimized for:

- perfect detection accuracy
- low latency
- a very short hot path between `LB -> API -> scorer`

Key points of the current state:

- search is already in the best known form for this project
- `2` workers per API gave the best trade-off observed so far
- tuning on the internal Unix channel produced real gains
- more aggressive experiments with a global queue and extra workers did not pay off
