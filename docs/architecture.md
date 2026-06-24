# aria2-zero 架构文档

## 1. 系统总览

aria2-zero 采用**单线程事件驱动架构**，以 Command 模式为核心抽象。所有网络 I/O、协议处理、BT 对等通信和 RPC 请求处理都在同一个事件循环中完成，通过 EventPoll 进行 I/O 多路复用。仅文件分配和 DNS 解析等阻塞操作通过 ThreadPool 卸载到工作线程。

```
┌─────────────────────────────────────────────────────────────┐
│                  应用层 / RPC 客户端 / libaria2               │
├─────────────────────────────────────────────────────────────┤
│              RPC 方法分发 (JSON-RPC / XML-RPC)                │
├─────────────────────────────────────────────────────────────┤
│            主事件循环 (DownloadEngine::run)                   │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌─────────────┐  │
│  │ routine  │  │ commands │  │ priority │  │  EventPoll  │  │
│  │ Commands │  │          │  │ Commands │  │(epoll/kq/..)│  │
│  └──────────┘  └──────────┘  └──────────┘  └─────────────┘  │
├────────┬────────┬────────┬────────┬────────┬────────────────┤
│  HTTP  │  FTP   │  BT    │  DHT   │  SFTP  │  File I/O      │
│  Cmds  │  Cmds  │  Cmds  │  Cmds  │  Cmds  │  (ThreadPool)  │
├────────┴────────┴────────┴────────┴────────┴────────────────┤
│                  存储层 (DiskAdaptor)                        │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────────┐   │
│  │SegmentMan    │  │ PieceStorage │  │ MultiDiskAdaptor │   │
│  └──────────────┘  └──────────────┘  └──────────────────┘   │
├─────────────────────────────────────────────────────────────┤
│              EventPoll 抽象层                                │
│  ┌────────┐ ┌────────┐ ┌────────┐ ┌──────┐ ┌──────────┐     │
│  │ epoll  │ │ kqueue │ │ select │ │ poll │ │  libuv   │     │
│  │(Linux) │ │(macOS) │ │ (通用)  │ │      │ │ (可选)   │     │
│  └────────┘ └────────┘ └────────┘ └──────┘ └──────────┘     │
└─────────────────────────────────────────────────────────────┘
```

## 2. 核心组件

### 2.1 DownloadEngine — 事件循环核心

**文件**: `src/core/DownloadEngine.h`, `src/core/DownloadEngine.cc`

DownloadEngine 是整个系统的心脏，拥有并管理：

| 成员 | 类型 | 职责 |
|------|------|------|
| `eventPoll_` | `unique_ptr<EventPoll>` | I/O 多路复用 |
| `commands_` | `deque<unique_ptr<Command>>` | 普通命令队列 |
| `routineCommands_` | `deque<unique_ptr<Command>>` | 每轮执行的例行命令 |
| `requestGroupMan_` | `unique_ptr<RequestGroupMan>` | 下载任务管理 |
| `fileAllocationMan_` | `unique_ptr<FileAllocationMan>` | 文件预分配管理 |
| `checkIntegrityMan_` | `unique_ptr<CheckIntegrityMan>` | 完整性校验管理 |
| `socketPool_` | `multimap<string, SocketPoolEntry>` | 连接复用池 |
| `threadPool_` | `unique_ptr<ThreadPool>` | 阻塞 I/O 工作线程 (4 线程) |
| `dnsCache_` | `unique_ptr<DNSCache>` | DNS 解析缓存 |
| `btRegistry_` | `unique_ptr<BtRegistry>` | BitTorrent 全局注册表 |

### 2.2 Command — 任务抽象

**文件**: `src/core/Command.h`

Command 是所有操作的基类。每个 Command 拥有：
- 唯一标识 `cuid_t` (int64_t)
- 状态 (`STATUS_INACTIVE` / `STATUS_ACTIVE` / `STATUS_REALTIME`)
- 优先级 (`PRIORITY_NORMAL` / `PRIORITY_HIGH`)
- 事件回调 (`readEventReceived()` / `writeEventReceived()` / `errorEventReceived()`)

核心方法 `execute()` 返回 `true` 表示完成（删除命令），`false` 表示需要继续（重新入队）。

### 2.3 RequestGroup — 下载任务

**文件**: `src/core/RequestGroup.h`

