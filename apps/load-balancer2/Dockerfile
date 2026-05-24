FROM alpine:3.21 AS build

RUN apk add --no-cache build-base

WORKDIR /app
COPY apps/load-balancer2/src/main.c ./main.c
COPY apps/load-balancer2/src/envs.c ./envs.c
COPY apps/load-balancer2/src/envs.h ./envs.h
COPY apps/load-balancer2/src/server.c ./server.c
COPY apps/load-balancer2/src/server.h ./server.h

RUN mkdir -p /out && cc -O3 -std=c11 -Wall -Wextra -o /out/load-balancer2 main.c envs.c server.c

FROM alpine:3.21

COPY --from=build /out/load-balancer2 /usr/local/bin/load-balancer2

EXPOSE 9999

ENTRYPOINT ["/usr/local/bin/load-balancer2"]
