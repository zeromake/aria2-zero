# aria2-zero 多线程改造可行性分析报告

> 分析日期: 2026-07-01
> 分析维度: 核心架构 | 网络I/O与协议层 | 数据结构与存储层

---

## 一、项目架构现状

aria2-zero 采用**单线程事件驱动主循环**架构。`DownloadEngine::run()` 是唯一的事件循环（`src/core/DownloadEngine.cc:185-222`），所有网络 I/O 通过 EventPoll 多路复用，所有操作封装为 Command 对象由主循环按状态顺序派发执行。ThreadPool（4 线程）仅用于阻塞磁盘 I/O（文件分配、完整性校验、文件关闭/刷盘）。

**核心发现：DownloadEngine、EventPoll、所有协议层 Command 以及核心数据结构均没有任何 mutex、lock_guard、atomic 或同步原语。** 已有同步仅存在于：ThreadPool 任务队列（`mutex` + `condition_variable`）、AsyncTask 完成标记（`atomic<bool>`）、Logger（`mutex`）、DiskAdaptor 文件 I/O（`mutex`）以及打开文件计数器（`atomic<size_t>`）。

### 主事件循环执行流程

```
while (!commands_.empty() || !routineCommands_.empty()) {
    waitData();                    // EventPoll::poll() 阻塞等待 I/O 事件
    global::wallclock().reset();   // 重置全局时钟
    calculateStatistics();         // 统计计算
    if (刷新间隔到达) {
        executeCommand(commands_, STATUS_ALL);
    } else {
        executeCommand(commands_, STATUS_ACTIVE);
    }
    executeCommand(routineCommands_, STATUS_ALL);
    afterEachIteration();          // 检查全局停止信号
}
```

### 现有 ThreadPool 交互模式

当前线程交互模式清晰：**快照 -> 提交 -> 轮询 -> 回调**

```
主线程                    工作线程
  |                         |
  +- submit() -------------+
  |   running_ = true       |
  |   finished_ = false     +- work()
  |                         +- finished_ = true (release)
  |                         +- engine->wakeupPoll()
  +- checkFinished() <------+
  |   (acquire fence)       |
  |   running_ = false      |
  +- rethrowIfException()   |
```

关键约束：**工作线程不可访问 DownloadEngine 的任何可变共享状态**。

---

## 二、核心数据结构线程安全性分析

### 2.1 BitfieldMan — 风险等级: 致命

**文件**: `src/core/BitfieldMan.h`, `src/core/BitfieldMan.cc`

所有上层 PieceStorage、SegmentMan 都依赖此底层数据结构。

**核心问题 — 字节级读-改-写竞态** (`BitfieldMan.cc:561-574`):

```cpp
bool BitfieldMan::setBitInternal(unsigned char* bitfield, size_t index, bool on)
{
  unsigned char mask = 128 >> (index % 8);
  if (on) {
    bitfield[index / 8] |= mask;   // 非原子 read-modify-write
  } else {
    bitfield[index / 8] &= ~mask;  // 非原子 read-modify-write
  }
}
```

`|=` 和 `&=` 在 `unsigned char` 上不是原子的。两个线程同时设置同一字节中的不同位（如 bit 0 和 bit 1 都在 `bitfield_[0]`），最后一个写入者会覆盖另一个的修改，导致**静默丢失位设置**。8 个连续 piece 索引共享一个字节，BT 下载中碰撞概率极高。

此外 `setBit()` 调用后的 `updateCache()` 遍历整个位域重算 5 个缓存值，并发 `setBit()` 会导致缓存值基于不一致的位域状态。

### 2.2 DefaultPieceStorage — 风险等级: 致命

**文件**: `src/core/DefaultPieceStorage.h:73-303`, `src/core/DefaultPieceStorage.cc`

