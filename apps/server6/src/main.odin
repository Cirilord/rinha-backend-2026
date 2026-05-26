package main

import "core:fmt"
import "core:net"
import "core:os"
import posix "core:sys/posix"

RESPONSE :: "HTTP/1.1 200 OK\r\n" +
	"Content-Type: application/json\r\n" +
	"Connection: keep-alive\r\n" +
	"Content-Length: 18\r\n" +
	"\r\n" +
	"{\"approved\":false}"

MAX_REQUEST_SIZE :: 64 * 1024
INVALID_FD :: posix.FD(-1)

fd_is_valid :: proc(fd: posix.FD) -> bool {
	return i32(fd) >= 0
}

fd_to_socket :: proc(fd: posix.FD) -> net.TCP_Socket {
	return net.TCP_Socket(net.Socket(i64(i32(fd))))
}

is_space :: proc(ch: u8) -> bool {
	return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'
}

str_bytes :: proc(s: string) -> []u8 {
	return transmute([]u8)s
}

trim_ascii_space :: proc(s: string) -> string {
	if len(s) == 0 {
		return s
	}

	start := 0
	for start < len(s) && is_space(s[start]) {
		start += 1
	}

	end := len(s)
	for end > start && is_space(s[end-1]) {
		end -= 1
	}

	return s[start:end]
}

parse_content_length_line :: proc(line: []u8) -> (value: int, ok: bool) {
	key := str_bytes("Content-Length:")
	if len(line) < len(key) {
		return
	}

	for i := 0; i < len(key); i += 1 {
		if line[i] != key[i] {
			return
		}
	}

	i := len(key)
	for i < len(line) && (line[i] == ' ' || line[i] == '\t') {
		i += 1
	}

	if i >= len(line) {
		return
	}

	parsed := 0
	for i < len(line) {
		ch := line[i]
		if ch < '0' || ch > '9' {
			return
		}
		parsed = parsed*10 + int(ch-'0')
		i += 1
	}

	value = parsed
	ok = true
	return
}

find_header_end :: proc(buf: []u8) -> int {
	if len(buf) < 4 {
		return -1
	}

	for i := 0; i+3 < len(buf); i += 1 {
		if buf[i] == '\r' && buf[i+1] == '\n' && buf[i+2] == '\r' && buf[i+3] == '\n' {
			return i + 4
		}
	}

	return -1
}

parse_content_length_from_headers :: proc(headers: []u8) -> int {
	line_start := 0
	for i := 0; i+1 < len(headers); i += 1 {
		if headers[i] == '\r' && headers[i+1] == '\n' {
			line := headers[line_start:i]
			if len(line) == 0 {
				break
			}

			value, ok := parse_content_length_line(line)
			if ok {
				return value
			}

			line_start = i + 2
		}
	}

	return 0
}

read_full_request :: proc(sock: net.TCP_Socket, buf: []u8) -> (used: int, ok: bool) {
	header_end := -1
	expected_total := -1

	for used < len(buf) {
		bytes_read, recv_err := net.recv_tcp(sock, buf[used:])
		if recv_err != nil || bytes_read <= 0 {
			return
		}
		used += bytes_read

		if header_end < 0 {
			header_end = find_header_end(buf[:used])
			if header_end >= 0 {
				content_length := parse_content_length_from_headers(buf[:header_end])
				expected_total = header_end + content_length
				if expected_total > len(buf) {
					return
				}
			}
		}

		if expected_total >= 0 && used >= expected_total {
			ok = true
			return
		}
	}

	return
}

send_all :: proc(sock: net.TCP_Socket, data: []u8) -> bool {
	offset := 0
	for offset < len(data) {
		bytes_sent, send_err := net.send_tcp(sock, data[offset:])
		if send_err != nil || bytes_sent <= 0 {
			return false
		}
		offset += bytes_sent
	}
	return true
}

set_unix_path :: proc(addr: ^posix.sockaddr_un, path: string) -> bool {
	if len(path) == 0 || len(path) >= len(addr.sun_path) {
		return false
	}

	copy(addr.sun_path[:], str_bytes(path))
	addr.sun_path[len(path)] = 0
	return true
}

create_unix_server :: proc(path: string) -> (fd: posix.FD, ok: bool) {
	fd = posix.socket(.UNIX, .STREAM)
	if !fd_is_valid(fd) {
		return
	}

	remove_err := os.remove(path)
	_ = remove_err

	addr: posix.sockaddr_un
	addr = {}
	addr.sun_family = .UNIX
	if !set_unix_path(&addr, path) {
		_ = posix.close(fd)
		fd = INVALID_FD
		return
	}

	addr_len := posix.socklen_t(size_of(posix.sockaddr_un))
	if posix.bind(fd, cast(^posix.sockaddr)&addr, addr_len) != .OK {
		_ = posix.close(fd)
		fd = INVALID_FD
		return
	}

	if posix.listen(fd, 256) != .OK {
		_ = posix.close(fd)
		fd = INVALID_FD
		return
	}

	ok = true
	return
}

handle_client :: proc(sock: net.TCP_Socket) {
	defer net.close(sock)

	buffer: [MAX_REQUEST_SIZE]u8
	for {
		_, request_ok := read_full_request(sock, buffer[:])
		if !request_ok {
			return
		}
		if !send_all(sock, str_bytes(RESPONSE)) {
			return
		}
	}
}

main :: proc() {
	socket_path := trim_ascii_space(os.get_env("UNIX_SOCKET_PATH", context.allocator))
	if len(socket_path) == 0 {
		fmt.eprintln("server6: UNIX_SOCKET_PATH is required and cannot be empty")
		return
	}

	server_fd, ok := create_unix_server(socket_path)
	if !ok {
		fmt.eprintf("server6: failed to bind unix socket '%s'\n", socket_path)
		return
	}
	defer posix.close(server_fd)
	defer os.remove(socket_path)

	fmt.eprintf("server6 (odin) listening on unix socket %s\n", socket_path)

	for {
		client_fd := posix.accept(server_fd, nil, nil)
		if !fd_is_valid(client_fd) {
			continue
		}

		handle_client(fd_to_socket(client_fd))
	}
}
