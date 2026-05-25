FROM alpine:3.21 AS build

RUN apk add --no-cache zig build-base

WORKDIR /app
COPY apps/server3/src/main.zig ./main.zig

RUN mkdir -p /out && \
    zig build-exe \
      -O ReleaseFast \
      -target native-linux-musl \
      -fstrip \
      -lc \
      -femit-bin=/out/server3 \
      main.zig

FROM alpine:3.21

COPY --from=build /out/server3 /usr/local/bin/server3

ENTRYPOINT ["/usr/local/bin/server3"]
