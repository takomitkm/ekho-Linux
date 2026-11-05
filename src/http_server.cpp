/***************************************************************************
 * ekho-linux -- minimal HTTP/1.1 server. See http_server.h for the rationale.
 * SPDX-License-Identifier: GPL-3.0-or-later
 **************************************************************************/
#include "http_server.h"

#include <errno.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace ekho {

namespace {

const size_t kMaxHeadBytes = 64 * 1024;
const size_t kMaxBodyBlock = 8192;

// send() with SIGPIPE suppression, so a client that vanishes mid-stream does
// not take the whole server down with it.
ssize_t sendNoSignal(int fd, const void* data, size_t len) {
#ifdef MSG_NOSIGNAL
  return ::send(fd, data, len, MSG_NOSIGNAL);
#else
  return ::send(fd, data, len, 0);
#endif
}

bool sendAllTo(int fd, const void* data, size_t len) {
  const char* p = static_cast<const char*>(data);
  size_t sent = 0;
  while (sent < len) {
    ssize_t n = sendNoSignal(fd, p + sent, len - sent);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (n == 0) return false;
    sent += static_cast<size_t>(n);
  }
  return true;
}

// Reads one line (terminated by LF) from a buffer that may already hold the
// start of the next line. Returns false on EOF or overlong input.
bool nextLine(const std::string& buf, size_t& pos, std::string& out) {
  out.clear();
  while (pos < buf.size()) {
    char c = buf[pos++];
    if (c == '\n') {
      if (!out.empty() && out[out.size() - 1] == '\r') out.erase(out.size() - 1);
      return true;
    }
    out.push_back(c);
    if (out.size() > kMaxHeadBytes) return false;
  }
  return false;
}

std::string lower(const std::string& in) {
  std::string out(in);
  for (size_t i = 0; i < out.size(); i++) {
    if (out[i] >= 'A' && out[i] <= 'Z') out[i] = (char)(out[i] - 'A' + 'a');
  }
  return out;
}

std::string trim(const std::string& in) {
  size_t b = 0, e = in.size();
  while (b < e && (in[b] == ' ' || in[b] == '\t')) b++;
  while (e > b && (in[e - 1] == ' ' || in[e - 1] == '\t')) e--;
  return in.substr(b, e - b);
}

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Number of live connections, so a flood cannot spawn unbounded threads.
volatile int gActiveConnections = 0;
const int kMaxConnections = 32;

}  // namespace

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

std::string httpUrlDecode(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); i++) {
    if (in[i] == '+') {
      out.push_back(' ');
    } else if (in[i] == '%' && i + 2 < in.size()) {
      int hi = hexVal(in[i + 1]);
      int lo = hexVal(in[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back((char)(hi * 16 + lo));
        i += 2;
      } else {
        out.push_back(in[i]);
      }
    } else {
      out.push_back(in[i]);
    }
  }
  return out;
}

std::string httpUrlEncode(const std::string& in) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  for (size_t i = 0; i < in.size(); i++) {
    unsigned char c = (unsigned char)in[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
        c == '~') {
      out.push_back((char)c);
    } else {
      out.push_back('%');
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 0x0F]);
    }
  }
  return out;
}

std::string httpJsonEscape(const std::string& in) {
  std::string out;
  for (size_t i = 0; i < in.size(); i++) {
    unsigned char c = (unsigned char)in[i];
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char tmp[8];
          snprintf(tmp, sizeof tmp, "\\u%04x", c);
          out += tmp;
        } else {
          out.push_back((char)c);
        }
    }
  }
  return out;
}

std::string HttpRequest::param(const std::string& name,
                              const std::string& fallback) const {
  size_t pos = 0;
  while (pos <= query.size()) {
    size_t amp = query.find('&', pos);
    std::string pair = query.substr(
        pos, amp == std::string::npos ? std::string::npos : amp - pos);
    size_t eq = pair.find('=');
    std::string key = httpUrlDecode(eq == std::string::npos ? pair
                                                           : pair.substr(0, eq));
    if (key == name) {
      return eq == std::string::npos
                 ? std::string()
                 : httpUrlDecode(pair.substr(eq + 1));
    }
    if (amp == std::string::npos) break;
    pos = amp + 1;
  }
  return fallback;
}