一个 RequestGroup 代表一个下载任务（一个 URL 或一组镜像 URL），包含：
- `DownloadContext` — 文件元信息（大小、分片哈希等）
- `SegmentMan` — 分段管理器（将文件分为多个字节范围并行下载）
- `PieceStorage` — 分片存储（跟踪哪些分片已下载）
- 状态: `STATE_WAITING` → `STATE_ACTIVE`

### 2.4 EventPoll — I/O 多路复用

**文件**: `src/poll/EventPoll.h`

抽象接口，事件类型：
- `EVENT_READ` — Socket 可读
- `EVENT_WRITE` — Socket 可写
- `EVENT_ERROR` — Socket 错误
- `EVENT_HUP` — 连接断开

平台实现:

| 实现 | 平台 | 文件 |
|------|------|------|
| EpollEventPoll | Linux | `src/poll/epoll/` |
| KqueueEventPoll | macOS/BSD | `src/poll/kqueue/` |
| SelectEventPoll | 通用 | `src/poll/select/` |
| PollEventPoll | POSIX | `src/poll/poll/` |
| PortEventPoll | Solaris | `src/poll/port/` |
| LibuvEventPoll | 可选 | `src/poll/libuv/` |

### 2.5 事件系统 (Event.h)

**文件**: `src/core/Event.h`

模板化的事件处理系统：

```
EventPoll
  └── SocketEntry (每个 socket 一个)
       ├── CommandEvent (READ) → Command A
       ├── CommandEvent (WRITE) → Command B
       └── ADNSEvent → AsyncNameResolver
```

- `CommandEvent<SocketEntry, EventPoll>` — 关联 socket 事件和 Command
- `ADNSEvent<SocketEntry, EventPoll>` — 异步 DNS 事件
- `SocketEntry` — 管理单个 socket 上的多个事件监听

## 3. 主循环详解

```
DownloadEngine::run(bool oneshot):
  while (commands 非空 || routineCommands 非空):
    ┌─ 1. waitData()
    │     eventPoll_->poll(timeout)
    │     // timeout = noWait ? 0ms : refreshInterval (默认 1s)
    │
    ├─ 2. calculateStatistics()
    │     更新传输速率统计
    │
    ├─ 3. executeCommand(commands_, statusFilter)
    │     if 刷新间隔已过:
    │       执行所有命令 (STATUS_ALL)
    │     else:
    │       仅执行 ACTIVE/REALTIME 命令
    │
    │     对每个命令:
    │       transitStatus()     // 更新内部状态
    │       if execute() == true:
    │         删除命令 (已完成)
    │       else:
    │         清除 I/O 事件标志, 重新入队
    │
    ├─ 4. executeCommand(routineCommands_, STATUS_ALL)
    │     例行命令始终执行
    │
    └─ 5. afterEachIteration()
          处理关闭信号 (SIGINT/SIGTERM)
          halted → 对所有 RequestGroup 请求停止

  onEndOfRun()    // 清理, 关闭文件, 保存状态
```

**关键时间常量**:
- `A2_DELTA_MILLIS = 300ms` — 主循环迭代间隔
- `DEFAULT_REFRESH_INTERVAL = 1s` — 状态刷新间隔

## 4. 协议命令链

### 4.1 HTTP 下载命令链

```
HttpInitiateConnectionCommand
  │ DNS 解析 + 连接建立
  ↓
HttpRequestCommand
  │ 发送 HTTP 请求头 (包括 Range)
  ↓
HttpResponseCommand
  │ 接收并解析 HTTP 响应头
  ↓
HttpDownloadCommand (extends DownloadCommand)
  │ 读取响应体, 通过 StreamFilter 处理 (chunked/gzip)
  │ 写入 PieceStorage → DiskAdaptor
  ↓
CheckIntegrityCommand
    校验下载数据完整性
```

### 4.2 FTP 下载命令链

```
FtpInitiateConnectionCommand
  │ DNS 解析 + 连接建立
  ↓
FtpNegotiationCommand
  │ FTP 控制通道协商 (USER/PASS/TYPE/SIZE/REST/RETR)
  ↓
FtpDownloadCommand (extends DownloadCommand)
  │ 数据通道读取
  ↓
CheckIntegrityCommand
```

### 4.3 BitTorrent 命令链

