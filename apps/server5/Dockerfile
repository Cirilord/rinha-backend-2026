FROM --platform=linux/amd64 ubuntu:24.04 AS build

RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      ca-certificates \
      g++ && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY apps/server5/src/main.cpp ./main.cpp

RUN g++ -O3 -DNDEBUG -std=c++20 -Wall -Wextra -o server5 main.cpp

FROM --platform=linux/amd64 ubuntu:24.04

RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      ca-certificates \
      libstdc++6 && \
    rm -rf /var/lib/apt/lists/*

COPY --from=build /app/server5 /usr/local/bin/server5

ENTRYPOINT ["/usr/local/bin/server5"]
