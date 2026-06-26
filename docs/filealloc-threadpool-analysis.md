# FileAllocationCommand ThreadPool 异步方案分析

## 1. 方案概述

`FileAllocationCommand` 将文件预分配操作从主事件循环线程卸载到 ThreadPool，是 aria2-zero 中**唯一成功异步化的 RealtimeCommand**。

### 1.1 调用链路

```
DownloadEngine::run()  [主线程]
  → executeCommand()
    → RealtimeCommand::execute()
      → setStatusRealtime()          // 标记为实时命令，每轮都执行
      → e_->setNoWait(true)          // 主循环不等待，立即下一轮
      → FileAllocationCommand::executeInternal()
        ├─ [首次] ThreadPool::enqueue(executeInternalImpl)   // 提交到工作线程
        └─ [后续] future_->wait_for(100ns)                   // 非阻塞轮询
             ├─ ready  → future_->get() → 处理结果
             └─ timeout → addCommand(this) → 下一轮继续轮询
```

### 1.2 核心代码逻辑

```cpp
bool FileAllocationCommand::executeInternal()
{
  bool result = false;

  // 阶段 1: 首次调用，提交任务到 ThreadPool
  if (future_ == nullptr) {
    auto f = getDownloadEngine()->getThreadPool()->enqueue(
        &FileAllocationCommand::executeInternalImpl, this);
    future_ = make_unique<std::future<ExecuteResult>>(std::move(f));
  }

  // 阶段 2: 非阻塞轮询工作线程结果
  if (future_->wait_for(100_ns) == std::future_status::ready) {
    auto execResult = future_->get();
    future_ = nullptr;
    result = std::get<1>(execResult);           // bool: 是否完成
    auto commands = std::move(std::get<0>(execResult));  // 后续命令
    if (result && commands != nullptr) {
      getDownloadEngine()->addCommand(std::move(*commands));
      commands.release();
      getDownloadEngine()->setNoWait(true);
    }
  }

  // 阶段 3: 未完成则重新入队
  if (!result) {
    getDownloadEngine()->addCommand(std::unique_ptr<Command>(this));
  }
  return result;
}
```

```cpp
// 在 ThreadPool 工作线程中执行
FileAllocationCommand::ExecuteResult
FileAllocationCommand::executeInternalImpl()
{
  if (getRequestGroup()->isHaltRequested()) {
    return {nullptr, true};
  }
  fileAllocationEntry_->allocateChunk();        // 实际的文件分配 I/O
  if (fileAllocationEntry_->finished()) {
    // 日志 + 创建后续命令
    A2_LOG_DEBUG(fmt(..., timer_.difference(global::wallclock())...));
    auto commands = make_unique<vector<unique_ptr<Command>>>();
    fileAllocationEntry_->prepareForNextAction(*commands, getDownloadEngine());
    return {std::move(commands), true};
  }
  return {nullptr, false};
}
```

### 1.3 状态机

```
                    ┌──────────────┐
                    │ future_==null│  (初始)
                    └──────┬───────┘
                           │ enqueue → ThreadPool
                           ▼
              ┌─────────────────────────┐
              │  future_ != null        │
         ┌───▶│  wait_for(100ns)        │◀──────┐
         │    └──────┬──────────────────┘       │
         │           │                          │
    timeout          │ ready                    │
    (未完成)         ▼                          │
         │    ┌──────────────┐                  │
         │    │ future_->get │                  │
         │    └──┬───────────┘                  │
         │       │                              │
         │       ├─ result==false ──────────────┘
         │       │   (allocateChunk 未完成,     enqueue 下一轮)
         │       │    future_=null)
         │       │
         └───────┤
                 └─ result==true ──► 完成, 添加后续命令, 销毁 Command
```

---

## 2. 设计优点

### 2.1 主循环零阻塞

`wait_for(100ns)` 实际上是一次 `std::future` 的状态检查（内部通常是一次 mutex trylock + 条件变量状态读取），耗时约 100-500ns，对事件循环几乎无影响。主循环在工作线程执行文件分配期间可以继续处理网络 I/O。

### 2.2 通过返回值传递结果，避免跨线程共享状态

