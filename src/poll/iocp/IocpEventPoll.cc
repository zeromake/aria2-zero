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
#include "IocpEventPoll.h"

#include <algorithm>
#include <cstring>
#include <numeric>

#include "Command.h"
#include "LogFactory.h"
#include "Logger.h"
#include "util.h"
#include "a2functional.h"
#include "fmt.h"

// CancelIoEx 取消操作时内核设置的 NTSTATUS 值
#ifndef STATUS_CANCELLED
#  define STATUS_CANCELLED ((LONG)0xC0000120L)
#endif

namespace aria2 {

IocpEventPoll::KSocketEntry::KSocketEntry(sock_t s)
    : SocketEntry<KCommandEvent, KADNSEvent>(s),
      readPending(false),
      writePending(false)
{
  memset(&readOv, 0, sizeof(readOv));
  memset(&writeOv, 0, sizeof(writeOv));
}

int accumulateEvent(int events, const IocpEventPoll::KEvent& event)
{
  return events | event.getEvents();
}

int IocpEventPoll::KSocketEntry::getEvents()
{
#ifdef ENABLE_ASYNC_DNS
  return std::accumulate(
      adnsEvents_.begin(), adnsEvents_.end(),
      std::accumulate(commandEvents_.begin(), commandEvents_.end(), 0,
                      accumulateEvent),
      accumulateEvent);
#else  // !ENABLE_ASYNC_DNS
  return std::accumulate(commandEvents_.begin(), commandEvents_.end(), 0,
                         accumulateEvent);
#endif // !ENABLE_ASYNC_DNS
}

IocpEventPoll::IocpEventPoll()
{
  FD_ZERO(&rfds_);
  FD_ZERO(&wfds_);
  FD_ZERO(&efds_);
  iocp_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
  if (iocp_) {
    A2_LOG_INFO("IOCP event poll initialized successfully");
  }
  else {
    A2_LOG_ERROR("Failed to create I/O completion port");
  }
}

IocpEventPoll::~IocpEventPoll()
{
  // 统计挂起操作数，取消后按数量排空，确保所有完成通知都已出队
  int pendingCount = 0;
  for (auto& pair : socketEntries_) {
    auto& entry = pair.second;
    if (entry.readPending || entry.writePending) {
      CancelIoEx((HANDLE)entry.getSocket(), nullptr);
      if (entry.readPending) {
        ++pendingCount;
      }
      if (entry.writePending) {
        ++pendingCount;
      }
    }
  }
  if (iocp_) {
    // 按挂起操作数排空完成通知，确保内核不再引用 OVERLAPPED 内存后再释放。
    // 设置总超时上限（1 秒）防止异常情况下无限等待。
    OVERLAPPED_ENTRY entries[64];
    ULONG count = 0;
    int totalWaitMs = 0;
    while (pendingCount > 0 && totalWaitMs < 1000) {
      if (GetQueuedCompletionStatusEx(iocp_, entries, 64, &count, 10,
                                      FALSE)) {
        pendingCount -= static_cast<int>(count);
      }
      totalWaitMs += 10;
    }
    CloseHandle(iocp_);
    iocp_ = nullptr;
  }
}

bool IocpEventPoll::good() const { return iocp_ != nullptr; }

void IocpEventPoll::poll(const struct timeval& tv)
{
  DWORD timeout = tv.tv_sec * 1000 + tv.tv_usec / 1000;

  // 仅当存在写方向回退（连接中 socket）时缩短等待，避免连接完成检测延迟。
  // 读方向回退（监听 socket）是长期存在的，不需要压缩超时。
  for (auto& pair : fallbackSockets_) {
    if (pair.second & IEV_WRITE) {
      if (timeout > 100) {
        timeout = 100;
      }
      break;
    }
  }

  OVERLAPPED_ENTRY entries[IOCP_MAX_EVENTS];
  ULONG count = 0;

  BOOL ok = GetQueuedCompletionStatusEx(iocp_, entries, IOCP_MAX_EVENTS,
                                        &count, timeout, FALSE);

  if (ok && count > 0) {
    for (ULONG i = 0; i < count; ++i) {
      // wakeup() 发送的唤醒信号
      if (entries[i].lpCompletionKey == WAKEUP_KEY) {
        continue;
      }

      // 通过 CompletionKey（而非 OVERLAPPED 指针）获取 socket，
      // 避免在条目已被删除时访问已释放的内存
      sock_t socket = static_cast<sock_t>(entries[i].lpCompletionKey);
      auto it = socketEntries_.find(socket);
      if (it == socketEntries_.end()) {
        continue; // socket 已被移除，忽略孤立的完成通知
      }

      auto& socketEntry = it->second;
      auto* ov =
          reinterpret_cast<OverlappedEntry*>(entries[i].lpOverlapped);

      // 更新挂起状态
      if (ov->eventType == IEV_READ) {
        socketEntry.readPending = false;
      }
      else {
        socketEntry.writePending = false;
      }

      // CancelIoEx 取消的操作以 STATUS_CANCELLED 完成，跳过事件派发。
      // 但需调用 rearmEvents：取消期间可能已有新事件注册（如 c-ares 重新注册），
      // 此时 pending 标志刚被清除，需要为新事件提交零字节操作。
      if (entries[i].Internal != 0) {
        if (static_cast<LONG>(entries[i].Internal) == STATUS_CANCELLED) {
          rearmEvents(socketEntry);
          continue;
        }
      }

      // 派发事件给所有注册在此 socket 上的 Command。
      // 不额外派发 IEV_ERROR：异常完成（如连接重置）让 Command 在后续
      // recv()/send() 中自行检测，与 SelectEventPoll 行为一致。
      socketEntry.processEvents(ov->eventType == IEV_READ ? IEV_READ
                                                          : IEV_WRITE);

      // 重新提交零字节操作（IOCP 是 one-shot 语义，每次完成后需重新提交）
      rearmEvents(socketEntry);
    }
  }

  // 通过 select() 检测零字节操作无法覆盖的 socket（连接中、监听等）
  pollFallbackSockets();

  // 清理已无事件且无挂起操作的条目（延迟删除，确保 OVERLAPPED 内存安全）
  for (auto it = socketEntries_.begin(); it != socketEntries_.end();) {
    auto& e = it->second;
    if (e.eventEmpty() && !e.readPending && !e.writePending) {
      fallbackSockets_.erase(it->first);
      it = socketEntries_.erase(it);
    }
    else {
      ++it;
    }
  }

#ifdef ENABLE_ASYNC_DNS
  // c-ares 可能在回调中创建或关闭 socket，需每轮重新注册
  for (auto& entry : nameResolverEntries_) {
    entry.second.processTimeout();
    entry.second.removeSocketEvents(this);
    entry.second.addSocketEvents(this);
  }
#endif // ENABLE_ASYNC_DNS
}

void IocpEventPoll::wakeup()
{
  // 通过 WAKEUP_KEY 标识的空完成通知唤醒 GetQueuedCompletionStatusEx
  PostQueuedCompletionStatus(iocp_, 0, WAKEUP_KEY, nullptr);
}

namespace {
int translateEvents(EventPoll::EventType events)
{
  int newEvents = 0;
  if (EventPoll::EVENT_READ & events) {
    newEvents |= IocpEventPoll::IEV_READ;
  }
  if (EventPoll::EVENT_WRITE & events) {
    newEvents |= IocpEventPoll::IEV_WRITE;
  }
  if (EventPoll::EVENT_ERROR & events) {
    newEvents |= IocpEventPoll::IEV_ERROR;
  }
  if (EventPoll::EVENT_HUP & events) {
    newEvents |= IocpEventPoll::IEV_HUP;
  }
  return newEvents;
}
} // namespace

bool IocpEventPoll::addEvents(sock_t socket,
                              const IocpEventPoll::KEvent& event)
{
  auto i = socketEntries_.lower_bound(socket);
  if (i != std::end(socketEntries_) && (*i).first == socket) {
    auto& socketEntry = (*i).second;

    if (socketEntry.eventEmpty()) {
      // 条目处于延迟清理状态（事件已清空但操作仍挂起）。
      // 不重置 pending 标志——让取消完成自然到达后由 poll() 清除，
      // 避免在旧 OVERLAPPED 操作完成前复用同一结构。
      // rearmEvents 通过 postZeroByteRecv/Send 的 pending 守卫自动跳过
      // 已挂起的方向，仅提交未挂起方向的新操作。

      // socket 句柄可能被 OS 复用（旧 socket 已关闭），尝试重新关联 IOCP。
      // 如果是同一 socket（如 c-ares 重新注册），返回 ERROR_INVALID_PARAMETER，
      // 无影响。
      HANDLE h =
          CreateIoCompletionPort((HANDLE)socket, iocp_, (ULONG_PTR)socket, 0);
      if (!h && GetLastError() != ERROR_INVALID_PARAMETER) {
        A2_LOG_DEBUG(
            fmt("Failed to re-associate socket %d with IOCP, error=%lu",
                (int)socket, (unsigned long)GetLastError()));
      }
    }

    event.addSelf(&socketEntry);
  }
  else {
    // piecewise_construct 原地构造，避免 move KSocketEntry
    // （move 会改变 OverlappedEntry 地址，若有挂起操作则内核持有旧地址指针）
    i = socketEntries_.emplace_hint(
        i, std::piecewise_construct, std::forward_as_tuple(socket),
        std::forward_as_tuple(socket));

    // 将 socket 关联到 IOCP 完成端口
    HANDLE h =
        CreateIoCompletionPort((HANDLE)socket, iocp_, (ULONG_PTR)socket, 0);
    if (!h) {
      DWORD err = GetLastError();
      if (err != ERROR_INVALID_PARAMETER) {
        A2_LOG_DEBUG(
            fmt("Failed to associate socket %d with IOCP, error=%lu",
                (int)socket, err));
        socketEntries_.erase(i);
        return false;
      }
    }

    event.addSelf(&(*i).second);
  }

  rearmEvents((*i).second);
  return true;
}

bool IocpEventPoll::addEvents(sock_t socket, Command* command,
                              EventPoll::EventType events)
{
  int iocpEvents = translateEvents(events);
  return addEvents(socket, KCommandEvent(command, iocpEvents));
}

#ifdef ENABLE_ASYNC_DNS
bool IocpEventPoll::addEvents(sock_t socket, Command* command, int events,
                              const std::shared_ptr<AsyncNameResolver>& rs)
{
  return addEvents(socket, KADNSEvent(rs, command, socket, events));
}
#endif // ENABLE_ASYNC_DNS

bool IocpEventPoll::deleteEvents(sock_t socket,
                                 const IocpEventPoll::KEvent& event)
{
  auto i = socketEntries_.find(socket);
  if (i == std::end(socketEntries_)) {
    A2_LOG_DEBUG(fmt("Socket %d is not found in SocketEntries.", (int)socket));
    return false;
  }

  auto& socketEntry = (*i).second;
  event.removeSelf(&socketEntry);

  if (socketEntry.eventEmpty()) {
    fallbackSockets_.erase(socket);
    if (socketEntry.readPending || socketEntry.writePending) {
      // 取消所有挂起的零字节操作，条目将在 poll() 收到取消通知后被清理
      CancelIoEx((HANDLE)socket, nullptr);
    }
    else {
      socketEntries_.erase(i);
    }
  }
  else {
    // 仍有其他事件注册，只取消不再需要的方向
    int events = socketEntry.getEvents();
    if (!(events & IEV_READ) && socketEntry.readPending) {
      CancelIoEx((HANDLE)socket, &socketEntry.readOv.overlapped);
    }
    if (!(events & IEV_WRITE) && socketEntry.writePending) {
      CancelIoEx((HANDLE)socket, &socketEntry.writeOv.overlapped);
    }
    rearmEvents(socketEntry);
    // 清理不再需要的 fallback 方向位，避免残留导致无意义的 select() 轮询
    auto fit = fallbackSockets_.find(socket);
    if (fit != fallbackSockets_.end()) {
      fit->second &= events;
      if (fit->second == 0) {
        fallbackSockets_.erase(fit);
      }
    }
  }
  return true;
}

#ifdef ENABLE_ASYNC_DNS
bool IocpEventPoll::deleteEvents(sock_t socket, Command* command,
                                 const std::shared_ptr<AsyncNameResolver>& rs)
{
  return deleteEvents(socket, KADNSEvent(rs, command, socket, 0));
}
#endif // ENABLE_ASYNC_DNS

bool IocpEventPoll::deleteEvents(sock_t socket, Command* command,
                                 EventPoll::EventType events)
{
  int iocpEvents = translateEvents(events);
  return deleteEvents(socket, KCommandEvent(command, iocpEvents));
}

#ifdef ENABLE_ASYNC_DNS
bool IocpEventPoll::addNameResolver(
    const std::shared_ptr<AsyncNameResolver>& resolver, Command* command)
{
  auto key = std::make_pair(resolver.get(), command);
  auto itr = nameResolverEntries_.lower_bound(key);

  if (itr != std::end(nameResolverEntries_) && (*itr).first == key) {
    return false;
  }

  itr = nameResolverEntries_.insert(
      itr, std::make_pair(key, KAsyncNameResolverEntry(resolver, command)));
  (*itr).second.addSocketEvents(this);
  return true;
}

bool IocpEventPoll::deleteNameResolver(
    const std::shared_ptr<AsyncNameResolver>& resolver, Command* command)
{
  auto key = std::make_pair(resolver.get(), command);
  auto itr = nameResolverEntries_.find(key);
  if (itr == std::end(nameResolverEntries_)) {
    return false;
  }

  (*itr).second.removeSocketEvents(this);
  nameResolverEntries_.erase(itr);
  return true;
}
#endif // ENABLE_ASYNC_DNS

void IocpEventPoll::postZeroByteRecv(KSocketEntry& entry)
{
  if (entry.readPending) {
    return;
  }

  memset(&entry.readOv.overlapped, 0, sizeof(OVERLAPPED));
  entry.readOv.socket = entry.getSocket();
  entry.readOv.eventType = IEV_READ;

  // 零字节 WSARecv + MSG_PEEK：不消耗任何数据，仅当 socket 可读时完成。
  // 必须使用 MSG_PEEK，否则 UDP socket 上的零字节接收会消耗并丢弃数据报。
  WSABUF buf = {0, nullptr};
  DWORD flags = MSG_PEEK, bytes = 0;
  int r = WSARecv(entry.getSocket(), &buf, 1, &bytes, &flags,
                  &entry.readOv.overlapped, nullptr);

  if (r == 0 || WSAGetLastError() == WSA_IO_PENDING) {
    entry.readPending = true;
    // IOCP 通知成功挂起，移除可能存在的读回退标记
    auto fit = fallbackSockets_.find(entry.getSocket());
    if (fit != fallbackSockets_.end()) {
      fit->second &= ~IEV_READ;
      if (fit->second == 0) {
        fallbackSockets_.erase(fit);
      }
    }
  }
  else {
    int err = WSAGetLastError();
    A2_LOG_DEBUG(
        fmt("Zero-byte WSARecv failed for socket %d, error=%d",
            (int)entry.getSocket(), err));
    if (err == WSAENOTCONN || err == WSAEINVAL) {
      // 监听 socket 或未连接 socket，回退到 select() 检测可读
      fallbackSockets_[entry.getSocket()] |= IEV_READ;
    }
  }
}

void IocpEventPoll::postZeroByteSend(KSocketEntry& entry)
{
  if (entry.writePending) {
    return;
  }

  memset(&entry.writeOv.overlapped, 0, sizeof(OVERLAPPED));
  entry.writeOv.socket = entry.getSocket();
  entry.writeOv.eventType = IEV_WRITE;

  // 零字节 WSASend：不发送任何数据，仅当 socket 可写时完成
  WSABUF buf = {0, nullptr};
  DWORD bytes = 0;
  int r = WSASend(entry.getSocket(), &buf, 1, &bytes, 0,
                  &entry.writeOv.overlapped, nullptr);

  if (r == 0 || WSAGetLastError() == WSA_IO_PENDING) {
    entry.writePending = true;
    // IOCP 通知成功挂起，移除可能存在的写回退标记
    auto fit = fallbackSockets_.find(entry.getSocket());
    if (fit != fallbackSockets_.end()) {
      fit->second &= ~IEV_WRITE;
      if (fit->second == 0) {
        fallbackSockets_.erase(fit);
      }
    }
  }
  else {
    int err = WSAGetLastError();
    if (err == WSAENOTCONN) {
      // socket 正在执行非阻塞 connect()，零字节 WSASend 无法挂起，
      // 回退到 select() 轮询检测连接完成
      fallbackSockets_[entry.getSocket()] |= IEV_WRITE;
      A2_LOG_DEBUG(
          fmt("Socket %d is connecting, using select() fallback",
              (int)entry.getSocket()));
    }
    else {
      A2_LOG_DEBUG(
          fmt("Zero-byte WSASend failed for socket %d, error=%d",
              (int)entry.getSocket(), err));
    }
  }
}

void IocpEventPoll::rearmEvents(KSocketEntry& entry)
{
  int events = entry.getEvents();
  if (events & IEV_READ) {
    postZeroByteRecv(entry);
  }
  if (events & IEV_WRITE) {
    postZeroByteSend(entry);
  }
}

void IocpEventPoll::pollFallbackSockets()
{
  if (fallbackSockets_.empty()) {
    return;
  }

  FD_ZERO(&rfds_);
  FD_ZERO(&wfds_);
  FD_ZERO(&efds_);

  for (auto& pair : fallbackSockets_) {
    sock_t fd = pair.first;
    int dir = pair.second;
    if (dir & IEV_READ) {
      FD_SET(fd, &rfds_);
    }
    if (dir & IEV_WRITE) {
      FD_SET(fd, &wfds_);
      FD_SET(fd, &efds_); // Windows 在 efds 中报告 connect() 失败
    }
  }

  struct timeval zero = {0, 0};
  // Windows 的 select() 忽略第一个参数（nfds），传 0 即可
  int ret = select(0, &rfds_, &wfds_, &efds_, &zero);
  if (ret <= 0) {
    return;
  }

  // 收集就绪的 socket（避免在遍历中修改 fallbackSockets_）
  std::vector<std::pair<sock_t, int>> ready; // <socket, 就绪方向>
  for (auto& pair : fallbackSockets_) {
    sock_t fd = pair.first;
    int events = 0;
    if (FD_ISSET(fd, &rfds_)) {
      events |= IEV_READ;
    }
    if (FD_ISSET(fd, &wfds_)) {
      events |= IEV_WRITE;
    }
    if (FD_ISSET(fd, &efds_)) {
      events |= IEV_ERROR;
    }
    if (events) {
      ready.emplace_back(fd, events);
    }
  }

  for (auto& pair : ready) {
    sock_t fd = pair.first;
    int events = pair.second;

    auto it = socketEntries_.find(fd);
    if (it == socketEntries_.end()) {
      fallbackSockets_.erase(fd);
      continue;
    }

    auto& socketEntry = it->second;

    if (events & IEV_ERROR) {
      // 连接失败，不重新提交零字节操作（socket 处于错误状态）
      A2_LOG_DEBUG(
          fmt("Socket %d connect failed (detected via select)", (int)fd));
      socketEntry.processEvents(events);
      fallbackSockets_.erase(fd);
    }
    else {
      socketEntry.processEvents(events);
      // 尝试切换到 IOCP 零字节通知（连接完成后 WSASend/WSARecv 可能成功）
      rearmEvents(socketEntry);
      // rearmEvents 成功时会自动从 fallbackSockets_ 中移除对应方向
    }
  }
}

} // namespace aria2
