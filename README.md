# Rinha de Backend 2026 — C + HAProxy

A low-latency, low-overhead backend implementation for Rinha de Backend 2026.

## Architecture

```text
client
  -> haproxy (load balancer)
      -> api1 (C)
      -> api2 (C)
```

## Current stack

- C API (no HTTP framework)
- HAProxy as load balancer / reverse proxy
- Binary vector index (`references.idx`) loaded with `mmap`
- Docker Compose for local orchestration

## Project layout

```text
.
├── docker-compose.yml
├── Dockerfile
├── haproxy.cfg
├── scripts/build_binary_references.py
├── resources/
│   ├── references.json.gz
│   └── references.idx
├── src/
│   ├── main.c
│   ├── server.c
│   ├── utils.c
│   ├── transaction_context.c
│   ├── x-score.c
│   └── ...
└── test/
    ├── smoke.js
    ├── test.js
    └── test-data.json
```

## Runtime flow

- `scripts/build_binary_references.py` reads `resources/references.json.gz`.
- The script generates `resources/references.idx` in a binary format.
- The API loads the index at startup using `mmap`.
- For each transaction:
- A 14-dim vector is built in `transaction_context.c`.
- `x-score.c` runs exact kNN (`k=5`) with specialist partitions and `key-first` pruning.
- The response returns `approved` and `fraud_score`.

## API environment variables

- `PORT` (required)
- `WORKERS` (required)

Notes:
- `k=5` is fixed in the hot path (`fraud_score = fraud_count/5`).
- search mode is fixed to specialist `key-first` in the code.

## Run locally

```bash
docker compose up --build -d
```

View logs:

```bash
docker compose logs -f api1 api2 haproxy
```

Stop:

```bash
docker compose down
```

## Load tests

Smoke test:

```bash
docker run --rm -i \
  -v "$PWD:/work" -w /work \
  grafana/k6 run -e BASE_URL=http://host.docker.internal:9999 test/smoke.js
```

Main benchmark:

```bash
docker run --rm -i \
  -v "$PWD:/work" -w /work \
  grafana/k6 run -e BASE_URL=http://host.docker.internal:9999 test/test.js
```

Result file:

- `test/results.json`

## Notes

- Hot path focus: socket -> parse -> vectorize -> classify -> respond.
- Binary index avoids heavy JSON parsing at startup.
- CPU/memory limits can be enabled in `docker-compose.yml` to simulate challenge constraints.

## References

- Official challenge repo: <https://github.com/zanfranceschi/rinha-de-backend-2026>
- Challenge website: <https://rinhadebackend.com.br/>
