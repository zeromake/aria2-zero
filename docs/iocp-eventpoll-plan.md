# Windows IOCP EventPoll 实施方案

## 1. 现状分析

### 1.1 当前 Windows 事件模型

Windows 平台在未启用 libuv 时，使用 `SelectEventPoll` 作为事件轮询实现：

```
DownloadEngine::run()
  → waitData()
    → SelectEventPoll::poll()
      → select(fdmax+1, &rfds, &wfds, &efds, &tv)
      → 遍历 socketEntries_ 检查 FD_ISSET
      → command->readEventReceived() / writeEventReceived()
```

### 1.2 select 的已知问题

| 问题 | 说明 |
|------|------|
| **FD_SETSIZE 限制** | MSVC 默认 64，项目已提升到 32768（`common.h`），但仍有上限 |
| **O(n) 扫描** | 每次 `poll()` 需遍历所有注册 socket 检查 FD_ISSET |
| **fd_set 复制开销** | 每次调用前 memcpy 三份 fd_set（rfds/wfds/efds） |
| **dummy socket hack** | Winsock select() 不允许空 fd_set，需常驻一个哑 socket |
| **非标准 efds 语义** | Windows 在 efds 中报告 connect() 失败，POSIX 在 wfds 中报告 |

### 1.3 已有的 EventPoll 实现

| 实现 | 平台 | 文件 |
|------|------|------|
| SelectEventPoll | 全平台 (fallback) | `src/poll/select/` |
| EpollEventPoll | Linux | `src/poll/epoll/` |
| KqueueEventPoll | macOS/BSD | `src/poll/kqueue/` |
| PollEventPoll | POSIX (xmake 未启用) | `src/poll/poll/` |
| PortEventPoll | Solaris (xmake 未启用) | `src/poll/port/` |
| LibuvEventPoll | 全平台 (可选 `--uv=true`) | `src/poll/libuv/` |

### 1.4 EventPoll 接口

```cpp
class EventPoll {
  virtual void poll(const struct timeval& tv) = 0;
  virtual void wakeup() = 0;
  virtual bool addEvents(sock_t socket, Command* command, EventType events) = 0;
  virtual bool deleteEvents(sock_t socket, Command* command, EventType events) = 0;
  // + AsyncNameResolver 相关方法
};
```

接口是**就绪通知（Reactor）模型**：告知"socket X 可读/可写"，由 Command 自行调用 `recv()`/`send()`。

### 1.5 当前 I/O 流程

```
Command 注册事件:
  AbstractCommand::setReadCheckSocket(socket)
    → DownloadEngine::addSocketForReadCheck(socket, command)
      → EventPoll::addEvents(fd, command, EVENT_READ)

数据到达:
  EventPoll::poll() 检测到 fd 可读
    → command->setStatusActive()
    → command->readEventReceived()

主循环执行:
  DownloadEngine::executeCommand()
    → command->execute()
      → socket->readData(buf, len)   // 非阻塞 recv()
        → 返回数据 或 WOULDBLOCK → wantRead_ = true
```

所有 socket 使用非阻塞模式（`ioctlsocket(FIONBIO)`），读写操作在 `Command::execute()` 中同步调用 `recv()`/`send()`，依赖 `WOULDBLOCK` 判断是否需要重新注册事件。

---

## 2. 方案选型

### 2.1 Reactor vs Proactor

| 特性 | Reactor (select/epoll) | Proactor (IOCP) |
|------|----------------------|-----------------|
| 通知内容 | "socket 可读" | "读操作已完成，数据在 buffer 中" |
| I/O 执行者 | 应用层调用 recv() | 内核异步完成 |
| 缓冲区管理 | 应用按需分配 | 必须提前分配并提交给内核 |
| 触发时机 | 就绪时 | 完成时 |

### 2.2 三种候选方案

| 维度 | A: IOCP Reactor | B: IOCP Proactor | C: libuv 默认 |
|------|-----------------|-------------------|---------------|
| 核心思路 | 零字节 WSARecv 做就绪通知 | 完整 Overlapped I/O | 启用已有 LibuvEventPoll |
| 开发成本 | 中 (2-3天) | 高 (2-4周) | 低 (配置修改) |
| 侵入性 | 低 (新增文件) | 极高 (修改核心接口) | 无 |
| 外部依赖 | 无 | 无 | libuv |
| 兼容性 | 与现有代码 100% 兼容 | 需重构核心模块 | 已验证 |
| 风险 | 低 | 高 | 低 |

