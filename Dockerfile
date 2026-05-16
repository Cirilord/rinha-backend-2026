FROM gcc:14 AS builder

WORKDIR /app

RUN apt-get update && apt-get install -y --no-install-recommends python3 && rm -rf /var/lib/apt/lists/*

COPY src/main.c /app/src/main.c
COPY src/responses.c /app/src/responses.c
COPY src/server.c /app/src/server.c
COPY src/utils.c /app/src/utils.c
COPY src/transaction_context.c /app/src/transaction_context.c
COPY src/env.c /app/src/env.c
COPY src/x-score.c /app/src/x-score.c
COPY src/responses.h /app/src/responses.h
COPY src/server.h /app/src/server.h
COPY src/utils.h /app/src/utils.h
COPY src/transaction_context.h /app/src/transaction_context.h
COPY src/env.h /app/src/env.h
COPY src/x-score.h /app/src/x-score.h
COPY scripts/build_binary_references.py /app/scripts/build_binary_references.py
COPY resources/references.json.gz /app/resources/references.json.gz

RUN python3 /app/scripts/build_binary_references.py

RUN gcc -O3 -s -o /app/server /app/src/main.c /app/src/responses.c /app/src/server.c /app/src/utils.c /app/src/transaction_context.c /app/src/env.c /app/src/x-score.c

FROM debian:bookworm-slim

WORKDIR /app

EXPOSE 9999

COPY --from=builder /app/server /app/server
COPY --from=builder /app/resources /app/resources

CMD ["/app/server"]