```
BtInitiateConnectionCommand
  │ 连接到 peer
  ↓
PeerInteractionCommand
  │ 握手 → 持续消息交换
  │ 通过 DefaultBtInteractive 管理:
  │   - 握手 (handshake)
  │   - 状态消息 (choke/unchoke/interested)
  │   - 分片请求 (request/piece/cancel)
  │   - 扩展消息 (extended/metadata)
  ↓
DefaultBtMessageDispatcher
    messageQueue_ (待发送消息队列)
    requestSlots_ (待响应请求槽位)
```

### 4.4 AbstractCommand — 协议命令基类

**文件**: `src/core/AbstractCommand.h`, `src/core/AbstractCommand.cc`

所有协议命令的共同基类，管理：
- Socket 事件注册/注销 (`setReadCheckSocket()` / `setWriteCheckSocket()`)
- 超时检测 (`checkPoint_` 计时器)
- DNS 解析 (`resolveHostname()` 同步/异步)
- 连接验证 (`checkIfConnectionEstablished()`)
- 重试逻辑 (`prepareForRetry()` → 创建 CreateRequestCommand)
- 分段管理 — 通过 SegmentMan 获取/释放下载分段

`execute()` 实现流程:
```
1. shouldProcess() → 检查 socket 是否就绪 (可读/可写/有缓冲数据)
2. 如果就绪 → 调用 executeInternal() (子类实现协议逻辑)
3. 如果超时 → 标记服务器错误, 触发重试
4. 返回 true (完成) 或 false (继续)
```

## 5. 存储层

### 5.1 层次结构

```
PieceStorage (分片跟踪)
  └── DiskAdaptor (磁盘 I/O 抽象)
       ├── DirectDiskAdaptor (单文件)
       └── MultiDiskAdaptor (多文件)
            └── DiskWriter (实际文件 I/O)
                 └── DefaultDiskWriter / AbstractDiskWriter
```

### 5.2 分段下载 (SegmentMan)

**文件**: `src/core/SegmentMan.h`

SegmentMan 将文件分为多个字节范围分段，分配给不同的 Command 并行下载：

```
文件 [0 ──────────────────────── 100MB]
      [Seg0: 0-25M] [Seg1: 25-50M] [Seg2: 50-75M] [Seg3: 75-100M]
       ↓              ↓              ↓              ↓
      Cmd A          Cmd B          Cmd C          Cmd D
```

关键方法：
- `getSegment(cuid, minSplitSize)` — 为命令分配未完成分段
- `completeSegment(cuid, segment)` — 标记分段完成
- `cancelSegment(cuid)` — 连接失败时释放分段

### 5.3 文件预分配 (FileAllocationMan)

**文件**: `src/storage/FileAllocationMan.h`

`FileAllocationMan = SequentialPicker<FileAllocationEntry>`

按顺序执行文件预分配，每次分配一个文件。分配策略：
- `AdaptiveFileAllocationIterator` — 优先 fallocate，失败回退 truncate
- `FallocFileAllocationIterator` — Linux fallocate 系统调用
- `TruncFileAllocationIterator` — POSIX truncate

通过 `FileAllocationCommand` 在主循环中增量执行 (`allocateChunk()`)。

### 5.4 完整性校验 (CheckIntegrityMan)

**文件**: `src/core/CheckIntegrityMan.h`

`CheckIntegrityMan = SequentialPicker<CheckIntegrityEntry>`

下载完成后校验数据完整性：
1. `validateChunk()` — 增量哈希校验
2. 校验通过 → `onDownloadFinished()` (如 BT 做种)
3. 校验失败 → `onDownloadIncomplete()` (重新下载损坏分片)

### 5.5 磁盘缓存 (WrDiskCache)

**文件**: `src/storage/WrDiskCache.h`

写缓存层，批量合并小写入操作以减少系统调用次数。

## 6. RPC 子系统

### 6.1 架构

```
HTTP/WebSocket 请求
  ↓
HttpServerCommand (接收请求)
  ↓
RpcMethodFactory::getMethod(methodName)
  ↓
RpcMethod::process(RpcRequest, DownloadEngine*)
  ↓
RpcResponse (返回结果)
```

### 6.2 RPC 方法列表

