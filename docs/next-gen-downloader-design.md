# 下一代下载引擎：从零设计方案

> 综合 aria2-zero 架构经验、Surge/Gopeed 等新锐下载器的优化策略、Google "The Tail at Scale" 论文的长尾延迟理论，以及 FastBioDL 的自适应并发控制研究，设计一个面向 2025+ 的高性能多协议下载引擎。

---

## 一、设计目标与核心理念

### 1.1 设计目标

| 优先级 | 目标 | 量化指标 |
|--------|------|---------|
| P0 | 极致下载速度 | 单文件下载速度达到链路带宽 95%+ |
| P0 | 消除长尾延迟 | P99 完成时间 ≤ 理论最优时间的 1.1 倍 |
| P0 | 多协议支持 | HTTP/HTTPS/FTP/SFTP/BitTorrent/Metalink/ED2K |
| P1 | 多核利用 | 线性扩展至 16 核 |
| P1 | 低资源占用 | 空闲时 < 10MB RSS，活跃时 < 200MB |
| P2 | 可扩展性 | 插件/扩展机制，支持自定义协议和后处理 |
| P2 | 跨平台 | Windows/Linux/macOS，移动端 API 兼容 |

### 1.2 核心理念

**"观测 → 决策 → 行动" 闭环调度**

传统下载器（包括 aria2）采用静态分片策略：预分配 N 个 segment，每个连接领取一个 segment 下载到底。这种策略存在根本缺陷——无法应对运行时的带宽波动、CDN 节点差异、服务器限速等动态因素。

新引擎的核心理念是将下载过程建模为**在线优化问题**：

```
目标函数: 最小化 T_total（总完成时间）
约束条件: 连接数 ≤ C_max, 带宽 ≤ BW_limit
控制变量: 分片大小, 连接数, 连接分配, 投机请求

在每个决策周期 Δt 内:
  1. 观测: 收集各连接的吞吐量、延迟、剩余字节数
  2. 决策: 运行调度算法，决定是否 steal/hedge/resize/add/drop
  3. 行动: 执行决策，调整连接状态
```

---

## 二、架构总览

### 2.1 分层架构

```
┌─────────────────────────────────────────────────────────────────┐
│  Layer 4: 用户接口层                                             │
│  CLI / TUI / JSON-RPC API / WebSocket                           │
├─────────────────────────────────────────────────────────────────┤
│  Layer 3: 任务编排层                                             │
│  TaskOrchestrator · GlobalRateLimiter · QueueScheduler           │
├─────────────────────────────────────────────────────────────────┤
│  Layer 2: 传输优化层 ★ 核心创新                                   │
│  ChunkScheduler · WorkStealer · HedgeEngine · BandwidthProber   │
│  AdaptiveConcurrency · MirrorRanker · ConnectionPool            │
├─────────────────────────────────────────────────────────────────┤
│  Layer 1: 协议层                                                 │
│  HTTP/2 · FTP · SFTP · BitTorrent · Metalink · ED2K             │
├─────────────────────────────────────────────────────────────────┤
│  Layer 0: 运行时层                                               │
│  Coroutine Runtime · EventLoop (per-core) · Channel Bus         │
│  DiskIO (io_uring/IOCP) · TLS · DNS · TimerWheel               │
└─────────────────────────────────────────────────────────────────┘
```

### 2.2 线程模型

延续 [greenfield-multicore-architecture.md](greenfield-multicore-architecture.md) 的结论：**协程 + per-thread EventLoop + 任务亲和性 + Channel**。

```
┌─────────────┐
│ Coordinator  │  1 个线程: 任务编排, 全局限速, 调度决策
└──────┬──────┘
       │ Channel Bus (lock-free SPSC/MPSC)
  ┌────┼────┬────┬─────┐
  ▼    ▼    ▼    ▼     ▼
┌────┐┌────┐┌────┐┌────┐┌────┐
│IO 0││IO 1││IO 2││IO 3││RPC │  IO Worker: per-core EventLoop + 协程
└────┘└────┘└────┘└────┘└────┘  RPC: 独立线程, 状态快照缓存
  │    │    │    │
  └────┴────┴────┘
       │
  ┌────▼────┐
  │ DiskIO  │  2~4 线程: io_uring (Linux) / IOCP (Windows)
  └─────────┘
```

关键变化（相对于 greenfield 文档）：

- **Coordinator 承担调度决策**：不再只是简单的任务分配，而是持续运行传输优化算法
- **IO Worker 上报细粒度指标**：每 200ms 上报 per-connection 的吞吐量/延迟/剩余量
- **双向控制流**：Coordinator 可以向 IO Worker 发出 steal/hedge/resize 指令

### 2.3 I/O 模型：Proactor 而非 Reactor

#### 为什么选 Proactor

| 特性 | Reactor (select/epoll/kqueue) | Proactor (io_uring/IOCP) |
|------|------------------------------|--------------------------|
| 通知内容 | "socket 可读" | "读操作已完成，数据在 buffer 中" |
| I/O 执行者 | 应用层调用 recv() | 内核异步完成 |
| 系统调用次数 | 2 次 (poll + recv) | 1 次 (提交即完成) |
| 缓冲区管理 | 按需分配 | 预分配并提交给内核 |
| 零拷贝 | 不可能 | io_uring registered buffers / IOCP 直接写入 |

协程的调用形式天然就是 Proactor 语义：

```
auto data = co_await stream.read(buf);   // "读完成后恢复我"
co_await stream.write(data);             // "写完成后恢复我"
```

如果底层用 Reactor，协程内部需要多一层：等就绪 → 调 recv() → 返回数据，凭空多一次状态转换。

#### 来自 aria2-zero 的实战教训

aria2-zero 在 Windows 上实现了 [IOCP Reactor 模式](iocp-eventpoll-plan.md)（用零字节 WSARecv 做就绪通知，保持 Reactor 接口不变）。这个方案踩了 7 个坑：

| 坑 | 根本原因 |
|----|---------|
| UDP 数据报丢失 | 零字节 WSARecv 在 UDP 上消耗数据报（Proactor 语义泄漏到 Reactor 接口） |
| OVERLAPPED 内存安全 | 删除 socketEntry 时内核仍持有 OVERLAPPED 引用 |
| 连接中 socket 回退 select | 非阻塞 connect 后对 socket 做零字节 WSASend 失败 |
| IOCP 关联不可逆 | 与 epoll 的 CTL_DEL/CTL_ADD 对称语义不同 |
| socket 句柄复用 | 旧 socket 关闭后 OS 复用相同句柄值 |
| 错误完成误派发 | 零字节操作失败时不能派发 IEV_ERROR |
| 析构排空队列 | CancelIoEx 后必须排空完成队列才能释放内存 |

**这些坑的共同本质：把 Proactor 硬塞进 Reactor 接口。** 从零设计不需要付这个代价。

#### 各平台 I/O 后端

```
Linux:   io_uring  — 原生 Proactor, 提交 SQE → 收割 CQE
                     SQPOLL 模式下零系统调用提交
                     registered buffers 实现零拷贝
                     Linux 6.15 支持 ZC Rx (硬件 DMA 直写用户态内存)

Windows: IOCP      — 原生 Proactor, WSARecv(buf) → GQCS 完成
                     重叠 I/O + 完成端口批量收割
                     GetQueuedCompletionStatusEx 一次取多个事件

macOS:   kqueue     — 仅 Reactor, 无原生 Proactor 等价物
                     需要适配层: EV_READ 就绪 → recv(buf) → 作为"完成"通知

Linux (旧内核): epoll — Reactor, 与 kqueue 同等适配方案
```

