/* <!-- copyright */
/*
 * aria2 - The high speed download utility
 *
 * Copyright (C) 2009 Tatsuhiro Tsujikawa
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of portions of this program with the
 * OpenSSL library under certain conditions as described in each
 * individual source file, and distribute linked combinations
 * including the two.
 * You must obey the GNU General Public License in all respects
 * for all of the code used other than OpenSSL.  If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so.  If you
 * do not wish to do so, delete this exception statement from your
 * version.  If you delete this exception statement from all source
 * files in the program, then also delete it here.
 */
/* copyright --> */
#ifndef D_IOCP_EVENT_POLL_H
#define D_IOCP_EVENT_POLL_H

#include "EventPoll.h"

#include <map>

#include "Event.h"
#include "a2functional.h"
#ifdef ENABLE_ASYNC_DNS
#  include "AsyncNameResolver.h"
#endif // ENABLE_ASYNC_DNS

namespace aria2 {

class IocpEventPoll : public EventPoll {
private:
  class KSocketEntry;

  typedef Event<KSocketEntry> KEvent;
  typedef CommandEvent<KSocketEntry, IocpEventPoll> KCommandEvent;
  typedef ADNSEvent<KSocketEntry, IocpEventPoll> KADNSEvent;
  typedef AsyncNameResolverEntry<IocpEventPoll> KAsyncNameResolverEntry;
  friend class AsyncNameResolverEntry<IocpEventPoll>;

  // 扩展 OVERLAPPED 结构，关联 socket 和事件类型
  struct OverlappedEntry {
    OVERLAPPED overlapped; // 必须是第一个成员，以便在完成通知中强制转换
    sock_t socket;         // 冗余字段（socket 通过 CompletionKey 获取），保留用于调试
    int eventType;         // IEV_READ 或 IEV_WRITE
  };

  class KSocketEntry : public SocketEntry<KCommandEvent, KADNSEvent> {
  public:
    OverlappedEntry readOv;
    OverlappedEntry writeOv;
    bool readPending;  // 是否有未完成的零字节 WSARecv
    bool writePending; // 是否有未完成的零字节 WSASend

    KSocketEntry(sock_t socket);

    KSocketEntry(const KSocketEntry&) = delete;
    KSocketEntry(KSocketEntry&&) = delete;
    KSocketEntry& operator=(const KSocketEntry&) = delete;
    KSocketEntry& operator=(KSocketEntry&&) = delete;

    // 聚合所有 CommandEvent/ADNSEvent 的事件掩码
    int getEvents();
  };

  friend int accumulateEvent(int events, const KEvent& event);

private:
  typedef std::map<sock_t, KSocketEntry> KSocketEntrySet;
  KSocketEntrySet socketEntries_;

  // 零字节操作无法挂起的 socket，通过 select() 轮询检测就绪。
  // value 为方向掩码（IEV_READ / IEV_WRITE）。
  // 典型场景：连接中 socket（WSASend 失败 WSAENOTCONN）、
  //           监听 socket（WSARecv 失败 WSAENOTCONN/WSAEINVAL）。
  std::map<sock_t, int> fallbackSockets_;

  // pollFallbackSockets() 使用的 fd_set，作为类成员避免栈分配
  // （FD_SETSIZE=32768 时每个约 256KB，放栈上有溢出风险）
  fd_set rfds_;
  fd_set wfds_;
  fd_set efds_;

#ifdef ENABLE_ASYNC_DNS
  typedef std::map<std::pair<AsyncNameResolver*, Command*>,
                   KAsyncNameResolverEntry>
      KAsyncNameResolverEntrySet;
  KAsyncNameResolverEntrySet nameResolverEntries_;
#endif // ENABLE_ASYNC_DNS

  HANDLE iocp_;

  // wakeup() 使用的特殊 CompletionKey，使用 INVALID_SOCKET 值确保不与有效句柄冲突
  static const ULONG_PTR WAKEUP_KEY = static_cast<ULONG_PTR>(-1);
  // GetQueuedCompletionStatusEx 每次最多取出的事件数
  static const ULONG IOCP_MAX_EVENTS = 128;

  bool addEvents(sock_t socket, const KEvent& event);

  bool deleteEvents(sock_t socket, const KEvent& event);

#ifdef ENABLE_ASYNC_DNS
  bool addEvents(sock_t socket, Command* command, int events,
                 const std::shared_ptr<AsyncNameResolver>& rs);

  bool deleteEvents(sock_t socket, Command* command,
                    const std::shared_ptr<AsyncNameResolver>& rs);
#endif // ENABLE_ASYNC_DNS

  // 提交零字节 WSARecv 作为可读就绪通知
  void postZeroByteRecv(KSocketEntry& entry);
  // 提交零字节 WSASend 作为可写就绪通知
  void postZeroByteSend(KSocketEntry& entry);
  // 根据当前注册的事件重新提交零字节操作（IOCP one-shot 语义）
  void rearmEvents(KSocketEntry& entry);
  // 使用 select() 检测零字节操作无法覆盖的 socket（连接中、监听等）
  void pollFallbackSockets();

public:
  IocpEventPoll();

  bool good() const;

  virtual ~IocpEventPoll();

  virtual void poll(const struct timeval& tv) CXX11_OVERRIDE;

  virtual void wakeup() CXX11_OVERRIDE;

  virtual bool addEvents(sock_t socket, Command* command,
                         EventPoll::EventType events) CXX11_OVERRIDE;

  virtual bool deleteEvents(sock_t socket, Command* command,
                            EventPoll::EventType events) CXX11_OVERRIDE;
#ifdef ENABLE_ASYNC_DNS

  virtual bool
  addNameResolver(const std::shared_ptr<AsyncNameResolver>& resolver,
                  Command* command) CXX11_OVERRIDE;
  virtual bool
  deleteNameResolver(const std::shared_ptr<AsyncNameResolver>& resolver,
                     Command* command) CXX11_OVERRIDE;
#endif // ENABLE_ASYNC_DNS

  static const int IEV_READ = 1;
  static const int IEV_WRITE = 1 << 1;
  static const int IEV_ERROR = 1 << 2;
  static const int IEV_HUP = 1 << 3;
};

} // namespace aria2

#endif // D_IOCP_EVENT_POLL_H
