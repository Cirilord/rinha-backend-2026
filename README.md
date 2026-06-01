# rinha-backend

High-performance C implementation for **Rinha de Backend 2026**, using:
- a custom TCP load balancer
- two API instances
- Unix socket FD passing (`SCM_RIGHTS`) between LB and APIs
- a compile-time XGBoost model for fraud scoring

## 1. Architecture

### Services
- `load-balancer`
  - Listens on `:9999`
  - Accepts TCP connections
  - Forwards accepted client FDs to API instances via `sendmsg(..., SCM_RIGHTS)`
- `api1`, `api2`
  - Listen on Unix sockets (`/shared/api1.sock`, `/shared/api2.sock`)
  - Receive forwarded client FDs from the LB
  - Parse HTTP request and return scoring result

### FD Passing Flow
1. LB accepts TCP client.
2. LB sends client FD to one API (round-robin).
3. API reads request directly from received FD.
4. API writes HTTP response to same FD and closes it.

This avoids extra TCP hops between LB and API.

## 2. Strategy Used in Each App

### `apps/load-balancer`
- **Round-robin dispatch** across API Unix sockets.
- **Persistent control sockets** to APIs (reconnect only on failure).
- **Architecture-specific syscall path** (`x86_64`/`aarch64`) for `sendmsg(SCM_RIGHTS)` without generic fallback.
- **Linux-only epoll listener loop** with non-blocking accept drain.
- **Non-blocking listener + accept drain loop**: uses `accept4(..., SOCK_NONBLOCK)` and drains until `EAGAIN` per wakeup.
- **Zero-copy request forwarding at LB layer**: accept -> select upstream -> pass client FD -> close local duplicate.
- **Non-Linux editor mocks** under `packages/mocks/sys/epoll.h` and `packages/mocks/sys/syscall.h`, intended only to avoid local typing/tooling errors.
- **Minimal dependencies** (single C binary).

### `apps/server`
- **Linux-only epoll control-channel multiplexing** (`MAX_CTRL_CONNS`) for FD passing from LB.
- **Control channel accepts via `accept4(..., SOCK_NONBLOCK)`** for LB FD-passing sockets.
- **Non-Linux editor mocks** reuse `packages/mocks/sys/epoll.h` for local typing/tooling compatibility.
- **Minimal HTTP parsing** optimized for the challenge endpoints:
  - `GET /ready`
  - `POST /fraud-score`
  - fixed-format request-line matching with a single header-boundary scan
- **Fast body extraction with known request length** (`get_body(..., request_len, ...)`) to avoid extra scans.
- **Warm-up phase** at startup to reduce first-request latency variance:
  - touches parser/vector path
  - touches xgboost prediction path

### Scoring (`apps/server/src/xgboost_model.c`)
- Uses a pre-trained **XGBoost binary classifier** compiled directly into C source.
- Inference runs without loading external model files at runtime.
- Request vector (14 features) is evaluated through generated trees and converted to probability.
- Final decision is threshold-based and mapped to `fraud_count`:
  - predicted legit -> `0` (`approved: true`)
  - predicted fraud -> `5` (`approved: false`)

## 3. XGBoost Scoring Details

This section describes the model strategy used in `apps/server/src/xgboost_model.c`.

### Concepts Used (Names)

- **Gradient Boosted Trees (XGBoost)**
- **Feature Normalization**
- **Binary Logistic Objective**
- **Threshold Calibration**
- **Compile-time C Inference Code Generation**

### Data Representation

- Each request is transformed into a 14-dimensional feature vector.
- Training uses `resources/references.json.gz` vectors and labels.
- Runtime uses the same vectorization path from `transaction_context`.

### Query Flow

1. Parse request body and build vector.
2. Run XGBoost tree traversal for all generated trees.
3. Convert margin to probability with sigmoid.
4. Apply calibrated fraud threshold.
5. Return fraud response bucket (`fraud_count=0` for legit, `fraud_count=5` for fraud).

## 4. Build System (Makefile)

Root `Makefile` controls binary compilation used by Docker builds.

Targets:
- `make server`
- `make load-balancer`
- `make clean`

Architecture-aware flags (`TARGETARCH`):
- `amd64`: adds `-mavx2 -mfma -march=haswell`
- `arm64`: no AVX2 flags

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

### `scripts/train_xgboost_and_generate_c.py`
Trains XGBoost from references and emits compile-time C inference files.

- Input modes:
  - `references`: `resources/references.json.gz`
  - `test-data`: `test/test-data.json` (uses request payloads + `expected_approved`)
- Output: `apps/server/src/xgboost_model.h`
- Output: `apps/server/src/xgboost_model.c`
- Trains a binary fraud classifier and calibrates fraud threshold using validation data.

Commands:

```bash
python3 scripts/train_xgboost_and_generate_c.py
```

```bash
python3 scripts/train_xgboost_and_generate_c.py \
  --dataset-mode test-data \
  --n-estimators 300 \
  --max-depth 10 \
  --learning-rate 0.06 \
  --subsample 1.0 \
  --colsample 1.0
```

### `scripts/build_binary_references.py` (legacy utility)
Builds the old binary nearest-neighbor index format.

- Input: `resources/references.json.gz`
- Output: `resources/references.idx`
- Not used by the current runtime scorer.

Command:

```bash
python3 scripts/build_binary_references.py
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
│   └── server
│       ├── Dockerfile
│       └── src/
├── resources
│   ├── references.json.gz
│   └── references.idx
├── scripts
│   ├── train_xgboost_and_generate_c.py
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
- `api1`: `0.47 CPU`, `150MB`
- `api2`: `0.47 CPU`, `150MB`
- `load-balancer`: `0.06 CPU`, `50MB`

Total: **1.00 CPU / 350MB**.

## 9. Environment Variables

### API
- `UNIX_SOCKET_PATH` (required)

### Load Balancer
- `PORT` (default `9999`)
- `WORKER_SOCKETS` (comma-separated Unix socket list)

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
  -e BASE_URL=http://load-balancer:9999 \
  grafana/k6 run test/smoke.js
```

### Full load/scoring test (k6)

```bash
docker run --rm -i \
  --network rinha-backend_rinha \
  -v "$PWD:/work" -w /work \
  -e BASE_URL=http://load-balancer:9999 \
  grafana/k6 run test/test.js
```

Result summary is written to:
- `test/results.json`

## 12. Building for Final Submission (`linux-amd64`)

Official target is `linux-amd64`.  
This project enables AVX2 on `amd64` builds through the Makefile flags.

For Docker buildx:

```bash
docker buildx build --platform linux/amd64 -f apps/server/Dockerfile .
docker buildx build --platform linux/amd64 -f apps/load-balancer/Dockerfile .
```

## 13. Notes

- Regenerate `apps/server/src/xgboost_model.c` and `apps/server/src/xgboost_model.h`
  whenever you retrain the XGBoost model.
- For local performance comparison, always run with:
  - same compose limits
  - `--compatibility`
  - same k6 script/options
