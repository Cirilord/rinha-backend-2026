FROM --platform=linux/amd64 rust:1-slim-bookworm AS build

WORKDIR /app
COPY apps/server4/Cargo.toml ./Cargo.toml
COPY apps/server4/src ./src

RUN cargo build --release

FROM --platform=linux/amd64 debian:bookworm-slim

RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      ca-certificates && \
    rm -rf /var/lib/apt/lists/*

COPY --from=build /app/target/release/server4 /usr/local/bin/server4

ENTRYPOINT ["/usr/local/bin/server4"]
