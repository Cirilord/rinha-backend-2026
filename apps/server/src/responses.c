#include "responses.h"

#define RESPONSE(s)                                                            \
  { (s), sizeof(s) - 1 }

const Response RESPONSE_BAD_REQUEST = RESPONSE("HTTP/1.1 400 Bad Request\r\n"
                                               "Content-Length: 0\r\n"
                                               "Connection: close\r\n\r\n");

const Response RESPONSE_FRAUD_00 =
    RESPONSE("HTTP/1.1 200 OK\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: 35\r\n"
             "Connection: close\r\n"
             "\r\n"
             "{\"approved\":true,\"fraud_score\":0.0}");

const Response RESPONSE_FRAUD_02 =
    RESPONSE("HTTP/1.1 200 OK\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: 35\r\n"
             "Connection: close\r\n"
             "\r\n"
             "{\"approved\":true,\"fraud_score\":0.2}");

const Response RESPONSE_FRAUD_04 =
    RESPONSE("HTTP/1.1 200 OK\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: 35\r\n"
             "Connection: close\r\n"
             "\r\n"
             "{\"approved\":true,\"fraud_score\":0.4}");

const Response RESPONSE_FRAUD_06 =
    RESPONSE("HTTP/1.1 200 OK\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: 36\r\n"
             "Connection: close\r\n"
             "\r\n"
             "{\"approved\":false,\"fraud_score\":0.6}");

const Response RESPONSE_FRAUD_08 =
    RESPONSE("HTTP/1.1 200 OK\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: 36\r\n"
             "Connection: close\r\n"
             "\r\n"
             "{\"approved\":false,\"fraud_score\":0.8}");

const Response RESPONSE_FRAUD_10 =
    RESPONSE("HTTP/1.1 200 OK\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: 36\r\n"
             "Connection: close\r\n"
             "\r\n"
             "{\"approved\":false,\"fraud_score\":1.0}");

const Response RESPONSE_NOT_FOUND = RESPONSE("HTTP/1.1 404 Not Found\r\n"
                                             "Content-Length: 0\r\n"
                                             "Connection: close\r\n\r\n");

const Response RESPONSE_READY = RESPONSE("HTTP/1.1 200 OK\r\n"
                                         "Content-Length: 0\r\n"
                                         "Connection: close\r\n\r\n");