`executeInternalImpl()` 通过 `ExecuteResult` 元组返回所有结果（后续命令列表 + 完成标志），主线程在 `future_->get()` 后拿到所有权，不需要共享可变状态。这是正确的"消息传递"模式。

### 2.3 利用 RealtimeCommand 特性保证轮询频率

`RealtimeCommand::execute()` 每次执行都调用 `setStatusRealtime()` + `setNoWait(true)`，保证主循环**每轮**都会执行此命令，不会因为 EventPoll timeout（默认 1s）而延迟轮询 future 状态。

### 2.4 增量分配

`allocateChunk()` 每次只分配一个 chunk（而非整个文件），所以每次 `executeInternalImpl()` 执行时间有上限。如果未完成（`finished() == false`），返回 `{nullptr, false}`，主线程收到后重新 enqueue 下一轮。

---

## 3. 风险点与问题

### 3.1 ⬤ 线程安全：`global::wallclock()` 竞态读取 — LOW

**位置**: `executeInternalImpl()` 行 106

```cpp
timer_.difference(global::wallclock())
```

`global::wallclock()` 是一个全局 `Timer` 静态实例，由主线程在每轮 `executeCommand()` 前调用 `reset()` 更新。工作线程在无锁情况下读取此值。

**分析**: `Timer` 内部存储的是 `std::chrono::steady_clock::time_point`，在 x64 上是 8 字节原子对齐读取。虽然标准上属于 data race（UB），但实际上：
- 此处仅用于日志输出的耗时统计
- x64 对齐 8 字节读取不会撕裂
- 即使读到过时值，最多日志中的秒数偏差 1s

**严重度**: LOW — 理论上是 UB，实际无功能影响。

### 3.2 ⬤ 线程安全：`A2_LOG_DEBUG` 无锁日志 — MEDIUM

**位置**: `executeInternalImpl()` 行 103

```cpp
A2_LOG_DEBUG(fmt(MSG_ALLOCATION_COMPLETED, ...));
```

`Logger::writeLog()` 内部调用 `fpp_->printf()` 和 `fpp_->flush()` 写入日志文件，以及 `global::cout()->printf()` 写入控制台。**Logger 没有互斥锁保护**。

主线程和工作线程同时写日志会导致：
- 日志行交错/损坏
- `FILE*` 或 `BufferedFile` 内部缓冲区竞态

**分析**: 此日志调用仅在文件分配**完成**时触发一次，与主线程日志并发的概率很低。但如果有多个 `FileAllocationCommand` 同时执行（多个下载任务同时预分配），多个工作线程可能同时写日志。

**严重度**: MEDIUM — 低概率触发，但一旦触发可能导致日志损坏甚至崩溃。

### 3.3 ⬤ 线程安全：`prepareForNextAction` 跨线程访问引擎状态 — HIGH

**位置**: `executeInternalImpl()` 行 110

```cpp
fileAllocationEntry_->prepareForNextAction(*commands, getDownloadEngine());
```

`prepareForNextAction()` 是虚函数，有两个主要实现：

**`StreamFileAllocationEntry::prepareForNextAction()`**:
- `rg->getDownloadContext()` — 读取 RequestGroup 的共享状态
- `dctx->resetDownloadStartTime()` — **写入** DownloadContext 时间戳
- `diskAdaptor->enableMmap()` — **修改** DiskAdaptor 内部状态
- `dctx->getFileEntries()` — 遍历文件条目列表
- 创建 `CreateRequestCommand` / `InitiateConnectionCommand` 等命令对象

**`BtFileAllocationEntry::prepareForNextAction()`**:
- 类似地访问 RequestGroup、DownloadContext、PieceStorage 等共享对象

**分析**: 这些操作在 ThreadPool 工作线程中执行，但访问的对象（RequestGroup、DownloadContext、DownloadEngine）也被主线程持续使用。没有任何同步机制保护这些跨线程访问。

**但有一个重要的缓解因素**: 文件预分配是下载开始**之前**的步骤。在分配完成前，RequestGroup 不会创建下载命令，所以 DownloadContext 等对象在分配期间不会被其他命令并发修改。这是一种**隐式的时序保证**，但不是线程安全保证。

