FROM --platform=linux/amd64 ubuntu:24.04 AS build

ARG ODIN_RELEASE=dev-2026-05
ARG ODIN_ARCHIVE=odin-linux-amd64-dev-2026-05.tar.gz

RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      ca-certificates \
      curl \
      build-essential \
      clang && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /opt
RUN curl -fsSLO "https://github.com/odin-lang/Odin/releases/download/${ODIN_RELEASE}/${ODIN_ARCHIVE}" && \
    mkdir -p /opt/odin && \
    tar -xzf "${ODIN_ARCHIVE}" --strip-components=1 -C /opt/odin && \
    rm -f "${ODIN_ARCHIVE}"

WORKDIR /app
COPY apps/load-balancer6/src ./src
RUN /opt/odin/odin build /app/src -out:/app/load-balancer6 -o:speed

FROM --platform=linux/amd64 ubuntu:24.04

RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      ca-certificates && \
    rm -rf /var/lib/apt/lists/*

COPY --from=build /app/load-balancer6 /usr/local/bin/load-balancer6

EXPOSE 9999

ENTRYPOINT ["/usr/local/bin/load-balancer6"]
