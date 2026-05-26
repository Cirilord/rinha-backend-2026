# rinha-backend

High-performance implementation for **Rinha de Backend 2026**, using:
- a custom TCP load balancer
- two API instances
- Unix socket FD passing (`SCM_RIGHTS`) between LB and APIs
- a prebuilt binary index for fraud scoring

## 1. Architecture

### Services
- `load-balancer`
  - Listens on `:9999`
  - Accepts TCP connections
  - Forwards accepted client FDs to API instances via `sendmsg(..., SCM_RIGHTS)`
- `load-balancer2` (experimental)
  - Event-loop backend by platform:
    - Linux: `epoll`
    - macOS: `kqueue`
  - Parses `PORT` and `WORKER_SOCKETS` env vars
  - Enables `SO_KEEPALIVE` on accepted client TCP sockets
  - Dispatches accepted client FDs to workers via Unix sockets + `SCM_RIGHTS` (round-robin)
- `load-balancer3` (experimental, Zig)
  - Parses `PORT` and `WORKER_SOCKETS` env vars
  - Accepts TCP clients and dispatches accepted FDs via `SCM_RIGHTS`
  - Persistent worker control sockets with reconnect-on-failure
  - Round-robin worker selection
- `load-balancer5` (experimental, C++)
  - C++ implementation:
    - worker/port env parsing
    - round-robin and dispatch decisions
  - Accepts TCP clients and dispatches accepted FDs via `SCM_RIGHTS`
  - Persistent worker control sockets with reconnect-on-failure
  - Round-robin worker selection
- `load-balancer6` (experimental, Odin)
  - Parses `PORT` and `BACKENDS` env vars
  - Accepts TCP clients and proxies requests to backend TCP servers
  - Per-connection backend retry across configured targets
  - Response relay from backend to client
- `api1`, `api2`
  - Listen on Unix sockets (`/shared/api1.sock`, `/shared/api2.sock`)
  - Receive forwarded client FDs from the LB
  - Parse HTTP request and return scoring result
- `server3-1`, `server3-2` (benchmark stack, Zig)
  - Minimal FD-passing workers used to isolate LB overhead in load tests
  - Linux `epoll` event loop for control sockets + client FDs
  - Parses request headers/body boundary (`Content-Length`) before responding
  - Return a fixed JSON response (`{"approved":false}`)
- `server5-1`, `server5-2` (benchmark stack, C++)
  - C++ implementation:
    - control socket loop
    - request completeness parser (`Content-Length` / headers-body boundary)
  - Receives forwarded client FDs via `SCM_RIGHTS`
  - Parses request headers/body boundary (`Content-Length`) before responding
  - Return a fixed JSON response (`{"approved":false}`)
- `server6-1`, `server6-2` (benchmark stack, Odin)
  - Odin implementation:
    - TCP listener + request completeness parser (`Content-Length`)
  - Responds with fixed JSON payload (`{"approved":false}`)
  - Used with `load-balancer6` via TCP backends

### FD Passing Flow
1. LB accepts TCP client.
2. LB sends client FD to one API (round-robin).
3. API reads request directly from received FD.
4. API writes HTTP response to same FD and closes it.

This avoids extra TCP hops between LB and API.
`load-balancer6`/`server6` is a separate benchmark stack using TCP proxying between LB and workers.

## 2. Strategy Used in Each App

### `apps/load-balancer`
- **Round-robin dispatch** across API Unix sockets.
- **Modulo-free round-robin hot path** (`wrap` with branch instead of `%`) to avoid integer division in the accept/forward loop.
- **Persistent control sockets** to APIs (reconnect only on failure).
- **Optional Linux syscall fast path** (`x86_64`/`aarch64`) for `sendmsg(SCM_RIGHTS)` with C fallback on other targets.
- **Non-blocking listener + accept drain loop**: accepts until `EAGAIN` per wakeup to reduce burst overhead.
- **Small hot path**: accept -> select upstream -> send FD -> close client FD in LB process.
- **Minimal dependencies** (single C binary).