**方案 B 不推荐**：需修改 EventPoll 接口、SocketCore、所有 Command 子类、TLS 层，收益与成本不成比例。aria2 的瓶颈在网络带宽和磁盘 I/O，不在事件通知的微秒级差异。

**方案 C 不推荐**：增加外部依赖，libuv 的 `uv_poll_t` 在 Windows 上走未文档化的 AFD 接口，且引入事件循环生命周期管理复杂性。

**选定方案 A：原生 IOCP Reactor**。零字节 WSARecv 作为就绪通知，保持 Reactor 架构不变，无外部依赖，无需修改 SocketCore 和任何 Command。

---

## 3. 关键设计决策

### 3.1 与 libuv 共存策略

**IOCP 作为 Windows 平台新增默认选项，select 保留为回退，libuv 通过 `--uv=true` 独立启用。**

`--event-poll` 选项通过编译期 `HAVE_*` 宏控制可用值列表（`OptionHandlerFactory.cc:284-318`），运行时在 `DownloadEngineFactory.cc:72-130` 的 `createEventPoll()` 中实例化。当前 Windows 仅有 `select` 一个选项。

接入方式与 Linux epoll 对称：

```
Windows 平台 (--uv=false):
  可用选项: {iocp, select}
  默认值: iocp

Windows 平台 (--uv=true):
  可用选项: {libuv, select}    // libuv 启用时 native 后端不编译，已有行为
  默认值: libuv
```

### 3.2 多 Command 绑定同一 socket

**实际存在，必须支持。**

所有 EventPoll 后端的 `SocketEntry` 使用 `std::deque<CommandEvent>` 存储多个命令事件（`SelectEventPoll.h:86`、`Event.h:189`）。`CommandEvent::operator==` 按 `command_` 指针比较，不同 Command 注册同一 socket 时成为独立条目。

触发场景 — RPC 命令交接（`HttpServerCommand.cc:258`）：

```
HttpServerCommand::execute():
  1. 创建 HttpServerBodyCommand → 构造函数调用 addSocketForReadCheck(socket_, this)
     → 新 Command 注册到 socket
  2. execute() 返回 true
  3. DownloadEngine::executeCommand() (DownloadEngine.cc:147) 调用 com.reset()
     → 旧 Command 析构 → deleteSocketForReadCheck(socket_, this)
```

步骤 1~3 之间两个不同 Command 同时注册在同一 socket 上。IocpEventPoll 复用 `Event.h` 的 `SocketEntry` 模板体系，完成通知到达时遍历 `commandEvents` 逐一派发。

### 3.3 c-ares (AsyncNameResolver) 集成

**c-ares socket 注册到 IOCP，走统一 `addEvents` 路径。**

c-ares socket 是标准 Winsock `SOCKET`（`ares_socket_t` = `SOCKET` = `sock_t`，见 `a2netcompat.h:97`）。Epoll 后端已通过统一 `addEvents` 路径处理 c-ares socket（`Event.h:320-337`，`EpollEventPoll.cc:232-237`），IOCP 实现完全对称。

零字节 WSARecv 不消耗数据，仅作为就绪通知。完成后 c-ares 调用自己的 `recv()`/`send()` 正常读写。对 UDP socket（c-ares 主要使用）同样有效。

需要处理的特殊情况：

- **动态 socket 生命周期**：参照 Epoll（`EpollEventPoll.cc:140-151`），每次 `poll()` 后执行 `removeSocketEvents` + `addSocketEvents` 重新注册
- **pending 操作取消**：`removeSocketEvents` 时调用 `CancelIoEx`，被取消的操作以 `ERROR_OPERATION_ABORTED` 完成，在 `poll()` 中忽略

### 3.4 GetQueuedCompletionStatusEx 批量获取

**直接使用 `GetQueuedCompletionStatusEx`，同时将 `_WIN32_WINNT` 提升到 0x0600。**

项目当前目标是 Windows XP（`common.h:78-82` 定义 `_WIN32_WINNT=0x501`），需要提升至 Vista (0x0600)。`GetQueuedCompletionStatusEx` 一次取出多个完成事件，在高并发 BT 场景下有明显收益。

提升理由：Windows XP 于 2014 年终止支持，LibuvEventPoll 已局部使用 Vista API（`LibuvEventPoll.cc:38-41`）。

---

## 4. 详细设计

### 4.1 文件结构

