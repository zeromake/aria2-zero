# 从零构建多核下载引擎：架构选型与详细设计

> 探讨性延伸文档 — 假设不受 aria2 历史代码约束，从零设计一个高性能多协议下载工具的最优多核架构。

---

## 一、架构候选方案对比

### 1.1 四种候选架构

| 架构 | 核心思想 | 代表项目 |
|------|---------|---------|
| **纯 Actor 模型** | 每个实体是独立 Actor，通过 mailbox 通信，零共享状态 | Erlang/OTP, Akka |
| **协程 + 线程亲和** | 每连接一个协程，同任务协程绑定同一线程，跨线程走 Channel | Go net/http, tokio |
| **多线程 + 共享锁** | 传统多线程，共享数据结构加 mutex/rwlock 保护 | 传统 C++ 服务器 |
| **单线程事件驱动** | 单线程主循环 + 异步 I/O，工作线程仅处理阻塞操作 | aria2, Redis, nginx (单 worker) |

### 1.2 下载工具场景的特殊约束

下载工具不同于一般的网络服务器，有以下独特特征：

**高频共享状态访问**：BT 下载中，同一 torrent 的所有 peer 连接需要频繁访问共享的分片位图（bitfield）、分片稀有度统计、已用分片集合。每收到一个 block 就要更新这些状态。一个活跃 torrent 可能有 200+ peer 连接，每秒产生数千次共享状态访问。

**跨连接协调**：BT 的 Rarest-First 分片选择、Choke/Unchoke 算法、EndGame 模式都需要跨所有 peer 连接的全局视图。这不是简单的"每连接独立处理"。

**混合 I/O 模式**：网络 I/O（小包高频）和磁盘 I/O（大块低频）并存。网络 I/O 适合非阻塞多路复用，磁盘 I/O 适合异步提交（io_uring/IOCP）。

**多协议异构**：HTTP 连接是无状态的请求-响应，FTP 是有状态的控制+数据双通道，BT 是全双工的持久连接。不同协议对并发模型的需求不同。

### 1.3 评估矩阵

| 评估维度 | 纯 Actor | 协程+线程亲和 | 多线程+锁 | 单线程事件驱动 |
|---------|---------|-------------|---------|--------------|
| BT 分片热路径性能 | **差** — PieceManager Actor 成为串行瓶颈 | **优** — 同线程内直接访问，零开销 | **中** — 锁竞争 | **优** — 无锁 |
| 多核利用率 | **优** — 天然分布 | **优** — per-core EventLoop | **优** — 但锁竞争限制扩展性 | **差** — 单核 |
| 代码复杂度 | **中** — 消息定义多，跨 Actor 事务复杂 | **低** — 协程代码接近同步风格 | **高** — 锁顺序、死锁防护 | **低** — 无并发 |
| 故障隔离 | **优** — Actor 独立崩溃重启 | **中** — 线程级隔离 | **差** — 一个线程崩溃影响全局 | **差** — 单点故障 |
| 跨组件事务 | **差** — 需要 saga/补偿 | **优** — 同线程内直接函数调用 | **中** — 需要跨锁事务 | **优** — 顺序执行 |
| 延迟抖动 | **中** — mailbox 排队 | **低** — 直接调度 | **高** — 锁等待 | **低** — 确定性 |

### 1.4 结论

**协程 + 线程亲和 + Channel** 是下载工具场景下的最优架构。它在热路径上保持了单线程架构的零开销优势（同一下载任务的所有连接在同一线程内，分片管理无锁），同时通过多个 EventLoop 线程实现多核利用，跨线程通信通过类型安全的 Channel 完成。

---

## 二、推荐架构：协程 + 分层隔离

### 2.1 总体架构图

```
┌────────────────────────────────────────────────────────────┐
│                   Coordinator Thread (主线程)                │
│                                                            │
│  ┌────────────────┐  ┌────────────────────────────────┐   │
│  │ Task Scheduler │  │ Global Rate Limiter             │   │
│  │ (任务分配/迁移) │  │ (令牌桶, 批量分发)               │   │
│  └───────┬────────┘  └──────────┬─────────────────────┘   │
└──────────┼──────────────────────┼─────────────────────────┘
           │                      │
     ┌─────▼──────────────────────▼──────┐
     │          Channel Bus              │
     │     (MPMC lock-free ring buffers) │
     └──┬─────┬──────┬──────┬───────┬───┘
        │     │      │      │       │
  ┌─────▼──┐ ┌▼────┐ ┌▼────┐ ┌─────▼──────┐ ┌────▼────────┐
  │IO Wkr 0│ │Wkr 1│ │Wkr 2│ │ DiskIO Pool│ │ RPC Thread  │
  │        │ │     │ │     │ │            │ │             │
  │EventLoop│ │    │ │     │ │io_uring/   │ │ HTTP Listen │
  │ + 协程  │ │    │ │     │ │IOCP        │ │ WebSocket   │
  │        │ │     │ │     │ │            │ │ JSON-RPC    │
  │私有状态:│ │     │ │     │ │            │ │             │
  │TaskA   │ │TaskB│ │TaskC│ │            │ │ 状态快照缓存 │
  └────────┘ └─────┘ └─────┘ └────────────┘ └─────────────┘
```

### 2.2 核心设计原则

| 原则 | 含义 | 理由 |
|------|------|------|
| **任务亲和性** | 同一下载任务的所有连接绑定到同一 IO 线程 | 分片管理、peer 交互等热路径无需跨线程，零锁开销 |
| **协程即连接** | 每个网络连接是一个协程，代码像同步写法 | 消除回调地狱，状态机自然表达为顺序代码 |
| **Channel 跨边界** | 线程间通信只走 Channel，不共享可变状态 | 编译期保证线程安全，无死锁风险 |
| **I/O 分层** | 网络 I/O (EventLoop) 和磁盘 I/O (io_uring/IOCP) 分离 | 各自使用最优的系统 API |
| **批量汇总** | 统计/限速等全局操作通过批量消息而非逐次通知 | 减少跨线程消息数量 |

