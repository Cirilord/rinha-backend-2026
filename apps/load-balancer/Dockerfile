FROM gcc:14-bookworm AS build
ARG TARGETARCH
WORKDIR /src
COPY Makefile ./Makefile
COPY apps/load-balancer/src ./apps/load-balancer/src
RUN make clean && make load-balancer TARGETARCH=${TARGETARCH} && mv load-balancer /build-load-balancer

FROM debian:bookworm-slim
WORKDIR /app
COPY --from=build /build-load-balancer /usr/local/bin/load-balancer
ENV PORT=9999
ENV WORKER_SOCKETS=/shared/backend-1.sock,/shared/backend-2.sock
EXPOSE 9999
CMD ["/usr/local/bin/load-balancer"]