```
src/poll/iocp/
├── IocpEventPoll.h
└── IocpEventPoll.cc
```

### 4.2 核心数据结构

```cpp
class IocpEventPoll : public EventPoll {
  // OVERLAPPED 扩展结构，关联回 socket 和事件类型
  struct OverlappedEntry {
    OVERLAPPED overlapped;    // 必须是第一个成员
    sock_t socket;
    int eventType;            // EVENT_READ 或 EVENT_WRITE
  };

  // 复用 Event.h 的模板体系
  using KCommandEvent = CommandEvent<IocpEventPoll>;
  using KADNSEvent = ADNSEvent<IocpEventPoll>;
  using KEvent = Event<KCommandEvent, KADNSEvent>;

  struct KSocketEntry : public SocketEntry<KCommandEvent, KADNSEvent> {
    OverlappedEntry readOv;
    OverlappedEntry writeOv;
    bool readPending = false;
    bool writePending = false;

    KSocketEntry(sock_t s);
    int getEvents();          // 聚合所有 CommandEvent/ADNSEvent 的事件掩码
  };

  HANDLE iocp_;
  std::map<sock_t, KSocketEntry> socketEntries_;
  static constexpr ULONG_PTR WAKEUP_KEY = 0;
  static constexpr int IOCP_MAX_EVENTS = 128;

#ifdef ENABLE_ASYNC_DNS
  using KAsyncNameResolverEntry = AsyncNameResolverEntry<IocpEventPoll>;
  std::map<
    std::pair<std::shared_ptr<AsyncNameResolver>, Command*>,
    KAsyncNameResolverEntry
  > nameResolverEntries_;
#endif
};
```

### 4.3 poll() 实现

```cpp
void IocpEventPoll::poll(const struct timeval& tv) {
  DWORD timeout = tv.tv_sec * 1000 + tv.tv_usec / 1000;
  OVERLAPPED_ENTRY entries[IOCP_MAX_EVENTS];
  ULONG count = 0;

  BOOL ok = GetQueuedCompletionStatusEx(
      iocp_, entries, IOCP_MAX_EVENTS, &count, timeout, FALSE);

  if (ok && count > 0) {
    for (ULONG i = 0; i < count; ++i) {
      if (entries[i].lpCompletionKey == WAKEUP_KEY) {
        continue;  // wakeup() 信号
      }

      auto* ov = reinterpret_cast<OverlappedEntry*>(entries[i].lpOverlapped);
      auto it = socketEntries_.find(ov->socket);
      if (it == socketEntries_.end()) {
        continue;  // socket 已被移除
      }

      auto& socketEntry = it->second;

      if (ov->eventType == EVENT_READ) {
        socketEntry.readPending = false;
      } else {
        socketEntry.writePending = false;
      }

      // CancelIoEx 取消的操作，忽略
      if (entries[i].Internal != 0) {
        DWORD error = RtlNtStatusToDosError((NTSTATUS)entries[i].Internal);
        if (error == ERROR_OPERATION_ABORTED) {
          continue;
        }
      }

      // 派发事件给所有注册的 Command
      socketEntry.processEvents(
          ov->eventType == EVENT_READ ? IEV_READ : IEV_WRITE);

      // 重新提交零字节操作（IOCP one-shot 语义）
      rearmEvents(socketEntry);
    }
  }

#ifdef ENABLE_ASYNC_DNS
  for (auto& entry : nameResolverEntries_) {
    entry.second.processTimeout();
    entry.second.removeSocketEvents(this);
    entry.second.addSocketEvents(this);
  }
#endif
}
```

### 4.4 零字节操作

```cpp
void IocpEventPoll::postZeroByteRecv(KSocketEntry& entry) {
  if (entry.readPending) return;
  memset(&entry.readOv.overlapped, 0, sizeof(OVERLAPPED));
  entry.readOv.socket = entry.getSocket();
  entry.readOv.eventType = EVENT_READ;
  WSABUF buf = {0, nullptr};
  DWORD flags = 0, bytes = 0;
  int r = WSARecv(entry.getSocket(), &buf, 1, &bytes, &flags,
                  &entry.readOv.overlapped, nullptr);
  if (r == 0 || WSAGetLastError() == WSA_IO_PENDING) {
    entry.readPending = true;
  }
}

void IocpEventPoll::postZeroByteSend(KSocketEntry& entry) {
  if (entry.writePending) return;
  memset(&entry.writeOv.overlapped, 0, sizeof(OVERLAPPED));
  entry.writeOv.socket = entry.getSocket();
  entry.writeOv.eventType = EVENT_WRITE;
  WSABUF buf = {0, nullptr};
  DWORD bytes = 0;
  int r = WSASend(entry.getSocket(), &buf, 1, &bytes, 0,
                  &entry.writeOv.overlapped, nullptr);
  if (r == 0 || WSAGetLastError() == WSA_IO_PENDING) {
    entry.writePending = true;
  }
}
```