### 2.3 线程模型

```
线程类型          数量              职责
─────────────────────────────────────────────────────────
Coordinator      1                任务调度、全局协调、限速令牌分发
IO Worker        N (= CPU cores)  网络 I/O + 协议处理
DiskIO           M (= 2~4)       磁盘读写、文件分配、校验
RPC              1                JSON-RPC 服务、状态查询、WebSocket 推送
DNS Resolver     1                专用 DNS 解析线程 (c-ares)
```

**IO Worker 线程数量选择**：默认 `std::thread::hardware_concurrency() - 2`（留一个给 Coordinator，一个给 DiskIO），最少 2 个，最多 16 个。每个 IO Worker 拥有完全独立的 EventLoop，不与其他 IO Worker 共享任何可变状态。

**RPC 为何独立线程**：RPC 有自己的 HTTP/WebSocket 监听 I/O，JSON 序列化/反序列化是 CPU 密集型操作（`tellActive` 可能涉及数百个任务的 JSON 构建），前端 UI 通常每 1-2 秒轮询一次。放在 Coordinator 中会阻塞任务调度和限速令牌分发。独立线程的额外开销可忽略，但避免了 RPC 序列化阻塞协调逻辑的问题。RPC 线程维护本地状态快照缓存，各 IO Worker 每 500ms 推送一次状态摘要，查询类请求直接读缓存实现零跨线程通信。

---

## 三、各子系统详细设计

### 3.1 协程运行时

#### 协程类型定义

```cpp
// 可等待的异步任务
template<typename T>
class Task {
    // C++20 coroutine promise_type
    // co_await 时让出执行权给 EventLoop
    // 完成时自动恢复等待者
};

// 每个网络连接是一个协程
Task<void> handle_http_connection(TcpStream stream, DownloadTask& task) {
    auto request = co_await build_request(task);
    co_await stream.write(request.serialize());

    HttpResponseParser parser;
    while (!parser.is_complete()) {
        auto buf = co_await stream.read();
        parser.feed(buf);
    }

    auto response = parser.finish();
    // ... 处理响应头、开始下载数据
    while (auto segment = task.segment_man().get_segment()) {
        auto data = co_await stream.read_exact(segment.length());
        task.piece_storage().write_block(segment, data);
        disk_channel.send(WriteRequest{segment, data});  // 异步写盘
    }
}

// BT peer 连接也是一个协程
Task<void> handle_bt_peer(TcpStream stream, BtContext& ctx) {
    co_await bt_handshake(stream, ctx);

    while (true) {
        auto msg = co_await read_bt_message(stream);

        switch (msg.type()) {
        case BtMessageType::Piece:
            // 直接访问 ctx.piece_storage — 同线程，无锁
            ctx.piece_storage().complete_block(msg.index(), msg.begin(), msg.data());
            break;
        case BtMessageType::Have:
            ctx.peer_storage().update_bitfield(peer_id, msg.index());
            // 触发分片请求逻辑 — 同线程内直接调用
            co_await request_pieces(stream, ctx);
            break;
        // ...
        }
    }
}
```

#### EventLoop 设计

每个 IO Worker 线程运行一个 EventLoop：

```cpp
class EventLoop {
public:
    // 注册 socket 事件监听
    void register_io(SocketHandle fd, IoInterest interest, CoroutineHandle coro);

    // 注册定时器
    TimerHandle set_timer(Duration delay, CoroutineHandle coro);

    // 从 Channel 接收消息（非阻塞，集成到 poll 中）
    void register_channel_receiver(ChannelReceiver& rx);

    // 主循环
    void run() {
        while (!shutdown_) {
            // 1. poll 网络事件 + channel 事件 + 定时器
            auto events = poller_.poll(timeout);

            // 2. 恢复就绪的协程
            for (auto& event : events) {
                event.coroutine.resume();
            }

            // 3. 处理 channel 消息（任务分配、限速令牌等）
            process_channel_messages();

            // 4. 执行定时任务（统计上报、keepalive 等）
            fire_expired_timers();
        }
    }

private:
    Poller poller_;         // epoll/kqueue/io_uring/IOCP 封装
    TimerWheel timers_;     // 时间轮定时器
    bool shutdown_ = false;
};
```

#### 平台 Poller 封装

```cpp
// 统一的底层 I/O 多路复用抽象
class Poller {
public:
    // 不同平台的最优实现
    // Linux:   io_uring (优先) 或 epoll
    // Windows: IOCP
    // macOS:   kqueue

    // 核心方法
    void add(SocketHandle fd, IoInterest interest, Token token);
    void modify(SocketHandle fd, IoInterest interest, Token token);
    void remove(SocketHandle fd);
    PollResult poll(Duration timeout);

    // 集成 Channel 通知
    // Linux:   eventfd
    // Windows: PostQueuedCompletionStatus
    // macOS:   EVFILT_USER
    void register_wakeup(WakeupHandle handle);
};
```

### 3.2 Channel 通信系统

#### Channel 类型

```cpp
// 多生产者单消费者 (MPSC) — 最常用
// 多个 IO 线程向 Coordinator 发送统计/事件
template<typename T>
class MpscChannel {
    Sender<T> sender();     // 可 clone，多线程安全
    Receiver<T> receiver(); // 单消费者
};

// 单生产者单消费者 (SPSC) — 最快
// Coordinator 向特定 IO 线程发送命令
template<typename T>
class SpscChannel {
    // lock-free ring buffer, cache-line padded
    // 适合 Coordinator → 特定 IO Worker 的专用通道
};

// 广播 Channel — 全局事件
// 如 shutdown 信号、全局配置变更
template<typename T>
class BroadcastChannel {
    // 每个 receiver 维护独立的读指针
    // seq-lock 或 epoch-based 实现
};
```

#### 消息类型定义

