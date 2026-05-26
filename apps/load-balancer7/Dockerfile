FROM --platform=linux/amd64 nimlang/nim:2.2.4 AS build

WORKDIR /app
COPY apps/load-balancer7/src ./src
RUN nim c -d:release --opt:speed --mm:orc -o:/app/load-balancer7 /app/src/main.nim

FROM --platform=linux/amd64 ubuntu:24.04
RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      ca-certificates && \
    rm -rf /var/lib/apt/lists/*
COPY --from=build /app/load-balancer7 /usr/local/bin/load-balancer7
EXPOSE 9999
ENTRYPOINT ["/usr/local/bin/load-balancer7"]
