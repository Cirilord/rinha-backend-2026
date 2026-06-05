FROM alpine:3.22 AS build

RUN apk add --no-cache build-base linux-headers python3 py3-numpy make

ARG CC=gcc
ARG TARGETARCH

WORKDIR /src
COPY Makefile /src/Makefile
COPY apps/server/src/ /src/apps/server/src/
COPY resources/references.json.gz /src/resources/references.json.gz
COPY scripts/build_references_idx.py /src/scripts/build_references_idx.py
RUN mkdir -p /src/resources
RUN python3 /src/scripts/build_references_idx.py
RUN make -C /src server CC="${CC}" TARGETARCH="${TARGETARCH}"

FROM scratch
COPY --from=build /src/server /server
COPY --from=build /src/resources/references.idx /resources/references.idx
ENTRYPOINT ["/server"]