```cpp
// Coordinator → IO Worker
struct TaskAssignment {
    TaskId task_id;
    DownloadSpec spec;       // URL / torrent info / metalink
    RateLimitTokens tokens;  // 初始限速令牌
};

struct TaskCommand {
    TaskId task_id;
    enum { Pause, Resume, Cancel, ChangeOption } action;
    Option new_option;       // 仅 ChangeOption 时有值
};

struct RateLimitGrant {
    size_t bytes_allowed;    // 批量授予的字节配额
};

// IO Worker → Coordinator
struct TaskProgress {
    TaskId task_id;
    uint64_t downloaded_bytes;
    uint64_t uploaded_bytes;
    uint32_t connections;
    SpeedSample speed;       // 聚合后的速度采样
};

struct TaskCompleted {
    TaskId task_id;
    Result<CompletionInfo, ErrorInfo> result;
};

struct RpcResponse {
    RequestId req_id;
    JsonValue result;
};

// IO Worker → DiskIO
struct DiskWriteRequest {
    FileHandle file;
    int64_t offset;
    OwnedBuffer data;        // 所有权转移，零拷贝
    std::optional<CompletionCallback> on_done;
};

struct DiskReadRequest {
    FileHandle file;
    int64_t offset;
    size_t length;
    CompletionCallback on_done;
};

// DiskIO → IO Worker
struct DiskWriteComplete {
    TaskId task_id;
    int64_t offset;
    Result<size_t, IoError> result;
};
```

#### Channel 使用模式

```
Coordinator ──SPSC──→ IO Worker #0       (任务分配、命令、限速令牌)
Coordinator ──SPSC──→ IO Worker #1
Coordinator ──SPSC──→ IO Worker #2

IO Worker #0 ──MPSC──→ Coordinator       (进度上报、任务完成)
IO Worker #1 ──MPSC──→ Coordinator       (共用同一个 MPSC)
IO Worker #2 ──MPSC──→ Coordinator

IO Worker #0 ──MPSC──→ RPC Thread        (状态快照推送, 每 500ms)
IO Worker #1 ──MPSC──→ RPC Thread
IO Worker #2 ──MPSC──→ RPC Thread

RPC Thread   ──SPSC──→ Coordinator       (addUri/remove/pause 等修改请求)
Coordinator  ──SPSC──→ RPC Thread        (修改请求的执行结果)
Coordinator  ──Broadcast──→ RPC Thread   (TaskCompleted 等事件 → WS 推送)

IO Worker #0 ──SPSC──→ DiskIO Pool       (写入请求)
DiskIO Pool  ──SPSC──→ IO Worker #0      (写入完成通知)

Coordinator ──Broadcast──→ All           (shutdown、全局配置变更)
```

### 3.3 任务调度与亲和性

#### 任务分配策略

```cpp
class TaskScheduler {
public:
    // 将新下载任务分配到负载最低的 IO Worker
    WorkerId assign_task(const DownloadSpec& spec) {
        WorkerId target = find_least_loaded_worker();

        // 考虑亲和性：同一服务器的多个下载优先分配到同一线程
        // 这样可以复用 TCP 连接池和 DNS 缓存
        if (auto affinity = find_server_affinity(spec.host())) {
            if (workers_[*affinity].load() < threshold_) {
                target = *affinity;
            }
        }

        worker_channels_[target].send(TaskAssignment{
            .task_id = next_task_id_++,
            .spec = spec,
        });
        return target;
    }

private:
    // 每个 worker 的当前负载（连接数 + 带宽占用）
    WorkerId find_least_loaded_worker() {
        return std::min_element(worker_loads_.begin(), worker_loads_.end())
               - worker_loads_.begin();
    }

    // 服务器亲和性映射：host → 上次分配的 worker
    std::unordered_map<std::string, WorkerId> server_affinity_;
};
```

#### 大型 BT 任务的特殊处理

一个活跃 torrent 可能有 200+ peer 连接。如果全部绑定到一个 IO Worker，该线程会成为瓶颈。解决方案：

```cpp
// BT 任务可以跨多个 IO Worker，但分片管理仍然由一个"所有者"线程负责
class BtTaskDistributor {
    WorkerId owner_worker_;          // 拥有 PieceStorage 的线程
    std::vector<WorkerId> peer_workers_;  // peer 连接可以分布到多个线程

    // peer 线程通过 Channel 向 owner 请求分片
    // owner 线程分配后通过 Channel 返回
    SpscChannel<PieceRequest> piece_request_channels_[MAX_WORKERS];
    SpscChannel<PieceGrant>   piece_grant_channels_[MAX_WORKERS];
};
```

```
                    ┌─────────────────────────┐
                    │   IO Worker #0 (Owner)  │
                    │                         │
                    │   PieceStorage (私有)     │
                    │   BitfieldMan  (私有)     │
                    │   PieceStatMan (私有)     │
                    │                         │
                    │   Peer A ←───────┐      │
                    │   Peer B ←───┐   │      │
                    └──────────────┼───┼──────┘
                                   │   │
            ┌──── piece_grant ─────┘   │
            │                          │
            │   piece_request ────►    │
            │                          │
    ┌───────▼────────────┐    ┌────────▼───────────┐
    │  IO Worker #1      │    │  IO Worker #2      │
    │                    │    │                    │
    │  Peer C            │    │  Peer E            │
    │  Peer D            │    │  Peer F            │
    │                    │    │  Peer G            │
    │  (只处理网络 I/O,   │    │  (只处理网络 I/O,   │
    │   分片请求转发      │    │   分片请求转发      │
    │   给 Owner)        │    │   给 Owner)        │
    └────────────────────┘    └────────────────────┘
```

这种设计下：
- 分片选择、bitfield 更新、稀有度计算仍然是**单线程无锁**操作（在 Owner 线程内）
- 网络 I/O（BT 消息收发）分布在多个 Worker 线程
- 唯一的跨线程通信是分片请求/授予，频率远低于 bitfield 操作（一个分片包含多个 block，请求频率是 block 频率除以 blocks-per-piece）