### 4.5 wakeup

```cpp
void IocpEventPoll::wakeup() {
  PostQueuedCompletionStatus(iocp_, 0, WAKEUP_KEY, nullptr);
}
```

不再需要 WakeupPipe 的 TCP socketpair hack。

### 4.6 addEvents / deleteEvents

```cpp
bool IocpEventPoll::addEvents(sock_t socket, const KEvent& event) {
  auto it = socketEntries_.lower_bound(socket);
  if (it != socketEntries_.end() && it->first == socket) {
    event.addSelf(&it->second);
  } else {
    it = socketEntries_.emplace_hint(it, socket, KSocketEntry(socket));
    CreateIoCompletionPort((HANDLE)socket, iocp_, (ULONG_PTR)socket, 0);
    event.addSelf(&it->second);
  }
  rearmEvents(it->second);
  return true;
}

bool IocpEventPoll::deleteEvents(sock_t socket, const KEvent& event) {
  auto it = socketEntries_.find(socket);
  if (it == socketEntries_.end()) return false;

  event.removeSelf(&it->second);

  int remaining = it->second.getEvents();
  if (remaining == 0) {
    if (it->second.readPending || it->second.writePending) {
      CancelIoEx((HANDLE)socket, nullptr);
    }
    socketEntries_.erase(it);
  } else {
    rearmEvents(it->second);
  }
  return true;
}

void IocpEventPoll::rearmEvents(KSocketEntry& entry) {
  int events = entry.getEvents();
  if (events & IEV_READ)  postZeroByteRecv(entry);
  if (events & IEV_WRITE) postZeroByteSend(entry);
}
```

---

## 5. 修改清单

| 文件 | 操作 | 说明 |
|------|------|------|
| `src/poll/iocp/IocpEventPoll.h` | **新建** | IOCP EventPoll 头文件 |
| `src/poll/iocp/IocpEventPoll.cc` | **新建** | IOCP EventPoll 实现 (~400-500 行) |
| `src/core/common.h` | **修改** | `_WIN32_WINNT` / `WINVER` 从 0x501 → 0x0600 |
| `compat/getaddrinfo.h` | **修改** | `_WIN32_WINNT` 从 0x501 → 0x0600 |
| `src/poll/libuv/LibuvEventPoll.cc` | **修改** | 移除局部 `_WIN32_WINNT` 覆盖（不再需要） |
| `src/poll/EventPollImport.h` | **修改** | 添加 `#ifdef HAVE_IOCP` include |
| `src/core/DownloadEngineFactory.cc` | **修改** | 添加 `V_IOCP` 创建分支 |
| `src/parser/OptionHandlerFactory.cc` | **修改** | 添加 `HAVE_IOCP` 默认值和允许值 |
| `src/util/prefs.h` | **修改** | 声明 `V_IOCP` |
| `src/util/prefs.cc` | **修改** | 定义 `V_IOCP = "iocp"` |
| `xmake.lua` | **修改** | Windows 编译 iocp 源码，定义 `HAVE_IOCP` |
| `config.h.in` | **修改** | 添加 `HAVE_IOCP` 宏模板 |

**无需修改**：`SocketCore.cc`、`DownloadEngine.cc`、所有 Command 子类。

---

## 6. 实施阶段

### Phase 1: 环境准备

- `_WIN32_WINNT` 提升至 0x0600（`common.h`、`getaddrinfo.h`）
- 移除 `LibuvEventPoll.cc` 的局部覆盖
- `config.h.in` 添加 `HAVE_IOCP`
- `xmake.lua` Windows 平台添加 iocp 编译配置
- `prefs.h/cc` 添加 `V_IOCP`

### Phase 2: 核心实现

- 实现 IocpEventPoll 类，复用 `Event.h` 模板体系
- `poll()` — `GetQueuedCompletionStatusEx` 批量获取
- `addEvents()` / `deleteEvents()` — 零字节 WSARecv/WSASend 注册
- `wakeup()` — `PostQueuedCompletionStatus`
- c-ares 集成 — `addNameResolver` / `deleteNameResolver`

