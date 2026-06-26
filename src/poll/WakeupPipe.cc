#include "WakeupPipe.h"

#include <cstring>

#include "a2io.h"
#include "LogFactory.h"
#include "Logger.h"
#include "fmt.h"

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <unistd.h>
#  include <fcntl.h>
#endif

namespace aria2 {

WakeupPipe::WakeupPipe()
{
  fds_[0] = fds_[1] = (sock_t)-1;

#ifdef _WIN32
  // Windows 没有 pipe()，用 TCP loopback socketpair 模拟：
  // 创建临时监听 socket → connect → accept → 关闭监听 socket
  sock_t listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == INVALID_SOCKET) {
    A2_LOG_ERROR("WakeupPipe: failed to create listener socket");
    return;
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;

  if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::closesocket(listener);
    A2_LOG_ERROR("WakeupPipe: bind failed");
    return;
  }
  if (::listen(listener, 1) != 0) {
    ::closesocket(listener);
    A2_LOG_ERROR("WakeupPipe: listen failed");
    return;
  }

  socklen_t len = sizeof(addr);
  ::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &len);

  fds_[1] = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fds_[1] == INVALID_SOCKET) {
    ::closesocket(listener);
    A2_LOG_ERROR("WakeupPipe: failed to create write socket");
    return;
  }
  if (::connect(fds_[1], reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) !=
      0) {
    ::closesocket(fds_[1]);
    fds_[1] = (sock_t)-1;
    ::closesocket(listener);
    A2_LOG_ERROR("WakeupPipe: connect failed");
    return;
  }
  fds_[0] = ::accept(listener, nullptr, nullptr);
  ::closesocket(listener);
  if (fds_[0] == INVALID_SOCKET) {
    ::closesocket(fds_[1]);
    fds_[1] = (sock_t)-1;
    A2_LOG_ERROR("WakeupPipe: accept failed");
    return;
  }

  // 两端都设为非阻塞，避免 signal()/drain() 阻塞调用线程
  u_long flag = 1;
  ::ioctlsocket(fds_[0], FIONBIO, &flag);
  ::ioctlsocket(fds_[1], FIONBIO, &flag);
#else
  // POSIX: 直接用 pipe() 创建管道
  if (::pipe(fds_) != 0) {
    A2_LOG_ERROR("WakeupPipe: pipe() failed");
    fds_[0] = fds_[1] = -1;
    return;
  }
  ::fcntl(fds_[0], F_SETFL, ::fcntl(fds_[0], F_GETFL) | O_NONBLOCK);
  ::fcntl(fds_[1], F_SETFL, ::fcntl(fds_[1], F_GETFL) | O_NONBLOCK);
#endif
}

WakeupPipe::~WakeupPipe()
{
#ifdef _WIN32
  if (fds_[0] != (sock_t)-1) {
    ::closesocket(fds_[0]);
  }
  if (fds_[1] != (sock_t)-1) {
    ::closesocket(fds_[1]);
  }
#else
  if (fds_[0] != -1) {
    ::close(fds_[0]);
  }
  if (fds_[1] != -1) {
    ::close(fds_[1]);
  }
#endif
}

void WakeupPipe::signal()
{
  // 构造失败时 fds_[1] 为无效值，跳过避免向无效 fd 写入
  if (fds_[1] == (sock_t)-1) {
    return;
  }
  char c = 'w';
#ifdef _WIN32
  ::send(fds_[1], &c, 1, 0);
#else
  ::write(fds_[1], &c, 1);
#endif
}

void WakeupPipe::drain()
{
  if (fds_[0] == (sock_t)-1) {
    return;
  }
  char buf[64];
#ifdef _WIN32
  while (::recv(fds_[0], buf, sizeof(buf), 0) > 0) {
  }
#else
  while (::read(fds_[0], buf, sizeof(buf)) > 0) {
  }
#endif
}

} // namespace aria2