| 分类 | 方法 |
|------|------|
| 下载控制 | `addUri`, `addTorrent`, `addMetalink`, `remove`, `pause`, `unpause` |
| 状态查询 | `tellStatus`, `tellActive`, `tellWaiting`, `tellStopped` |
| 详情查询 | `getUris`, `getFiles`, `getServers`, `getPeers` |
| 配置管理 | `getOption`, `changeOption`, `getGlobalOption`, `changeGlobalOption` |
| 系统控制 | `shutdown`, `forceShutdown`, `saveSession`, `getVersion` |
| 统计信息 | `getGlobalStat`, `getSessionInfo` |
| 批量操作 | `system.multicall`, `system.listMethods` |

### 6.3 集成方式

RPC 方法**不在独立线程**中执行。它们在主循环中被调用，直接操作 DownloadEngine 的状态：
- `addUri()` → 创建 RequestGroup → 添加到 RequestGroupMan 等待队列
- `pause()` → 在 RequestGroup 上设置暂停标志
- `tellStatus()` → 读取 RequestGroup 和 SegmentMan 状态

## 7. BitTorrent 子系统

### 7.1 组件结构

```
BtRegistry (全局注册表)
  └── BtObject (每个种子一个)
       ├── DownloadContext — 种子文件元信息
       ├── PieceStorage — 分片数据存储
       ├── PeerStorage — 对等节点管理
       ├── BtAnnounce — Tracker 通告
       └── BtRuntime — 运行时状态
```

### 7.2 对等连接管理

每个 peer 连接由一个 `PeerInteractionCommand` 管理：
- `DefaultBtInteractive` — 实现 BT 交互协议
- `DefaultBtMessageDispatcher` — 消息队列和请求槽位管理
  - `messageQueue_`: 待发送的 BT 消息 (Request, Piece, Have 等)
  - `requestSlots_`: 未响应的请求追踪

所有 peer 通信在主线程完成，通过 EventPoll 监听 socket 事件触发消息收发。

### 7.3 DHT (分布式哈希表)

DHT 使用 UDP 通信，同样作为 Command 在主循环中执行。

## 8. ThreadPool 使用场景

**文件**: `compat/ThreadPool.h`

ThreadPool 初始化为 **4 个工作线程**，仅用于以下阻塞操作：

| 场景 | 说明 |
|------|------|
| 文件预分配 | fallocate/truncate 可能阻塞 |
| DNS 解析 | 同步 DNS 查询的异步包装 |
| 磁盘 I/O | 大文件读写操作 |

ThreadPool **不**处理网络事件或协议逻辑。所有网络 I/O 通过 EventPoll 非阻塞完成。

## 9. 关闭流程

```
SIGINT (Ctrl+C):
  第 1 次 → globalHaltRequested = 1 → 优雅停止所有下载
  第 2 次 → globalHaltRequested = 2 → 更积极的停止
  第 3 次 → globalHaltRequested = 3 → 强制退出

SIGTERM:
  → globalHaltRequested = 3 → 强制退出

DownloadEngine::afterEachIteration():
  halt=1 → setNoWait(true), requestHalt() 所有 RequestGroup
  halt=3 → requestForceHalt(), 紧急关闭
```

## 10. 公共库 API

**文件**: `include/aria2/aria2.h`

aria2-zero 可作为库嵌入使用：

```cpp
#include <aria2/aria2.h>

aria2::libraryInit();
aria2::Session* session = aria2::sessionNew(options, config);

// 添加下载
aria2::A2Gid gid;
aria2::addUri(session, &gid, uris, options);

// 运行事件循环
while (aria2::run(session, aria2::RUN_ONCE)) {
    // 处理回调/状态更新
}

aria2::sessionFinal(session);
aria2::libraryDeinit();
```

`run()` 的 `RUN_ONCE` 模式对应 `DownloadEngine::run(oneshot=true)`，执行一次主循环迭代后返回。

## 11. TLS 实现

| 实现 | 平台 | 文件 |
|------|------|------|
| OpenSSL/QUICTLS/LibreSSL | Linux/Windows | `src/tls/libssl/` |
| WinTLS (SChannel) | Windows | `src/tls/wintls/` |
| Apple Security | macOS/iOS | `src/tls/apple/` |
| GnuTLS | Linux | `src/tls/gnutls/` |

默认使用 QUICTLS (支持 TLS 1.3)，可通过 `use_quictls=false` 切换到 LibreSSL。
