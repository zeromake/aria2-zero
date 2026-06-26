# ThreadPool 卸载方案重新设计

## 1. 目标

将当前 `FileAllocationCommand` 和 `TimeBasedAsyncCommand` 中的 ThreadPool 轮询模式（`wait_for(100ns)` 循环）改为**触发式**：工作线程完成后主动唤醒 EventPoll，主循环立即响应。

同时修复现有方案中的线程安全问题。

---

## 2. 当前问题

### 2.1 轮询模式的缺陷

```
现有流程:
  主线程                          工作线程
  ──────                          ────────
  enqueue(task) ──────────────►  开始执行
  wait_for(100ns) → timeout      ...执行中...
  addCommand(this) → 重新入队    ...执行中...
  [等待下一轮主循环 ≤ 1s]        ...执行中...
  wait_for(100ns) → timeout      ...执行中...
  [等待下一轮主循环 ≤ 1s]        执行完毕 ← 结果闲置!
  wait_for(100ns) → ready ✓      
  future_->get()                 
```

问题：工作线程完成后，主循环可能正在 `eventPoll_->poll()` 中阻塞等待（最长 1s），无法立即取回结果。

### 2.2 线程安全问题

| 问题 | 位置 | 严重度 |
|------|------|--------|
| `prepareForNextAction` 在工作线程访问共享状态 | FileAllocationCommand | HIGH |
| Command 析构时 future 未完成 → use-after-free | 两者 | MEDIUM |
| Logger 无锁写入 | 两者 | MEDIUM |

---

## 3. 设计方案

### 3.1 核心思路：EventPoll 唤醒管道

在 EventPoll 中添加一个**唤醒 fd**（wakeup fd），工作线程完成后写入 1 字节，EventPoll 从阻塞的 `poll()`/`select()`/`epoll_wait()` 中立即返回。

```
改进流程:
  主线程                          工作线程
  ──────                          ────────
  enqueue(task) ──────────────►  开始执行
  eventPoll_->poll(1s) 阻塞      ...执行中...
       ↑                         执行完毕
       │ wakeup fd 可读!  ◄──── write(wakeupFd, "x")
       ↓
  poll() 立即返回
  检查完成标志 → ready ✓
  处理结果
```

### 3.2 分层架构

```
┌─────────────────────────────────────────────────┐
│  Command 层                                      │
│  FileAllocationCommand / CheckIntegrityCommand   │
│  AutoSaveCommand / SaveSessionCommand            │
│  (仅关心：提交任务 / 处理结果)                     │
├─────────────────────────────────────────────────┤
│  DownloadEngine 层                               │
│  asyncComplete_ : atomic<bool>                   │
│  completedFutures_ : mutex-protected queue       │
│  (收集完成的 future，唤醒 EventPoll)              │
├─────────────────────────────────────────────────┤
│  EventPoll 层                                    │
│  wakeup() : 写 1 字节到唤醒管道                   │
│  (被唤醒后 poll() 立即返回)                       │
└─────────────────────────────────────────────────┘
```

---

## 4. 实现步骤

### Step 1: EventPoll 添加 `wakeup()` 接口

**修改文件**: `src/poll/EventPoll.h`

```cpp
class EventPoll {
public:
  // ... 现有接口 ...

  // 从任意线程调用，唤醒正在 poll() 中阻塞的主线程
  virtual void wakeup() = 0;
};
```

### Step 2: 各平台实现唤醒管道

每个 EventPoll 实现类添加唤醒机制。核心模式相同，仅系统调用不同。

#### 通用模式（epoll / kqueue / poll / port / select）

