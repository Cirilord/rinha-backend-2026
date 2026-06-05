FROM alpine:3.22 AS build

RUN apk add --no-cache build-base linux-headers make

ARG CC=gcc
ARG TARGETARCH

WORKDIR /src
COPY Makefile /src/Makefile
COPY apps/load-balancer/src/ /src/apps/load-balancer/src/
RUN make -C /src load-balancer CC="${CC}" TARGETARCH="${TARGETARCH}"

FROM scratch
COPY --from=build /src/load-balancer /load-balancer
ENTRYPOINT ["/load-balancer"]