**风险场景**:
- 主线程调用 `RequestGroup::setHaltRequested()` 时，工作线程正在 `prepareForNextAction` 中读取同一 RequestGroup
- 主线程处理 RPC 请求（如 `changeOption`）修改选项，工作线程正在读取选项

**严重度**: HIGH — 依赖隐式时序保证而非显式同步，在特定竞态条件下可能崩溃。

### 3.4 ⬤ 线程安全：`isHaltRequested()` 竞态读取 — LOW

**位置**: `executeInternalImpl()` 行 98

```cpp
if (getRequestGroup()->isHaltRequested()) {
```

`isHaltRequested()` 读取 `haltRequested_` 布尔值，该值可能被主线程（信号处理 / RPC shutdown）修改。无锁读取，标准上是 data race。

**分析**: 即使读到过时的 `false`，最坏情况是多执行一次 `allocateChunk()`，之后主线程会正常停止整个流程。功能上是安全的。

**严重度**: LOW — 理论 UB，实际无功能影响。应使用 `std::atomic<bool>` 修复。

### 3.5 ⬤ 异常处理：工作线程异常传播 — OK

`executeInternalImpl()` 中如果 `allocateChunk()` 抛出异常，异常会被 `std::packaged_task` 捕获并存储在 `std::future` 中。主线程调用 `future_->get()` 时异常会重新抛出，被 `RealtimeCommand::execute()` 的 `catch (RecoverableException&)` 捕获，调用 `handleException()`。

**结论**: 异常传播路径正确，无问题。

### 3.6 ⬤ 生命周期：Command 析构时 future 未完成 — MEDIUM

**场景**: 如果 `FileAllocationCommand` 在 `future_` 仍在执行时被销毁（例如强制停止），析构链路为：

1. `FileAllocationCommand::~FileAllocationCommand()` → `dropPickedEntry()`
2. `future_` 的 `unique_ptr` 析构 → `std::future::~future()` **不等待完成**（`std::future` 析构不 join，只有 `std::async` 返回的 future 才会 join）
3. 工作线程仍在执行 `executeInternalImpl()`，访问已被销毁的 `this->fileAllocationEntry_`、`this->timer_`、`this` 本身

**分析**: `std::packaged_task` + `ThreadPool::enqueue` 模式下，`future` 析构不等待任务完成。如果 `FileAllocationCommand` 在析构时 `future_` 仍在执行，会产生悬垂指针访问。

**触发条件**:
- 用户按 Ctrl+C 强制停止
- RPC `forceShutdown` 或 `forceRemove` 
- `allocateChunk()` 执行时间较长（大文件 fallocate）

**严重度**: MEDIUM — 正常停止流程中 `halt` 标志会在工作线程结束前被检测到，但强制停止场景下可能触发 use-after-free。

### 3.7 ⬤ 性能：轮询开销与 ThreadPool 竞争 — LOW

每次 `executeInternal()` 调用在 future 未就绪时直接返回并重新入队。`RealtimeCommand` 保证每轮主循环都执行，主循环间隔 `A2_DELTA_MILLIS = 300ms`。因此 future 状态检查频率约为 3 次/秒（加上 `setNoWait` 会更频繁），开销极低。

ThreadPool 默认 4 线程。如果多个下载同时预分配（通过 `FileAllocationMan` 的 `SequentialPicker` 串行调度），实际只有一个 `FileAllocationCommand` 在执行。但如果将此模式推广到 CheckIntegrityCommand 等其他命令，需注意 ThreadPool 槽位竞争。

**严重度**: LOW — 当前实现无性能问题。

---

## 4. 与 CheckIntegrityCommand 对比

| 维度 | FileAllocationCommand | CheckIntegrityCommand |
|------|----------------------|----------------------|
| 基类 | RealtimeCommand | RealtimeCommand |
| I/O 类型 | fallocate/truncate（单次调用，耗时确定） | read + hash（循环读取整个 piece） |
| 异步化 | ✅ ThreadPool + future | ❌ 直接在主线程执行 |
| 单次耗时 | 1 chunk = 通常 < 100ms | 1 piece = 最大 16MB 读取 + 哈希 |
| 对事件循环影响 | 无（轮询 100ns） | 严重（阻塞数十到数百毫秒） |