```cpp
// 构造函数中创建管道
#ifdef _WIN32
  // Windows: 使用 localhost TCP socketpair
  sock_t wakeupFds_[2];  // [0]=读端, [1]=写端
  // 用 127.0.0.1 TCP socketpair 模拟管道
#else
  // POSIX: 使用 pipe()
  int wakeupFds_[2];     // [0]=读端, [1]=写端
  pipe(wakeupFds_);
  // 设置非阻塞
  fcntl(wakeupFds_[0], F_SETFL, O_NONBLOCK);
  fcntl(wakeupFds_[1], F_SETFL, O_NONBLOCK);
#endif

// 将 wakeupFds_[0] 注册到 poll 机制（仅监听可读）

// wakeup() 实现 — 线程安全
void wakeup() {
  char c = 'w';
  // send/write 对于小数据量是原子的，无需加锁
#ifdef _WIN32
  ::send(wakeupFds_[1], &c, 1, 0);
#else
  ::write(wakeupFds_[1], &c, 1);
#endif
}

// poll() 中唤醒 fd 就绪时，排空管道
void drainWakeupPipe() {
  char buf[64];
#ifdef _WIN32
  while (::recv(wakeupFds_[0], buf, sizeof(buf), 0) > 0) {}
#else
  while (::read(wakeupFds_[0], buf, sizeof(buf)) > 0) {}
#endif
}
```

#### 各实现的注册方式

| 实现 | 注册方式 | 唤醒 fd 处理 |
|------|---------|-------------|
| **EpollEventPoll** | `epoll_ctl(epfd_, EPOLL_CTL_ADD, wakeupFds_[0], {EPOLLIN})` | `epoll_wait` 返回后检查是否为 wakeup fd，调用 `drainWakeupPipe()` |
| **KqueueEventPoll** | `kevent(kqfd_, {EV_ADD, EVFILT_READ, wakeupFds_[0]})` | 同上 |
| **PollEventPoll** | `pollfds_` 数组末尾追加 `{wakeupFds_[0], POLLIN}` | 同上 |
| **PortEventPoll** | `port_associate(port_, PORT_SOURCE_FD, wakeupFds_[0], POLLIN)` | 同上，注意需重新 associate |
| **SelectEventPoll** | `FD_SET(wakeupFds_[0], &rfdset_)` | 同上 |

#### LibuvEventPoll 特殊处理

libuv 已有内建的跨线程唤醒机制 `uv_async_t`，无需管道：

```cpp
// 构造函数
uv_async_t wakeupHandle_;
uv_async_init(loop_, &wakeupHandle_, [](uv_async_t*) {
  // 回调为空即可，目的仅是让 uv_run() 返回
});

// wakeup() — 线程安全
void wakeup() {
  uv_async_send(&wakeupHandle_);  // libuv 保证线程安全
}
```

#### Windows socketpair 辅助函数

Windows 没有 `pipe()`，需用 TCP loopback 模拟：

```cpp
// 在 EventPoll 基类或工具函数中提供
static int createSocketPair(sock_t fds[2]) {
  sock_t listener = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;  // 系统分配端口
  bind(listener, (sockaddr*)&addr, sizeof(addr));
  listen(listener, 1);
  // 获取分配的端口号
  socklen_t len = sizeof(addr);
  getsockname(listener, (sockaddr*)&addr, &len);
  fds[1] = socket(AF_INET, SOCK_STREAM, 0);
  connect(fds[1], (sockaddr*)&addr, sizeof(addr));
  fds[0] = accept(listener, nullptr, nullptr);
  closesocket(listener);
  // 设置非阻塞
  u_long flag = 1;
  ioctlsocket(fds[0], FIONBIO, &flag);
  ioctlsocket(fds[1], FIONBIO, &flag);
  return 0;
}
```

### Step 3: DownloadEngine 添加异步完成通知

**修改文件**: `src/core/DownloadEngine.h`, `src/core/DownloadEngine.cc`