// ---------------------------------------------------------------------------
// HttpConnection
// ---------------------------------------------------------------------------

HttpConnection::HttpConnection(int fd)
    : fd_(fd), began_(false), chunked_(false), broken_(false) {
  pthread_mutex_init(&writeMutex_, nullptr);
}

HttpConnection::~HttpConnection() { pthread_mutex_destroy(&writeMutex_); }

void HttpConnection::addHeader(const std::string& name,
                               const std::string& value) {
  headers_.push_back(name + ": " + value + "\r\n");
}

bool HttpConnection::sendAll(const void* data, size_t len) {
  if (broken_) return false;
  if (!sendAllTo(fd_, data, len)) {
    broken_ = true;
    return false;
  }
  return true;
}

void HttpConnection::fail() { broken_ = true; }

bool HttpConnection::beginBody(int status, const char* reason, bool chunked) {
  pthread_mutex_lock(&writeMutex_);
  char line[128];
  snprintf(line, sizeof line, "HTTP/1.1 %d %s\r\n", status, reason);
  std::string head(line);
  for (size_t i = 0; i < headers_.size(); i++) head += headers_[i];
  if (chunked) {
    head += "Transfer-Encoding: chunked\r\n";
  }
  // Every response closes the connection: it keeps the framing unambiguous and
  // the streaming endpoint has no use for a second request on the same socket.
  head += "Connection: close\r\n\r\n";
  bool ok = sendAll(head.data(), head.size());
  began_ = true;
  chunked_ = chunked;
  pthread_mutex_unlock(&writeMutex_);
  return ok;
}

bool HttpConnection::beginLengthBody(int status, const char* reason,
                                     size_t length) {
  pthread_mutex_lock(&writeMutex_);
  char line[128];
  snprintf(line, sizeof line, "HTTP/1.1 %d %s\r\n", status, reason);
  std::string head(line);
  for (size_t i = 0; i < headers_.size(); i++) head += headers_[i];
  char cl[64];
  snprintf(cl, sizeof cl, "Content-Length: %zu\r\n", length);
  head += cl;
  head += "Connection: close\r\n\r\n";
  bool ok = sendAll(head.data(), head.size());
  began_ = true;
  chunked_ = false;
  pthread_mutex_unlock(&writeMutex_);
  return ok;
}

bool HttpConnection::writeRaw(const void* data, size_t len) {
  if (len == 0) return true;
  pthread_mutex_lock(&writeMutex_);
  bool ok = sendAll(data, len);
  pthread_mutex_unlock(&writeMutex_);
  return ok;
}

bool HttpConnection::writeChunk(const void* data, size_t len) {
  if (len == 0) return true;  // an empty chunk would terminate the body
  char head[32];
  snprintf(head, sizeof head, "%zx\r\n", len);
  pthread_mutex_lock(&writeMutex_);
  bool ok = sendAll(head, strlen(head)) && sendAll(data, len) &&
            sendAll("\r\n", 2);
  pthread_mutex_unlock(&writeMutex_);
  return ok;
}

bool HttpConnection::endChunkedBody() {
  pthread_mutex_lock(&writeMutex_);
  bool ok = sendAll("0\r\n\r\n", 5);
  pthread_mutex_unlock(&writeMutex_);
  return ok;
}

// ---------------------------------------------------------------------------
// HttpBodyReader
// ---------------------------------------------------------------------------

HttpBodyReader::HttpBodyReader(int fd, bool chunked, long long contentLength,
                               const char* preloaded, size_t preloadedLen)
    : fd_(fd),
      chunked_(chunked),
      remaining_(contentLength),
      declared_(contentLength),
      done_(false),
      chunkRemaining_(0),
      preload_(preloaded != nullptr ? preloaded : "",
               preloaded != nullptr ? preloadedLen : 0),
      preloadPos_(0),
      pos_(0),
      len_(0),
      eof_(false) {
  if (!chunked_ && remaining_ <= 0 && preload_.empty()) done_ = true;
}