### 3.4 磁盘 I/O 子系统

#### Linux: io_uring

```cpp
class IoUringDiskEngine {
public:
    IoUringDiskEngine(unsigned queue_depth = 256) {
        io_uring_queue_init(queue_depth, &ring_, 0);
    }

    // 提交写入请求 — 零拷贝
    void submit_write(int fd, const void* buf, size_t len, off_t offset,
                      CompletionToken token) {
        auto* sqe = io_uring_get_sqe(&ring_);
        io_uring_prep_write(sqe, fd, buf, len, offset);
        io_uring_sqe_set_data(sqe, token);
        io_uring_submit(&ring_);
    }

    // 提交预分配
    void submit_fallocate(int fd, off_t offset, off_t len,
                          CompletionToken token) {
        auto* sqe = io_uring_get_sqe(&ring_);
        io_uring_prep_fallocate(sqe, fd, 0, offset, len);
        io_uring_sqe_set_data(sqe, token);
        io_uring_submit(&ring_);
    }

    // 收割完成事件
    std::vector<CompletionEvent> reap() {
        std::vector<CompletionEvent> results;
        struct io_uring_cqe* cqe;
        while (io_uring_peek_cqe(&ring_, &cqe) == 0) {
            results.push_back({
                .token = io_uring_cqe_get_data(cqe),
                .result = cqe->res,
            });
            io_uring_cqe_seen(&ring_, cqe);
        }
        return results;
    }

private:
    struct io_uring ring_;
};
```

#### Windows: IOCP

```cpp
class IocpDiskEngine {
public:
    IocpDiskEngine() {
        iocp_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    }

    void associate_file(HANDLE file) {
        CreateIoCompletionPort(file, iocp_, 0, 0);
    }

    // 提交写入 — offset 在 OVERLAPPED 中指定，无需 seek
    void submit_write(HANDLE file, const void* buf, DWORD len, int64_t offset,
                      OverlappedContext* ctx) {
        ctx->overlapped.Offset = static_cast<DWORD>(offset);
        ctx->overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
        WriteFile(file, buf, len, NULL, &ctx->overlapped);
    }

    // 收割完成事件
    std::vector<CompletionEvent> reap(DWORD timeout_ms) {
        std::vector<CompletionEvent> results;
        OVERLAPPED_ENTRY entries[64];
        ULONG count = 0;
        GetQueuedCompletionStatusEx(iocp_, entries, 64, &count, timeout_ms, FALSE);
        for (ULONG i = 0; i < count; i++) {
            auto* ctx = CONTAINING_RECORD(entries[i].lpOverlapped,
                                          OverlappedContext, overlapped);
            results.push_back({
                .ctx = ctx,
                .bytes_transferred = entries[i].dwNumberOfBytesTransferred,
            });
        }
        return results;
    }

private:
    HANDLE iocp_;
};
```

#### 写缓存设计

```cpp
class WriteCache {
    // 每个 IO Worker 拥有独立的 WriteCache 实例 — 无锁
    // 缓存满或定时刷新时，批量提交到 DiskIO 线程

    struct CacheEntry {
        int64_t offset;
        OwnedBuffer data;
        Instant queued_at;
    };

    // 按文件分组的缓存
    std::unordered_map<FileId, std::vector<CacheEntry>> file_caches_;
    size_t total_cached_ = 0;
    size_t max_cache_size_;  // 默认 16MB per worker

public:
    void cache_write(FileId file, int64_t offset, OwnedBuffer data) {
        total_cached_ += data.size();
        file_caches_[file].push_back({offset, std::move(data), Instant::now()});

        if (total_cached_ >= max_cache_size_) {
            flush_largest_file();
        }
    }

    void flush_largest_file() {
        // 找到缓存最多的文件，合并相邻 block，批量提交
        auto it = std::max_element(file_caches_.begin(), file_caches_.end(),
            [](auto& a, auto& b) { return total_size(a.second) < total_size(b.second); });

        auto merged = merge_adjacent_blocks(it->second);
        for (auto& block : merged) {
            disk_channel_.send(DiskWriteRequest{
                .file = it->first,
                .offset = block.offset,
                .data = std::move(block.data),
            });
        }
        total_cached_ -= total_size(it->second);
        it->second.clear();
    }
};
```

### 3.5 全局限速

传统做法是在主循环中直接操作 EventPoll 的 socket 事件注册（aria2 当前方案），这与多线程架构不兼容。推荐**令牌桶 + 批量分发**。

```cpp
// Coordinator 线程拥有令牌桶
class GlobalRateLimiter {
    TokenBucket download_bucket_;  // 全局下载限速
    TokenBucket upload_bucket_;    // 全局上传限速

    // 每 50ms 向各 IO Worker 批量分发令牌
    // 而非每个 read/write 调用都跨线程请求
    void distribute_tokens() {
        size_t total_available = download_bucket_.drain();
        // 按各 worker 的活跃连接数比例分配
        for (auto& worker : workers_) {
            size_t grant = total_available * worker.weight() / total_weight;
            worker.token_channel.send(RateLimitGrant{grant});
        }
    }
};

// IO Worker 线程内的本地令牌池
class LocalTokenPool {
    size_t available_ = 0;  // 本线程私有，无锁

public:
    void refill(size_t tokens) { available_ += tokens; }

    // 协程在读/写前获取令牌
    // 令牌不足时协程挂起，等待下一轮分发
    Task<size_t> acquire(size_t requested) {
        while (available_ == 0) {
            co_await next_token_grant_;  // 等待 Coordinator 的下一轮分发
        }
        size_t granted = std::min(requested, available_);
        available_ -= granted;
        co_return granted;
    }
};
```

### 3.6 DNS 解析