```cpp
class DownloadEngine {
  // ... 现有成员 ...

  // 工作线程完成的回调队列（线程安全）
  std::mutex asyncMutex_;
  std::vector<std::function<void()>> asyncCallbacks_;

public:
  // 工作线程调用：注册回调 + 唤醒 EventPoll
  void postAsync(std::function<void()> callback) {
    {
      std::lock_guard<std::mutex> lock(asyncMutex_);
      asyncCallbacks_.push_back(std::move(callback));
    }
    eventPoll_->wakeup();
  }

  // 主线程调用：在 run() 循环中执行所有已完成的回调
  void processAsyncCallbacks() {
    std::vector<std::function<void()>> callbacks;
    {
      std::lock_guard<std::mutex> lock(asyncMutex_);
      callbacks.swap(asyncCallbacks_);
    }
    for (auto& cb : callbacks) {
      cb();
    }
  }
};
```

**修改 `DownloadEngine::run()`**: 在 `waitData()` 之后、`executeCommand()` 之前添加：

```cpp
int DownloadEngine::run(bool oneshot) {
  while (...) {
    if (!commands_.empty()) {
      waitData();
    }
    noWait_ = false;
    global::wallclock().reset();

    processAsyncCallbacks();  // ← 新增：处理工作线程完成的回调

    calculateStatistics();
    // ... 现有逻辑 ...
  }
}
```

### Step 4: 重构 FileAllocationCommand

**原则**: 工作线程只执行纯 I/O（`allocateChunk`），所有引擎状态操作在主线程完成。

**修改文件**: `src/storage/FileAllocationCommand.h`, `src/storage/FileAllocationCommand.cc`

```cpp
// .h
class FileAllocationCommand : public RealtimeCommand {
  FileAllocationEntry* fileAllocationEntry_;
  Timer timer_;
  bool asyncRunning_ = false;   // 是否有任务在工作线程中
  bool asyncFinished_ = false;  // 工作线程是否已完成本轮

  void asyncWork();             // 工作线程执行的纯 I/O
public:
  // ...
  bool executeInternal() override;
  bool handleException(Exception& e) override;
};
```

```cpp
// .cc
void FileAllocationCommand::asyncWork()
{
  // 仅在工作线程执行纯 I/O，不访问任何引擎状态
  fileAllocationEntry_->allocateChunk();
}

bool FileAllocationCommand::executeInternal()
{
  if (getRequestGroup()->isHaltRequested()) {
    return true;
  }

  if (!asyncRunning_) {
    // 提交到 ThreadPool
    asyncRunning_ = true;
    asyncFinished_ = false;

    getDownloadEngine()->getThreadPool()->enqueue([this]() {
      asyncWork();
      // 完成后通知主线程
      asyncFinished_ = true;
      getDownloadEngine()->eventPoll_->wakeup();
    });
  }

  if (asyncFinished_) {
    asyncRunning_ = false;
    asyncFinished_ = false;

    if (fileAllocationEntry_->finished()) {
      // 在主线程中执行引擎状态操作
      A2_LOG_DEBUG(fmt(MSG_ALLOCATION_COMPLETED,
          static_cast<long int>(
              std::chrono::duration_cast<std::chrono::seconds>(
                  timer_.difference(global::wallclock())).count()),
          getRequestGroup()->getTotalLength()));

      std::vector<std::unique_ptr<Command>> commands;
      fileAllocationEntry_->prepareForNextAction(commands, getDownloadEngine());
      getDownloadEngine()->addCommand(std::move(commands));
      getDownloadEngine()->setNoWait(true);
      return true;
    }
    // chunk 未完成，继续提交下一轮
  }

  // 未完成，重新入队等待
  getDownloadEngine()->addCommand(std::unique_ptr<Command>(this));
  return false;
}
```

**关键改进**:
1. `asyncWork()` 只做 `allocateChunk()`，不访问 Logger、DownloadEngine、RequestGroup
2. `prepareForNextAction()` 移到主线程 `executeInternal()` 中
3. 完成通知通过 `wakeup()` 唤醒 EventPoll
4. `asyncFinished_` 标记用 `bool` 而非 `std::future`（更简单，配合 `wakeup` 不需要 future 的同步语义）

### Step 5: 重构 TimeBasedAsyncCommand