#### io_uring 版本可用性与回退策略

io_uring 从 Linux 5.1 (2019) 引入，但下载器所需的**网络操作跨越了多个内核版本**才逐步完善：

| 内核版本 | io_uring 网络能力 | 代表发行版 |
|---------|------------------|-----------|
| < 5.1 | **无 io_uring** | RHEL 7 (3.10), RHEL 8 (4.18) |
| 5.1 - 5.5 | 仅文件 I/O (read/write/fsync) | Ubuntu 20.04 (5.4) |
| 5.6 - 5.12 | buffer 注册, SQPOLL, 但**无 socket recv/send** | — |
| **5.13** | **IORING_OP_RECV / IORING_OP_SEND** — 网络 Proactor 最低可用版本 | — |
| 5.15 | 更完整的 socket 操作 | Ubuntu 22.04, RHEL 9 (5.14 无 recv/send) |
| 6.0 | multishot accept, sendmsg/recvmsg | — |
| 6.1 | 网络 I/O 的 fixed buffers — 网络零拷贝前置条件 | Debian 12 |
| 6.8+ | 成熟稳定 | Ubuntu 24.04 |
| 6.15 | ZC Rx (硬件 DMA 直写用户态内存) — 终极零拷贝 | 前沿内核 |

**关键结论：RHEL 8 (4.18) 和 Ubuntu 20.04 (5.4) 完全无法使用 io_uring 做网络 I/O。** 即使是 RHEL 9 (5.14) 也缺少 recv/send 操作（5.13 才加入）。此外还有额外的可用性限制：

- **容器环境**：Docker 默认 seccomp profile 阻止 io_uring 系统调用
- **安全策略**：Google 在 ChromeOS/Android 上禁用 io_uring（历史漏洞频繁）
- **云厂商内核**：部分云厂商的定制内核可能裁剪 io_uring 支持

因此 **epoll 回退路径不是可选项，而是硬性要求**。Linux 上的 IoEngine 实现为三级检测：

```cpp
// 运行时探测, 自动选择最优后端
std::unique_ptr<IoEngine> create_linux_io_engine() {
    // 1. 尝试 io_uring: 探测 IORING_OP_RECV 是否可用
    if (auto engine = IoUringEngine::try_create()) {
        // 检测内核是否支持网络操作 (≥5.13)
        // 同时检测 seccomp/容器是否阻止 io_uring
        if (engine->probe_op(IORING_OP_RECV)) {
            return engine;  // 完整 Proactor
        }
        // 内核有 io_uring 但无网络操作 (5.1-5.12):
        // 可以仅用于磁盘 I/O, 网络走 epoll
    }

    // 2. epoll: Reactor 包装为 Proactor
    //    就绪通知 → recv(buf) → 作为完成事件恢复协程
    return std::make_unique<EpollProactorEngine>();
}
```

磁盘 I/O 和网络 I/O 可以使用**不同后端**：在 5.1-5.12 内核上，磁盘 I/O 走 io_uring（原生支持），网络 I/O 走 epoll 适配层。这是合理的混合策略，因为磁盘 I/O 的零拷贝收益更大（大块顺序写入），而网络 I/O 的 Proactor 优势主要体现在减少系统调用次数。

#### 统一 Proactor 抽象层

```cpp
// 应用层接口 — 统一 Proactor 语义
// 无论底层是 io_uring、IOCP、epoll 适配还是 kqueue 适配, 接口一致
class AsyncSocket {
public:
    // 提交读操作, 完成后协程恢复, 数据在 buf 中
    Task<size_t> async_recv(MutableBuffer buf);

    // 提交写操作, 完成后协程恢复
    Task<size_t> async_send(ConstBuffer buf);

    // 零拷贝发送文件 (Linux: sendfile via io_uring, Win: TransmitFile)
    // epoll/kqueue 回退: 用户态 read + send
    Task<size_t> async_sendfile(FileHandle src, int64_t offset, size_t count);
};

// 底层分平台实现
class IoEngine {
public:
    // 提交异步操作
    void submit(IoOp op);

    // 收割完成事件, 恢复对应协程
    void reap_completions();

    // 能力探测 — 运行时确定可用功能
    bool supports(IoCapability cap);
    // IoCapability::ZeroCopyRecv     → io_uring ≥6.1
    // IoCapability::RegisteredBuffers → io_uring ≥5.6
    // IoCapability::NativeProactor    → io_uring ≥5.13 / IOCP
    // IoCapability::SendFile          → io_uring / IOCP TransmitFile

    // 平台实现:
    // Linux:  IoUringEngine       — io_uring ≥5.13 原生 Proactor
    // Linux:  EpollProactorEngine — epoll Reactor 包装为 Proactor (回退)
    // Windows: IocpEngine         — IOCP 原生 Proactor
    // macOS:  KqueueProactorEngine — kqueue Reactor 包装为 Proactor
};
```

epoll/kqueue 的 Proactor 适配层各约 200 行：就绪通知到达后立即执行 recv/send，然后将结果作为"完成事件"恢复协程。应用层代码完全不感知底层差异，仅在 `supports()` 查询时能区分后端能力（例如决定是否启用零拷贝路径）。

#### TLS 层的特殊处理

TLS（OpenSSL/Schannel）的 `SSL_read`/`SSL_write` 是同步接口，无法直接利用 Proactor 的内核级 I/O。处理方式：

```
Proactor 完成                    TLS 解密                    应用层
───────────────────────────────────────────────────────────────
内核完成 encrypted data 接收  →  SSL_read 解密到明文 buf  →  协程恢复, 得到明文
应用层提供明文 data          →  SSL_write 加密到密文 buf  →  Proactor 提交密文发送
```

这意味着 TLS 连接上 Proactor 的零拷贝优势受限（加解密本身需要缓冲区），但仍然保留了减少系统调用次数的收益。Windows 上如果用 Schannel 的 SSPI，可以与 IOCP 更紧密集成。

---

## 三、传输优化层：消除长尾延迟

这是本设计的核心创新，融合了 Surge 的 HealthCheck/StealWork/HedgeWork、Google 的 Hedged Requests、FastBioDL 的自适应并发控制、以及 IDM 的动态分片策略。

### 3.1 动态分片管理器 (ChunkScheduler)

#### 设计哲学

抛弃传统的"预分配固定 segment"模式，采用**按需分配 + 动态调整**：

```
传统 aria2 模式:
  文件 = [Seg0][Seg1][Seg2][Seg3]  ← 启动时固定分配, 各 segment 大小相等
  Conn0→Seg0, Conn1→Seg1, Conn2→Seg2, Conn3→Seg3
  问题: Conn2 慢了 → 整个下载被 Conn2 拖住

新模式:
  文件 = [====已完成====][C0:当前][C1:当前][未分配空间]
  每个连接只领取一个 "工作窗口" (work window)
  窗口完成后回来领取下一个
  窗口大小根据连接速度动态调整
```

#### 工作窗口 (Work Window) 算法

