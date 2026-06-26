#ifndef D_WAKEUP_PIPE_H
#define D_WAKEUP_PIPE_H

#include "common.h"
#include "a2netcompat.h"

namespace aria2 {

// 跨线程唤醒管道，用于工作线程完成后唤醒主线程的 EventPoll。
// POSIX 使用 pipe()，Windows 使用 TCP loopback socketpair 模拟。
class WakeupPipe {
  sock_t fds_[2]; // [0]=读端(注册到 EventPoll), [1]=写端(工作线程写入)

public:
  WakeupPipe();
  ~WakeupPipe();

  WakeupPipe(const WakeupPipe&) = delete;
  WakeupPipe& operator=(const WakeupPipe&) = delete;

  // 构造是否成功（fd 是否有效）
  bool isValid() const { return fds_[0] != (sock_t)-1; }

  sock_t readFd() const { return fds_[0]; }

  // 线程安全：向写端写入 1 字节，唤醒阻塞在 poll()/select() 中的主线程
  void signal();

  // 排空读端所有数据，在 EventPoll::poll() 返回后调用
  void drain();
};

} // namespace aria2

#endif // D_WAKEUP_PIPE_H
