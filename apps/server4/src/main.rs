use std::env;
use std::io::{IoSliceMut, Read, Write};
use std::net::TcpStream;
use std::os::fd::{AsRawFd, FromRawFd, RawFd};
use std::os::unix::net::UnixListener;
use std::path::Path;

use nix::cmsg_space;
use nix::errno::Errno;
use nix::sys::socket::{recvmsg, ControlMessageOwned, MsgFlags};

const CLIENT_BUFFER_SIZE: usize = 8192;
const RESPONSE: &[u8] = b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: 18\r\n\r\n{\"approved\":false}";

fn parse_content_length_line(line: &[u8]) -> Option<usize> {
    const PREFIX: &[u8] = b"Content-Length:";
    if !line.starts_with(PREFIX) {
        return None;
    }

    let mut i = PREFIX.len();
    while i < line.len() && (line[i] == b' ' || line[i] == b'\t') {
        i += 1;
    }
    if i >= line.len() {
        return None;
    }

    let mut value: usize = 0;
    let mut has_digits = false;

    while i < line.len() {
        let ch = line[i];
        if !ch.is_ascii_digit() {
            return None;
        }

        has_digits = true;
        value = value.checked_mul(10)?.checked_add((ch - b'0') as usize)?;
        i += 1;
    }

    if has_digits {
        Some(value)
    } else {
        None
    }
}

fn read_full_request(stream: &mut TcpStream) -> bool {
    let mut buffer = [0u8; CLIENT_BUFFER_SIZE];
    let mut used = 0usize;
    let mut scan_pos = 0usize;
    let mut line_start = 0usize;
    let mut content_length = 0usize;
    let mut expected_total = 0usize;
    let mut headers_done = false;
    let mut content_length_seen = false;

    while used + 1 < buffer.len() {
        let read_end = buffer.len() - 1;
        let n = match stream.read(&mut buffer[used..read_end]) {
            Ok(0) => return false,
            Ok(n) => n,
            Err(error) => {
                if error.kind() == std::io::ErrorKind::Interrupted {
                    continue;
                }
                return false;
            }
        };
        used += n;

        if !headers_done {
            while scan_pos + 1 < used {
                if buffer[scan_pos] == b'\r' && buffer[scan_pos + 1] == b'\n' {
                    let line_len = scan_pos.saturating_sub(line_start);
                    if line_len == 0 {
                        headers_done = true;
                        let body_offset = scan_pos + 2;
                        expected_total = body_offset + content_length;
                        if expected_total + 1 > buffer.len() {
                            return false;
                        }
                        scan_pos += 2;
                        break;
                    }

                    if !content_length_seen {
                        if let Some(parsed) =
                            parse_content_length_line(&buffer[line_start..scan_pos])
                        {
                            content_length = parsed;
                            content_length_seen = true;
                        }
                    }

                    scan_pos += 2;
                    line_start = scan_pos;
                    continue;
                }
                scan_pos += 1;
            }
        }

        if headers_done && used >= expected_total {
            return true;
        }
    }

    false
}

fn send_response(stream: &mut TcpStream) -> bool {
    let mut written = 0usize;

    while written < RESPONSE.len() {
        match stream.write(&RESPONSE[written..]) {
            Ok(0) => return false,
            Ok(n) => written += n,
            Err(error) => {
                if error.kind() == std::io::ErrorKind::Interrupted {
                    continue;
                }
                return false;
            }
        }
    }

    true
}

fn recv_fd_blocking(control_fd: RawFd) -> Result<Option<RawFd>, ()> {
    loop {
        let mut data = [0u8; 1];
        let mut iov = [IoSliceMut::new(&mut data)];
        let mut cmsg_space = cmsg_space!([RawFd; 1]);

        match recvmsg::<()>(
            control_fd,
            &mut iov,
            Some(&mut cmsg_space),
            MsgFlags::empty(),
        ) {
            Ok(message) => {
                if message.bytes == 0 {
                    return Ok(None);
                }

                if let Ok(cmsgs) = message.cmsgs() {
                    for cmsg in cmsgs {
                        if let ControlMessageOwned::ScmRights(fds) = cmsg {
                            if let Some(fd) = fds.first() {
                                return Ok(Some(*fd));
                            }
                        }
                    }
                }

                return Err(());
            }
            Err(Errno::EINTR) => continue,
            Err(_) => return Err(()),
        }
    }
}

fn process_client_fd(client_fd: RawFd) {
    // SAFETY: `client_fd` is received via SCM_RIGHTS and ownership is transferred.
    let mut stream = unsafe { TcpStream::from_raw_fd(client_fd) };
    if read_full_request(&mut stream) {
        let _ = send_response(&mut stream);
    }
}

fn main() {
    let socket_path = match env::var("UNIX_SOCKET_PATH") {
        Ok(path) if !path.trim().is_empty() => path,
        _ => {
            eprintln!("UNIX_SOCKET_PATH is required and cannot be empty");
            std::process::exit(1);
        }
    };

    if Path::new(&socket_path).exists() {
        let _ = std::fs::remove_file(&socket_path);
    }

    let listener = match UnixListener::bind(&socket_path) {
        Ok(listener) => listener,
        Err(error) => {
            eprintln!("failed to bind unix socket '{}': {}", socket_path, error);
            std::process::exit(1);
        }
    };

    eprintln!("server4 (rust) listening for fd passing at {}", socket_path);

    loop {
        let (control, _) = match listener.accept() {
            Ok(conn) => conn,
            Err(error) => {
                if error.kind() == std::io::ErrorKind::Interrupted {
                    continue;
                }
                continue;
            }
        };

        let control_fd = control.as_raw_fd();
        loop {
            match recv_fd_blocking(control_fd) {
                Ok(Some(client_fd)) => process_client_fd(client_fd),
                Ok(None) => break,
                Err(_) => break,
            }
        }
    }
}