### `apps/server`
- **poll-based control-channel multiplexing** (`MAX_CTRL_CONNS`) so one API process can handle multiple LB control connections.
- **Optional Linux syscall fast path** (`x86_64`/`aarch64`) for `recvmsg(SCM_RIGHTS)` with C fallback on other targets.
- **Minimal HTTP parsing** optimized for the challenge endpoints:
  - `GET /ready`
  - `POST /fraud-score`
  - fixed-format request-line matching
  - single-pass header parsing that extracts `Content-Length` and body offset directly (no extra body scan)
- **Warm-up phase** at startup to reduce first-request latency variance:
  - touches parser/vector path
  - touches detector index pages

### Scoring (`apps/server/src/detector.c`)
- Uses quantized vectors and top-k nearest-neighbor style lookup.
- Builds partition-key lookup tables once at index-open time to accelerate same-key candidate selection.
- Includes **early pruning / early-exit**:
  - partition bound pruning
  - same-key partition first
  - node branch-and-bound when partitions have internal subindex nodes
  - direct leaf-scan fast path when index roots are leaf nodes
  - leaf scan early stop when top-k threshold is good enough
- SIMD hot path in `scan_block`:
  - `__AVX2__` on `amd64`
  - `__ARM_NEON__` on `arm64`
  - scalar fallback otherwise
- Query SIMD constants are pre-expanded once per request.
- Distance accumulation uses chunked 32-bit partial sums widened to 64-bit.

## 3. Vector Search Details (detector)

This section describes the vector search strategy used in `apps/server/src/detector.c`.

### Concepts Used (Names)

- **Exact k-Nearest Neighbors (Exact k-NN)**
- **Feature Normalization**
- **Quantization (float -> int16 / q16)**
- **Space Partitioning (Partition Key)**
- **Bounding Boxes (AABB: Axis-Aligned Bounding Box)**
- **Leaf-Partition Fast Path**
- **Branch-and-Bound Tree Traversal**
- **Top-K Maintenance**
- **Lower-Bound Pruning**
- **Early Exit Heuristic**
- **AoSoA (Array of Structures of Arrays)**
- **SIMD Vectorization (AVX2 / NEON)**

### Data Representation

- Each request is transformed into a 14-dimensional feature vector.
- Features are normalized and quantized to `int16` (`q16`) with scale `10000`.
- References are stored in `resources/references.idx` (generated by `scripts/build_binary_references.py`).

### Index Layout

- Header: metadata (magic, scale, dims, counts).
- Partition directory: partition key + min/max bounding boxes + root.
- Node directory: tree nodes with child pointers and bounding boxes.
- Vector storage: AoSoA blocks (`LANES=8`) for SIMD-friendly scans.
- Labels: packed per block lane (fraud/legit).

### Partition Key Strategy

The partition key is computed from selected quantized dimensions:
- sign/boolean-like features (`q[5]`, `q[9]`, `q[10]`, `q[11]`)
- bucketed MCC-risk feature (`q[12]`)
- high-threshold indicators (`q[2]`, `q[8]`)

This puts likely neighbors close in partition space and reduces search work.

### Query Flow

1. Quantize query vector (`double -> int16 q16`).
2. Initialize top-k (`K=5`) with max distances.
3. Search matching partition key first.
4. Evaluate remaining partitions by lower-bound distance (sorted candidates).
5. Traverse subindex nodes with branch-and-bound (or direct leaf scan when partition root is a leaf).
6. Scan selected leaf blocks (AoSoA + SIMD) and update top-k.
7. Count fraud labels in top-k and return fraud count.

### SIMD and Hot Path

The `scan_block` hot path is architecture-specific:
- `__AVX2__` for `amd64`
- `__ARM_NEON__` for `arm64`
- scalar fallback otherwise

### Pruning and Early Exit