**无任何同步原语**。关键可变状态：
- `bitfieldMan_` — 分片完成状态位域
- `usedPieces_` (`std::set<shared_ptr<Piece>>`) — 正在使用的分片集合
- `haves_` (`std::deque<HaveEntry>`) — 已完成分片通告队列
- `nextHaveIndex_` (`uint64_t`) — 单调递增计数器

**最危险的操作 — `checkOutPiece()`** (`DefaultPieceStorage.cc:120-144`):

```cpp
std::shared_ptr<Piece> DefaultPieceStorage::checkOutPiece(size_t index, cuid_t cuid)
{
  bitfieldMan_->setUseBit(index);           // 步骤1
  std::shared_ptr<Piece> piece = findUsedPiece(index);  // 步骤2
  if (!piece) {
    piece = std::make_shared<Piece>(...);   // 步骤3
    addUsedPiece(piece);                    // 步骤4
  }
  piece->addUser(cuid);                     // 步骤5
}
```

典型的 check-then-act 复合操作。两个 BT peer 命令可能同时看到同一 piece 未被使用，各自创建并插入 `usedPieces_`，破坏 `std::set` 内部红黑树结构。

### 2.3 SegmentMan — 风险等级: 高

**文件**: `src/core/SegmentMan.h:76-230`, `src/core/SegmentMan.cc`

**无任何同步原语**。核心的 `usedSegmentEntries_` (`std::deque<shared_ptr<SegmentEntry>>`) 是分片所有权注册表。

**特别危险**: `getCleanSegmentIfOwnerIsIdle()` (`SegmentMan.cc:254-283`) 检查其他 cuid 拥有的分片状态，判定空闲则**窃取**。这是根本性的单线程协作模式，多线程下无法简单加锁解决，需要重新设计分片分配协议。

`cancelSegment()` 在迭代 `usedSegmentEntries_` 时执行 erase，多线程下迭代器失效崩溃。

### 2.4 DownloadContext — 风险等级: 高

**文件**: `src/core/DownloadContext.h:58-236`

通过 `shared_ptr` 被 RequestGroup、PieceStorage、SegmentMan、BT 协议类、RPC 方法广泛共享。

**热路径 `updateDownload()`** (`DownloadContext.cc:308-315`) 每个数据接收命令都调用。内部 `SpeedCalc` 使用 `std::deque<std::pair<Timer, size_t>>`，push/pop 完全不安全。同时写入全局 `RequestGroupMan::netStat_`，被 RPC 状态查询读取。

### 2.5 PeerStorage / BtRuntime — 风险等级: 高

**DefaultPeerStorage** 维护三个容器（`unusedPeers_`、`usedPeers_`、`uniqPeers_`）加 `badPeers_` 映射，全部无锁。`returnPeer()` 中调用 `executeChoke()` 遍历 `usedPeers_`，同时 `usedPeers_` 正在被修改。

**BtRuntime** 的 `connections_` 计数器为非原子 `++`/`--`，`halt_` 为普通 `bool`，跨线程可见性不保证。

### 2.6 RequestGroup — 风险等级: 高

`RequestGroup` (`src/core/RequestGroup.h`) 持有大量非线程安全成员：
- `numStreamConnection_` / `numStreamCommand_` / `numCommand_` — 无原子保护
- `haltRequested_` / `forceHaltRequested_` — 普通 bool
- `asyncCleanupPending_` — 普通 bool

---

## 三、DownloadEngine 共享状态分析

| 成员变量 | 用途 | 并发风险 |
|---------|------|---------|
| `eventPoll_` | I/O 事件多路复用 | **极高** — socket 注册/注销/poll 必须同线程 |
| `commands_` | 普通命令队列 | **极高** — 主循环遍历 + Command 自注册 |
| `routineCommands_` | 例行命令队列 | **极高** |
| `requestGroupMan_` | 下载任务管理器 | **极高** — 几乎所有 Command 都访问 |
| `socketPool_` | HTTP 连接池 | 高 — 多 Command 共享复用连接 |
| `cookieStorage_` | Cookie 存储 | 高 |
| `dnsCache_` | DNS 缓存 | 高 — 无锁 `std::set` |
| `cuidCounter_` | 命令唯一 ID 计数器 | 高 — 非原子 `count_++` |
| `btRegistry_` | BT 协议注册表 | 高 |
| `threadPool_` | 工作线程池 | 低 — 已有 mutex 保护 |