```cpp
// 方案一：专用 DNS 线程 + Channel (推荐)
class DnsResolver {
    // 独立线程运行 c-ares 事件循环
    // 接收请求通过 MPSC Channel
    // 返回结果通过 per-worker SPSC Channel

    MpscReceiver<DnsRequest> requests_;
    std::array<SpscSender<DnsResult>, MAX_WORKERS> result_channels_;

    void run() {
        ares_channel channel;
        ares_init(&channel);

        while (!shutdown_) {
            // 处理新请求
            while (auto req = requests_.try_recv()) {
                ares_gethostbyname(channel, req->host.c_str(), AF_UNSPEC, callback, req);
            }
            // 驱动 c-ares 事件循环
            ares_process_fd(channel, ...);
            wait_for_events(channel, 10ms);
        }
    }
};

// IO Worker 中的使用方式
Task<AddrInfo> resolve(std::string_view host) {
    dns_channel_.send(DnsRequest{host, worker_id_});
    co_return co_await dns_result_receiver_.recv();
}
```

### 3.7 RPC 子系统（独立线程）

RPC Server 运行在独立线程，拥有自己的 EventLoop，负责所有 JSON-RPC 的网络 I/O、序列化和 WebSocket 推送。

**独立线程的理由：**
- JSON 序列化/反序列化是 CPU 密集型操作（`tellActive` 可能涉及数百任务的完整 JSON 构建）
- 前端 UI（AriaNg 等）通常每 1-2 秒轮询一次，高频序列化不应阻塞任务调度
- WebSocket 长连接的 keepalive、帧分片、GZip 压缩是独立的 I/O 工作
- 一个线程的额外开销可忽略，但阻塞协调逻辑的问题一旦出现很难排查

```cpp
class RpcServer {
    // 独立线程，拥有自己的 EventLoop
    EventLoop event_loop_;

    // 接收各 IO Worker 推送的状态快照
    MpscReceiver<TaskSnapshot> snapshot_receiver_;

    // 向 Coordinator 发送修改类请求
    SpscSender<RpcMutationRequest> mutation_sender_;

    // 接收 Coordinator 的执行结果
    SpscReceiver<RpcMutationResult> result_receiver_;

    // 接收 Coordinator 广播的事件（用于 WebSocket 推送）
    BroadcastReceiver<TaskEvent> event_receiver_;

    // 本地状态快照缓存 — RPC 线程私有，无锁
    std::unordered_map<TaskId, TaskSnapshot> snapshot_cache_;

    // 已连接的 WebSocket 客户端
    std::vector<WebSocketConnection> ws_clients_;

public:
    void run() {
        // 监听 HTTP + WebSocket
        auto listener = event_loop_.bind_tcp(rpc_port_);

        event_loop_.spawn(accept_loop(std::move(listener)));
        event_loop_.spawn(snapshot_updater());
        event_loop_.spawn(event_broadcaster());
        event_loop_.run();
    }

private:
    // 接受新连接
    Task<void> accept_loop(TcpListener listener) {
        while (true) {
            auto stream = co_await listener.accept();
            event_loop_.spawn(handle_connection(std::move(stream)));
        }
    }

    // 处理单个 RPC 连接
    Task<void> handle_connection(TcpStream stream) {
        // 判断是 HTTP 还是 WebSocket 升级
        auto first_bytes = co_await stream.peek();
        if (is_websocket_upgrade(first_bytes)) {
            co_await handle_websocket(std::move(stream));
        } else {
            co_await handle_http_rpc(std::move(stream));
        }
    }

    Task<void> handle_http_rpc(TcpStream stream) {
        while (true) {
            auto request_bytes = co_await stream.read();
            auto rpc_request = parse_json_rpc(request_bytes);

            JsonValue result;
            if (is_query(rpc_request)) {
                // 查询类：直接从本地快照缓存响应，零跨线程通信
                result = handle_query(rpc_request);
            } else {
                // 修改类：投递给 Coordinator，等待结果
                mutation_sender_.send(RpcMutationRequest{
                    .id = rpc_request.id,
                    .method = rpc_request.method,
                    .params = rpc_request.params,
                });
                auto mutation_result = co_await result_receiver_.recv();
                result = mutation_result.value;
            }

            // JSON 序列化 + 可选 GZip 压缩 — 全在 RPC 线程完成
            auto response_bytes = serialize_response(rpc_request.id, result);
            if (client_accepts_gzip) {
                response_bytes = gzip_compress(response_bytes);
            }
            co_await stream.write(response_bytes);
        }
    }

    // 查询类请求处理 — 全部从本地缓存读取
    JsonValue handle_query(const RpcRequest& req) {
        if (req.method == "aria2.tellActive") {
            JsonArray arr;
            for (auto& [id, snap] : snapshot_cache_) {
                if (snap.status == TaskStatus::Active) {
                    arr.push_back(snapshot_to_json(snap));
                }
            }
            return arr;
        }
        if (req.method == "aria2.tellStatus") {
            auto it = snapshot_cache_.find(req.task_id());
            if (it != snapshot_cache_.end()) {
                return snapshot_to_json(it->second);
            }
            return make_error("task not found");
        }
        if (req.method == "aria2.getGlobalStat") {
            return global_stat_to_json(global_snapshot_);
        }
        // ...
    }

    // 定期从 Channel 接收 IO Worker 推送的快照更新
    Task<void> snapshot_updater() {
        while (true) {
            auto snapshot = co_await snapshot_receiver_.recv();
            snapshot_cache_[snapshot.task_id] = snapshot;  // 本线程私有，无锁
        }
    }

    // WebSocket 事件广播
    Task<void> event_broadcaster() {
        while (true) {
            auto event = co_await event_receiver_.recv();
            auto json = event_to_json(event);
            // 广播给所有 WebSocket 客户端
            for (auto& ws : ws_clients_) {
                ws.send(json);
            }
        }
    }

    Task<void> handle_websocket(TcpStream stream) {
        auto ws = co_await websocket_upgrade(std::move(stream));
        ws_clients_.push_back(ws);

        while (true) {
            auto frame = co_await ws.read();
            if (frame.is_close()) break;

            // WebSocket 上的 RPC 请求，处理方式与 HTTP 相同
            auto rpc_request = parse_json_rpc(frame.text());
            // ... 同 handle_http_rpc 的逻辑
        }

        std::erase(ws_clients_, ws);
    }
};
```

