FROM alpine:3.22 AS build

RUN apk add --no-cache build-base linux-headers python3 py3-numpy

ARG CC=gcc
ARG TARGETARCH
ARG CFLAGS_BASE="-Ofast -DNDEBUG -fomit-frame-pointer -flto -fno-plt -s -static -std=c11 -Wall -Wextra"
ARG CFLAGS_ARCH=""

WORKDIR /src
COPY apps/server/src/ /src/
COPY resources/references.json.gz /src/resources/references.json.gz
COPY scripts/build_references_idx.py /tools/build_references_idx.py
RUN mkdir -p /out /src/resources
RUN python3 /tools/build_references_idx.py
RUN ${CC} ${CFLAGS_BASE} ${CFLAGS_ARCH} -o /out/server /src/*.c

FROM scratch
COPY --from=build /out/server /server
COPY --from=build /src/resources/references.idx /resources/references.idx
ENTRYPOINT ["/server"]
