/***************************************************************************
 * ekho-linux -- unit tests: the HTTP layer.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Covers src/http_server.cpp: the hand-rolled HTTP/1.1 server the streaming
 * endpoint runs on, plus the free helpers (url codec, JSON escaping, query
 * parsing). The server cases run against a live HttpServer on loopback with a
 * test handler -- no engine, no voice data -- which is the tier upstream never
 * had: its only automated test was a demo that went green without data.
 *
 * The cases below encode the properties the header file promises: framing is
 * never guessed, a pipelined body byte is never lost, malformed input gets an
 * HTTP error rather than a hang, and a flood cannot spawn unbounded threads.
 **************************************************************************/
#include "check.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "http_server.h"

using ekho::HttpBodyReader;
using ekho::HttpConnection;
using ekho::HttpRequest;
using ekho::HttpServer;
using ekho::HttpServerOptions;
using ekho::httpJsonEscape;
using ekho::httpUrlDecode;
using ekho::httpUrlEncode;
using namespace ektest;

namespace {

const int kClientTimeoutSec = 5;

void msleep(int ms) {
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (ms % 1000) * 1000000L;
  nanosleep(&ts, nullptr);
}

int connectTo(int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  if (connect(fd, (struct sockaddr*)&addr, sizeof addr) < 0) {
    close(fd);
    return -1;
  }
  struct timeval tv;
  tv.tv_sec = kClientTimeoutSec;
  tv.tv_usec = 0;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  return fd;
}

bool sendAll(int fd, const char* data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    ssize_t n = ::send(fd, data + sent, len - sent, 0);
    if (n <= 0) return false;
    sent += static_cast<size_t>(n);
  }
  return true;
}

// One socketpair per body-reader case: shutdown(SHUT_WR) is one-way for the
// life of the descriptor, and reusing a half-closed pair is how the first
// version of this test died of SIGPIPE instead of reporting anything.
struct Pair {
  int a;  // writer side
  int b;  // reader side
  Pair() {
    int sv[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
      a = b = -1;
      return;
    }
    a = sv[0];
    b = sv[1];
  }
  ~Pair() {
    if (a >= 0) close(a);
    if (b >= 0) close(b);
  }
  Pair(const Pair&) = delete;
  Pair& operator=(const Pair&) = delete;
};