**数据流总结：**

```
IO Workers ──(每 500ms)──→ RPC Thread: TaskSnapshot    (状态推送)
                                │
                                ├─ tellActive/tellStatus → 直接读快照缓存
                                ├─ getGlobalStat         → 直接读全局快照
                                │
RPC Thread ──(修改请求)──→ Coordinator ──→ IO Worker: 执行
Coordinator ──(执行结果)──→ RPC Thread: 序列化后返回客户端
                                │
Coordinator ──(Broadcast)──→ RPC Thread: TaskEvent → WebSocket 推送
```

**快照延迟权衡：** 500ms 的快照间隔意味着 RPC 查询结果最多滞后 500ms。对于下载工具的 UI 刷新来说完全可接受（前端本身也是每 1-2 秒轮询一次）。如果需要更实时的反馈，可以：
- 将快照间隔调低到 200ms（代价是跨线程消息量增加 2.5 倍）
- 对特定事件（如任务完成、出错）走 Broadcast Channel 实时推送，不依赖快照周期

---

## 四、BT 协议的多核设计

BT 协议是多线程改造最大的挑战，也是多核收益最大的场景。

### 4.1 分片管理 — Owner 线程模式

```cpp
class BtPieceManager {
    // 仅在 Owner IO Worker 线程内访问 — 无锁
    BitfieldMan bitfield_;
    std::set<std::shared_ptr<Piece>> used_pieces_;
    PieceStatMan piece_stats_;

public:
    // 本线程的 peer 协程直接调用
    std::optional<PieceRequest> select_piece(PeerId peer, const Bitfield& peer_has) {
        // Rarest-First 选择 — 直接访问 piece_stats_，零开销
        auto candidates = bitfield_.get_missing_indexes(peer_has);
        auto index = piece_stats_.select_rarest(candidates);
        if (!index) return std::nullopt;

        auto piece = checkout_piece(*index, peer);
        return PieceRequest{*index, piece->next_block()};
    }

    // 远程 peer 线程通过 Channel 请求
    // Owner 处理 Channel 消息时调用此方法
    PieceGrant handle_remote_request(WorkerId from, PeerId peer,
                                     const Bitfield& peer_has) {
        auto req = select_piece(peer, peer_has);
        return PieceGrant{req};
    }

    void complete_piece(size_t index) {
        bitfield_.set_bit(index);
        // 通知所有 peer 线程发送 HAVE 消息
        for (auto& worker : peer_workers_) {
            worker.channel.send(BtEvent::Have{index});
        }
    }
};
```

### 4.2 Choke/Unchoke — 定时快照

```cpp
// 每 10 秒执行一次，在 Owner 线程内
Task<void> choke_algorithm(BtContext& ctx) {
    while (true) {
        co_await sleep(10s);

        // 1. 收集所有 peer 的上传/下载速度（包括远程线程的 peer）
        //    远程线程定期上报速度统计到 Owner 的 Channel
        auto stats = collect_peer_stats();

        // 2. 执行 Choke 算法 — 纯计算，无 I/O
        auto decisions = compute_choke_decisions(stats);

        // 3. 分发 choke/unchoke 指令到各 peer 线程
        for (auto& [peer, decision] : decisions) {
            auto worker = find_peer_worker(peer);
            worker.channel.send(BtEvent::Choke{peer, decision});
        }
    }
}
```

### 4.3 DHT — 独立子系统

```cpp
// DHT 运行在自己的 IO Worker 线程（或 Coordinator 线程）
// 与下载任务通过 Channel 交互

class DhtEngine {
    // DHT 路由表、查询状态等全部是本线程私有

    // 发现新 peer 时通过 Channel 通知相关任务的 Owner 线程
    void on_peers_found(InfoHash hash, std::vector<PeerAddr> peers) {
        auto owner = task_registry_.find_owner(hash);
        owner.channel.send(BtEvent::NewPeers{peers});
    }
};
```

---

## 五、协议处理的协程化示例

### 5.1 HTTP 下载 — 协程化

```cpp
Task<void> http_download(DownloadTask& task, HttpSpec spec) {
    auto& seg_man = task.segment_man();

    while (auto segment = seg_man.get_segment()) {
        // 建立连接（可能复用连接池）
        auto stream = co_await connect_or_reuse(spec.host, spec.port);

        // TLS 握手（如果 HTTPS）
        if (spec.is_https()) {
            stream = co_await tls_handshake(stream, spec.host);
        }

        // 发送 Range 请求
        auto request = build_range_request(spec, segment);
        co_await stream.write(request);

        // 读取响应头
        auto response = co_await read_http_response(stream);
        if (response.status() != 206) {
            seg_man.cancel_segment(segment);
            continue;
        }

        // 流式读取数据
        size_t received = 0;
        while (received < segment.length()) {
            // 限速：获取令牌
            auto allowed = co_await rate_limiter_.acquire(8192);

            // 读取数据
            auto data = co_await stream.read(allowed);
            if (data.empty()) break;

            // 写入缓存（同线程，无锁）
            write_cache_.cache_write(segment.file_id(),
                                     segment.offset() + received,
                                     std::move(data));
            received += data.size();

            // 更新进度（同线程，无锁）
            task.context().update_download(data.size());
        }

        seg_man.complete_segment(segment);

        // 归还连接到池中
        connection_pool_.release(spec.host, std::move(stream));
    }
}
```

### 5.2 FTP 下载 — 协程化