```cpp
struct WorkWindow {
    int64_t offset;
    int64_t length;
    ConnectionId owner;
    Instant assigned_at;
    int64_t bytes_completed;
};

class ChunkScheduler {
    // 未分配的文件区域 (interval tree)
    IntervalTree<int64_t> unassigned_;
    // 活跃窗口
    std::vector<WorkWindow> active_windows_;

    // 窗口大小 = 连接速度 × 目标完成时间
    // 目标完成时间固定为 T_window (默认 5 秒)
    // 快连接得到大窗口, 慢连接得到小窗口
    int64_t compute_window_size(ConnectionId conn) {
        auto speed = conn_stats_[conn].ema_speed;  // 指数移动平均速度
        auto size = speed * T_WINDOW;
        return std::clamp(size, MIN_WINDOW, MAX_WINDOW);
    }

    // 连接完成当前窗口后, 请求下一个窗口
    std::optional<WorkWindow> next_window(ConnectionId conn) {
        auto size = compute_window_size(conn);
        auto region = unassigned_.allocate(size);
        if (!region) return std::nullopt;
        return WorkWindow{region->start, region->length, conn, now()};
    }
};
```

**为什么 T_WINDOW = 5 秒？**
- 太小（<1s）：HTTP Range 请求的建连/握手开销占比过大
- 太大（>30s）：失去动态调整能力，退化为静态分片
- 5 秒窗口：在开销和灵活性之间取得平衡，且与 HealthCheck 周期（3s）形成错位，避免共振

#### 对比 IDM 的"二分法"

IDM 每次找最大的未完成 segment 对半分。这比固定分片好，但仍是被动式的——只有当有空闲连接时才分割。本设计的工作窗口是主动式的：每个连接完成窗口后自然获取下一个，窗口大小与速度成正比，天然实现负载均衡。

### 3.2 健康检查与慢连接检测 (HealthMonitor)

借鉴 Surge 的 HealthCheck，但做了显著增强：

```cpp
class HealthMonitor {
    // 每 3 秒评估一次所有活跃连接

    struct ConnHealth {
        double ema_speed;           // 指数移动平均速度 (α=0.3)
        double speed_variance;      // 速度方差 (检测不稳定连接)
        int stall_count;            // 停滞次数 (速度为 0 的采样周期数)
        Instant last_progress;      // 最后一次有进度的时间
        double efficiency_ratio;    // 实际速度 / 该 mirror 的历史最佳速度
    };

    enum class Verdict {
        Healthy,           // 正常
        Degraded,          // 性能下降, 标记观察
        Slow,              // 慢连接, 触发 StealWork
        Stalled,           // 停滞, 触发 HedgeWork 或替换
        Dead,              // 连接已死, 立即替换
    };

    Verdict evaluate(ConnectionId conn) {
        auto& h = health_[conn];
        auto mean_speed = compute_global_mean_speed();

        // 停滞检测: 超过 10 秒无进度
        if (now() - h.last_progress > 10s) return Verdict::Dead;

        // 停顿检测: 连续 3 次采样速度为 0
        if (h.stall_count >= 3) return Verdict::Stalled;

        // Surge 策略: 低于均值的 0.3 倍
        // 增强: 同时考虑方差, 避免误判波动型连接
        if (h.ema_speed < mean_speed * 0.3 && h.speed_variance < STABLE_THRESHOLD) {
            return Verdict::Slow;
        }

        // 性能下降: 低于该 mirror 历史最佳的 50%
        if (h.efficiency_ratio < 0.5) return Verdict::Degraded;

        return Verdict::Healthy;
    }
};
```

**相对于 Surge 的改进：**
- 增加了速度方差检测，避免将带宽波动大但均值正常的连接误判为慢连接
- 引入 efficiency_ratio，检测单个 mirror/CDN 节点的性能退化
- 分级处理（Degraded → Slow → Stalled → Dead），而非 Surge 的二值判断

### 3.3 工作窃取 (WorkStealer)

当 HealthMonitor 判定某连接为 Slow 时，启动工作窃取：

```cpp
class WorkStealer {
    // 从慢连接的剩余窗口中窃取一部分给快连接

    StealResult try_steal(ConnectionId thief, ConnectionId victim) {
        auto& victim_window = active_windows_[victim];
        auto remaining = victim_window.remaining();

        // 窃取条件: 剩余量 > 2 × MIN_WINDOW
        if (remaining < 2 * MIN_WINDOW) {
            return StealResult::TooSmall;  // 触发 HedgeWork
        }

        // 按比例窃取: 快连接速度 / (快 + 慢) × 剩余量
        auto thief_speed = conn_stats_[thief].ema_speed;
        auto victim_speed = conn_stats_[victim].ema_speed;
        auto steal_ratio = thief_speed / (thief_speed + victim_speed);
        auto steal_size = remaining * steal_ratio;

        // 从受害者窗口尾部切割
        auto stolen_region = victim_window.split_tail(steal_size);
        return StealResult::Success{stolen_region};
    }
};
```

**对比 Surge：** Surge 的 StealWork 只在"快连接空闲"时才触发。本设计是 Coordinator 主动扫描并调度，不需要等连接空闲——Coordinator 可以同时创建新连接来执行窃取。

### 3.4 投机对冲 (HedgeEngine)

融合 Google "The Tail at Scale" 的 Hedged Requests 和 Surge 的 HedgeWork：

```cpp
class HedgeEngine {
    // 两种对冲模式:

    // 模式 1: 尾部对冲 (Tail Hedge)
    // 当下载进入尾部阶段 (已完成 > 90%), 对所有活跃连接发起对冲
    // 原理: 尾部阶段剩余数据少, 对冲的额外带宽开销可忽略
    //        但能显著降低 P99 完成时间
    void tail_hedge(TaskId task) {
        for (auto& window : active_windows_of(task)) {
            if (window.remaining() < HEDGE_THRESHOLD) {
                // 用另一个连接(可能是不同 mirror)下载同一区域
                auto hedge_conn = acquire_connection(task);
                hedge_conn.download(window.offset + window.bytes_completed,
                                    window.remaining());
                hedges_.push_back({window.owner, hedge_conn.id(),
                                   window.remaining_range()});
            }
        }
    }

    // 模式 2: 超时对冲 (Timeout Hedge)
    // 当某连接的响应时间超过 P95 预期时, 立即用另一条连接对冲
    // 参考 Google 论文: 等到 P95 时间点再对冲, 仅增加 ~5% 负载
    void timeout_hedge(ConnectionId slow_conn) {
        auto p95_time = conn_stats_[slow_conn].p95_response_time;
        auto elapsed = now() - last_request_time_[slow_conn];

        if (elapsed > p95_time) {
            auto hedge_conn = acquire_connection_different_mirror(slow_conn);
            if (hedge_conn) {
                // 复制相同的 Range 请求
                hedge_conn.download(slow_conn.current_range());
                hedges_.push_back({slow_conn, hedge_conn.id(),
                                   slow_conn.current_range()});
            }
        }
    }

    // 对冲完成处理: 谁先完成用谁的结果, 取消另一个
    void on_hedge_complete(ConnectionId winner, Range range) {
        auto it = find_hedge(winner);
        if (it != hedges_.end()) {
            auto loser = (it->conn_a == winner) ? it->conn_b : it->conn_a;
            cancel_download(loser, range);
            hedges_.erase(it);
        }
    }
};
```

**对冲的代价控制：**
- 仅在下载尾部（>90%）或超过 P95 响应时间时触发
- 对冲请求数 ≤ 当前活跃连接数的 50%
- 配合熔断器：如果对冲成功率 < 20%（说明不是长尾问题而是带宽瓶颈），自动关闭对冲

