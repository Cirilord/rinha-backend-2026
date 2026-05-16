#include "responses.h"

#define RESPONSE(s) \
  { (s), sizeof(s) - 1 }

const Response RESPONSE_OK = RESPONSE(
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 2\r\n"
    "Connection: close\r\n"
    "\r\n"
    "ok");

const Response FRAUD_RESPONSES[FRAUD_RESPONSES_LEN] = {
    RESPONSE(
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 35\r\n"
        "Connection: close\r\n"
        "\r\n"
        "{\"approved\":true,\"fraud_score\":0.0}"),
    RESPONSE(
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 35\r\n"
        "Connection: close\r\n"
        "\r\n"
        "{\"approved\":true,\"fraud_score\":0.2}"),
    RESPONSE(
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 35\r\n"
        "Connection: close\r\n"
        "\r\n"
        "{\"approved\":true,\"fraud_score\":0.4}"),
    RESPONSE(
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 36\r\n"
        "Connection: close\r\n"
        "\r\n"
        "{\"approved\":false,\"fraud_score\":0.6}"),
    RESPONSE(
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 36\r\n"
        "Connection: close\r\n"
        "\r\n"
        "{\"approved\":false,\"fraud_score\":0.8}"),
    RESPONSE(
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 36\r\n"
        "Connection: close\r\n"
        "\r\n"
        "{\"approved\":false,\"fraud_score\":1.0}"),
};

const Response RESPONSE_READY = RESPONSE("HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
const Response RESPONSE_NOT_FOUND =
    RESPONSE("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
const Response RESPONSE_BAD_REQUEST =
    RESPONSE("HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