size_t HttpBodyReader::pull(char* out, size_t n) {
  size_t got = 0;
  // Bytes that arrived in the same segment as the request head come first.
  while (got < n && preloadPos_ < preload_.size()) {
    out[got++] = preload_[preloadPos_++];
  }
  while (got < n) {
    if (pos_ < len_) {
      size_t avail = std::min(n - got, len_ - pos_);
      memcpy(out + got, buf_ + pos_, avail);
      pos_ += avail;
      got += avail;
      continue;
    }
    if (eof_) break;
    ssize_t r = recv(fd_, buf_, sizeof buf_, 0);
    if (r < 0) {
      if (errno == EINTR) continue;
      eof_ = true;
      break;
    }
    if (r == 0) {
      eof_ = true;
      break;
    }
    pos_ = 0;
    len_ = static_cast<size_t>(r);
  }
  return got;
}

int HttpBodyReader::fill() {
  unsigned char c = 0;
  if (pull(reinterpret_cast<char*>(&c), 1) != 1) return -1;
  return static_cast<int>(c);
}

bool HttpBodyReader::readLine(std::string& out) {
  out.clear();
  for (;;) {
    int c = fill();
    if (c < 0) return false;
    if (c == '\n') {
      if (!out.empty() && out[out.size() - 1] == '\r') out.erase(out.size() - 1);
      return true;
    }
    out.push_back((char)c);
    if (out.size() > 1024) return false;
  }
}

ssize_t HttpBodyReader::read(char* out, size_t maxLen) {
  if (done_ || maxLen == 0) return 0;

  if (!chunked_) {
    if (remaining_ <= 0) {
      done_ = true;
      return 0;
    }
    size_t want = static_cast<size_t>(
        std::min<long long>(static_cast<long long>(maxLen), remaining_));
    size_t got = pull(out, want);
    if (got == 0) {
      // Truncated body: say so rather than pretending it ended cleanly.
      done_ = true;
      return eof_ ? -1 : 0;
    }
    remaining_ -= static_cast<long long>(got);
    if (remaining_ <= 0) done_ = true;
    return static_cast<ssize_t>(got);
  }

  // chunked
  if (chunkRemaining_ == 0) {
    std::string line;
    for (;;) {
      if (!readLine(line)) {
        // EOF where a chunk-size line is due. A well-formed body always ends
        // with the zero chunk, so this is a truncated stream, not a clean
        // end -- reporting 0 here made truncation indistinguishable from
        // completion.
        done_ = true;
        return -1;
      }
      // Strip any chunk extension (";name=value").
      size_t semi = line.find(';');
      if (semi != std::string::npos) line.erase(semi);
      line = trim(line);
      if (!line.empty()) break;
    }
    char* end = nullptr;
    unsigned long long size = strtoull(line.c_str(), &end, 16);
    if (end == line.c_str()) {
      done_ = true;
      return -1;
    }
    if (size == 0) {
      // Trailer section, terminated by an empty line.
      for (int i = 0; i < 64; i++) {
        std::string trailer;
        if (!readLine(trailer) || trailer.empty()) break;
      }
      done_ = true;
      return 0;
    }
    chunkRemaining_ = static_cast<size_t>(size);
  }

  size_t want = std::min(maxLen, chunkRemaining_);
  size_t got = pull(out, want);
  if (got == 0) {
    done_ = true;
    return -1;
  }
  chunkRemaining_ -= got;
  if (chunkRemaining_ == 0) {
    // Consume the CRLF that terminates this chunk.
    std::string line;
    if (!readLine(line)) {
      done_ = true;
      return -1;
    }
  }
  return static_cast<ssize_t>(got);
}

// ---------------------------------------------------------------------------
// HttpServer
// ---------------------------------------------------------------------------