### 3.5 自适应并发控制 (AdaptiveConcurrency)

借鉴 FastBioDL 的在线优化方法，但适配到下载场景：

```cpp
class AdaptiveConcurrency {
    // 效用函数: U(throughput, concurrency) = throughput / k^concurrency
    // k = 1.02 (来自 FastBioDL 的实验最优值)
    static constexpr double K = 1.02;

    int current_concurrency_;
    int min_concurrency_ = 1;
    int max_concurrency_ = 32;
    double probe_interval_ = 3.0;  // 秒

    struct ProbeResult {
        int concurrency;
        double throughput;
        double utility;
    };
    std::deque<ProbeResult> history_;

    // 每个探测周期结束时调用
    void on_probe_complete(double throughput) {
        double utility = throughput / std::pow(K, current_concurrency_);
        history_.push_back({current_concurrency_, throughput, utility});

        // 保留最近 10 个采样
        if (history_.size() > 10) history_.pop_front();

        // 梯度估计: 比较当前效用与上一周期
        if (history_.size() >= 2) {
            auto& prev = history_[history_.size() - 2];
            auto& curr = history_.back();
            double gradient = (curr.utility - prev.utility)
                            / (curr.concurrency - prev.concurrency + 0.01);

            if (gradient > 0) {
                // 效用上升, 继续增加并发
                adjust_concurrency(+1);
            } else if (gradient < -EPSILON) {
                // 效用下降, 减少并发
                adjust_concurrency(-1);
            }
            // 梯度接近 0, 保持不变
        } else {
            // 初始阶段: 从 4 开始, 逐步增加
            adjust_concurrency(+1);
        }
    }

    void adjust_concurrency(int delta) {
        current_concurrency_ = std::clamp(
            current_concurrency_ + delta,
            min_concurrency_, max_concurrency_
        );
    }
};
```

**与 Surge 固定 32 连接的对比：**

| | Surge (固定 32 连接) | 自适应并发 |
|--|---------------------|-----------|
| 高速服务器 | 可能过度消耗服务器资源 | 自动收敛到最优连接数 |
| 限速服务器 | 32 个慢连接，浪费资源 | 自动降到 1-2 个连接 |
| CDN 节点差异 | 所有连接同等对待 | 配合 MirrorRanker 调整 |
| 家庭带宽 (10Mbps) | 32 连接争抢，效率降低 | 收敛到 4-8 连接 |
| 数据中心 (10Gbps) | 32 连接不够 | 可扩展到上限 |

### 3.6 镜像排序器 (MirrorRanker)

多镜像/CDN 场景下的智能选择：

```cpp
class MirrorRanker {
    struct MirrorStats {
        std::string url;
        double ema_speed;            // 历史平均速度
        double ema_rtt;              // 历史平均 RTT
        double success_rate;         // 成功率
        int active_connections;      // 当前活跃连接数
        int max_connections;         // 该 mirror 的最大并发 (探测得出)
        Instant last_failure;        // 最后失败时间
        double score;                // 综合评分
    };

    // 综合评分公式
    double compute_score(const MirrorStats& m) {
        // 速度权重 0.5, RTT 权重 0.2, 成功率权重 0.2, 负载权重 0.1
        double speed_score = m.ema_speed / max_speed_across_mirrors_;
        double rtt_score = 1.0 - (m.ema_rtt / max_rtt_across_mirrors_);
        double success_score = m.success_rate;
        double load_score = 1.0 - (double(m.active_connections) / m.max_connections);

        return 0.5 * speed_score + 0.2 * rtt_score
             + 0.2 * success_score + 0.1 * load_score;
    }

    // 连接分配: 按评分比例分配连接
    // 不是"全部给最快的", 而是加权分配
    // 这样即使最快 mirror 限速, 其他 mirror 仍有连接
    std::vector<std::pair<MirrorId, int>> allocate_connections(int total) {
        double total_score = 0;
        for (auto& m : mirrors_) total_score += m.score;

        std::vector<std::pair<MirrorId, int>> allocation;
        for (auto& m : mirrors_) {
            int n = std::max(1, int(total * m.score / total_score));
            n = std::min(n, m.max_connections - m.active_connections);
            allocation.push_back({m.id, n});
        }
        return allocation;
    }
};
```

### 3.7 连接池 (ConnectionPool)

```cpp
class ConnectionPool {
    // per IO Worker, 无锁
    // key = (host, port, is_tls)

    struct PoolEntry {
        TcpStream stream;
        Instant idle_since;
        int requests_served;    // HTTP keep-alive 已服务请求数
        bool is_http2;          // HTTP/2 可多路复用, 不需要多连接
    };

    std::unordered_map<HostKey, std::vector<PoolEntry>> pool_;

    // HTTP/2 多路复用: 单连接多流
    // 如果服务器支持 HTTP/2, 不需要开多个 TCP 连接
    // 而是在单连接上并发多个 Range 请求
    std::optional<Http2Session> get_h2_session(const HostKey& key) {
        // HTTP/2 连接是长期持有的, 不放回池中
    }

    // 连接预热: 当任务创建时, 提前建立连接
    // 减少首字节延迟
    void warmup(const HostKey& key, int count) {
        for (int i = 0; i < count; i++) {
            spawn_coroutine([this, key]() -> Task<void> {
                auto stream = co_await TcpStream::connect(key.host, key.port);
                if (key.is_tls) {
                    stream = co_await tls_handshake(stream, key.host);
                }
                pool_[key].push_back({std::move(stream), now(), 0});
            });
        }
    }
};
```

---

## 四、协议层设计

### 4.1 HTTP/HTTPS 下载协程

```cpp
Task<void> http_download_worker(
    DownloadTask& task,
    ConnectionId conn_id,
    ChunkScheduler& scheduler,
    HealthMonitor& monitor
) {
    while (true) {
        // 1. 从调度器获取工作窗口
        auto window = scheduler.next_window(conn_id);
        if (!window) break;  // 没有更多工作

        // 2. 获取或复用连接
        auto stream = co_await pool_.acquire_or_connect(task.host());

        // 3. HTTP/2 判断: 如果是 H2, 走多路复用路径
        if (stream.is_http2()) {
            co_await http2_stream_download(stream, *window, monitor, conn_id);
            continue;
        }

        // 4. 发送 Range 请求
        auto request = build_range_request(task.url(), window->offset,
                                           window->offset + window->length - 1);
        co_await stream.write(request);

        // 5. 读取响应
        auto response = co_await read_http_response(stream);
        if (response.status() == 429) {
            // 服务器限速 → 指数退避
            co_await sleep(backoff_.next());
            scheduler.return_window(*window);
            continue;
        }
        if (response.status() != 206 && response.status() != 200) {
            scheduler.return_window(*window);
            monitor.report_failure(conn_id);
            continue;
        }

        backoff_.reset();

        // 6. 流式读取数据, 持续上报指标
        int64_t received = 0;
        while (received < window->length) {
            auto allowed = co_await rate_limiter_.acquire(READ_CHUNK);
            auto data = co_await stream.read(allowed);
            if (data.empty()) break;

            write_cache_.cache_write(task.file_id(),
                                     window->offset + received,
                                     std::move(data));
            received += data.size();

            // 上报指标给 HealthMonitor (同线程, 直接写)
            monitor.report_progress(conn_id, data.size());
        }

        if (received == window->length) {
            scheduler.complete_window(*window);
        } else {
            // 部分完成, 归还剩余部分
            scheduler.partial_complete(*window, received);
        }

        // 7. 归还连接
        pool_.release(task.host(), std::move(stream));
    }
}
```

