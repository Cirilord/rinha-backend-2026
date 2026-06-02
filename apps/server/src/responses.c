#include "responses.h"

#define RESPONSE(s) {(s), sizeof(s) - 1}

const Response RESPONSE_BAD_REQUEST_VARIANTS[2] = {
  RESPONSE("HTTP/1.1 400 Bad Request\r\n"
           "Content-Length: 0\r\n"
           "Connection: close\r\n\r\n"),
  RESPONSE("HTTP/1.1 400 Bad Request\r\n"
           "Content-Length: 0\r\n"
           "Connection: keep-alive\r\n\r\n"),
};

const Response RESPONSE_FRAUD_VARIANTS[2][FRAUD_RESPONSES_LEN] = {
  {
    RESPONSE("HTTP/1.1 200 OK\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: 35\r\n"
             "Connection: close\r\n"
             "\r\n"
             "{\"approved\":true,\"fraud_score\":0.0}"),
    RESPONSE("HTTP/1.1 200 OK\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: 35\r\n"
             "Connection: close\r\n"
             "\r\n"
             "{\"approved\":true,\"fraud_score\":0.2}"),
    RESPONSE("HTTP/1.1 200 OK\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: 35\r\n"
             "Connection: close\r\n"
             "\r\n"
             "{\"approved\":true,\"fraud_score\":0.4}"),
    RESPONSE("HTTP/1.1 200 OK\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: 36\r\n"
             "Connection: close\r\n"
             "\r\n"
             "{\"approved\":false,\"fraud_score\":0.6}"),
    RESPONSE("HTTP/1.1 200 OK\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: 36\r\n"
             "Connection: close\r\n"
             "\r\n"
             "{\"approved\":false,\"fraud_score\":0.8}"),
    RESPONSE("HTTP/1.1 200 OK\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: 36\r\n"
             "Connection: close\r\n"
             "\r\n"
             "{\"approved\":false,\"fraud_score\":1.0}"),
  },
  {
    RESPONSE("HTTP/1.1 200 OK\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: 35\r\n"
             "Connection: keep-alive\r\n"
             "\r\n"
             "{\"approved\":true,\"fraud_score\":0.0}"),
    RESPONSE("HTTP/1.1 200 OK\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: 35\r\n"
             "Connection: keep-alive\r\n"
             "\r\n"
             "{\"approved\":true,\"fraud_score\":0.2}"),
    RESPONSE("HTTP/1.1 200 OK\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: 35\r\n"
             "Connection: keep-alive\r\n"
             "\r\n"
             "{\"approved\":true,\"fraud_score\":0.4}"),
    RESPONSE("HTTP/1.1 200 OK\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: 36\r\n"
             "Connection: keep-alive\r\n"
             "\r\n"
             "{\"approved\":false,\"fraud_score\":0.6}"),
    RESPONSE("HTTP/1.1 200 OK\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: 36\r\n"
             "Connection: keep-alive\r\n"
             "\r\n"
             "{\"approved\":false,\"fraud_score\":0.8}"),
    RESPONSE("HTTP/1.1 200 OK\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: 36\r\n"
             "Connection: keep-alive\r\n"
             "\r\n"
             "{\"approved\":false,\"fraud_score\":1.0}"),
  },
};

const Response RESPONSE_NOT_FOUND_VARIANTS[2] = {
  RESPONSE("HTTP/1.1 404 Not Found\r\n"
           "Content-Length: 0\r\n"
           "Connection: close\r\n\r\n"),
  RESPONSE("HTTP/1.1 404 Not Found\r\n"
           "Content-Length: 0\r\n"
           "Connection: keep-alive\r\n\r\n"),
};

const Response RESPONSE_READY_VARIANTS[2] = {
  RESPONSE("HTTP/1.1 200 OK\r\n"
           "Content-Length: 0\r\n"
           "Connection: close\r\n\r\n"),
  RESPONSE("HTTP/1.1 200 OK\r\n"
           "Content-Length: 0\r\n"
           "Connection: keep-alive\r\n\r\n"),
};