```cpp
Task<void> ftp_download(DownloadTask& task, FtpSpec spec) {
    // 控制连接
    auto control = co_await TcpStream::connect(spec.host, spec.port);

    auto welcome = co_await ftp_read_response(control);
    co_await ftp_command(control, "USER", spec.user);
    co_await ftp_command(control, "PASS", spec.pass);
    co_await ftp_command(control, "TYPE", "I");

    while (auto segment = task.segment_man().get_segment()) {
        // PASV 模式建立数据连接
        auto pasv_response = co_await ftp_command(control, "PASV");
        auto data_addr = parse_pasv_response(pasv_response);
        auto data = co_await TcpStream::connect(data_addr);

        // REST + RETR
        co_await ftp_command(control, "REST", std::to_string(segment.offset()));
        co_await ftp_command(control, "RETR", spec.path);

        // 流式接收 — 与 HTTP 相同的模式
        size_t received = 0;
        while (received < segment.length()) {
            auto allowed = co_await rate_limiter_.acquire(8192);
            auto buf = co_await data.read(allowed);
            if (buf.empty()) break;
            write_cache_.cache_write(segment.file_id(),
                                     segment.offset() + received,
                                     std::move(buf));
            received += buf.size();
        }

        seg_man.complete_segment(segment);
    }

    co_await ftp_command(control, "QUIT");
}
```

### 5.3 BT Peer — 协程化

```cpp
Task<void> bt_peer_handler(TcpStream stream, BtContext& ctx, PeerId peer_id) {
    // 握手
    co_await bt_handshake(stream, ctx.info_hash(), ctx.local_peer_id());

    // 交换 bitfield
    co_await send_bitfield(stream, ctx.piece_storage().bitfield());

    // 主消息循环
    while (true) {
        auto msg = co_await read_bt_message(stream);

        switch (msg.type()) {
        case BtMsg::Bitfield:
            ctx.peer_storage().set_bitfield(peer_id, msg.bitfield());
            co_await request_pieces(stream, ctx, peer_id);
            break;

        case BtMsg::Have:
            ctx.peer_storage().update_have(peer_id, msg.index());
            ctx.piece_stats().increment(msg.index());
            break;

        case BtMsg::Piece: {
            auto& ps = ctx.piece_storage();
            ps.write_block(msg.index(), msg.begin(), msg.data());

            // 写盘
            write_cache_.cache_write(...);

            // 检查 piece 是否完整
            if (ps.is_piece_complete(msg.index())) {
                // 校验哈希（CPU 密集，可以 co_await 让出）
                auto hash = co_await compute_hash_async(msg.index());
                if (hash == ctx.expected_hash(msg.index())) {
                    ps.complete_piece(msg.index());
                    // 广播 HAVE 给所有 peer
                    ctx.broadcast_have(msg.index());
                } else {
                    ps.discard_piece(msg.index());
                }
            }

            // 继续请求下一个 block
            co_await request_pieces(stream, ctx, peer_id);
            break;
        }

        case BtMsg::Choke:
            ctx.peer_storage().set_choked(peer_id, true);
            break;

        case BtMsg::Unchoke:
            ctx.peer_storage().set_choked(peer_id, false);
            co_await request_pieces(stream, ctx, peer_id);
            break;

        case BtMsg::Request:
            if (!ctx.peer_storage().is_choked(peer_id)) {
                auto data = co_await read_piece_data(msg.index(), msg.begin(), msg.length());
                co_await send_piece(stream, msg.index(), msg.begin(), data);
                ctx.context().update_upload(data.size());
            }
            break;

        case BtMsg::KeepAlive:
            break;
        }
    }
}
```

---

## 六、与纯 Actor 模型的详细对比

### 6.1 BT 分片选择场景

**Actor 模型：**
```
PeerActor                PieceManagerActor          (跨线程消息往返)
   |                          |
   |--- PieceRequest -------->|
   |                          |-- 查询 bitfield
   |                          |-- 计算稀有度
   |                          |-- 选择分片
   |<-- PieceGrant -----------|
   |                          |
   |--- BlockComplete ------->|
   |                          |-- 更新 bitfield
   |                          |-- 检查完整性
   |<-- PieceComplete --------|
```

每次分片请求需要 **2 次跨线程消息**。200 个 peer 都需要经过 PieceManagerActor 串行处理。

**协程 + 线程亲和：**
```
PeerCoroutine (同线程)    PieceStorage (同线程私有)
   |                          |
   |--- 直接函数调用 --------->|  (内联，零开销)
   |<-- 返回值 ---------------|
```

同线程内直接调用，**零消息传递开销**。200 个 peer 协程按 EventLoop 调度顺序依次执行，与单线程架构性能相同。

### 6.2 性能对比估算

假设场景：1 个活跃 torrent，200 个 peer 连接，每个 piece 16KB block。

| 操作 | Actor 模型 | 协程+线程亲和 |
|------|-----------|-------------|
| 分片请求 | ~2us (消息排队 + 线程切换) | ~50ns (函数调用) |
| bitfield 更新 | ~1us (消息) | ~10ns (直接写) |
| 速度统计 | ~500ns (消息) | ~20ns (本地更新) |
| HAVE 广播 | ~200us (200 条消息) | ~5us (遍历本线程 peer) |

### 6.3 何时 Actor 模型更优

Actor 模型在以下场景有优势：

1. **大规模分布式系统**：跨机器通信时，消息传递是唯一选择，Actor 的 location transparency 是杀手级特性
2. **故障隔离要求高**：如 Erlang 的 supervisor tree，一个 Actor 崩溃可以精确重启而不影响其他
3. **状态持久化需求**：Actor 的 mailbox 天然是可序列化的，便于实现 event sourcing
4. **动态拓扑**：Actor 可以动态创建/销毁，适合 peer 数量频繁变化的场景

但对于下载工具这种**单机、高频共享状态、性能敏感**的场景，协程 + 线程亲和更合适。

---

## 七、技术选型建议

### 7.1 语言选择