### Phase 3: 集成与测试

- 接入 `DownloadEngineFactory` 和 `OptionHandlerFactory`
- 测试 HTTP/HTTPS 下载
- 测试 BT 高并发场景（验证无 FD_SETSIZE 限制）
- 测试 RPC 命令交接（验证多 Command per socket）
- 测试 DNS 解析（验证 c-ares 集成）
- 测试 `--event-poll=select` 回退

### 预估

- 代码量：~400-500 行核心 + ~50 行构建配置
- 工期：2-3 天
- 风险：低

---

## 7. 实施变更记录

### 7.1 相对于原方案的设计变更

#### 变更 1：零字节 WSARecv 必须使用 MSG_PEEK

**问题**：原方案的零字节 `WSARecv` 未使用 `MSG_PEEK`。在 TCP socket 上零字节接收不消耗数据（符合预期），但在 **UDP socket**（c-ares DNS 使用）上，零字节接收会消耗并丢弃整个数据报。导致 DNS 响应丢失，名称解析超时。

**修复**：`postZeroByteRecv` 的 `flags` 参数从 `0` 改为 `MSG_PEEK`。对 TCP socket 无副作用（零字节 peek 同样在数据就绪时完成），对 UDP socket 保证数据报不被消耗。

```cpp
DWORD flags = MSG_PEEK, bytes = 0;  // 而非 flags = 0
```

#### 变更 2：连接中 socket 的 select() 回退

**问题**：原方案未考虑非阻塞 `connect()` 后的 socket 状态。对连接中的 socket 调用零字节 `WSASend` 会立即失败（`WSAENOTCONN`），导致该 socket 无法获得 IOCP 通知，连接完成事件丢失。

**修复**：新增 `connectingSockets_` 集合和 `pollConnectingSockets()` 方法。当 `postZeroByteSend` 失败并返回 `WSAENOTCONN` 时，将 socket 加入集合。在 `poll()` 中通过 `select()` 轮询这些 socket，连接完成后派发事件并切换回 IOCP 通知。同时缩短 IOCP 等待超时至 100ms，避免连接检测延迟。

#### 变更 3：IOCP 关联不可逆，需容忍 ERROR_INVALID_PARAMETER

**问题**：IOCP 的 `CreateIoCompletionPort` 关联是单向的——socket 关联后不可取消（无 "remove" 操作），关联在 socket 生命周期内持续有效。c-ares 每轮 `poll()` 执行 `removeSocketEvents` + `addSocketEvents` 重新注册 socket，导致 `deleteEvents` 删除条目后 `addEvents` 重新创建条目并再次调用 `CreateIoCompletionPort`，此时返回 `ERROR_INVALID_PARAMETER`（socket 已关联）。

**修复**：`addEvents` 新建条目时，对 `CreateIoCompletionPort` 的 `ERROR_INVALID_PARAMETER` 错误静默忽略。

#### 变更 4：延迟删除 socketEntry，保护 OVERLAPPED 内存安全

**问题**：原方案在 `deleteEvents` 中当事件为空时直接 `erase` 条目。但如果有挂起的零字节操作（`readPending` / `writePending`），内核仍持有对 `OverlappedEntry`（嵌入在 `KSocketEntry` 中）的引用。`erase` 后内核访问已释放内存。

**修复**：`deleteEvents` 中当有挂起操作时调用 `CancelIoEx` 但**不删除条目**。在 `poll()` 末尾扫描清理：仅当条目无事件且无挂起操作时才删除。

#### 变更 5：socket 句柄复用时重新关联 IOCP

**问题**：当旧 socket 关闭（IOCP 关联随之消失）后 OS 复用了相同句柄值创建新 socket，`addEvents` 在已有条目路径中不会调用 `CreateIoCompletionPort`，新 socket 未关联到 IOCP，零字节操作的完成通知无法送达。

**修复**：在已有条目路径中，当 `eventEmpty() == true` 时调用 `CreateIoCompletionPort`。新 socket 则成功关联；同一 socket 重新注册则返回 `ERROR_INVALID_PARAMETER`，无影响。

#### 变更 6：wakeup 使用 INVALID_SOCKET 作为 CompletionKey