---

## 四、EventPoll 层分析

### 4.1 实现概览

项目提供 7 种 EventPoll 实现：Select、Poll、Epoll、Kqueue、Port、Libuv、**IOCP**。**全部 7 种实现内部完全没有锁保护。** 唯一跨线程安全的方法是 `wakeup()`，依赖底层 OS API 的原子性。

| 实现 | wakeup 机制 | 安全来源 |
|------|------------|---------|
| Select/Poll/Epoll/Kqueue/Port | `WakeupPipe::signal()` 写管道 | OS 管道写入原子性 |
| Libuv | `uv_async_send()` | libuv 内建线程安全 |
| IOCP | `PostQueuedCompletionStatus()` | Win32 API 原子性 |

### 4.2 多线程化风险

| 风险项 | 级别 | 说明 |
|--------|------|------|
| `socketEntries_` 并发修改 | **极高** | 所有实现中均为无锁 `std::map` |
| 事件分派回调 | **极高** | 直接修改 Command 的 `readEvent_`/`writeEvent_` |
| Epoll `data.ptr` 悬空指针 | **极高** | 并发修改 map 导致指针指向已释放元素 |
| Libuv `uv_loop_t` | **极高** | 除 `uv_async_send` 外不支持跨线程调用 |

**结论：EventPoll 被设计为纯单线程组件，多线程需要多实例模型。**

---

## 五、网络协议栈分析

### 5.1 HTTP/HTTPS — 并行化友好度: 中

**协议层自身**：所有状态（HttpConnection、HttpRequest、HttpResponse、HttpHeader）均为每连接独立，线程安全性好。

**共享状态耦合点**：
- **CookieStorage** — 存在"读即写"问题：`criteriaFind()` 查找时更新 LRU，`shared_mutex` 读写锁无法直接使用
- **AuthConfigFactory** — `basicCreds_` 在 401 响应时被写入
- **ServerStatMan** — `getOrCreateServerStat()` 存在 TOCTOU 竞争，返回的 `shared_ptr<ServerStat>` 被多 Command 直接修改字段

### 5.2 FTP — 并行化友好度: 高

36 状态协商状态机完全每连接私有。双 Socket 模型（控制+数据）在 PASV 模式下需原子切换事件监听。对 DownloadEngine 共享状态依赖低。

### 5.3 BitTorrent — 并行化友好度: 极低

共享状态最密集的部分。同一 torrent 的所有 peer 连接之间存在至少 10 个高频共享可变数据结构。

**五大关键竞态路径：**

1. **BT 分片选择与分配**（最高竞争）：`getMissingPiece()` → `getAllMissingUnusedIndexes()` → `RarestPieceSelector::select()` → `checkOutPiece()`，两个 peer 线程可能选择同一 index，同时 `setUseBit` 导致位图不一致，`std::set::insert` 可能导致红黑树结构损坏崩溃

2. **BT 分片完成**：`BtPieceMessage::doReceivedAction()` → `completeBlock()` + `updateHash()` → `completePiece()` → `setBit()` + `unsetUseBit()` → `advertisePiece()`。`updateHash()` 依赖 `nextBegin_` 顺序

3. **BT Choke 算法**：`executeChoke()` 遍历所有活跃 peer 并修改标志，与各 peer 连接的 `decideChoking()` 冲突

4. **EndGame 模式 Piece 共享**：同一 Piece 可被多个 peer 连接同时请求，`completeBlock()`/`updateHash()`/`addUser()` 并发执行

5. **速度统计双级更新**：`updateDownload(bytes)` 同时写入每任务和全局 `NetStat`

