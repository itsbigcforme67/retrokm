// Line-based TCP link to the hub's panel port.  Plain BSD sockets, which both
// lwIP (on the Tab5) and Linux (the desktop build) provide.
#pragma once
#include <string>
#include <functional>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#if defined(PANEL_NATIVE)
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <errno.h>
#else
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <errno.h>
#endif

uint32_t nowMs();

class HubLink {
 public:
  std::function<void(const std::string&)> onLine;

  void begin(const char* host, int port) {
    host_ = host;
    port_ = port;
  }

  bool connected() const { return fd_ >= 0 && up_; }
  const std::string& host() const { return host_; }
  int port() const { return port_; }

  void send(const std::string& line) {
    if (!connected()) return;
    std::string s = line + "\n";
    if (::send(fd_, s.data(), s.size(), 0) < 0) drop();
  }

  // Call often.  netReady: the network itself is up (WiFi joined).
  void poll(bool netReady) {
    uint32_t now = nowMs();
    if (fd_ < 0) {
      if (!netReady || (tried_ && now - lastTry_ < 2000)) return;
      tried_ = true;
      lastTry_ = now;
      open();
      return;
    }
    if (!up_) {  // connect in progress
      fd_set w;
      FD_ZERO(&w);
      FD_SET(fd_, &w);
      timeval tv = {0, 0};
      if (select(fd_ + 1, nullptr, &w, nullptr, &tv) > 0) {
        int err = 0;
        socklen_t len = sizeof err;
        getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err) { drop(); return; }
        up_ = true;
        lastRx_ = lastPing_ = now;
        send("layout");
      } else if (now - lastTry_ > 4000) {
        drop();
      }
      return;
    }
    char buf[2048];
    for (;;) {
      int n = recv(fd_, buf, sizeof buf, 0);
      if (n > 0) {
        lastRx_ = now;
        in_.append(buf, n);
        size_t nl;
        while ((nl = in_.find('\n')) != std::string::npos) {
          std::string line = in_.substr(0, nl);
          in_.erase(0, nl + 1);
          if (onLine) onLine(line);
        }
        if (in_.size() > 256 * 1024) in_.clear();
        continue;
      }
      if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) drop();
      break;
    }
    if (fd_ >= 0 && now - lastPing_ > 4000) {  // the hub drops panels that go quiet
      lastPing_ = now;
      send("ping");
    }
    if (fd_ >= 0 && now - lastRx_ > 15000) drop();  // no pong: hub or WiFi gone
  }

 private:
  std::string host_, in_;
  int port_ = 0, fd_ = -1;
  bool up_ = false, tried_ = false;
  uint32_t lastTry_ = 0, lastRx_ = 0, lastPing_ = 0;

  void open() {
    addrinfo hints, *res = nullptr;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port[8];
    snprintf(port, sizeof port, "%d", port_);
    if (getaddrinfo(host_.c_str(), port, &hints, &res) != 0 || !res) return;
    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ >= 0) {
      int one = 1;
      setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
      fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL, 0) | O_NONBLOCK);
      if (connect(fd_, res->ai_addr, res->ai_addrlen) < 0 && errno != EINPROGRESS) drop();
    }
    freeaddrinfo(res);
  }

  void drop() {
    if (fd_ >= 0) close(fd_);
    fd_ = -1;
    up_ = false;
    in_.clear();
  }
};