HttpServer::HttpServer(const HttpServerOptions& options, HttpHandler handler,
                       void* user)
    : options_(options), handler_(handler), user_(user), listenFd_(-1),
      stopping_(0) {}

HttpServer::~HttpServer() {
  if (listenFd_ >= 0) close(listenFd_);
}

bool HttpServer::listen() {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    perror("ekho-server: socket");
    return false;
  }
  int yes = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(options_.port));
  addr.sin_addr.s_addr =
      options_.bindAddr == "0.0.0.0" ? INADDR_ANY : inet_addr(options_.bindAddr.c_str());

  if (::bind(fd, (struct sockaddr*)&addr, sizeof addr) < 0) {
    fprintf(stderr, "ekho-server: bind %s:%d: %s\n", options_.bindAddr.c_str(),
            options_.port, strerror(errno));
    close(fd);
    return false;
  }
  if (::listen(fd, options_.backlog) < 0) {
    perror("ekho-server: listen");
    close(fd);
    return false;
  }
  listenFd_ = fd;
  return true;
}

void HttpServer::stop() {
  stopping_ = 1;
  // shutdown() on the listening socket makes a blocked accept() return, so the
  // caller can stop the server from a signal handler. It is async-signal-safe,
  // unlike closing the descriptor or taking a lock.
  if (listenFd_ >= 0) ::shutdown(listenFd_, SHUT_RDWR);
}

int HttpServer::serve() {
  while (!stopping_) {
    struct sockaddr_in peer;
    socklen_t peerLen = sizeof peer;
    int fd = accept(listenFd_, (struct sockaddr*)&peer, &peerLen);
    if (fd < 0) {
      if (errno == EINTR) continue;
      if (stopping_) break;
      perror("ekho-server: accept");
      continue;
    }
    if (gActiveConnections >= kMaxConnections) {
      const char* busy =
          "HTTP/1.1 503 Service Unavailable\r\n"
          "Content-Length: 0\r\nConnection: close\r\n\r\n";
      sendAllTo(fd, busy, strlen(busy));
      close(fd);
      continue;
    }

    char ip[64];
    snprintf(ip, sizeof ip, "%s", inet_ntoa(peer.sin_addr));
    char peerStr[96];
    snprintf(peerStr, sizeof peerStr, "%s:%d", ip, ntohs(peer.sin_port));

    // A stalled client must not pin a thread forever, and a slow reader must
    // not block the writer indefinitely either.
    struct timeval tv;
    tv.tv_sec = 30;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    __sync_add_and_fetch(&gActiveConnections, 1);

    ConnArg* arg = new ConnArg;
    arg->server = this;
    arg->fd = fd;
    arg->peer = peerStr;

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&tid, &attr, &HttpServer::connectionEntry, arg) != 0) {
      perror("ekho-server: pthread_create");
      __sync_sub_and_fetch(&gActiveConnections, 1);
      close(fd);
      delete arg;
    }
    pthread_attr_destroy(&attr);
  }
  return 0;
}

void* HttpServer::connectionEntry(void* arg) {
  ConnArg* a = static_cast<ConnArg*>(arg);
  a->server->handleConnection(a->fd, a->peer);
  close(a->fd);
  __sync_sub_and_fetch(&gActiveConnections, 1);
  delete a;
  return nullptr;
}

