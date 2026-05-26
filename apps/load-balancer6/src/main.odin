package main

import "core:fmt"
import "core:net"
import "core:os"
import "core:strconv"
import posix "core:sys/posix"

DEFAULT_PORT :: 9999
MAX_BACKENDS :: 8
MAX_REQUEST_SIZE :: 64 * 1024
INVALID_FD :: posix.FD(-1)

Backend_List :: struct {
	items: [MAX_BACKENDS]string,
	count: int,
}

backends: Backend_List
backend_sockets: [MAX_BACKENDS]net.TCP_Socket
backend_connected: [MAX_BACKENDS]bool
next_backend: int

fd_is_valid :: proc(fd: posix.FD) -> bool {
	return i32(fd) >= 0
}

fd_to_socket :: proc(fd: posix.FD) -> net.TCP_Socket {
	return net.TCP_Socket(net.Socket(i64(i32(fd))))
}

socket_to_fd :: proc(sock: net.TCP_Socket) -> posix.FD {
	return posix.FD(i32(i64(net.Socket(sock))))
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

parse_port :: proc() -> int {
	value := trim_ascii_space(os.get_env("PORT", context.allocator))
	if len(value) == 0 {
		return DEFAULT_PORT
	}

	parsed, ok := strconv.parse_int(value, 10)
	if !ok || parsed <= 0 || parsed > 65535 {
		return DEFAULT_PORT
	}

	return parsed
}

parse_backends :: proc(raw: string, out: ^Backend_List) -> bool {
	out.count = 0
	if len(raw) == 0 {
		return false
	}

	start := 0
	for i := 0; i <= len(raw); i += 1 {
		if i == len(raw) || raw[i] == ',' {
			item := trim_ascii_space(raw[start:i])
			if len(item) > 0 {
				if out.count >= MAX_BACKENDS {
					return false
				}
				out.items[out.count] = item
				out.count += 1
			}
			start = i + 1
		}
	}

	return out.count > 0
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

read_full_http_message :: proc(sock: net.TCP_Socket, buf: []u8) -> (used: int, ok: bool) {
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

set_unix_path :: proc(addr: ^posix.sockaddr_un, path: string) -> bool {
	if len(path) == 0 || len(path) >= len(addr.sun_path) {
		return false
	}

	copy(addr.sun_path[:], str_bytes(path))
	addr.sun_path[len(path)] = 0
	return true
}

close_backend :: proc(index: int) {
	if index < 0 || index >= backends.count {
		return
	}
	if backend_connected[index] {
		_ = posix.close(socket_to_fd(backend_sockets[index]))
		backend_connected[index] = false
	}
}

connect_backend :: proc(index: int) -> bool {
	if index < 0 || index >= backends.count {
		return false
	}
	if backend_connected[index] {
		return true
	}

	fd := posix.socket(.UNIX, .STREAM)
	if !fd_is_valid(fd) {
		return false
	}

	addr: posix.sockaddr_un
	addr = {}
	addr.sun_family = .UNIX
	if !set_unix_path(&addr, backends.items[index]) {
		_ = posix.close(fd)
		return false
	}

	addr_len := posix.socklen_t(size_of(posix.sockaddr_un))
	if posix.connect(fd, cast(^posix.sockaddr)&addr, addr_len) != .OK {
		_ = posix.close(fd)
		return false
	}

	backend_sockets[index] = fd_to_socket(fd)
	backend_connected[index] = true
	return true
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

choose_backend :: proc() -> int {
	if backends.count <= 0 {
		return 0
	}

	idx := next_backend
	next_backend += 1
	if next_backend >= backends.count {
		next_backend = 0
	}
	return idx
}

handle_client :: proc(client: net.TCP_Socket) {
	defer net.close(client)

	if backends.count == 0 {
		return
	}

	request_buf: [MAX_REQUEST_SIZE]u8
	request_len, request_ok := read_full_http_message(client, request_buf[:])
	if !request_ok {
		return
	}

	start := choose_backend()
	for offset := 0; offset < backends.count; offset += 1 {
		idx := (start + offset) % backends.count
		if !connect_backend(idx) {
			continue
		}

		backend := backend_sockets[idx]
		ok := send_all(backend, request_buf[:request_len])
		if ok {
			response_buf: [MAX_REQUEST_SIZE]u8
			response_len, response_ok := read_full_http_message(backend, response_buf[:])
			if !response_ok {
				ok = false
			} else {
				ok = send_all(client, response_buf[:response_len])
			}
		}

		if ok {
			return
		}

		close_backend(idx)
	}
}

main :: proc() {
	port := parse_port()
	backend_raw := trim_ascii_space(os.get_env("WORKER_SOCKETS", context.allocator))
	if !parse_backends(backend_raw, &backends) {
		fmt.eprintln("load-balancer6: WORKER_SOCKETS is required and cannot be empty")
		return
	}

	endpoint := net.Endpoint{address = net.IP4_Any, port = port}
	server, listen_err := net.listen_tcp(endpoint, 4096)
	if listen_err != nil {
		fmt.eprintf("load-balancer6: failed to listen on port %d: %v\n", port, listen_err)
		return
	}
	defer net.close(server)

	fmt.eprintf("load-balancer6 (odin) listening on :%d with %d workers\n", port, backends.count)

	for i := 0; i < backends.count; i += 1 {
		backend_connected[i] = false
	}

	for {
		client, _, accept_err := net.accept_tcp(server)
		if accept_err != nil {
			continue
		}

		handle_client(client)
	}
}
