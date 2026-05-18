FROM gcc:14-bookworm AS build
WORKDIR /build
COPY apps/server/src/main.c ./main.c
COPY apps/server/src/responses.c ./responses.c
COPY apps/server/src/responses.h ./responses.h
COPY apps/server/src/server.c ./server.c
COPY apps/server/src/server.h ./server.h
COPY apps/server/src/transaction_context.c ./transaction_context.c
COPY apps/server/src/transaction_context.h ./transaction_context.h
COPY apps/server/src/x-score.c ./x-score.c
COPY apps/server/src/x-score.h ./x-score.h
RUN gcc -O3 -Wall -Wextra -std=c11 -o server main.c server.c responses.c transaction_context.c x-score.c

FROM debian:bookworm-slim
WORKDIR /app
COPY --from=build /build/server /usr/local/bin/server
RUN mkdir -p /app/resources
COPY resources/references.idx /app/resources/references.idx
ENV UNIX_SOCKET_PATH=/shared/api.sock
CMD ["/usr/local/bin/server"]