- Skip partition and node branches when bounds are worse than current top-k worst distance.
- Stop early when confidence threshold is reached (`X_SCORE_EARLY_DISTANCE_MILLI`, default `143`).

This is the main reason search stays fast under load.

## 4. Build System (Makefile)

Root `Makefile` controls binary compilation used by Docker builds.

Targets:
- `make server`
- `make load-balancer`
- `make clean`

Architecture-aware flags (`TARGETARCH`):
- `server`:
  - `amd64`: `-O3` globally + `-mavx2 -mfma -march=haswell`, plus `-fno-plt`
  - `detector.c`: compiled with `-O2` (same SIMD arch flags) to reduce hot-path code bloat from aggressive inlining, plus `-fno-plt`
  - `arm64`: `-O3` globally and `detector.c` with `-O2` (without AVX2/FMA flags), plus `-fno-plt`
- `load-balancer`:
  - `amd64`: `-O3` without AVX2/FMA flags to avoid AVX state-transition overhead (`vzeroupper`) in the LB hot path, plus `-fno-plt`
  - `arm64`: `-O3` plus `-fno-plt`

Examples:

```bash
make server TARGETARCH=arm64
make load-balancer TARGETARCH=arm64
```

```bash
make server TARGETARCH=amd64
make load-balancer TARGETARCH=amd64
```

### Git Hooks (Lefthook)

This repo includes `lefthook.yml` to run C formatting on `pre-commit`.

Hook behavior:
- formats staged `*.c` and `*.h` files with `clang-format`
- re-stages fixed files automatically (`stage_fixed: true`)

Setup:

```bash
brew install lefthook clang-format
lefthook install
```

Run manually (optional):

```bash
lefthook run pre-commit
```

## 5. Python Scripts

### `scripts/build_binary_references.py`
Builds the binary scoring index used at runtime.

- Input: `resources/references.json.gz`
- Output: `resources/references.idx`
- Format: custom little-endian structure with header, partitions, nodes, vectors, labels.
- Builds a second-level subindex inside each partition (small binary tree over sub-buckets),
  enabling branch-and-bound pruning before leaf vector scan.
- Current default split profile emphasizes `day_of_week` (three thresholds) plus
  `amount`, `tx_count_24h`, and `merchant_avg_amount`.

Command:

```bash
python3 scripts/build_binary_references.py
```

### `scripts/tune_partition_cutoffs.py`
Suggests partition cutoff values by analyzing `resources/references.idx`.

- Computes thresholds (weighted Gini split) for selected vector dimensions.
- Helps tune partition key behavior used in index/scoring logic.

Command:

```bash
python3 scripts/tune_partition_cutoffs.py --index resources/references.idx
```

Optional JSON output:

```bash
python3 scripts/tune_partition_cutoffs.py \
  --index resources/references.idx \
  --json /tmp/cutoffs.json
```

## 6. Project Structure

```text
.
├── Makefile
├── docker-compose.yml
├── apps
│   ├── load-balancer
│   │   ├── Dockerfile
│   │   └── src/main.c
│   ├── load-balancer2
│   │   ├── Dockerfile
│   │   └── src/
│   ├── load-balancer3
│   │   ├── Dockerfile
│   │   └── src/main.zig
│   ├── load-balancer4
│   │   ├── Dockerfile
│   │   └── src/main.rs
│   ├── load-balancer5
│   │   ├── Dockerfile
│   │   └── src/main.cpp
│   ├── load-balancer6
│   │   ├── Dockerfile
│   │   └── src/main.odin
│   ├── server3
│   │   ├── Dockerfile
│   │   └── src/main.zig
│   ├── server4
│   │   ├── Dockerfile
│   │   └── src/main.rs
│   ├── server5
│   │   ├── Dockerfile
│   │   └── src/main.cpp
│   ├── server6
│   │   ├── Dockerfile
│   │   └── src/main.odin
│   └── server
│       ├── Dockerfile
│       └── src/
├── resources
│   ├── references.json.gz
│   └── references.idx
├── scripts
│   ├── build_binary_references.py
│   └── tune_partition_cutoffs.py
└── test
    ├── smoke.js
    ├── test.js
    ├── test-data.json
    └── results.json
```