void HttpServer::handleConnection(int fd, const std::string& peer) {
  // Read the request head. Anything past the blank line belongs to the body and
  // is handed to the body reader, so a client that pipelines the head and the
  // first body byte in one write does not lose that byte.
  std::string head;
  char buf[4096];
  size_t headEnd = std::string::npos;
  while (head.size() < kMaxHeadBytes) {
    ssize_t n = recv(fd, buf, sizeof buf, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      return;
    }
    if (n == 0) break;
    head.append(buf, static_cast<size_t>(n));
    headEnd = head.find("\r\n\r\n");
    if (headEnd != std::string::npos) break;
  }
  if (headEnd == std::string::npos) {
    const char* bad = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    sendAllTo(fd, bad, strlen(bad));
    return;
  }

  // Keep the CRLF that terminates the last header line. Cutting at headEnd
  // leaves the final header with no line terminator, and nextLine() reports a
  // line only when it sees one -- so the last header was silently dropped.
  // That is invisible with curl's usual header order (it costs you only
  // Content-Length/Transfer-Encoding or Expect) until a client relies on it:
  // dropping Expect means no 100-continue is sent and an uploading client
  // gives up, and dropping Transfer-Encoding turns a perfectly framed chunked
  // request into a 411.
  std::string headPart = head.substr(0, headEnd + 2);
  std::string rest = head.substr(headEnd + 4);

  HttpRequest req;
  req.peer = peer;

  size_t pos = 0;
  std::string line;
  if (!nextLine(headPart, pos, line)) return;

  // "METHOD SP TARGET SP VERSION"
  size_t sp1 = line.find(' ');
  size_t sp2 = sp1 == std::string::npos ? std::string::npos
                                        : line.find(' ', sp1 + 1);
  if (sp1 == std::string::npos || sp2 == std::string::npos) {
    const char* bad = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    sendAllTo(fd, bad, strlen(bad));
    return;
  }
  req.method = line.substr(0, sp1);
  req.target = line.substr(sp1 + 1, sp2 - sp1 - 1);
  req.version = line.substr(sp2 + 1);

  size_t q = req.target.find('?');
  if (q == std::string::npos) {
    req.path = req.target;
  } else {
    req.path = req.target.substr(0, q);
    req.query = req.target.substr(q + 1);
  }

  while (nextLine(headPart, pos, line)) {
    if (line.empty()) break;
    size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    req.headers[lower(line.substr(0, colon))] = trim(line.substr(colon + 1));
  }

  HttpConnection conn(fd);

  std::string te = lower(req.header("transfer-encoding"));
  bool chunkedBody = te.find("chunked") != std::string::npos;
  long long contentLength = 0;
  std::string cl = req.header("content-length");
  // An explicit "Content-Length: 0" is framing -- it says "the body is empty" --
  // and must not be confused with no framing at all. Only a request carrying
  // neither header is unframed. (handleTts answers an empty body with 400; this
  // check is about framing, not about content.)
  bool hasContentLength = !cl.empty();
  if (hasContentLength) contentLength = atoll(cl.c_str());

  if (!chunkedBody && !hasContentLength &&
      (req.method == "POST" || req.method == "PUT")) {
    // No body framing at all: refuse rather than read until EOF, which would
    // make a streaming request hang until the client closed its side.
    std::string bodyText;
    std::string msg = "{\"error\":\"POST requires Content-Length or "
                      "Transfer-Encoding: chunked\"}\n";
    conn.addHeader("Content-Type", "application/json");
    conn.beginLengthBody(411, "Length Required", msg.size());
    conn.writeRaw(msg.data(), msg.size());
    return;
  }

  // curl and friends send Expect: 100-continue for bodies; without this they
  // stall for a second before sending anything.
  std::string expect = lower(req.header("expect"));
  if (expect.find("100-continue") != std::string::npos) {
    const char* cont = "HTTP/1.1 100 Continue\r\n\r\n";
    if (!sendAllTo(fd, cont, strlen(cont))) return;
  }

  // Body bytes that arrived in the same read as the head have to reach the
  // reader, or a client that pipelines them would lose their first bytes.
  HttpBodyReader body(fd, chunkedBody, contentLength, rest.data(),
                      rest.size());

  if (!handler_(conn, req, body, user_)) {
    std::string msg = "{\"error\":\"not found\",\"path\":\"" +
                      httpJsonEscape(req.path) + "\"}\n";
    conn.addHeader("Content-Type", "application/json");
    conn.beginLengthBody(404, "Not Found", msg.size());
    conn.writeRaw(msg.data(), msg.size());
  }
}

}  // namespace ekho
