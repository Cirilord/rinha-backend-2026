# rinha-backend

C implementation for Rinha de Backend 2026, with:
- `load-balancer` listening on TCP port `9999`
- `api1` and `api2` receiving connections through **FD passing** (`SCM_RIGHTS`) over Unix sockets
- minimal HTTP parsing
- transaction parsing and vector pipeline for scoring

## Architecture

- `apps/load-balancer`
  - Listens on `0.0.0.0:9999`
  - Uses round-robin across `/shared/api1.sock` and `/shared/api2.sock`
  - Forwards accepted sockets to API instances via `sendmsg(..., SCM_RIGHTS)`

- `apps/server`
  - Listens on a Unix socket (`UNIX_SOCKET_PATH`)
  - Receives FDs from the LB via `recvmsg(..., SCM_RIGHTS)`
  - Endpoints:
    - `GET /ready`
    - `POST /fraud-score`

## Tech Stack and Decisions

- Language: C (C11)
- Build: GCC with `-O3`
- Runtime: Docker + Docker Compose
- LB/API IPC: Unix domain socket + FD passing (`SCM_RIGHTS`)
- Reference data: index file in `resources/references.idx`

## Project Structure

- `apps/load-balancer/src/main.c`
- `apps/load-balancer/Dockerfile`
- `apps/server/src/main.c`
- `apps/server/src/server.c`
- `apps/server/src/transaction_context.c`
- `apps/server/src/x-score.c`
- `docker-compose.yml`
- `test/` (k6 benchmark scripts)

## How to Run

From the project root:

```bash
docker compose up --build -d
```

Follow logs:

```bash
docker compose logs -f
```

Stop everything:

```bash
docker compose down
```

## How to Test with curl

Health check:

```bash
curl -i http://localhost:9999/ready
```

Fraud score:

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

## How to Benchmark with k6

With the stack running:

```bash
docker run --rm -i \
  --network rinha-backend_rinha \
  -v "$PWD:/work" -w /work \
  -e BASE_URL=http://load-balancer:9999 \
  grafana/k6 run test/test.js
```

Generated summary:
- `test/results.json`

## Environment Variables

### API
- `UNIX_SOCKET_PATH` (required)
- `X_SCORE_INDEX_PATH` (required)

### Load Balancer
- `PORT` (expected default: `9999`)
- `WORKER_SOCKETS` (comma-separated list)

## Notes

- Official Rinha submission requires `linux-amd64` compatibility.
- Compose setup must respect the competition resource budget (1 CPU / 350 MB total).
- Depending on your current benchmark phase, fraud scoring in `main.c` may be temporarily simplified for latency testing.