## 7. Build and Run with Docker Compose

### Build + start

Use `--compatibility` so local Compose enforces `deploy.resources.limits`.

```bash
docker compose --compatibility up -d --build
```

### Stop

```bash
docker compose down
```

### Logs

```bash
docker compose logs -f
```

## 8. Resource Limits (Rinha Budget)

Configured in `docker-compose.yml`:
- `server6-1`: `0.42 CPU`, `150MB`
- `server6-2`: `0.42 CPU`, `150MB`
- `lb6`: `0.16 CPU`, `50MB`

Total: **1.00 CPU / 350MB**.

CPU pinning (`cpuset`) currently configured:
- `lb6`: `"0"`
- `server6-1`: `"1"`
- `server6-2`: `"2"`

## 9. Environment Variables

### API
- `UNIX_SOCKET_PATH` (required)
- `X_SCORE_INDEX_PATH` (required)

### Load Balancer
- `PORT` (default `9999`)
- `WORKER_SOCKETS` (comma-separated Unix socket list)

### Load Balancer6 (Benchmark Stack, Odin)
- `PORT` (default `9999`)
- `BACKENDS` (required, comma-separated `host:port`, e.g. `server6-1:8081,server6-2:8082`)

### Server6 (Benchmark Stub, Odin)
- `PORT` (default `8081`/`8082` via compose)
- Runtime model: direct TCP server used by `load-balancer6`

## 10. API Endpoints

### `GET /ready`
Health/readiness endpoint.

### `POST /fraud-score`
Scores a transaction payload and returns decision fields.

## 11. Test Commands

### Curl quick check

```bash
curl -i http://localhost:9999/ready
```

```bash
curl -i http://localhost:9999/fraud-score \
  -X POST \
  -H 'Content-Type: application/json' \
  --data-raw '{
    "id": "tx-3576980410",
    "transaction": {
      "amount": 384.88,
      "installments": 3,
      "requested_at": "2026-03-11T20:23:35Z"
    },
    "customer": {
      "avg_amount": 769.76,
      "tx_count_24h": 3,
      "known_merchants": ["MERC-009", "MERC-001", "MERC-001"]
    },
    "merchant": {
      "id": "MERC-001",
      "mcc": "5912",
      "avg_amount": 298.95
    },
    "terminal": {
      "is_online": false,
      "card_present": true,
      "km_from_home": 13.7090520965
    },
    "last_transaction": {
      "timestamp": "2026-03-11T14:58:35Z",
      "km_from_current": 18.8626479774
    }
  }'
```

### Smoke test (k6)

```bash
docker run --rm -i \
  --network rinha-backend_rinha \
  -v "$PWD:/work" -w /work \
  -e BASE_URL=http://lb6:9999 \
  grafana/k6 run test/smoke.js
```

### Full load/scoring test (k6)

```bash
docker run --rm -i \
  --network rinha-backend_rinha \
  -v "$PWD:/work" -w /work \
  -e BASE_URL=http://lb6:9999 \
  grafana/k6 run test/test.js
```

Result summary is written to:
- `test/results.json`

## 12. Building for Final Submission (`linux-amd64`)

Official target is `linux-amd64`.  
This project enables AVX2 on `amd64` builds through the Makefile flags.

For Docker buildx:

```bash
docker buildx build --platform linux/amd64 -f apps/server6/Dockerfile .
docker buildx build --platform linux/amd64 -f apps/load-balancer6/Dockerfile .
```

## 13. Notes

- Keep `resources/references.idx` synchronized with `resources/references.json.gz` whenever you rebuild the index.
- For local performance comparison, always run with:
  - same compose limits
  - `--compatibility`
  - same k6 script/options