`CheckIntegrityCommand` 可以参照 `FileAllocationCommand` 的模式进行异步化，但需注意：

1. `validateChunk()` 访问 `DiskAdaptor` 读取文件数据，需确保此时没有其他命令并发写入同一文件区域
2. 校验结果（通过/失败）需要安全传回主线程处理
3. `CheckIntegrityEntry::onDownloadFinished()` / `onDownloadIncomplete()` 会操作 RequestGroup 状态，必须在主线程执行

---

## 5. 推广此模式的建议

### 5.1 通用异步 Command 模板

将 `FileAllocationCommand` 的 `enqueue` → `wait_for` → `get` 模式提取为通用基类：

```cpp
template <typename Result>
class AsyncRealtimeCommand : public RealtimeCommand {
  std::unique_ptr<std::future<Result>> future_;
protected:
  // 子类实现：在 ThreadPool 中执行
  virtual Result executeAsync() = 0;
  // 子类实现：在主线程处理结果
  virtual bool handleResult(Result result) = 0;

  bool executeInternal() override {
    if (!future_) {
      future_ = make_unique<std::future<Result>>(
          getDownloadEngine()->getThreadPool()->enqueue(
              &AsyncRealtimeCommand::executeAsync, this));
    }
    if (future_->wait_for(100ns) == std::future_status::ready) {
      auto result = future_->get();
      future_ = nullptr;
      return handleResult(std::move(result));
    }
    getDownloadEngine()->addCommand(unique_ptr<Command>(this));
    return false;  // 未完成
  }
};
```

### 5.2 适合异步化的候选 Command

| Command | 阻塞操作 | 异步化难度 | 收益 |
|---------|---------|-----------|------|
| **CheckIntegrityCommand** | 磁盘读取 + 哈希 | 中（需确保文件无并发写入） | **高** |
| **AutoSaveCommand** | fsync × N + 写控制文件 | 低（save 操作天然独立） | **高** |
| **SaveSessionCommand** | 序列化 + 写文件 | 低 | 中 |
| **DHTAutoSaveCommand** | 写 DHT 路由表 | 低 | 低 |

### 5.3 异步化前需解决的前置问题

1. **Logger 线程安全**: 添加 mutex 保护 `writeLog()`，或使用 lock-free 日志队列
2. **`global::wallclock()` 原子化**: 使用 `std::atomic<time_point>` 或在工作线程中使用独立时钟
3. **生命周期管理**: 在 Command 析构时等待 future 完成，或使用 `shared_ptr` + weak 引用
4. **共享状态隔离**: `prepareForNextAction` 等操作应回到主线程执行，不应在工作线程中访问 DownloadEngine 状态

---

## 6. 总结

`FileAllocationCommand` 的 ThreadPool 异步方案在**核心设计**上是正确的：提交任务 → 非阻塞轮询 → 取回结果。但在**线程安全细节**上存在多处隐患：

| 问题 | 严重度 | 修复方案 |
|------|--------|---------|
| `prepareForNextAction` 在工作线程中访问共享状态 | HIGH | 仅在工作线程中执行纯 I/O（`allocateChunk`），将 `prepareForNextAction` 移到主线程的 `handleResult` 中 |
| 析构时 future 未完成导致 use-after-free | MEDIUM | 在析构函数中 `future_->wait()` 等待完成，或使用 `shared_from_this` |
| `A2_LOG_DEBUG` 日志无锁 | MEDIUM | Logger 添加 mutex |
| `global::wallclock()` 竞态读取 | LOW | 使用 `std::atomic` 或工作线程独立时钟 |
| `isHaltRequested()` 竞态读取 | LOW | 使用 `std::atomic<bool>` |

这些问题在当前实际运行中触发概率较低（文件预分配通常在下载前快速完成，且由 `SequentialPicker` 串行调度），但如果要将此模式推广到其他 Command（尤其是高频执行的 CheckIntegrityCommand 和 AutoSaveCommand），必须先修复上述线程安全问题。