**修改文件**: `src/util/TimeBasedAsyncCommand.h`, `src/util/TimeBasedAsyncCommand.cc`

同样的模式：工作线程只执行 `process()`，`preProcess()`/`postProcess()` 在主线程。

```cpp
// .h
class TimeBasedAsyncCommand : public Command {
  DownloadEngine* e_;
  Timer checkPoint_;
  std::chrono::seconds interval_;
  bool exit_;
  bool routineCommand_;
  bool asyncRunning_ = false;
  bool asyncFinished_ = false;

public:
  virtual void preProcess() {}   // 主线程
  virtual void process() = 0;    // 工作线程
  virtual void postProcess() {}  // 主线程

  bool execute() override;
};
```

```cpp
// .cc
bool TimeBasedAsyncCommand::execute()
{
  preProcess();    // 主线程执行
  if (exit_) return true;

  if (!asyncRunning_ &&
      checkPoint_.difference(global::wallclock()) >= interval_) {
    checkPoint_ = global::wallclock();
    asyncRunning_ = true;
    asyncFinished_ = false;

    e_->getThreadPool()->enqueue([this]() {
      process();   // 工作线程执行纯 I/O
      asyncFinished_ = true;
      e_->eventPoll_->wakeup();
    });
  }

  if (asyncFinished_) {
    asyncRunning_ = false;
    asyncFinished_ = false;
  }

  postProcess();   // 主线程执行
  if (exit_) return true;

  if (routineCommand_) {
    e_->addRoutineCommand(std::unique_ptr<Command>(this));
  } else {
    e_->addCommand(std::unique_ptr<Command>(this));
  }
  return false;
}
```

### Step 6: 析构安全

**问题**: Command 析构时工作线程可能仍在执行 `asyncWork()`/`process()`，访问已销毁的 `this`。

**方案**: 使用 `shared_ptr<atomic<bool>>` 存活标记：

```cpp
class FileAllocationCommand : public RealtimeCommand {
  std::shared_ptr<std::atomic<bool>> alive_ =
      std::make_shared<std::atomic<bool>>(true);
  // ...
};

FileAllocationCommand::~FileAllocationCommand() {
  alive_->store(false);
  // 工作线程的 lambda 持有 alive_ 的拷贝（shared_ptr），
  // 检查 alive_ 后才访问 this
}

// enqueue 时：
auto alive = alive_;  // 拷贝 shared_ptr
getDownloadEngine()->getThreadPool()->enqueue([this, alive]() {
  asyncWork();
  if (alive->load()) {
    asyncFinished_ = true;
    getDownloadEngine()->eventPoll_->wakeup();
  }
  // alive==false 时 this 已销毁，不做任何操作
});
```

但这引入了复杂性。更简单的替代方案：

**替代方案 — 析构时等待完成**:

```cpp
FileAllocationCommand::~FileAllocationCommand() {
  if (asyncRunning_) {
    // 自旋等待工作线程完成（allocateChunk 单次执行时间有上限）
    while (!asyncFinished_) {
      std::this_thread::yield();
    }
  }
  getDownloadEngine()->getFileAllocationMan()->dropPickedEntry();
}
```

这更简单且安全。`allocateChunk()` 单次执行时间通常 < 100ms，自旋等待开销可接受。析构发生在强制停止场景，短暂阻塞可接受。

**注意**: `asyncFinished_` 需声明为 `std::atomic<bool>` 以保证跨线程可见性。`asyncRunning_` 仅在主线程读写，不需要 atomic。

### Step 7: Logger 线程安全

`process()` 中的子类实现（如 AutoSaveCommand）可能调用 `A2_LOG_*`。需要给 Logger 加锁。

**修改文件**: `src/util/Logger.h`, `src/util/Logger.cc`

```cpp
class Logger {
  std::mutex mutex_;  // 新增

  void writeLog(LEVEL level, ...) {
    std::lock_guard<std::mutex> lock(mutex_);  // 新增
    // ... 现有实现 ...
  }
};
```