### 5.4 TLS/SSL — 并行化友好度: 高

TLS Session 实例级隔离良好。主要障碍在 Context 层惰性初始化：

| 后端 | Context 安全性 | 关键风险点 |
|------|---------------|-----------|
| OpenSSL | 安全 | `ERR_error_string` 使用静态缓冲区 |
| WinTLS | **不安全** | `getCredHandle()` 惰性初始化无锁 |
| SChannel | **不安全** | `getCredHandle()` 惰性初始化无锁 |
| AppleTLS | 基本安全 | 静态密码套件缓存竞争 |
| GnuTLS | 安全 | 风险最低 |

**改造量有限**：`std::call_once` 保护惰性初始化即可，约 5-10 个文件。

### 5.5 Socket 与 DNS

**SocketCore** 无任何同步原语。最严重的问题是 `bindAddrs_` 和 `bindAddrsListIt_` 静态变量在 `establishConnection()` 中被**写入**（`SocketCore.cc:480-486`），轮询绑定地址机制在多线程下是严重竞争条件。

**DNS 缓存** (`DNSCache`) 为无锁 `std::set`，无 TTL/过期机制。c-ares 的 `ares_channel` 不是线程安全的，需要每线程一个 channel。

### 5.6 RPC — 并行化友好度: 低

几乎所有 RPC 方法通过 `DownloadEngine* e` 直接访问引擎子系统。`RpcMethodFactory::getMethod()` 使用全局静态 `std::map cache` 懒加载，无锁。

推荐方案：请求解析/响应序列化在工作线程，方法执行仍在主线程通过消息队列投递。

---

## 六、磁盘 I/O 层分析

### 6.1 文件写入并发安全 — 风险等级: 高

`AbstractDiskWriter::writeDataInternal` (`src/storage/AbstractDiskWriter.cc:447-468`):
- **mmap 路径**: 基于地址偏移，天然并发安全
- **非 mmap 路径**: `seek()` + `WriteFile()` 两步操作，共用同一 `fd_`，**最严重的数据损坏风险**

**改造建议**: 改用 `pwrite()`/`pwritev()` 或为每个文件加独立 mutex。

### 6.2 WrDiskCache — 风险等级: 高

完全没有锁保护。内部 `std::set<WrDiskCacheEntry*>` 按大小/时间排序，`int64_t total_` 跟踪缓存量。`add()`/`remove()`/`update()`/`ensureLimit()` 并发执行会破坏迭代器和总量计数。

### 6.3 已有磁盘层同步（少量正确实践）

- `MultiDiskAdaptor`: `std::mutex fileIoMutex_` 保护 `closeFile()`/`tryCloseFile()`/`flushOSBuffers()`
- `AbstractSingleDiskAdaptor`: `std::mutex fileIoMutex_` 保护类似操作
- `OpenedFileCounter`: `std::atomic<size_t>` CAS 无锁计数

### 6.4 CheckIntegrity 并行化潜力: 中等

已通过 `AsyncTask` 卸载到 ThreadPool，但单任务串行执行。每个 piece 的哈希计算完全独立（读数据→计算 MessageDigest→比较），天然可并行。阻碍是 `currentIndex_` 顺序递增设计和 `bitfield_->setBit()` 需要原子化。

---

## 七、第三方依赖线程安全性

| 依赖 | 库自身安全性 | 当前使用 | 多线程化难度 |
|------|------------|---------|-------------|
| **c-ares** | 单 channel 不可并发 | 每 AsyncNameResolver 独占 channel | **低** — 每线程独立 channel |
| **libssh2** | 同一 session 不可并发 | SocketCore 独占 SSHSession | **中** — 保证 session 不跨线程 |
| **SQLite3** | serialized 模式安全 | 仅启动时只读加载 | **低** — 无需改造 |
| **expat** | 单 parser 不可并发 | 每 parser 为栈局部/主线程独占 | **低** — 每线程独立 parser |
| **zlib** | 不同 z_stream 可并行 | 每 Filter/Encoder 独占 | **低** — 已满足独占 |
| **wslay** | **完全不线程安全** | 全部主线程执行 | **高** — 需序列化回主线程 |

