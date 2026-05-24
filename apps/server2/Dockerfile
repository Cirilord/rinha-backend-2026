FROM alpine:3.21 AS build

RUN apk add --no-cache build-base

WORKDIR /app
COPY apps/server2/src/main.c ./main.c

RUN mkdir -p /out && cc -O3 -std=c11 -Wall -Wextra -o /out/server2 main.c

FROM alpine:3.21

COPY --from=build /out/server2 /usr/local/bin/server2

ENTRYPOINT ["/usr/local/bin/server2"]