### 4.2 HTTP/2 多路复用

```cpp
Task<void> http2_multiplexed_download(
    DownloadTask& task,
    Http2Session& session,
    ChunkScheduler& scheduler
) {
    // HTTP/2 场景: 单连接, 多流, 并发请求
    // 不需要多 TCP 连接, 一个 session 上并发多个 Range 请求

    constexpr int MAX_CONCURRENT_STREAMS = 16;
    Semaphore stream_semaphore(MAX_CONCURRENT_STREAMS);

    while (auto window = scheduler.next_window(/*any*/)) {
        co_await stream_semaphore.acquire();

        // 每个窗口一个协程, 但共享同一个 TCP 连接
        spawn_coroutine([&, w = *window]() -> Task<void> {
            auto stream_id = session.open_stream();
            auto request = build_range_request(task.url(),
                                               w.offset, w.offset + w.length - 1);
            co_await session.send_headers(stream_id, request.headers());

            int64_t received = 0;
            while (received < w.length) {
                auto frame = co_await session.read_data(stream_id);
                write_cache_.cache_write(task.file_id(),
                                         w.offset + received,
                                         std::move(frame.data));
                received += frame.data.size();
            }

            session.close_stream(stream_id);
            scheduler.complete_window(w);
            stream_semaphore.release();
        });
    }
}
```

### 4.3 BitTorrent 协议

BT 的设计延续 greenfield 文档的 **Owner 线程模式**（分片管理单线程无锁），但增加传输优化层的集成：

```cpp
Task<void> bt_peer_handler(TcpStream stream, BtContext& ctx, PeerId peer_id) {
    co_await bt_handshake(stream, ctx.info_hash(), ctx.local_peer_id());
    co_await send_bitfield(stream, ctx.piece_manager().bitfield());

    while (true) {
        auto msg = co_await read_bt_message(stream);

        switch (msg.type()) {
        case BtMsg::Piece: {
            auto& pm = ctx.piece_manager();
            pm.write_block(msg.index(), msg.begin(), msg.data());

            // BT 场景的 HealthCheck: 监控 peer 的出块速度
            ctx.health_monitor().report_bt_block(peer_id, msg.data().size());

            if (pm.is_piece_complete(msg.index())) {
                auto valid = co_await verify_piece_hash(msg.index(), ctx);
                if (valid) {
                    pm.mark_verified(msg.index());
                    ctx.broadcast_have(msg.index());
                } else {
                    pm.discard_piece(msg.index());
                    // 记录该 peer 的坏数据, 降低信任度
                    ctx.peer_trust().penalize(peer_id);
                }
            }

            co_await request_pieces(stream, ctx, peer_id);
            break;
        }

        case BtMsg::Unchoke:
            // EndGame 模式: 当只剩 < 5% 的分片时,
            // 向所有 unchoked peer 请求相同的分片 (类似 HedgeWork)
            if (ctx.piece_manager().completion_ratio() > 0.95) {
                co_await request_endgame_pieces(stream, ctx, peer_id);
            } else {
                co_await request_pieces(stream, ctx, peer_id);
            }
            break;

        // ... 其他消息类型
        }
    }
}
```

**BT EndGame 与 HedgeWork 的统一视角：**
BT 协议的 EndGame 模式本质上就是 HedgeWork——在下载尾部向多个 peer 请求相同数据，谁先返回用谁的，取消其余请求。这个模式可以泛化到所有协议。

### 4.4 Metalink / 多源下载

Metalink 是多源下载的标准协议，天然适配 MirrorRanker：

```cpp
Task<void> metalink_download(DownloadTask& task, MetalinkSpec spec) {
    // 1. 解析 Metalink, 获取所有 mirror 和 checksum
    auto mirrors = parse_metalink(spec);
    auto& ranker = task.mirror_ranker();

    for (auto& mirror : mirrors) {
        ranker.add_mirror(mirror.url, mirror.location, mirror.priority);
    }

    // 2. 根据评分分配连接
    auto allocation = ranker.allocate_connections(adaptive_concurrency_.current());

    // 3. 每个 mirror 启动相应数量的 worker
    for (auto& [mirror_id, count] : allocation) {
        for (int i = 0; i < count; i++) {
            spawn_coroutine(
                http_download_worker(task, new_conn_id(),
                                     task.chunk_scheduler(),
                                     task.health_monitor())
            );
        }
    }

    // 4. 定期重新评估 mirror 评分, 调整分配
    while (!task.is_complete()) {
        co_await sleep(5s);
        auto new_allocation = ranker.allocate_connections(
            adaptive_concurrency_.current()
        );
        rebalance_workers(task, new_allocation);
    }

    // 5. 校验完整性
    if (spec.has_checksum()) {
        auto hash = co_await compute_file_hash(task.file_path(), spec.hash_type());
        if (hash != spec.expected_hash()) {
            // 完整性校验失败
        }
    }
}
```

---

## 五、Coordinator 调度循环

Coordinator 是整个系统的大脑，运行主调度循环：

```cpp
class Coordinator {
    void run() {
        while (!shutdown_) {
            // 1. 收集各 IO Worker 的指标 (Channel 接收)
            drain_metrics_channel();

            // 2. 运行 HealthMonitor
            for (auto& task : active_tasks_) {
                auto verdicts = health_monitor_.evaluate_all(task);

                for (auto& [conn, verdict] : verdicts) {
                    switch (verdict) {
                    case Verdict::Slow:
                        // 尝试工作窃取
                        try_steal_work(task, conn);
                        break;
                    case Verdict::Stalled:
                        // 投机对冲
                        hedge_engine_.timeout_hedge(conn);
                        break;
                    case Verdict::Dead:
                        // 替换连接
                        replace_connection(task, conn);
                        break;
                    default:
                        break;
                    }
                }
            }

            // 3. 检查尾部阶段, 启动尾部对冲
            for (auto& task : active_tasks_) {
                if (task.completion_ratio() > 0.9) {
                    hedge_engine_.tail_hedge(task.id());
                }
            }

            // 4. 自适应并发控制
            for (auto& task : active_tasks_) {
                task.adaptive_concurrency().on_probe_complete(
                    task.current_throughput()
                );
            }

            // 5. 分发限速令牌
            rate_limiter_.distribute_tokens();

            // 6. 处理 RPC 修改请求
            process_rpc_mutations();

            // 7. 推送状态快照给 RPC 线程
            if (now() - last_snapshot_ > 500ms) {
                push_snapshots_to_rpc();
                last_snapshot_ = now();
            }

            // 8. 等待下一个调度周期 (200ms)
            sleep_until_next_tick(200ms);
        }
    }
};
```

---

## 六、磁盘 I/O 子系统

延续 greenfield 文档的设计，增加写合并优化：

### 6.1 写缓存 + 合并 + 异步刷盘