---

## 八、全局/静态可变状态清单

| 变量 | 文件位置 | 风险 |
|------|---------|------|
| `global::wallclock()` | `src/util/wallclock.cc:43` | **极高** — 134 处/50 文件读取，每主循环迭代 reset |
| `SocketCore::bindAddrs_` | `src/network/SocketCore.cc:130-137` | **极高** — `establishConnection()` 中被写入 |
| `LogFactory` 6 个静态成员 | `src/core/LogFactory.cc:46-51` | **高** — 905 处/158 文件通过 `A2_LOG_*` 使用 |
| `DHTRegistry::data_/data6_` | `src/protocol/bt/DHTRegistry.cc:49-51` | **高** — DHT 运行时状态，51 处可变引用 |
| `SimpleRandomizer::randomizer_` | `src/util/SimpleRandomizer.cc:63` | **高** — 惰性初始化竞态 |
| `globalHaltRequested` | `src/core/DownloadEngine.cc:93` | **中** — `volatile sig_atomic_t` 非 `std::atomic` |
| `GroupId::set_` | `src/core/GroupId.cc:43` | **高** — 活跃 GID 注册表无锁 |
| `SingletonHolder<T>` | `src/core/SingletonHolder.h:44-58` | **高** — 57 个文件/217 处使用，无锁 |

---

## 九、内存管理模式

### 智能指针使用概况

| 类型 | 使用处数 | 说明 |
|------|---------|------|
| `std::shared_ptr` | **1143** | 核心对象广泛共享 |
| `std::unique_ptr` | 634 | 独占所有权 |
| `std::weak_ptr` | **2** | 几乎不用 — 缺少安全观察机制 |

### 裸指针共享

约 **164 个裸指针成员声明**分布在 81 个头文件中：
- `DownloadEngine*` 出现 16 次（所有 Command 通过 `e_` 持有）
- `RequestGroup*` 出现 10 次
- `DefaultBtMessageFactory` 单类持有 **10 个裸指针**
- `Event.h` 中 `Command*`: EventPoll 持有 Command 裸指针用于事件分发

### 所有权链

```
DownloadEngine (整个生命周期)
  +-- RequestGroupMan (unique_ptr)
  |     +-- requestGroups_: IndexedList<gid, shared_ptr<RequestGroup>>
  |     +-- wrDiskCache_ (unique_ptr)
  +-- BtRegistry (unique_ptr)
  |     +-- pool_: map<gid, unique_ptr<BtObject>>
  |           +-- BtObject: shared_ptr<DownloadContext/PieceStorage/PeerStorage/...>
  +-- commands_/routineCommands_: deque<unique_ptr<Command>>
  +-- ThreadPool (unique_ptr, 4 线程)

RequestGroup
  +-- downloadContext_ (shared_ptr)
  |     +-- ownerRequestGroup_ (裸指针反向引用)
  +-- segmentMan_ (shared_ptr)
  +-- pieceStorage_ (shared_ptr)
  +-- requestGroupMan_ (裸指针反向引用)
```

---

## 十、已发现的并发 Bug

### Bug 1: CheckIntegrityCommand 析构顺序 (use-after-free)

**文件**: `src/core/CheckIntegrityCommand.cc:61-65`

成员声明顺序：`entry_` 在 `asyncTask_` 之前。析构时先释放 entry 指向的对象（`dropPickedEntry()`），再等待工作线程（`asyncTask_` 析构调用 `waitForCompletion()`）。如果工作线程正在执行 `entry_->validateChunk()`，已释放的 entry 构成 use-after-free。

### Bug 2: FileAllocationCommand 同样的析构顺序

**文件**: `src/storage/FileAllocationCommand.cc:63-67`

同 Bug 1 的模式。

