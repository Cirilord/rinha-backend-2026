FROM --platform=linux/amd64 rust:1-slim-bookworm AS build

WORKDIR /app
COPY apps/load-balancer4/Cargo.toml ./Cargo.toml
COPY apps/load-balancer4/src ./src

RUN cargo build --release

FROM --platform=linux/amd64 debian:bookworm-slim

RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      ca-certificates && \
    rm -rf /var/lib/apt/lists/*

COPY --from=build /app/target/release/load-balancer4 /usr/local/bin/load-balancer4

EXPOSE 9999

ENTRYPOINT ["/usr/local/bin/load-balancer4"]