```cpp
class WriteCache {
    // per IO Worker, 无锁

    struct PendingWrite {
        int64_t offset;
        OwnedBuffer data;
    };

    // 按文件 + 区域排序, 合并相邻写入
    std::map<FileId, std::map<int64_t, OwnedBuffer>> ordered_writes_;
    size_t total_cached_ = 0;
    static constexpr size_t MAX_CACHE = 32 * 1024 * 1024;  // 32MB per worker

    void cache_write(FileId file, int64_t offset, OwnedBuffer data) {
        auto& file_writes = ordered_writes_[file];

        // 尝试合并: 如果新写入紧邻已有写入, 合并为一个大写入
        auto it = file_writes.lower_bound(offset);
        if (it != file_writes.begin()) {
            auto prev = std::prev(it);
            if (prev->first + prev->second.size() == offset) {
                // 紧邻前一个, 追加
                prev->second.append(data);
                total_cached_ += data.size();
                check_flush();
                return;
            }
        }

        total_cached_ += data.size();
        file_writes[offset] = std::move(data);
        check_flush();
    }

    void check_flush() {
        if (total_cached_ >= MAX_CACHE) {
            flush_all();
        }
    }

    void flush_all() {
        for (auto& [file, writes] : ordered_writes_) {
            // 合并后的写入, 可能一次 writev/scatter-gather 完成
            for (auto& [offset, data] : writes) {
                disk_channel_.send(DiskWriteRequest{file, offset, std::move(data)});
            }
            writes.clear();
        }
        total_cached_ = 0;
    }
};
```

### 6.2 平台 DiskIO 封装

```
Linux:   io_uring   — 零系统调用提交 (SQPOLL 模式), 零拷贝
Windows: IOCP       — 重叠 I/O, 完成端口批量收割
macOS:   kqueue     — 配合 pwrite 线程池 (macOS 无 io_uring 等价物)
```

### 6.3 文件预分配策略

```cpp
Task<void> preallocate_file(FileHandle file, int64_t size) {
    // 策略选择:
    // 1. fallocate (Linux): 最快, 不写零, 立即返回
    // 2. SetFileValidData (Windows): 类似 fallocate, 需要 SE_MANAGE_VOLUME
    // 3. SetEndOfFile (Windows fallback): 可能写零, 较慢
    // 4. 渐进分配: 不预分配, 按需扩展 (SSD 友好, 避免磁盘碎片)

    #if defined(__linux__)
        co_await disk_engine_.submit_fallocate(file, 0, size);
    #elif defined(_WIN32)
        if (has_manage_volume_privilege()) {
            SetFileValidData(file, size);
        } else {
            // 渐进分配: 每次扩展 64MB
            // 避免 SetEndOfFile 写零的长时间阻塞
        }
    #endif
}
```

---

## 七、完整数据流示例

### 7.1 单文件 HTTP 下载 (100MB, 4 个 mirror)

```
时间轴: ──────────────────────────────────────────────────────►

t=0s   TaskOrchestrator 创建任务
       MirrorRanker 评估 4 个 mirror, 初始评分均等
       AdaptiveConcurrency 初始并发 = 4
       ChunkScheduler 准备, 不预分配 segment

t=0.1s 4 个 HTTP 协程启动, 各领取 ~25MB 窗口
       ConnectionPool 并行建连 + TLS 握手

t=0.5s 4 个连接开始传输数据
       HealthMonitor 开始收集速度采样

t=3s   HealthMonitor 第一次评估:
       mirror A: 10 MB/s ★
       mirror B:  8 MB/s
       mirror C:  2 MB/s ← Slow! (< 0.3 × mean 6.67)
       mirror D:  7 MB/s

       WorkStealer: 从 C 的剩余窗口窃取 60% 给新连接 (分配到 A)
       MirrorRanker 更新评分: A > D > B >> C

t=6s   AdaptiveConcurrency: 吞吐量上升, 并发 4 → 6
       新增 2 个连接分配给 mirror A 和 D

t=8s   连接 C 的窃取后窗口完成, C 获得新的小窗口 (2MB)
       其他连接继续高速下载

t=12s  已完成 90%, 进入尾部阶段
       HedgeEngine.tail_hedge(): 对仍在工作的 2 个连接发起对冲
       多出 2 个对冲连接, 分配到不同 mirror

t=13s  对冲连接比原连接更快完成了最后的 chunk
       取消原连接, 下载完成

t=13.1s 校验文件完整性, 清理连接池
```

### 7.2 对比传统 aria2 同场景

```
传统 aria2:
t=0s   预分配 4 个 segment, 各 25MB
t=0.5s 4 个连接开始下载
t=2.5s A/B/D 已完成 (10s × 10/8/7 MB/s)
       C 仅完成 5MB (25% of 25MB), 还需 10 秒
t=12.5s C 终于完成 ← 整个下载被 C 拖了 10 秒

新设计: 13s vs 传统: 12.5s (理论值)
但实际中, C 的慢速会导致:
- 其他连接空闲等待 (浪费带宽)
- 如果 C 断连, 需要重连+重新请求

新设计在 t=3s 就检测到 C 慢并窃取工作, 总时间 ~13s
且在 C 可能断连时已有对冲保护
```

---

## 八、扩展机制

### 8.1 插件 API

```cpp
// 协议插件接口
class ProtocolPlugin {
public:
    virtual ~ProtocolPlugin() = default;

    // 协议匹配
    virtual bool can_handle(std::string_view uri) = 0;

    // 能力声明
    virtual ProtocolCapabilities capabilities() = 0;
    // { .supports_range = true, .supports_multiconnection = true,
    //   .supports_resume = true }

    // 创建下载协程
    virtual Task<void> download(DownloadTask& task,
                                ChunkScheduler& scheduler,
                                HealthMonitor& monitor) = 0;
};

// 后处理插件
class PostProcessor {
public:
    virtual ~PostProcessor() = default;
    virtual Task<void> process(const CompletedTask& task) = 0;
};

// 注册
engine.register_protocol(std::make_unique<Ed2kPlugin>());
engine.register_post_processor(std::make_unique<UnzipProcessor>());
```

### 8.2 脚本扩展 (Lua/WASM)

```cpp
// 用户可以用 Lua 脚本定义:
// - 自定义 URL 重写规则
// - 下载前/后回调
// - 自定义文件命名规则
// - 条件触发 (文件类型 → 后处理)

class ScriptEngine {
    // 沙箱化的 Lua VM 或 WASM runtime
    // 暴露有限 API: task info, file ops, http fetch
};
```

---

## 九、与现有方案的定量对比

### 9.1 理论分析

| 场景 | aria2 | Surge | 本设计 |
|------|-------|-------|--------|
| 4 mirror, 均匀带宽 | 100% 带宽利用 | 100% | 100% |
| 4 mirror, 1 个慢 (0.3x) | ~65% (被慢连接拖住) | ~85% (HealthCheck+StealWork) | ~95% (动态窗口+对冲) |
| CDN 节点切换 (mid-download) | 不感知 | HealthCheck 检测 | 实时检测+自动迁移 |
| 服务器 429 限速 | 重试+退避 | 重试 | 自适应降低并发 |
| 下载尾部 (最后 5%) | 等最慢连接 | HedgeWork | 尾部对冲+多 mirror |
| HTTP/2 服务器 | 多 TCP 连接 | 多 TCP 连接 | 单连接多路复用 |
| 小文件 (<1MB) | 单连接 | 32 连接 (过度) | 自适应 1-2 连接 |

### 9.2 长尾延迟改善

假设 4 连接下载 100MB，其中 1 个连接在 P99 时突然降速至 0.1x：

