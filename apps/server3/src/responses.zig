pub const Response = struct {
    data: []const u8,
};

pub const response_ready = Response{
    .data =
        "HTTP/1.1 200 OK\r\n" ++
        "Content-Length: 0\r\n" ++
        "Connection: close\r\n\r\n",
};

pub const response_not_found = Response{
    .data =
        "HTTP/1.1 404 Not Found\r\n" ++
        "Content-Length: 0\r\n" ++
        "Connection: close\r\n\r\n",
};

pub const response_bad_request = Response{
    .data =
        "HTTP/1.1 400 Bad Request\r\n" ++
        "Content-Length: 0\r\n" ++
        "Connection: close\r\n\r\n",
};

pub const response_fraud_00 = Response{
    .data =
        "HTTP/1.1 200 OK\r\n" ++
        "Content-Type: application/json\r\n" ++
        "Content-Length: 35\r\n" ++
        "Connection: close\r\n" ++
        "\r\n" ++
        "{\"approved\":true,\"fraud_score\":0.0}",
};

pub const response_fraud_02 = Response{
    .data =
        "HTTP/1.1 200 OK\r\n" ++
        "Content-Type: application/json\r\n" ++
        "Content-Length: 35\r\n" ++
        "Connection: close\r\n" ++
        "\r\n" ++
        "{\"approved\":true,\"fraud_score\":0.2}",
};

pub const response_fraud_04 = Response{
    .data =
        "HTTP/1.1 200 OK\r\n" ++
        "Content-Type: application/json\r\n" ++
        "Content-Length: 35\r\n" ++
        "Connection: close\r\n" ++
        "\r\n" ++
        "{\"approved\":true,\"fraud_score\":0.4}",
};

pub const response_fraud_06 = Response{
    .data =
        "HTTP/1.1 200 OK\r\n" ++
        "Content-Type: application/json\r\n" ++
        "Content-Length: 36\r\n" ++
        "Connection: close\r\n" ++
        "\r\n" ++
        "{\"approved\":false,\"fraud_score\":0.6}",
};

pub const response_fraud_08 = Response{
    .data =
        "HTTP/1.1 200 OK\r\n" ++
        "Content-Type: application/json\r\n" ++
        "Content-Length: 36\r\n" ++
        "Connection: close\r\n" ++
        "\r\n" ++
        "{\"approved\":false,\"fraud_score\":0.8}",
};

pub const response_fraud_10 = Response{
    .data =
        "HTTP/1.1 200 OK\r\n" ++
        "Content-Type: application/json\r\n" ++
        "Content-Length: 36\r\n" ++
        "Connection: close\r\n" ++
        "\r\n" ++
        "{\"approved\":false,\"fraud_score\":1.0}",
};

pub fn fraudResponseByCount(fraud_count: u8) Response {
    return switch (fraud_count) {
        0 => response_fraud_00,
        1 => response_fraud_02,
        2 => response_fraud_04,
        3 => response_fraud_06,
        4 => response_fraud_08,
        else => response_fraud_10,
    };
}
