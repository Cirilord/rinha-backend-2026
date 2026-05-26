import std/os
import std/strutils
import posix

const
  DefaultPort = 9999
  MaxMessageSize = 64 * 1024
  ReadBufferSize = 8192

proc closeSocket(fd: SocketHandle) =
  if cint(fd) >= 0:
    discard close(cint(fd))

proc htons(v: uint16): uint16 {.inline.} =
  when cpuEndian == littleEndian:
    ((v shl 8) or (v shr 8))
  else:
    v

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

proc parsePort(): int =
  let raw = trimAsciiSpace(getEnv("PORT", ""))
  if raw.len == 0:
    return DefaultPort

  try:
    let parsed = parseInt(raw)
    if parsed <= 0 or parsed > 65535:
      return DefaultPort
    return parsed
  except ValueError:
    return DefaultPort

proc parseWorkers(): seq[string] =
  let raw = trimAsciiSpace(getEnv("WORKER_SOCKETS", ""))
  if raw.len == 0:
    return @[]

  for item in raw.split(','):
    let worker = trimAsciiSpace(item)
    if worker.len > 0:
      result.add(worker)

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

proc connectUnixBackend(socketPath: string): SocketHandle =
  let fd = socket(AF_UNIX, SOCK_STREAM, 0)
  if cint(fd) < 0:
    return SocketHandle(-1)

  var addrUn: Sockaddr_un
  zeroMem(addr addrUn, sizeof(addrUn))
  addrUn.sun_family = TSa_Family(AF_UNIX)

  if not setUnixPath(addrUn, socketPath):
    closeSocket(fd)
    return SocketHandle(-1)

  if connect(fd, cast[ptr SockAddr](addr addrUn), SockLen(sizeof(addrUn))) != 0:
    closeSocket(fd)
    return SocketHandle(-1)

  fd

proc createTcpServer(port: int): SocketHandle =
  let fd = socket(AF_INET, SOCK_STREAM, 0)
  if cint(fd) < 0:
    return SocketHandle(-1)

  var reuse: cint = 1
  discard setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, addr reuse, SockLen(sizeof(reuse)))

  var addrIn: Sockaddr_in
  zeroMem(addr addrIn, sizeof(addrIn))
  addrIn.sin_family = TSa_Family(AF_INET)
  addrIn.sin_port = InPort(htons(uint16(port)))
  addrIn.sin_addr.s_addr = INADDR_ANY

  if bindSocket(fd, cast[ptr SockAddr](addr addrIn), SockLen(sizeof(addrIn))) != 0:
    closeSocket(fd)
    return SocketHandle(-1)

  if listen(fd, 4096) != 0:
    closeSocket(fd)
    return SocketHandle(-1)

  fd

proc handleClient(clientFd: SocketHandle; workers: seq[string]; rrNext: var int) =
  defer: closeSocket(clientFd)

  if workers.len == 0:
    return

  var request = ""
  if not readFullHttpMessage(clientFd, request):
    return

  let start = rrNext mod workers.len
  rrNext = (rrNext + 1) mod workers.len

  for attempt in 0 ..< workers.len:
    let idx = (start + attempt) mod workers.len
    let backendFd = connectUnixBackend(workers[idx])
    if cint(backendFd) < 0:
      continue

    var ok = sendAll(backendFd, request)

    if ok:
      var response = ""
      ok = readFullHttpMessage(backendFd, response)
      if ok:
        ok = sendAll(clientFd, response)

    closeSocket(backendFd)

    if ok:
      return

proc main() =
  discard sigignore(SIGPIPE)

  let workers = parseWorkers()
  if workers.len == 0:
    stderr.writeLine("load-balancer7: WORKER_SOCKETS is required and cannot be empty")
    quit(1)

  let port = parsePort()
  let serverFd = createTcpServer(port)
  if cint(serverFd) < 0:
    stderr.writeLine("load-balancer7: failed to listen on port " & $port)
    quit(1)

  defer: closeSocket(serverFd)

  stderr.writeLine("load-balancer7 (nim) listening on :" & $port & " with " & $workers.len & " workers")

  var rrNext = 0
  while true:
    let clientFd = accept(serverFd, nil, nil)
    if cint(clientFd) < 0:
      continue

    handleClient(clientFd, workers, rrNext)

when isMainModule:
  main()