这是最小改动。Logger 写入频率不高，mutex 开销可忽略。

---

## 5. 改动文件清单

| 文件 | 改动内容 | 改动量 |
|------|---------|-------|
| `src/poll/EventPoll.h` | 添加 `virtual void wakeup() = 0` | 1 行 |
| `src/poll/epoll/EpollEventPoll.{h,cc}` | 管道创建 + 注册 + wakeup + drain | ~40 行 |
| `src/poll/kqueue/KqueueEventPoll.{h,cc}` | 同上 | ~40 行 |
| `src/poll/poll/PollEventPoll.{h,cc}` | 同上 | ~40 行 |
| `src/poll/port/PortEventPoll.{h,cc}` | 同上 | ~40 行 |
| `src/poll/select/SelectEventPoll.{h,cc}` | 同上（含 Windows socketpair） | ~60 行 |
| `src/poll/libuv/LibuvEventPoll.{h,cc}` | uv_async_t 唤醒 | ~15 行 |
| `src/core/DownloadEngine.{h,cc}` | eventPoll_ 访问权限 / processAsyncCallbacks (可选) | ~10 行 |
| `src/storage/FileAllocationCommand.{h,cc}` | 重构为触发式 + 主线程 prepareForNextAction | ~50 行改动 |
| `src/util/TimeBasedAsyncCommand.{h,cc}` | 重构为触发式 + 主线程 pre/postProcess | ~30 行改动 |
| `src/util/Logger.{h,cc}` | 添加 mutex | ~5 行 |
| 总计 | | ~330 行 |

---

## 6. 改造顺序

```
Phase 1: EventPoll 唤醒基础设施
  ├─ 1.1 EventPoll.h 添加 wakeup() 纯虚函数
  ├─ 1.2 实现 Windows socketpair 工具函数
  ├─ 1.3 逐个实现 6 个 EventPoll 子类的唤醒管道
  └─ 1.4 Logger 添加 mutex

Phase 2: FileAllocationCommand 重构
  ├─ 2.1 asyncRunning_ / asyncFinished_ 替代 future_
  ├─ 2.2 asyncWork() 仅做 allocateChunk()
  ├─ 2.3 prepareForNextAction 移到主线程
  └─ 2.4 析构安全（等待完成）

Phase 3: TimeBasedAsyncCommand 重构
  ├─ 3.1 同样的触发式模式
  └─ 3.2 preProcess/postProcess 保持主线程

Phase 4: 推广到新 Command（后续）
  ├─ CheckIntegrityCommand → 异步 validateChunk
  └─ AutoSaveCommand → 继承 TimeBasedAsyncCommand
```

---

## 7. 风险评估

| 风险 | 概率 | 影响 | 缓解 |
|------|------|------|------|
| Windows socketpair 失败（端口耗尽） | 低 | EventPoll 无法创建 | 构造函数检查返回值，失败时回退到轮询模式 |
| wakeup fd 泄漏 | 低 | fd 耗尽 | 在 EventPoll 析构函数中关闭 |
| wakeup 管道写满（大量并发完成） | 极低 | wakeup 丢失 | 管道是非阻塞写入，满了也不影响——主循环下一轮 poll(0) 仍会检查。且管道默认 64KB 缓冲，不可能写满 |
| `asyncFinished_` 可见性问题 | 低 | 主线程看不到完成标志 | 使用 `std::atomic<bool>` + `memory_order_release/acquire` |
| allocateChunk 异常未捕获 | 中 | 工作线程异常终止，asyncFinished 永不为 true | 工作线程 lambda 中 try-catch，设置 error 标志 |

### 异常处理补充