```
aria2:
  P50 完成时间: 10s
  P99 完成时间: 50s  (被慢连接拖住 5x)
  P99/P50 比率: 5.0

Surge (HealthCheck + StealWork):
  P50: 10s
  P99: 15s  (3s 检测 + 窃取后追回)
  P99/P50: 1.5

本设计 (动态窗口 + 对冲):
  P50: 10s
  P99: 12s  (窗口自然限制暴露面 + 尾部对冲)
  P99/P50: 1.2
```

关键差异：本设计通过小窗口（5 秒目标完成时间）限制了单连接变慢的"爆炸半径"——最多浪费 5 秒的工作量，而非整个 segment 的数十秒。

---

## 十、技术选型

### 10.1 推荐：C++20

| 组件 | 选型 | 理由 |
|------|------|------|
| 语言标准 | C++20 | 原生 coroutine, 与 aria2-zero 代码库可渐进迁移 |
| 协程运行时 | 自建 (基于 C++20 coroutine) | 完全控制调度策略 |
| 网络 I/O | io_uring (Linux) / IOCP (Win) / kqueue (macOS) | 各平台最优 Proactor |
| 磁盘 I/O | io_uring (Linux) / IOCP (Win) / ThreadPool (macOS) | 异步非阻塞 |
| Channel | lock-free SPSC (boost.lockfree 或自建) | 热路径零锁 |
| HTTP 解析 | llhttp | Node.js 验证过的高性能解析器 |
| HTTP/2 | nghttp2 | 成熟的 HTTP/2 实现 |
| TLS | OpenSSL 3.x / Schannel (Win) | 平台原生 TLS |
| DNS | c-ares | 异步 DNS 解析 |
| JSON | simdjson (解析) + rapidjson (生成) | SIMD 加速 |
| 构建 | xmake | 沿用 aria2-zero 构建系统 |
| 内存分配 | mimalloc | 多线程友好，碎片低 |

C++20 的核心优势是 **与 aria2-zero 代码库的渐进迁移路径**。协议解析器、TLS 集成、BT 协议栈等成熟代码可以逐步迁移而非重写。劣势是协程运行时需要自建——C++20 只提供语言级 coroutine 原语（promise_type、co_await），不提供 executor 和 EventLoop，需要约 3000-5000 行基础设施代码。

### 10.2 备选 A：Zig (0.16+)

