FROM gcc:14-bookworm AS build
ARG TARGETARCH
WORKDIR /src
COPY Makefile ./Makefile
COPY apps/server/src ./apps/server/src
RUN make server TARGETARCH=${TARGETARCH} && mv server /build-server

FROM debian:bookworm-slim
WORKDIR /app
COPY --from=build /build-server /usr/local/bin/server
ENV UNIX_SOCKET_PATH=/shared/api.sock
CMD ["/usr/local/bin/server"]