```cpp
getDownloadEngine()->getThreadPool()->enqueue([this]() {
  try {
    asyncWork();
  } catch (...) {
    asyncException_ = std::current_exception();  // 新增成员
  }
  asyncFinished_.store(true, std::memory_order_release);
  getDownloadEngine()->eventPoll_->wakeup();
});

// executeInternal 中检查：
if (asyncFinished_.load(std::memory_order_acquire)) {
  asyncRunning_ = false;
  if (asyncException_) {
    std::rethrow_exception(asyncException_);  // 由 RealtimeCommand::execute() catch
  }
  // ... 正常处理 ...
}
```

---

## 8. 对比总结

| 维度 | 现有方案（轮询） | 新方案（触发） |
|------|---------------|-------------|
| 响应延迟 | 最长 1s（等待 EventPoll timeout） | < 1ms（wakeup 立即返回） |
| CPU 开销 | 每轮主循环 1 次 wait_for（极低） | 仅完成时 1 次 wakeup（更低） |
| 线程安全 | prepareForNextAction 在工作线程 ❌ | 仅 allocateChunk 在工作线程 ✅ |
| 析构安全 | future 析构不等待 ❌ | 析构时自旋等待 ✅ |
| 异常传播 | 通过 future::get() ✅ | 通过 exception_ptr ✅ |
| 代码复杂度 | 简单（future） | 稍复杂（wakeup pipe + atomic） |
| 改动范围 | 无 | ~330 行，跨 EventPoll 所有实现 |

---

## 9. Code Review 后续修复

初版实现经 code review 发现 10 个问题，已全部修复。以下为修复内容。

### 9.1 提取 AsyncTask 工具类

`FileAllocationCommand` 和 `TimeBasedAsyncCommand` 中的异步状态机（`asyncRunning_` / `asyncFinished_` / `asyncException_` + 析构自旋 + enqueue lambda）完全重复。提取为共享工具类 `src/util/AsyncTask.h`：

```cpp
class AsyncTask {
  bool running_ = false;
  std::atomic<bool> finished_{false};
  std::exception_ptr exception_;
public:
  void submit(ThreadPool& pool, DownloadEngine* engine, std::function<void()> work);
  bool checkFinished();       // acquire-load + 重置 finished_ 为 false
  bool hasException() const;
  void rethrowIfException();
  void waitForCompletion();   // 析构用 yield 自旋
};
```

`checkFinished()` 返回 true 时自动重置 `finished_` 为 false，消除残留旧值导致的重复进入问题。

`submit()` 使用新增的 `ThreadPool::enqueueDetached()` 方法（`compat/ThreadPool.h`），跳过 `std::packaged_task` + `std::future` 的堆分配开销。

### 9.2 WakeupPipe 有效性检查

构造失败时 `fds_[1]` 保持为 `(sock_t)-1`。`signal()` 和 `drain()` 原来无条件操作 fd，会向 `INVALID_SOCKET` / `-1` 发送数据，导致唤醒机制静默失效。

修复：`signal()` / `drain()` 开头检查 fd 有效性，无效时直接返回。新增 `isValid()` 方法。

### 9.3 FileAllocationCommand halt 处理

**原问题**：`isHaltRequested()` 返回 true 时直接 `return true`，即使异步任务仍在执行。调用方销毁命令对象，触发析构函数的 yield 自旋，阻塞主事件循环直到磁盘 I/O 完成（`FallocFileAllocationIterator` 可能阻塞数秒）。

**修复**：halt 时若异步任务未完成，重新入队等待下轮，而非直接返回触发析构：

```cpp
if (getRequestGroup()->isHaltRequested()) {
    if (asyncTask_.isRunning() && !asyncTask_.checkFinished()) {
        getDownloadEngine()->addCommand(std::unique_ptr<Command>(this));
        return false;  // 不触发析构，等下轮再检查
    }
    return true;
}
```

### 9.4 异步完成后 halt 复查

**原问题**：异步 `allocateChunk()` 完成后，未重新检查 `isHaltRequested()` 就调用 `prepareForNextAction()`，可能为已取消的下载创建后续命令。

**修复**：`asyncTask_.checkFinished()` 之后、`prepareForNextAction()` 之前重新检查 halt 状态。