### Bug 3: LogFactory::reconfigure() 锁协议违反

**文件**: `src/core/LogFactory.cc:82-93`

`reconfigure()` 直接调用 `logger_->closeFile()`，该方法是内部方法（需调用方持有 `mutex_`）。如果工作线程正在 `writeLog()` 中持有 `mutex_` 并使用 `fpp_`，`reconfigure()` 将 `fpp_` 置 null 导致 use-after-free。触发路径: `RpcMethodImpl.cc:1680` → `LogFactory::reconfigure()`。

---

## 十一、综合风险评估矩阵

### 各维度风险总览

| 维度 | 风险等级 | 核心问题 |
|------|---------|---------|
| BitfieldMan | **致命** | 字节级非原子 RMW，静默丢失位设置 |
| DefaultPieceStorage | **致命** | check-then-act 复合操作，std::set 并发修改 |
| 磁盘写入 (seek+write) | **高** | 文件指针竞态导致数据写入错误偏移 |
| SegmentMan | **高** | 分片窃取机制假设单线程协作 |
| WrDiskCache | **高** | 全无同步，缓存淘汰与写入并发 |
| 全局 wallclock | **高** | 134 处使用，每轮 reset，无原子保护 |
| 命令队列 | **极高** | commands_ 自引用 + EventPoll 绑定 |
| BT 协议层 | **极高** | 10+ 共享结构，跨 peer 强耦合 |
| EventPoll | **极高** | 纯单线程设计，所有实现无锁 |
| 日志系统 | **中** | Logger 内安全但 reconfigure() 有 bug |
| 裸指针反向引用 (164处) | **中** | 多线程下悬空指针风险 |
| 第三方依赖 | **低** | 除 wslay 外均支持实例级并发 |

### 各维度多线程化难度

| 维度 | 协议层自身安全性 | 共享状态耦合 | 改造难度 | 收益预期 |
|------|-----------------|-------------|---------|---------|
| EventPoll | 完全不安全 | 极高（核心枢纽） | **极高** | 低（已高效） |
| HTTP/HTTPS | 高（每连接独立） | 中 | **中等** | 中 |
| FTP | 高（每连接独立） | 低 | **较低** | 低 |
| BitTorrent | 极低 | 极高 | **极高** | 高（peer 多时） |
| TLS/SSL | 高（Session 独立） | 低 | **低** | 中 |
| Socket/DNS | 中 | 中 | **中等** | 中 |
| RPC | 中（解析独立） | 极高 | **高** | 低 |

---

## 十二、改造方案

### 方案 A: 渐进式 — 扩展 ThreadPool 卸载范围（推荐）

维持单线程事件循环主架构不变，将更多阻塞操作卸载到 ThreadPool。

#### 具体措施

1. **修复已知并发 bug**（优先）
   - 调整 `CheckIntegrityCommand`/`FileAllocationCommand` 的成员声明顺序（`asyncTask_` 应在 `entry_` 之前声明，确保先等待工作线程完成）
   - 修复 `LogFactory::reconfigure()` 的锁协议
   - `globalHaltRequested` 改为 `std::atomic<int>`

2. **扩展 ThreadPool 卸载范围**
   - 磁盘 I/O：将 `seek()+write()` 改为 `pwrite()`，消除文件指针竞态
   - TLS 握手 CPU 计算密集部分可卸载
   - 大文件哈希校验并行化（当前已单任务串行，可扩展为多分片并行）
   - 压缩/解压等 CPU 密集操作

3. **TLS Context 惰性初始化修复**
   - WinTLS/SChannel 的 `getCredHandle()` 用 `std::call_once` 保护
   - AppleTLS 静态密码套件缓存用 `std::call_once`

**改造量**: 约 10-15 个文件，100-200 行代码变更
**风险**: 低
**预期收益**: 减少主循环阻塞时间 30-50%

#### 优点
- 改动量小，风险可控
- 不触及核心数据结构和架构
- 已有成熟的 AsyncTask 模式可复用

