#include <arpa/inet.h>
#include <errno.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Worker {
  std::string path;
  int control_fd = -1;
};

static int parse_port() {
  const char* value = std::getenv("PORT");
  if (value == nullptr || value[0] == '\0') {
    return 9999;
  }

  char* end = nullptr;
  const long port = std::strtol(value, &end, 10);
  if (end == value || *end != '\0' || port <= 0 || port > 65535) {
    return 9999;
  }

  return static_cast<int>(port);
}

static std::vector<Worker> parse_workers() {
  const char* value = std::getenv("WORKER_SOCKETS");
  if (value == nullptr) {
    return {};
  }

  std::vector<Worker> workers;
  std::string raw(value);
  std::size_t start = 0;
  while (start < raw.size()) {
    std::size_t end = raw.find(',', start);
    if (end == std::string::npos) {
      end = raw.size();
    }

    std::string item = raw.substr(start, end - start);
    std::size_t l = 0;
    while (l < item.size() && std::isspace(static_cast<unsigned char>(item[l])) != 0) {
      l++;
    }
    std::size_t r = item.size();
    while (r > l && std::isspace(static_cast<unsigned char>(item[r - 1])) != 0) {
      r--;
    }

    if (r > l) {
      Worker worker;
      worker.path = item.substr(l, r - l);
      workers.push_back(worker);
    }

    start = end + 1;
  }

  return workers;
}

static void close_fd(int fd) {
  if (fd >= 0) {
    (void)close(fd);
  }
}

static int connect_worker(const std::string& path) {
  const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    return -1;
  }

  sockaddr_un addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

  if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    close_fd(fd);
    return -1;
  }

  return fd;
}

static bool ensure_connected(Worker& worker) {
  if (worker.control_fd >= 0) {
    return true;
  }

  worker.control_fd = connect_worker(worker.path);
  return worker.control_fd >= 0;
}

static void reset_connection(Worker& worker) {
  close_fd(worker.control_fd);
  worker.control_fd = -1;
}

static bool send_fd_once(int control_fd, int client_fd) {
  char data = 1;
  iovec io;
  io.iov_base = &data;
  io.iov_len = 1;

  char cmsgbuf[CMSG_SPACE(sizeof(int))];
  std::memset(cmsgbuf, 0, sizeof(cmsgbuf));

  msghdr msg;
  std::memset(&msg, 0, sizeof(msg));
  msg.msg_iov = &io;
  msg.msg_iovlen = 1;
  msg.msg_control = cmsgbuf;
  msg.msg_controllen = sizeof(cmsgbuf);

  cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
  if (cmsg == nullptr) {
    return false;
  }

  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(int));
  std::memcpy(CMSG_DATA(cmsg), &client_fd, sizeof(int));
  msg.msg_controllen = cmsg->cmsg_len;

  for (;;) {
    const ssize_t sent = sendmsg(control_fd, &msg, MSG_NOSIGNAL);
    if (sent == 1) {
      return true;
    }
    if (sent < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
}

static bool dispatch_client(std::vector<Worker>& workers, std::size_t& rr_next, int client_fd) {
  if (workers.empty()) {
    return false;
  }

  const std::size_t start = rr_next;
  rr_next = (rr_next + 1) % workers.size();

  for (std::size_t attempt = 0; attempt < workers.size(); attempt++) {
    Worker& worker = workers[(start + attempt) % workers.size()];

    if (!ensure_connected(worker)) {
      continue;
    }

    if (send_fd_once(worker.control_fd, client_fd)) {
      return true;
    }

    reset_connection(worker);
    if (ensure_connected(worker) && send_fd_once(worker.control_fd, client_fd)) {
      return true;
    }

    reset_connection(worker);
  }

  return false;
}

static int create_listener(int port) {
  const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    return -1;
  }

  int one = 1;
  (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(static_cast<uint16_t>(port));

  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    close_fd(fd);
    return -1;
  }

  if (listen(fd, 4096) != 0) {
    close_fd(fd);
    return -1;
  }

  return fd;
}

static void set_client_options(int client_fd) {
  int one = 1;
  (void)setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  (void)setsockopt(client_fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
}

}  // namespace

int main() {
  std::signal(SIGPIPE, SIG_IGN);

  std::vector<Worker> workers = parse_workers();
  if (workers.empty()) {
    std::fprintf(stderr, "WORKER_SOCKETS is required and cannot be empty\n");
    return 1;
  }

  const int port = parse_port();
  const int listener = create_listener(port);
  if (listener < 0) {
    std::fprintf(stderr, "failed to listen on port %d: %s\n", port, std::strerror(errno));
    return 1;
  }

  std::fprintf(stderr, "load-balancer5 (c++) listening on :%d with %zu workers\n", port,
               workers.size());

  std::size_t rr_next = 0;

  for (;;) {
    const int client_fd = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
    if (client_fd < 0) {
      if (errno == EINTR) {
        continue;
      }
      continue;
    }

    set_client_options(client_fd);
    (void)dispatch_client(workers, rr_next, client_fd);
    close_fd(client_fd);
  }
}
