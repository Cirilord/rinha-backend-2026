import std/os
import std/strutils
import posix

const
  MaxMessageSize = 64 * 1024
  ReadBufferSize = 8192
  Response = "HTTP/1.1 200 OK\r\n" &
             "Content-Type: application/json\r\n" &
             "Connection: close\r\n" &
             "Content-Length: 18\r\n" &
             "\r\n" &
             "{\"approved\":false}"

proc closeSocket(fd: SocketHandle) =
  if cint(fd) >= 0:
    discard close(cint(fd))

proc isSpaceChar(ch: char): bool =
  ch == ' ' or ch == '\t' or ch == '\r' or ch == '\n'

proc trimAsciiSpace(s: string): string =
  var start = 0
  while start < s.len and isSpaceChar(s[start]):
    inc start

  var stop = s.len
  while stop > start and isSpaceChar(s[stop - 1]):
    dec stop

  if stop <= start:
    return ""
  s[start ..< stop]

proc findHeaderEnd(payload: string): int =
  if payload.len < 4:
    return -1

  for i in 0 ..< payload.len - 3:
    if payload[i] == '\r' and payload[i + 1] == '\n' and
       payload[i + 2] == '\r' and payload[i + 3] == '\n':
      return i + 4

  -1

proc parseContentLengthFromHeaders(headers: string): int =
  for line in headers.split("\r\n"):
    if line.len == 0:
      break

    if line.startsWith("Content-Length:"):
      let raw = trimAsciiSpace(line["Content-Length:".len .. ^1])
      if raw.len == 0:
        return 0

      try:
        return parseInt(raw)
      except ValueError:
        return 0

  0

proc readFullHttpMessage(fd: SocketHandle; outMessage: var string): bool =
  outMessage.setLen(0)

  var headerEnd = -1
  var expectedTotal = -1
  var buffer: array[ReadBufferSize, char]

  while outMessage.len < MaxMessageSize:
    let n = recv(fd, addr buffer[0], buffer.len, 0)
    if n <= 0:
      return false

    var chunk = newString(n)
    copyMem(addr chunk[0], addr buffer[0], n)
    outMessage.add(chunk)

    if headerEnd < 0:
      headerEnd = findHeaderEnd(outMessage)
      if headerEnd >= 0:
        let contentLength = parseContentLengthFromHeaders(outMessage[0 ..< headerEnd])
        expectedTotal = headerEnd + contentLength
        if expectedTotal > MaxMessageSize:
          return false

    if expectedTotal >= 0 and outMessage.len >= expectedTotal:
      outMessage.setLen(expectedTotal)
      return true

  false

proc sendAll(fd: SocketHandle; data: string): bool =
  var offset = 0

  while offset < data.len:
    let remaining = data.len - offset
    let sent = send(fd, cast[pointer](unsafeAddr data[offset]), remaining, 0)
    if sent <= 0:
      return false

    offset += sent

  true

proc setUnixPath(addrUn: var Sockaddr_un; socketPath: string): bool =
  if socketPath.len == 0 or socketPath.len >= addrUn.sun_path.len:
    return false

  for i in 0 ..< socketPath.len:
    addrUn.sun_path[i] = socketPath[i]
  addrUn.sun_path[socketPath.len] = '\0'

  true

proc createUnixServer(socketPath: string): SocketHandle =
  let fd = socket(AF_UNIX, SOCK_STREAM, 0)
  if cint(fd) < 0:
    return SocketHandle(-1)

  discard unlink(socketPath.cstring)

  var addrUn: Sockaddr_un
  zeroMem(addr addrUn, sizeof(addrUn))
  addrUn.sun_family = TSa_Family(AF_UNIX)

  if not setUnixPath(addrUn, socketPath):
    closeSocket(fd)
    return SocketHandle(-1)

  if bindSocket(fd, cast[ptr SockAddr](addr addrUn), SockLen(sizeof(addrUn))) != 0:
    closeSocket(fd)
    return SocketHandle(-1)

  if listen(fd, 256) != 0:
    closeSocket(fd)
    return SocketHandle(-1)

  fd

proc handleClient(fd: SocketHandle) =
  defer: closeSocket(fd)

  var request = ""
  if not readFullHttpMessage(fd, request):
    return

  discard sendAll(fd, Response)

proc main() =
  discard sigignore(SIGPIPE)

  let socketPath = trimAsciiSpace(getEnv("UNIX_SOCKET_PATH", ""))
  if socketPath.len == 0:
    stderr.writeLine("server7: UNIX_SOCKET_PATH is required and cannot be empty")
    quit(1)

  let serverFd = createUnixServer(socketPath)
  if cint(serverFd) < 0:
    stderr.writeLine("server7: failed to create unix socket server at " & socketPath)
    quit(1)

  defer:
    closeSocket(serverFd)
    discard unlink(socketPath.cstring)

  stderr.writeLine("server7 (nim) listening on unix socket " & socketPath)

  while true:
    let clientFd = accept(serverFd, nil, nil)
    if cint(clientFd) < 0:
      continue

    handleClient(clientFd)

when isMainModule:
  main()