| 语言 | 协程支持 | 异步 I/O | 适合度 |
|------|---------|---------|--------|
| **Rust** | async/await + tokio | io_uring (tokio-uring), mio | **最优** — 编译期线程安全保证 |
| **C++20** | co_await (标准) | 需自建 executor 或用 liburing | **良好** — 生态不如 Rust 成熟 |
| **Go** | goroutine (原生) | net poller (内建) | **中等** — GC 暂停、缺少零拷贝 |
| **Zig** | async (内建) | io_uring 原生支持 | **新兴** — 生态不完善 |

### 7.2 C++ 技术栈推荐

| 组件 | 推荐方案 | 备选 |
|------|---------|------|
| 协程运行时 | C++20 coroutine + 自定义 executor | libcoro, cppcoro |
| 网络 I/O (Linux) | io_uring (liburing) | epoll |
| 网络 I/O (Windows) | IOCP | - |
| 网络 I/O (macOS) | kqueue | - |
| 磁盘 I/O | io_uring (Linux) / IOCP (Windows) | ThreadPool + pwrite |
| Channel | lock-free SPSC/MPSC ring buffer | moodycamel::ConcurrentQueue |
| TLS | OpenSSL / rustls-ffi | BoringSSL |
| HTTP 解析 | llhttp (Node.js 的解析器) | picohttpparser |
| DNS | c-ares (独立线程) | getaddrinfo_a |
| 定时器 | 时间轮 (Hierarchical Timing Wheel) | std::priority_queue |
| 内存分配 | mimalloc / jemalloc | tcmalloc |

### 7.3 Rust 技术栈推荐

| 组件 | 推荐方案 | 备选 |
|------|---------|------|
| 异步运行时 | tokio (多线程 work-stealing) | async-std, glommio (thread-per-core) |
| 网络 I/O | tokio::net (mio 封装) | io_uring via tokio-uring |
| 磁盘 I/O | tokio-uring (Linux) / tokio::fs (其他) | glommio |
| Channel | tokio::sync::mpsc / flume | crossbeam-channel |
| TLS | rustls | native-tls |
| HTTP | hyper / httparse | reqwest (客户端) |
| BT 协议 | 自建（基于 bytes crate 零拷贝解析） | - |
| 序列化 | serde + serde_json | simd-json |

---

## 八、工程估算

### 8.1 模块划分与工作量

| 模块 | 代码量估算 | 复杂度 | 依赖 |
|------|-----------|--------|------|
| 协程运行时 + EventLoop | 3000-5000 行 | 高 | 无 |
| Channel 通信 | 1000-2000 行 | 中 | 无 |
| Coordinator + 任务调度 | 2000-3000 行 | 中 | EventLoop, Channel |
| HTTP/HTTPS 协议 | 3000-4000 行 | 中 | EventLoop, TLS |
| FTP 协议 | 2000-3000 行 | 中 | EventLoop |
| BitTorrent 协议 | 8000-12000 行 | 高 | EventLoop, Channel, DiskIO |
| 磁盘 I/O (io_uring/IOCP) | 2000-3000 行 | 高 | 无 |
| 分片管理 (PieceStorage 等) | 3000-4000 行 | 中 | 无 |
| 写缓存 | 1000-1500 行 | 中 | DiskIO |
| 全局限速 | 500-1000 行 | 低 | Channel |
| DNS 解析 | 500-1000 行 | 低 | Channel |
| RPC (JSON-RPC) | 2000-3000 行 | 中 | Channel |
| TLS 集成 | 1000-2000 行 | 中 | EventLoop |
| Metalink 解析 | 1000-1500 行 | 低 | 无 |
| 配置 / CLI / 日志 | 2000-3000 行 | 低 | 无 |
| 测试 | 5000-8000 行 | - | - |
| **总计** | **~35000-55000 行** | | |

### 8.2 开发里程碑

| 阶段 | 内容 | 预计时间 |
|------|------|---------|
| **M0: 基础设施** | 协程运行时、EventLoop、Channel、DiskIO | 4-6 周 |
| **M1: HTTP 下载** | HTTP/HTTPS 协议、分片管理、写缓存、限速 | 3-4 周 |
| **M2: FTP 下载** | FTP 协议复用 M1 的分片/磁盘基础设施 | 2-3 周 |
| **M3: BT 下载** | BT 协议核心、DHT、PeerExchange | 6-8 周 |
| **M4: 生产就绪** | RPC、Metalink、配置系统、错误恢复、测试 | 4-6 周 |

### 8.3 与 aria2 改造的 ROI 对比

| 指标 | 改造 aria2 (方案 B) | 从零构建 |
|------|-------------------|---------|
| 工作量 | 3000-5000 行变更 | 35000-55000 行新代码 |
| 时间 | 4-8 周 | 20-30 周 |
| 风险 | 中-高（回归测试压力大） | 中（新代码无历史包袱） |
| 多核利用率 | 中（受限于跨 RG 共享状态） | 高（架构级隔离） |
| 长期维护性 | 低（两种模式混合，认知负担重） | 高（统一的协程风格） |
| 兼容性 | 保持 aria2 所有功能 | 需要重新实现所有功能 |

---

## 九、总结

如果不受历史代码约束从零设计，**协程 + per-thread EventLoop + 任务亲和性 + Channel** 是下载工具场景的最优多核架构。相比纯 Actor 模型：

- **热路径零开销**：同一下载任务的分片管理、peer 交互在同一线程内无锁完成
- **冷路径类型安全**：跨线程通信仅通过 Channel，编译期保证无数据竞争
- **自然扩展**：IO Worker 数量随 CPU 核心数线性增长
- **代码可读性**：协程代码接近同步风格，避免回调地狱和状态机爆炸

这种架构本质上是 **"多个单线程事件循环 + 协程调度 + Channel 协调"**，保持了单线程架构的简洁性，同时获得了多核并行能力。

纯 Actor 模型更适合分布式系统和故障隔离要求极高的场景，在单机高频共享状态的下载工具中，消息传递的开销和 Actor 热点瓶颈使其不是最优选择。