### 9.5 异常处理安全

**FileAllocationCommand**：工作线程 `catch(...)` 捕获所有异常，但 `RealtimeCommand::execute()` 仅 catch `RecoverableException`。`std::bad_alloc` 等非 `RecoverableException` 类型会逃逸导致 `std::terminate`。

修复：重抛时将非 `RecoverableException` 转换为 `DlAbortEx`：

```cpp
try { asyncTask_.rethrowIfException(); }
catch (RecoverableException&) { throw; }
catch (std::exception& e) { throw DL_ABORT_EX(e.what()); }
catch (...) { throw DL_ABORT_EX("unknown error in file allocation"); }
```

**TimeBasedAsyncCommand**：`execute()` 无 try/catch，异常逃逸到 `DownloadEngine::run()` 无人捕获。

修复：在 `execute()` 中捕获所有异常，记录日志后调用 `enableExit()`：

```cpp
try { asyncTask_.rethrowIfException(); }
catch (std::exception& ex) { A2_LOG_ERROR(fmt("async process error: %s", ex.what())); }
catch (...) { A2_LOG_ERROR("unknown exception in async process"); }
enableExit();
```

### 9.6 exit_ 数据竞争

**原问题**：`exit_` 是普通 `bool`。工作线程的 `process()` 可能调用 `enableExit()` 写入 `exit_ = true`，而主线程在 `execute()` 中无条件读取 `exit_`（早于 `asyncFinished_` 的 acquire-load），构成 C++ 数据竞争（未定义行为）。

**修复**：`exit_` 改为 `std::atomic<bool>`，`enableExit()` 使用 `store(relaxed)`，读取使用 `load(relaxed)`。

### 9.7 postProcess 行为一致性

**原问题**：旧代码中 `process()` 调用 `enableExit()` 后会跳过 `postProcess()` 直接返回。新代码中 `postProcess()` 在异步完成后无条件执行，行为不一致。

**修复**：异步完成后、`postProcess()` 前检查 `exit_`，为 true 则跳过 `postProcess()` 直接返回。

### 9.8 Logger 互斥锁范围扩展

**原问题**：`mutex_` 仅保护 `writeLog()`，但 `levelEnabled()` 读取 `fpp_`（`std::shared_ptr`）和其他非原子成员时无锁。工作线程调用 `A2_LOG_*` 时若主线程并发调用 `openFile()`/`closeFile()` 修改 `fpp_`，构成数据竞争。

**修复**：
- `levelEnabled()` 加锁
- `openFile()`、`closeFile()`、`setLogLevel()`、`setConsoleLogLevel()`、`setConsoleOutput()`、`setColorOutput()` 均加锁
- `writeLog()` 保持现有锁不变（与 `levelEnabled()` 不嵌套，因前者调用完毕后锁已释放）

### 9.9 修改文件清单

| 文件 | 改动 |
|------|------|
| `compat/ThreadPool.h` | 新增 `enqueueDetached()` 模板方法 |
| `src/util/AsyncTask.h` | **新建**：共享异步任务状态机 |
| `src/util/AsyncTask.cc` | **新建**：`submit()` 实现 |
| `src/poll/WakeupPipe.h` | 新增 `isValid()` |
| `src/poll/WakeupPipe.cc` | `signal()`/`drain()` 有效性检查 |
| `src/storage/FileAllocationCommand.h` | 使用 `AsyncTask` 替代手动三字段 |
| `src/storage/FileAllocationCommand.cc` | halt 处理 + 异常转换 + halt 复查 |
| `src/util/TimeBasedAsyncCommand.h` | 使用 `AsyncTask`，`exit_` 原子化 |
| `src/util/TimeBasedAsyncCommand.cc` | 异常捕获 + exit/postProcess 行为修复 |
| `src/util/Logger.h` | 配置方法声明调整（非 inline） |
| `src/util/Logger.cc` | `levelEnabled()` + 配置方法加锁 |
