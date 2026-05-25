use std::env;
use std::io::IoSlice;
use std::net::{TcpListener, TcpStream};
use std::os::fd::{AsRawFd, RawFd};
use std::os::unix::net::UnixStream;

use nix::errno::Errno;
use nix::sys::socket::sockopt::{KeepAlive, TcpNoDelay};
use nix::sys::socket::{sendmsg, setsockopt, ControlMessage, MsgFlags};

struct Worker {
    path: String,
    control: Option<UnixStream>,
}

fn parse_port() -> u16 {
    match env::var("PORT") {
        Ok(value) => value
            .trim()
            .parse::<u16>()
            .ok()
            .filter(|port| *port != 0)
            .unwrap_or(9999),
        Err(_) => 9999,
    }
}

fn parse_worker_sockets() -> Result<Vec<Worker>, String> {
    let value = env::var("WORKER_SOCKETS")
        .map_err(|_| "WORKER_SOCKETS is required and cannot be empty".to_string())?;

    let workers: Vec<Worker> = value
        .split(',')
        .map(|item| item.trim())
        .filter(|item| !item.is_empty())
        .map(|path| Worker {
            path: path.to_string(),
            control: None,
        })
        .collect();

    if workers.is_empty() {
        return Err("WORKER_SOCKETS is required and cannot be empty".to_string());
    }

    Ok(workers)
}

fn connect_worker(worker: &mut Worker) -> bool {
    if worker.control.is_some() {
        return true;
    }

    match UnixStream::connect(&worker.path) {
        Ok(stream) => {
            worker.control = Some(stream);
            true
        }
        Err(_) => false,
    }
}

fn close_worker(worker: &mut Worker) {
    let _ = worker.control.take();
}

fn send_fd_once(control_fd: RawFd, client_fd: RawFd) -> bool {
    let data = [0u8; 1];
    let iov = [IoSlice::new(&data)];
    let fds = [client_fd];
    let cmsg = [ControlMessage::ScmRights(&fds)];

    loop {
        match sendmsg::<()>(control_fd, &iov, &cmsg, MsgFlags::MSG_NOSIGNAL, None) {
            Ok(1) => return true,
            Ok(_) => return false,
            Err(Errno::EINTR) => continue,
            Err(_) => return false,
        }
    }
}

fn dispatch_client(workers: &mut [Worker], rr_next: &mut usize, client_fd: RawFd) -> bool {
    if workers.is_empty() {
        return false;
    }

    let start = *rr_next;
    *rr_next = (*rr_next + 1) % workers.len();

    for attempt in 0..workers.len() {
        let index = (start + attempt) % workers.len();
        let worker = &mut workers[index];

        if !connect_worker(worker) {
            continue;
        }

        if let Some(control) = worker.control.as_ref() {
            if send_fd_once(control.as_raw_fd(), client_fd) {
                return true;
            }
        }

        close_worker(worker);

        if connect_worker(worker) {
            if let Some(control) = worker.control.as_ref() {
                if send_fd_once(control.as_raw_fd(), client_fd) {
                    return true;
                }
            }
        }

        close_worker(worker);
    }

    false
}

fn set_client_socket_opts(stream: &TcpStream) {
    let _ = setsockopt(stream, TcpNoDelay, &true);
    let _ = setsockopt(stream, KeepAlive, &true);
}

fn main() {
    let port = parse_port();
    let mut workers = match parse_worker_sockets() {
        Ok(workers) => workers,
        Err(message) => {
            eprintln!("{message}");
            std::process::exit(1);
        }
    };

    let listener = match TcpListener::bind(("0.0.0.0", port)) {
        Ok(listener) => listener,
        Err(error) => {
            eprintln!("failed to listen on port {port}: {error}");
            std::process::exit(1);
        }
    };

    eprintln!(
        "load-balancer4 (rust) listening on :{port} with {} workers",
        workers.len()
    );

    let mut rr_next: usize = 0;

    loop {
        match listener.accept() {
            Ok((stream, _)) => {
                set_client_socket_opts(&stream);
                let client_fd = stream.as_raw_fd();
                let _ = dispatch_client(&mut workers, &mut rr_next, client_fd);
            }
            Err(error) => {
                if error.kind() == std::io::ErrorKind::Interrupted {
                    continue;
                }
            }
        }
    }
}