#### 缺点
- 受限于单线程主循环吞吐瓶颈
- 不能利用多核处理大量并发下载

---

### 方案 B: 中等规模 — 每 RequestGroup 独立线程

每个（或每组）RequestGroup 在独立线程中运行，拥有独立的 EventPoll + 命令队列。

#### 具体措施

1. **核心数据结构改造**
   - `BitfieldMan`: 改为 `std::atomic<uint8_t>[]` + `fetch_or`/`fetch_and`
   - `DefaultPieceStorage`: 加 `std::mutex` 保护事务操作
   - `SegmentMan`: 加 `std::mutex` + 重新设计分片窃取协议
   - `WrDiskCache`: 全局 mutex 或改为 per-RequestGroup 独立缓存

2. **全局状态改造**
   - `global::wallclock()` 改为 `thread_local`
   - `CUIDCounter` 改用 `std::atomic<int64_t>`
   - `SingletonHolder` 加锁或消除
   - `LogFactory` 初始化改为 `std::call_once`
   - `SocketCore::bindAddrs_` 加 `std::atomic` 或锁

3. **共享状态加锁**
   - `dnsCache_`、`socketPool_`、`cookieStorage_` 使用 read-write lock
   - `ServerStatMan` 采用 COW 策略
   - `RequestGroupMan` 队列操作加锁

4. **架构重设计**
   - `DownloadEngine` 拆分为全局协调器 + 每组执行器
   - 采用**多 EventPoll 实例模型**（每线程独立 EventPoll）
   - RPC 执行仍在主线程，通过消息队列投递
   - wslay WebSocket 保持在独立 I/O 线程

**改造量**: 40-60 个文件，3000-5000 行代码变更
**风险**: 中-高
**预期收益**: 多下载任务可并行，适合大量并发下载

#### 优点
- 充分利用多核处理能力
- 不同下载任务天然隔离

#### 缺点
- BT 协议的跨 RequestGroup 交互（DHT、PeerExchange）需额外同步
- 需要对 5+ 个核心类加锁
- 需要数周时间 + 大量回归测试

---

### 方案 C: 大规模重构 — Actor 模型（不推荐）

将核心子系统设计为独立 Actor，通过消息队列通信。

**优点**: 从根本上消除共享可变状态
**缺点**: 工作量巨大，需重写 Command 调度架构，ROI 不足以支撑

---

### 方案 D: Command 级别并行化（不可行）

在主循环内对 commands_ 队列中的 Command 并行 execute()。

**不可行原因**:
1. `Command::execute()` 内部 `e->addCommand(this)` 自注册，队列并发修改
2. EventPoll 的 `addEvents`/`deleteEvents` 不支持并发
3. 同 RequestGroup 的 Command 共享 SegmentMan，细粒度锁竞争抵消并行收益
4. 全局 wallclock 语义崩溃
5. 涉及 80+ 个 Command 子类文件

---

## 十三、结论

aria2-zero 的单线程事件驱动架构**经过精心设计**，核心优势在于**无锁编程带来的简洁性和确定性**。164 个裸指针反向引用、1143 处 `shared_ptr` 共享、零个核心数据结构有同步保护 — 项目从设计之初就没有考虑多线程并发。

**推荐路径**: **方案 A（渐进式）为首选**。首先修复已发现的 3 个并发 bug，然后沿着现有 AsyncTask 模式扩展 ThreadPool 卸载范围。这条路径改动最小、风险最低、且已有成熟模式可复用。

**方案 B（per-RequestGroup 线程）可作为中期目标**探索，特别是在需要支持大量并发下载的场景下。但需要投入数周时间进行系统性改造和大量回归测试。最理想的架构目标是**多 EventPoll 实例 + per-thread 连接亲和性**，将跨线程交互最小化到任务分配和统计汇总层面。

**方案 C 和 D 不推荐**，前者 ROI 不足，后者技术上不可行。
