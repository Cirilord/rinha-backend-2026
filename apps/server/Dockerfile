FROM alpine:3.22 AS build

RUN apk add --no-cache build-base linux-headers

ARG CC=gcc
ARG TARGETARCH
ARG CFLAGS_BASE="-Ofast -DNDEBUG -fomit-frame-pointer -flto -fno-plt -s -static -std=c11 -Wall -Wextra"
ARG CFLAGS_ARCH=""

WORKDIR /src
COPY apps/server/src/ /src/
RUN mkdir -p /out && \
    ${CC} ${CFLAGS_BASE} ${CFLAGS_ARCH} -o /out/server /src/*.c

FROM scratch
COPY --from=build /out/server /server
ENTRYPOINT ["/server"]
