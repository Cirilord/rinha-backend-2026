FROM alpine:3.21 AS build

RUN apk add --no-cache zig build-base

WORKDIR /app
COPY apps/load-balancer3/src/main.zig ./main.zig

RUN mkdir -p /out && \
    zig build-exe \
      -O ReleaseFast \
      -target native-linux-musl \
      -fstrip \
      -lc \
      -femit-bin=/out/load-balancer3 \
      main.zig

FROM alpine:3.21

COPY --from=build /out/load-balancer3 /usr/local/bin/load-balancer3

EXPOSE 9999

ENTRYPOINT ["/usr/local/bin/load-balancer3"]