Zig 0.16 引入了重新设计的 [Io 接口](https://kristoff.it/blog/zig-new-async-io/)，与本设计的 Proactor 协程架构高度对齐。

#### Zig Io 接口与本设计的映射

Zig 的新 async I/O 将并发原语从语言关键字移入标准库，通过 `io` 参数注入——与 Zig 的 allocator 注入模式一致：

```zig
// Zig 0.16: io 参数注入, 由调用者选择 I/O 实现
fn httpDownloadWorker(io: std.Io, task: *DownloadTask, scheduler: *ChunkScheduler) !void {
    while (scheduler.nextWindow()) |window| {
        // 与 C++20 的 co_await stream.read(buf) 语义相同
        // 但不需要 co_await 关键字 — 没有函数着色问题
        const stream = try std.net.tcpConnectToHost(io, task.host, task.port);
        defer stream.close();

        var buf: [8192]u8 = undefined;
        const n = try stream.read(io, &buf);
        // io 参数决定底层行为:
        //   Green Threads 模式 → io_uring 提交 + 用户态栈切换 (Proactor)
        //   Blocking 模式     → 直接调用 recv() (测试/调试)
        //   Thread Pool 模式  → 在工作线程上阻塞调用
    }
}
```

**本设计各组件在 Zig 中的对应：**

| 本设计组件 | C++20 实现 | Zig 0.16 实现 |
|-----------|-----------|--------------|
| 协程运行时 | 自建 executor (3000-5000 行) | **stdlib 内置** — Green Threads + io_uring |
| Proactor 抽象 | 自建 IoEngine (2000-3000 行) | **stdlib 内置** — `std.Io.Evented` |
| EventLoop | 自建 per-thread EventLoop | **stdlib 内置** — per-core green thread scheduler |
| 零拷贝 sendfile | 自建 io_uring SQE 封装 | **stdlib 内置** — `Io.sendFile()` |
| 协程并发 | co_await + 自建 spawn | `io.async()` + `Future.await()` |
| C 库互操作 | extern "C" + 手写 binding | **@cImport 直接导入头文件** — 零 binding 代码 |
| Channel | 自建 lock-free ring buffer | 自建 (Zig 无 stdlib Channel) |
| 定时器 | 自建时间轮 | 自建 (Zig 无 stdlib TimerWheel) |

#### 核心优势

**1. 基础设施免建**

C++20 方案中 Phase 0（协程运行时 + EventLoop + Poller）估计 6 周 5000-8000 行代码。Zig 的 stdlib 直接提供 green threads + io_uring/GCD 后端，这部分**归零**。可以从 Phase 1（HTTP 协议 + 传输优化）直接开始。

**2. 无函数着色**

C++20 的 `co_await` 将函数标记为协程（必须返回 `Task<T>`），这会向上传染——调用协程的函数也必须是协程。Zig 的 Io 接口完全消除了这个问题：同一份代码在 blocking / threaded / green-threaded 模式下行为一致，由**调用者**通过注入不同的 `io` 实例来选择执行模型。

```zig
// 同一个函数, 三种执行模式:
// 1. 单元测试: std.Io.blocking() — 同步阻塞, 确定性调试
// 2. 集成测试: std.Io.threadPool(4) — 4 线程阻塞复用
// 3. 生产环境: std.Io.evented() — io_uring green threads
httpDownloadWorker(io, &task, &scheduler);
```

这对下载器的测试策略意义重大：协议解析器和调度算法可以用 blocking I/O 做单元测试，无需 mock 异步运行时。

**3. C 互操作零成本**

下载器的核心依赖（c-ares、OpenSSL、nghttp2、libssh2、expat、zlib）全是 C 库。Zig 的 comptime `@cImport` 在编译期解析 C 头文件并生成类型安全的绑定，无需手写任何 FFI 代码：

```zig
const c = @cImport({
    @cInclude("ares.h");
    @cInclude("openssl/ssl.h");
    @cInclude("nghttp2/nghttp2.h");
});

// 直接调用, 类型安全
var channel: c.ares_channel = undefined;
c.ares_init(&channel);
```

C++20 虽然与 C 兼容，但 OpenSSL/c-ares 的头文件仍需要 `extern "C"` 包装、手动管理链接。Zig 还能直接编译 C 源码作为构建步骤的一部分（build.zig），无需单独的 C 编译流程。

**4. comptime 元编程**

Zig 的编译期执行能力远超 C++ 模板。可用于：
- 编译期生成 BT 消息解析表（替代运行时 switch-case）
- 编译期验证协议状态机转换的合法性
- 零开销的泛型 Channel 实现（comptime 特化消除 vtable）

#### 风险与缺陷

| 风险 | 严重度 | 缓解措施 |
|------|--------|---------|
| **Pre-1.0 API 不稳定** | 高 | Bun/TigerBeetle 已证明可在 pre-1.0 上构建大型系统；锁定 0.16.x 版本 |
| **IOCP 后端未落地** | 高 | 0.16 green threads 仅实现了 io_uring (Linux) 和 GCD (macOS)。Windows 可能需要自建 IOCP 适配层或等待上游 |
| **io_uring 路径性能回退** | 中 | 官方已知问题，非 release blocker，预计 0.16 GA 前修复 |
| **无 stdlib Channel** | 低 | 自建 lock-free SPSC/MPSC，Zig 的 `@atomicRmw` 提供原子操作原语 |
| **生态不如 C++** | 中 | 核心依赖全是 C 库，Zig 的 C 互操作覆盖了 99% 的需求 |
| **调试工具链不成熟** | 中 | GDB/LLDB 支持 Zig，但 green thread 的调试体验待验证 |

#### 与 C++20 方案的 ROI 对比

| 指标 | C++20 | Zig 0.16 |
|------|-------|----------|
| Phase 0 (运行时基础设施) | 6 周, 5000-8000 行 | **0 周** (stdlib 提供) |
| Phase 1-4 (协议+优化+RPC) | 28 周, 35000-47000 行 | 26 周, 30000-40000 行 |
| C 库集成成本 | 中 (手写 binding/extern) | **极低** (@cImport) |
| Proactor 抽象成本 | 高 (自建 per-platform IoEngine) | **极低** (stdlib Io 接口) |
| 测试成本 | 高 (需 mock 协程运行时) | **低** (blocking Io 直接单测) |
| Windows 支持风险 | 低 (成熟) | **高** (IOCP 后端待确认) |
| 总工期 | ~34 周 | ~26 周 (乐观) / ~30 周 (含 IOCP 适配) |

**结论：** 如果项目可以接受 pre-1.0 语言的风险，Zig 0.16 的 Io 接口是实现本设计最高效的路径——它把设计文档中最难的部分（Proactor 协程运行时）变成了开箱即用的基础设施。关键阻塞因素是 Windows IOCP 后端的成熟度。

### 10.3 备选 B：Rust

如果从零开始且团队有 Rust 经验，Rust 的 tokio 生态提供了最完整的异步运行时（epoll/kqueue/IOCP 全覆盖，io_uring 通过 tokio-uring 支持）。编译期线程安全保证消除了 Channel 实现中的数据竞争风险。但 Rust 的 async/await 有函数着色问题，且与 C 库互操作需要 unsafe + 手写 binding（或 bindgen），成本高于 Zig。考虑到与 aria2-zero 的代码复用路径，Rust 不是首选。

### 10.4 新兴语言评估

| 语言 | 可行性 | 核心障碍 |
|------|--------|---------|
| **Nim** | 不推荐 | asyncdispatch 是**单线程**模型，与 per-core EventLoop 架构根本矛盾；无 io_uring 支持；并发方向仍在演化（"The turbulent evolution of Nim's concurrency story"） |
| **D** | 不推荐 | 默认 GC 与高频 buffer 分配/释放冲突（下载热路径每秒数千次 alloc）；@nogc 需逐函数标注，等于用 D 语法写 C；生态在衰退，难以找到贡献者 |
| **C3** | 不可行 | 无 async/coroutine 支持；标准库 "less mature than the compiler"；无网络/并发库；用 C3 等于手搓全部基础设施 |

---

## 十一、开发路线图

### Phase 0: 基础运行时 (6 周)

```
[Week 1-2] 协程运行时 + EventLoop (per-platform Poller)
[Week 3-4] Channel Bus (SPSC/MPSC) + Coordinator 骨架
[Week 5-6] DiskIO Engine (io_uring/IOCP) + WriteCache
```

交付物：可以在多线程 EventLoop 上调度协程，跨线程 Channel 通信，异步磁盘读写。

### Phase 1: HTTP 核心 + 传输优化 (8 周)

```
[Week 1-2] HTTP/1.1 协议 (Range, keep-alive, chunked)
[Week 3-4] ChunkScheduler (动态窗口) + ConnectionPool
[Week 5-6] HealthMonitor + WorkStealer + AdaptiveConcurrency
[Week 7-8] HedgeEngine + MirrorRanker + TLS 集成
```

交付物：可以高性能下载 HTTP 文件，具备所有长尾优化能力。**这是 MVP。**

### Phase 2: 多协议 (6 周)

```
[Week 1-2] HTTP/2 多路复用
[Week 3-4] FTP/SFTP 协议
[Week 5-6] Metalink 多源下载
```

### Phase 3: BitTorrent (8 周)

```
[Week 1-2] BT 协议核心 (握手, 消息, 分片管理)
[Week 3-4] DHT + PeerExchange + 磁力链接
[Week 5-6] Choke/Unchoke + EndGame (统一为 HedgeWork)
[Week 7-8] BT 多线程扩展 (Owner 模式)
```

### Phase 4: 生产就绪 (6 周)

```
[Week 1-2] RPC Server (JSON-RPC + WebSocket)
[Week 3-4] CLI + 配置系统 + 日志
[Week 5-6] 错误恢复 + 断点续传 + 集成测试
```

总计约 **34 周**（8.5 个月），代码量预估 **40,000-60,000 行**。

---

## 十二、关键创新点总结

| # | 创新点 | 来源/灵感 | 效果 |
|---|--------|----------|------|
| 1 | 动态工作窗口 (替代固定 segment) | IDM 二分法的泛化 + 原创 | 慢连接爆炸半径从整个 segment 降至 5 秒 |
| 2 | 三级长尾防护: StealWork → HedgeWork → Replace | Surge + Google 论文 | P99/P50 比率从 5.0 降至 1.2 |
| 3 | 自适应并发控制 (梯度下降) | FastBioDL 论文 | 自动适配从家庭到数据中心的所有网络环境 |
| 4 | Mirror 加权分配 (替代全选最快) | 原创 | 最快 mirror 限速时仍有 fallback |
| 5 | HTTP/2 多路复用感知 | 现代 Web 标准 | 减少 TCP 连接数, 降低服务器负载 |
| 6 | BT EndGame = HedgeWork 统一 | 跨协议概念统一 | 简化实现, 共享优化逻辑 |
| 7 | 协程 + 任务亲和 (零锁热路径) | greenfield 文档 | BT 分片管理性能与单线程持平 |
| 8 | 写缓存合并 + scatter-gather 刷盘 | 数据库 WAL 思想 | 减少磁盘 IOPS, SSD 友好 |

---

## 参考资料

- [greenfield-multicore-architecture.md](greenfield-multicore-architecture.md) — aria2-zero 多核架构设计
- [iocp-eventpoll-plan.md](iocp-eventpoll-plan.md) — aria2-zero IOCP Reactor 实战经验与踩坑记录
- [Surge Download Manager](https://github.com/SurgeDM/Surge) — HealthCheck / StealWork / HedgeWork
- [The Tail at Scale (Jeff Dean, Luiz Barroso)](https://cacm.acm.org/research/the-tail-at-scale/) — Hedged Requests 理论
- [FastBioDL: Adaptive Parallel Downloader](https://arxiv.org/html/2508.05511v1) — 自适应并发控制
- [IDM Dynamic Segmentation](https://www.internetdownloadmanager.com/support/segmentation.html) — 二分法动态分片
- [Gopeed](https://github.com/GopeedLab/gopeed) — Go+Flutter 跨平台架构参考
- [Zig's New Async I/O (Loris Cro)](https://kristoff.it/blog/zig-new-async-io/) — Zig 0.16 Io 接口设计与函数着色解决方案
- [Zig's New Async I/O Text Version (Andrew Kelley)](https://andrewkelley.me/post/zig-new-async-io-text-version.html) — Io 接口技术细节：green threads, io_uring 集成, sendFile 零拷贝