**变更**：`WAKEUP_KEY` 从 `0` 改为 `static_cast<ULONG_PTR>(-1)`（即 `INVALID_SOCKET`），避免理论上与有效 socket 句柄值冲突。

#### 变更 7：析构函数排空 IOCP 完成队列

**问题**：析构函数调用 `CancelIoEx` 后立即 `CloseHandle(iocp_)`，但取消操作的完成通知尚在 IOCP 队列中，内核可能仍引用即将释放的 `OverlappedEntry` 内存。

**修复**：`CancelIoEx` 后循环调用 `GetQueuedCompletionStatusEx`（超时 100ms）排空所有完成通知，确保内核不再引用任何 OVERLAPPED 后再关闭句柄。

#### 变更 8：错误完成派发 IEV_ERROR

**变更**：`poll()` 中处理非取消类的异常完成（`Internal != 0 && != STATUS_CANCELLED`）时，额外派发 `IEV_ERROR`，使 Command 能通过 `errorEventReceived()` 快速检测连接异常（如连接重置），而非等待下次 I/O 操作超时。

### 7.2 踩坑记录

#### 坑 1：UDP socket 上零字节 WSARecv 会消耗数据报

**现象**：c-ares DNS 解析超时，所有 HTTP/HTTPS 下载失败。`select` 后端正常。

**原因**：UDP 是消息导向协议。`WSARecv` 即使 buffer 为 0，也会从接收队列中取出（extract）数据报。由于 buffer 不足以容纳数据报，数据被丢弃（`WSAEMSGSIZE`）。后续 c-ares 的 `recvfrom()` 拿不到数据。TCP 上零字节接收不消耗数据是因为 TCP 是流式协议，零字节只是检查就绪状态。

**教训**：IOCP Reactor 模式必须对所有 socket 类型（TCP/UDP）使用 `MSG_PEEK`，或者对 UDP socket 使用其他通知机制。

#### 坑 2：IOCP 关联不可逆，与 epoll 的 CTL_DEL/CTL_ADD 语义不同

**现象**：c-ares DNS 解析间歇失败。

**原因**：`EpollEventPoll` 的 c-ares 集成每轮 `poll()` 调用 `EPOLL_CTL_DEL` 移除 socket、`EPOLL_CTL_ADD` 重新添加。IOCP 没有 "remove" 操作——一旦 `CreateIoCompletionPort` 关联，在 socket 关闭前无法解除。重复关联返回 `ERROR_INVALID_PARAMETER`。

原始代码将此视为错误并 `erase` 条目、`return false`，导致 c-ares socket 事件永远无法注册。

**教训**：IOCP 与 epoll/kqueue 的关联模型有本质区别。epoll 的注册/注销是对称操作，IOCP 的关联是一次性的。

#### 坑 3：紧凑 fd_set 导致 select() 静默失败

**现象**：连接中的 socket 永远检测不到连接完成，下载卡死后超时退出。

**原因**：为避免栈上 `fd_set` 过大（FD_SETSIZE=32768 时约 256KB），尝试使用 `vector<char>` 分配紧凑的 `fd_set`（仅包含实际 socket 数量的 `fd_array`）。Windows 的 `select()` 实现看似只使用 `fd_count` 来确定检查范围，但实际可能访问超出紧凑缓冲区的内存，导致未定义行为。

**教训**：Windows 的 `fd_set` 不适合做紧凑分配。保持标准 `fd_set` 栈分配，连接中的 socket 数量极少，不会触及 FD_SETSIZE 限制。

#### 坑 4：零字节操作失败时不能派发 IEV_ERROR

**现象**：下载立即失败，报 "Network problem has occurred"。

**原因**：c-ares 的 `removeSocketEvents` + `addSocketEvents` 周期中，旧 socket 可能已被 c-ares 关闭。对已关闭的 socket 调用 `WSARecv` 返回 `WSAENOTSOCK`（10038）；对未连接的 TCP socket 调用 `WSARecv` 返回 `WSAENOTCONN`（10057）。这些是 c-ares socket 生命周期管理的正常瞬态错误。

如果在 `postZeroByteRecv` 失败时派发 `IEV_ERROR`，会导致 `CommandEvent::processEvents` 调用 `command_->errorEventReceived()`，进而使 AbstractCommand 中止下载。

**教训**：零字节操作失败应仅记录 DEBUG 日志，不能派发错误事件。socket 的实际错误状态由 Command 在后续 `recv()`/`send()` 中自行检测，或通过超时机制兜底。