struct Response {
  bool ok = false;         // got a parseable final response before EOF
  bool interim100 = false; // saw a 100 Continue interim response
  int status = 0;
  std::string reason;
  std::string headers;     // raw header block of the final response
  std::string body;
  std::string header(const std::string& name) const {
    std::string lower;
    for (char c : name) lower += (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
    size_t pos = 0;
    while (pos < headers.size()) {
      size_t eol = headers.find('\n', pos);
      if (eol == std::string::npos) eol = headers.size();
      std::string line = headers.substr(pos, eol - pos);
      pos = eol + 1;
      size_t colon = line.find(':');
      if (colon == std::string::npos) continue;
      std::string key;
      for (char c : line.substr(0, colon)) key += (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
      if (key != lower) continue;
      size_t b = colon + 1;
      while (b < line.size() && (line[b] == ' ' || line[b] == '\t')) b++;
      size_t e = line.size();
      while (e > b && (line[e - 1] == ' ' || line[e - 1] == '\t' || line[e - 1] == '\r')) e--;
      return line.substr(b, e - b);
    }
    return std::string();
  }
};

// Sends `request` verbatim, reads to EOF, and splits what came back. Handles
// (and records) 1xx interim responses.
Response roundtrip(int port, const std::string& request) {
  Response r;
  int fd = connectTo(port);
  if (fd < 0) return r;
  if (!sendAll(fd, request.data(), request.size())) {
    close(fd);
    return r;
  }
  shutdown(fd, SHUT_WR);  // the server may wait for EOF on unframed bodies

  std::string all;
  char buf[8192];
  for (;;) {
    ssize_t n = recv(fd, buf, sizeof buf, 0);
    if (n < 0) break;  // timeout or error: return what we have
    if (n == 0) break;
    all.append(buf, static_cast<size_t>(n));
    if (all.size() > 8 * 1024 * 1024) break;
  }
  close(fd);

  // Parse (skipping 1xx interim responses).
  size_t pos = 0;
  while (pos < all.size()) {
    size_t headEnd = all.find("\r\n\r\n", pos);
    if (headEnd == std::string::npos) return r;
    std::string head = all.substr(pos, headEnd - pos);
    std::string bodyPart = all.substr(headEnd + 4);
    size_t sp1 = head.find(' ');
    size_t sp2 = head.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) return r;
    int code = atoi(head.substr(sp1 + 1, sp2 - sp1 - 1).c_str());
    if (code >= 100 && code < 200) {
      r.interim100 = true;
      pos = headEnd + 4;
      continue;
    }
    r.ok = true;
    r.status = code;
    r.reason = head.substr(sp2 + 1);
    r.headers = head.substr(head.find("\r\n") + 2);
    // Content-Length framed: trust it; otherwise what arrived is the body
    // (the server always closes, so EOF framed it).
    std::string cl;
    size_t hpos = 0;
    while (hpos < r.headers.size()) {
      size_t eol = r.headers.find('\n', hpos);
      if (eol == std::string::npos) eol = r.headers.size();
      std::string line = r.headers.substr(hpos, eol - hpos);
      hpos = eol + 1;
      for (char& c : line) c = (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
      if (line.compare(0, 16, "content-length: ") == 0)
        cl = line.substr(16);
    }
    if (!cl.empty()) {
      size_t want = static_cast<size_t>(atoll(cl.c_str()));
      if (bodyPart.size() > want) bodyPart.resize(want);
    }
    r.body = bodyPart;
    break;
  }
  return r;
}

// De-chunks a chunked response body. Returns false if the framing is broken.
bool dechunk(const std::string& in, std::string* out, bool* cleanEnd) {
  size_t pos = 0;
  *cleanEnd = false;
  while (pos < in.size()) {
    size_t eol = in.find("\r\n", pos);
    if (eol == std::string::npos) return false;
    unsigned long len = strtoul(in.substr(pos, eol - pos).c_str(), nullptr, 16);
    pos = eol + 2;
    if (len == 0) {
      *cleanEnd = pos + 2 <= in.size();  // expect the final CRLF
      return true;
    }
    if (pos + len > in.size()) return false;
    out->append(in, pos, len);
    pos += len + 2;  // data + CRLF
  }
  return false;  // ran out before the zero chunk
}

// ---------------------------------------------------------------------------
// test handler
// ---------------------------------------------------------------------------

bool echoHandler(HttpConnection& conn, const HttpRequest& req,
                 HttpBodyReader& body, void* user) {
  (void)user;
  if (req.path == "/echo") {
    std::string got;
    char buf[4096];
    ssize_t n;
    bool truncated = false;
    while ((n = body.read(buf, sizeof buf)) > 0) got.append(buf, n);
    if (n < 0) truncated = true;

    conn.addHeader("X-Method", req.method);
    conn.addHeader("X-Path", req.path);
    conn.addHeader("X-Query", req.query);
    conn.addHeader("X-Version", req.version);
    conn.addHeader("X-Param-Wait", req.param("wait", "MISSING"));
    conn.addHeader("X-Header-Case", req.header("x-case"));
    conn.addHeader("X-Header-Dup", req.header("x-dup"));
    if (truncated) {
      conn.addHeader("X-Truncated", "1");
      conn.beginLengthBody(400, "Truncated body", 0);
      return true;
    }
    conn.addHeader("X-Echo-Len", std::to_string(got.size()));
    conn.beginLengthBody(200, "OK", got.size());
    if (!got.empty()) conn.writeRaw(got.data(), got.size());
    return true;
  }

  if (req.path == "/chunks") {
    const char* a = "part-one-";
    const char* b = "part-two";
    conn.addHeader("X-Chunked", "1");
    conn.beginBody(200, "OK", true);
    conn.writeChunk(a, strlen(a));
    conn.writeChunk(a, 0);  // must be a no-op, not a terminator
    conn.writeChunk(b, strlen(b));
    conn.endChunkedBody();
    return true;
  }
  return false;
}

}  // namespace

int main() {
  signal(SIGPIPE, SIG_IGN);

  // =========================================================================
  EK_SECTION("url codec: encode/decode round-trip over every byte");
  // Failure mode: a codec that mangles one byte breaks every percent-encoded
  // query parameter; sweeping all 256 values makes any single-byte slip a
  // named failure instead of a mysterious 400.
  {
    std::string all;
    for (int b = 0; b < 256; b++) all += static_cast<char>(b);
    std::string enc = httpUrlEncode(all);
    EK_STREQ(httpUrlDecode(enc), all);
    // '+' is a space in query context and must therefore never survive
    // unencoded through the encoder.
    EK_CHECK(enc.find('+') == std::string::npos);
    // Documented decode edges: invalid escapes pass through untouched.
    EK_STREQ(httpUrlDecode("a%20b"), "a b");
    EK_STREQ(httpUrlDecode("a+b"), "a b");
    EK_STREQ(httpUrlDecode("%zz"), "%zz");
    EK_STREQ(httpUrlDecode("a%2"), "a%2");
    EK_STREQ(httpUrlDecode("abc%"), "abc%");
    EK_STREQ(httpUrlEncode("a b~c/"), "a%20b~c%2F");
  }

  // =========================================================================
  EK_SECTION("json escape: quotes, backslash, control bytes, UTF-8 passthrough");
  {
    EK_STREQ(httpJsonEscape("a\"b\\c\nd\re\tf"), "a\\\"b\\\\c\\nd\\re\\tf");
    std::string ctrl("x");
    ctrl += char(1);
    ctrl += "y";
    EK_STREQ(httpJsonEscape(ctrl), "x\\u0001y");
    // 0x7F and every non-ASCII byte must pass through unchanged: the JSON
    // carries UTF-8 text, not \u escapes for it.
    std::string utf8 = "\xE4\xBD\xA0\x7F";
    EK_STREQ(httpJsonEscape(utf8), utf8);
  }

  // =========================================================================
  EK_SECTION("query param: parsing rules");
  {
    HttpRequest req;
    req.query = "wait=1&voice=%E6%99%AE&empt=&noval";
    EK_STREQ(req.param("wait"), "1");
    EK_STREQ(req.param("voice"), "\xE6\x99\xAE");  // percent-decoded
    EK_STREQ(req.param("empt", "FB"), "");    // present but empty beats fallback
    EK_STREQ(req.param("noval", "FB"), "");   // key with no '=': current rule
    EK_STREQ(req.param("missing", "FB"), "FB");
    // First occurrence wins.
    HttpRequest dup;
    dup.query = "wait=1&wait=2";
    EK_STREQ(dup.param("wait"), "1");
  }

  // =========================================================================
  EK_SECTION("body reader: Content-Length framing (socketpair)");
  // Failure mode: the reply must never be length-guessed -- that is the bug
  // in the old protocol this server replaces ("the last read was short").
  {
    {
      Pair p;
      sendAll(p.a, "hello", 5);
      shutdown(p.a, SHUT_WR);
      HttpBodyReader b(p.b, false, 5);
      char out[16] = {0};
      EK_EQ(b.read(out, sizeof out), 5);
      EK_CHECK(memcmp(out, "hello", 5) == 0);
      EK_EQ(b.read(out, sizeof out), 0);  // clean end
      EK_EQ(b.declaredLength(), 5);
    }

    // Sliced reads still reassemble byte for byte.
    {
      Pair p;
      sendAll(p.a, "0123456789", 10);
      shutdown(p.a, SHUT_WR);
      HttpBodyReader b(p.b, false, 10);
      char out[4];
      ssize_t n;
      std::string got;
      while ((n = b.read(out, sizeof out)) > 0) got.append(out, n);
      EK_STREQ(got, "0123456789");
      EK_EQ(n, 0);
    }

    // Truncated: declared 10, five arrived, EOF. Must say -1, not 0.
    {
      Pair p;
      sendAll(p.a, "hello", 5);
      shutdown(p.a, SHUT_WR);
      HttpBodyReader b(p.b, false, 10);
      char out[16];
      EK_EQ(b.read(out, 16), 5);
      EK_EQ(b.read(out, 16), -1);
    }

    // Content-Length: 0 is an empty body, not an error.
    {
      Pair p;
      shutdown(p.a, SHUT_WR);
      HttpBodyReader b(p.b, false, 0);
      char out[4];
      EK_EQ(b.read(out, 4), 0);
    }

    // Bytes pipelined into the request head are served before the socket.
    {
      Pair p;
      shutdown(p.a, SHUT_WR);
      HttpBodyReader b(p.b, false, 2, "XY", 2);
      char out[4] = {0};
      EK_EQ(b.read(out, sizeof out), 2);
      EK_CHECK(memcmp(out, "XY", 2) == 0);
      EK_EQ(b.read(out, sizeof out), 0);
    }
  }

  // =========================================================================
  EK_SECTION("body reader: chunked framing (socketpair)");
  // Failure mode: an LLM feeds tokens with chunked framing; a decoder that
  // hangs on a malformed chunk pins a server thread for the 30 s receive
  // timeout per request.
  {
    struct ChunkCase {
      const char* wire;
      const char* want;  // "" = a -1 is expected somewhere
      bool wantFail;
    };
    const ChunkCase cases[] = {
        {"5\r\nhello\r\n0\r\n\r\n", "hello", false},
        {"5;name=v\r\nhello\r\n0\r\n\r\n", "hello", false},        // extension
        {"5\r\nhello\r\n0\r\nX-Trailer: y\r\n\r\n", "hello", false},  // trailer
        {"A\r\n0123456789\r\n0\r\n\r\n", "0123456789", false},
        {"zz\r\nhello\r\n0\r\n\r\n", "", true},                 // not hex
        {"5\r\nhel", "", true},                                     // data truncated
    };
    for (const ChunkCase& c : cases) {
      Pair p;
      sendAll(p.a, c.wire, strlen(c.wire));
      shutdown(p.a, SHUT_WR);
      HttpBodyReader b(p.b, true, 0);
      char out[16];
      ssize_t n;
      std::string got;
      bool failed = false;
      while ((n = b.read(out, sizeof out)) > 0) got.append(out, n);
      if (n < 0) failed = true;
      if (c.wantFail) {
        check(failed, __FILE__, __LINE__,
              std::string("expected failure for wire \"") + c.wire + "\"");
      } else {
        check(got == c.want, __FILE__, __LINE__,
              std::string("wire \"") + c.wire + "\" decoded to \"" + got +
                  "\"");
      }
    }

    // EOF between a chunk's data and its terminating CRLF: documented
    // "malformed", reported as -1 on the same call. The bytes of that chunk do
    // not reach the caller -- the error flag is what tells it not to trust the
    // stream, so pin that the error is there.
    {
      Pair p;
      sendAll(p.a, "5\r\nhello", 8);
      shutdown(p.a, SHUT_WR);
      HttpBodyReader b(p.b, true, 0);
      char out[16];
      EK_EQ(b.read(out, sizeof out), -1);
    }

    // A chunked body that ends without its terminating zero chunk. The body
    // half is complete and every data chunk was well framed, so this reads as
    // a clean end (0) -- a truncated stream is indistinguishable from a
    // finished one. Found 2026-11-05; unfixed.
    {
      Pair p;
      sendAll(p.a, "5\r\nhello\r\n", 11);
      shutdown(p.a, SHUT_WR);
      HttpBodyReader b(p.b, true, 0);
      char out[16];
      EK_EQ(b.read(out, sizeof out), 5);
      EK_KNOWN(b.read(out, sizeof out) == -1,
               "a chunked body whose final zero chunk never arrived reads as "
               "a clean end (0), not a malformed body (-1)");
    }
  }

  // =========================================================================
  EK_SECTION("live server: request line, headers, query");
  // Failure mode: path/query/headers are what every endpoint dispatches on;
  // a parsing slip sends the request to the wrong handler silently.
  int port = 0;
  HttpServerOptions opt;
  opt.bindAddr = "127.0.0.1";
  HttpServer* srv = nullptr;
  std::thread serveThread;
  for (int p = 21406; p < 21426; p++) {
    opt.port = p;
    srv = new HttpServer(opt, echoHandler, nullptr);
    if (srv->listen()) {
      port = p;
      break;
    }
    delete srv;
    srv = nullptr;
  }
  if (srv == nullptr) return skip("test_http", "no bindable loopback port");
  serveThread = std::thread([&srv] { srv->serve(); });

  {
    Response r = roundtrip(port,
        "GET /echo?wait=1&v=%E6%99%AE HTTP/1.1\r\n"
        "Host: t\r\n"
        "X-CaSe:   MiXeD \r\n"
        "x-dup: first\r\n"
        "X-DUP: second\r\n"
        "\r\n");
    EK_CHECK(r.ok);
    EK_EQ(r.status, 200);
    EK_STREQ(r.header("x-method"), "GET");
    EK_STREQ(r.header("x-path"), "/echo");
    EK_STREQ(r.header("x-query"), "wait=1&v=%E6%99%AE");
    EK_STREQ(r.header("x-version"), "HTTP/1.1");
    EK_STREQ(r.header("x-param-wait"), "1");
    EK_STREQ(r.header("x-header-case"), "MiXeD");   // name lowercased, value trimmed
    EK_STREQ(r.header("x-header-dup"), "second");   // last value wins
    EK_STREQ(r.header("connection"), "close");
  }

  // -------------------------------------------------------------------------
  EK_SECTION("live server: last header is not dropped (regression)");
  // The head parser once cut the final header line because it kept no line
  // terminator after the last header -- invisible with curl's usual order,
  // fatal when the last header is Transfer-Encoding (a well-framed chunked
  // request turned into a 411). The fix is documented in the source; this
  // case is what keeps it fixed.
  {
    Response r = roundtrip(port,
        "POST /echo HTTP/1.1\r\n"
        "Host: t\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5\r\nhello\r\n0\r\n\r\n");
    EK_CHECK(r.ok);
    EK_EQ(r.status, 200);
    EK_STREQ(r.header("x-echo-len"), "5");
    EK_STREQ(r.body, "hello");
  }

  // -------------------------------------------------------------------------
  EK_SECTION("live server: body bytes pipelined into the head are not lost");
  // Failure mode: a client that sends head and first body byte in one write
  // used to lose that byte to the head reader.
  {
    Response r = roundtrip(port,
        "POST /echo HTTP/1.1\r\n"
        "Host: t\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello");
    EK_CHECK(r.ok);
    EK_EQ(r.status, 200);
    EK_STREQ(r.header("x-echo-len"), "5");
    EK_STREQ(r.body, "hello");
  }

  // -------------------------------------------------------------------------
  EK_SECTION("live server: 100-continue is answered");
  // Failure mode: without the interim response an uploading client stalls
  // for its own timeout before sending the body.
  {
    Response r = roundtrip(port,
        "POST /echo HTTP/1.1\r\n"
        "Host: t\r\n"
        "Expect: 100-continue\r\n"
        "Content-Length: 2\r\n"
        "\r\n"
        "hi");
    EK_CHECK(r.ok);
    EK_CHECK(r.interim100);
    EK_EQ(r.status, 200);
    EK_STREQ(r.body, "hi");
  }

  // -------------------------------------------------------------------------
  EK_SECTION("live server: framed-but-empty body reaches the handler");
  // Content-Length: 0 is framing ("the body is empty"), not the absence of
  // framing -- the handler must see a clean 0-byte end and answer, not 411.
  {
    Response r = roundtrip(port,
        "POST /echo HTTP/1.1\r\nHost: t\r\nContent-Length: 0\r\n\r\n");
    EK_CHECK(r.ok);
    EK_EQ(r.status, 200);
    EK_STREQ(r.header("x-echo-len"), "0");
  }

  // -------------------------------------------------------------------------
  EK_SECTION("live server: unframed POST is refused with 411");
  // Failure mode: reading until EOF would make a streaming client hang until
  // it closed its side.
  {
    Response r = roundtrip(port, "POST /echo HTTP/1.1\r\nHost: t\r\n\r\n");
    EK_CHECK(r.ok);
    EK_EQ(r.status, 411);
  }

  // -------------------------------------------------------------------------
  EK_SECTION("live server: oversized head gets 400, not a hang");
  // Failure mode: a client that never finishes the head pins a thread and a
  // buffer per connection.
  {
    std::string big(70 * 1024, 'x');
    Response r = roundtrip(port,
        "POST /echo HTTP/1.1\r\nHost: t\r\n" + big + "\r\n\r\n");
    EK_CHECK(r.ok);
    EK_EQ(r.status, 400);
  }

  // -------------------------------------------------------------------------
  EK_SECTION("live server: truncated body is reported to the handler");
  // The reader contract: a body that ends before Content-Length says so
  // (-1) rather than pretending a clean end.
  {
    int fd = connectTo(port);
    EK_CHECK(fd >= 0);
    std::string req =
        "POST /echo HTTP/1.1\r\nHost: t\r\nContent-Length: 100\r\n\r\nhello";
    sendAll(fd, req.data(), req.size());
    // Do NOT send the rest; just close.
    close(fd);
    // The server notices EOF, answers 400 (to a closed socket -- harmless),
    // and stays healthy; verify health below.
    Response h = roundtrip(port, "GET /echo HTTP/1.1\r\nHost: t\r\n\r\n");
    EK_CHECK(h.ok);
    EK_EQ(h.status, 200);
  }

  // -------------------------------------------------------------------------
  EK_SECTION("live server: chunked response framing");
  // Failure mode: writeChunk(0) must not terminate the body, and the zero
  // chunk must be present exactly once at the end.
  {
    Response r = roundtrip(port, "GET /chunks HTTP/1.1\r\nHost: t\r\n\r\n");
    EK_CHECK(r.ok);
    EK_EQ(r.status, 200);
    EK_STREQ(r.header("transfer-encoding"), "chunked");
    std::string body;
    bool clean = false;
    EK_CHECK(dechunk(r.body, &body, &clean));
    EK_STREQ(body, "part-one-part-two");
    EK_CHECK(clean);
  }

  // -------------------------------------------------------------------------
  EK_SECTION("live server: unknown path falls back to 404");
  {
    Response r = roundtrip(port, "GET /nope HTTP/1.1\r\nHost: t\r\n\r\n");
    EK_CHECK(r.ok);
    EK_EQ(r.status, 404);
    EK_CHECK(r.body.find("/nope") != std::string::npos);
  }

  // -------------------------------------------------------------------------
  EK_SECTION("live server: flood beyond 32 connections gets 503");
  // Failure mode: unbounded threads per connection is a remote OOM. The
  // guard lives in the accept loop; this walks right past the limit.
  // Kept last so its parked connections cannot perturb the cases above.
  {
    std::vector<int> socks;
    bool allConnected = true;
    for (int i = 0; i < 32 && allConnected; i++) {
      int fd = connectTo(port);
      if (fd < 0) {
        allConnected = false;
        break;
      }
      // Handler blocks in body.read until the socket closes: these stay
      // counted as active for the whole section.
      std::string req =
          "POST /echo HTTP/1.1\r\nHost: t\r\nContent-Length: 100\r\n\r\nabcdefghij";
      if (!sendAll(fd, req.data(), req.size())) {
        allConnected = false;
      }
      socks.push_back(fd);
      msleep(20);
    }
    if (allConnected) {
      msleep(300);  // let every handler enter its blocking read
      bool saw503 = false;
      for (int attempt = 0; attempt < 5 && !saw503; attempt++) {
        Response r = roundtrip(port, "GET /echo HTTP/1.1\r\nHost: t\r\n\r\n");
        if (r.ok && r.status == 503) saw503 = true;
        msleep(200);
      }
      EK_CHECK(saw503);
    } else {
      EK_CHECK_MSG(false, "could not open 32 connections for the flood test");
    }
    for (int fd : socks) close(fd);
    msleep(300);  // let the handlers unwind before the server stops
  }

  srv->stop();
  serveThread.join();
  delete srv;

  return summary("test_http");
}
