FROM gcc:14-bookworm AS build
WORKDIR /build
COPY src/main.c ./main.c
RUN gcc -O3 -Wall -Wextra -std=c11 -o load-balancer main.c

FROM debian:bookworm-slim
WORKDIR /app
COPY --from=build /build/load-balancer /usr/local/bin/load-balancer
ENV PORT=9999
ENV WORKER_SOCKETS=/shared/backend-1.sock,/shared/backend-2.sock
EXPOSE 9999
CMD ["/usr/local/bin/load-balancer"]
