# ThreadPool 卸载阻塞指南

本文档说明如何将 aria2-zero 事件循环中的阻塞操作卸载到 ThreadPool，供后续改造 `CheckIntegrityCommand`、`AutoSaveCommand` 等命令时参考。

## 1. 架构概览

```
┌─────────────────────────────────────────────────────┐
│  主线程 (DownloadEngine::run)                        │
│                                                      │
│  ┌──────────┐    ┌──────────┐    ┌───────────────┐   │
│  │ Command  │    │ Command  │    │  EventPoll    │   │
│  │ execute()│    │ execute()│    │  poll(timeout) │   │
│  └──────────┘    └──────────┘    └───────┬───────┘   │
│       │                                  │           │
│       │ submit()              wakeup() ◄─┘           │
│       ▼                          ▲                   │
├───────────────────────────────────┼───────────────────┤
│  ThreadPool (4 线程)              │                   │
│                                   │                   │
│  ┌────────┐  ┌────────┐          │                   │
│  │ Worker │  │ Worker │ ─────────┘                   │
│  │ 纯 I/O │  │ 纯 I/O │  完成后写管道唤醒 EventPoll   │
│  └────────┘  └────────┘                              │
└─────────────────────────────────────────────────────┘
```

核心原则：**工作线程只做纯 I/O，所有引擎状态操作在主线程完成。**

## 2. 基础设施

### 2.1 AsyncTask (`src/util/AsyncTask.h`)

封装 工作线程→主线程 的状态同步，是卸载阻塞操作的核心工具类。

```cpp
class AsyncTask {
  bool running_;                    // 仅主线程读写
  std::atomic<bool> finished_;      // 工作线程 store(release)，主线程 load(acquire)
  std::exception_ptr exception_;    // 工作线程捕获异常，主线程重抛
public:
  ~AsyncTask();                     // 自动等待工作线程完成

  void submit(ThreadPool& pool, DownloadEngine* engine,
              std::function<void()> work);   // 提交任务，完成后自动唤醒 EventPoll
  bool isRunning() const;                    // 是否有任务在执行
  bool checkFinished();                      // 检查完成并重置（可安全发起下一次 submit）
  bool hasException() const;                 // 是否有异常
  void rethrowIfException();                 // 重抛异常
  void waitForCompletion();                  // 阻塞等待完成（仅析构路径）
};
```

### 2.2 WakeupPipe (`src/poll/WakeupPipe.h`)

跨线程唤醒管道。POSIX 使用 `pipe()`，Windows 使用 TCP loopback socketpair。已集成到所有 EventPoll 实现中。

### 2.3 EventPoll::wakeup()

所有 EventPoll 实现均支持 `wakeup()`，工作线程调用后 `poll()` 立即返回。由 `AsyncTask::submit()` 内部自动调用，使用者无需关心。

### 2.4 Logger 线程安全

`Logger` 已添加 `std::mutex` 保护，工作线程可安全调用 `A2_LOG_*` 宏。

### 2.5 ThreadPool::enqueueDetached()

```cpp
template <class F> void enqueueDetached(F&& f);
```

与 `enqueue()` 不同，不创建 `std::packaged_task` / `std::future`，避免不必要的堆分配。`AsyncTask` 内部使用此方法。

## 3. 使用模式

### 3.1 模式 A：RealtimeCommand（一次性阻塞任务）

适用于文件预分配、完整性校验等需要反复提交-等待的场景。

**已有实现**：`FileAllocationCommand`

```cpp
class MyCommand : public RealtimeCommand {
  AsyncTask asyncTask_;

  bool executeInternal() override {
    if (getRequestGroup()->isHaltRequested()) {
      // 异步任务仍在运行时不直接返回，避免析构阻塞主循环
      if (asyncTask_.isRunning() && !asyncTask_.checkFinished()) {
        getDownloadEngine()->addCommand(std::unique_ptr<Command>(this));
        return false;
      }
      return true;
    }

    // 1. 提交阻塞操作到 ThreadPool
    if (!asyncTask_.isRunning()) {
      asyncTask_.submit(*getDownloadEngine()->getThreadPool(),
                        getDownloadEngine(),
                        [/* 捕获纯数据指针 */]() {
                          // 仅执行纯 I/O，不访问 DownloadEngine
                        });
    }

    // 2. 检查是否完成
    if (asyncTask_.checkFinished()) {
      // 3. 异常处理：转换为 RecoverableException 子类
      if (asyncTask_.hasException()) {
        try { asyncTask_.rethrowIfException(); }
        catch (RecoverableException&) { throw; }
        catch (std::exception& e) { throw DL_ABORT_EX(e.what()); }
        catch (...) { throw DL_ABORT_EX("unknown error"); }
      }

      // 4. 主线程处理结果（可安全访问引擎状态）
      // ...
      return true;  // 任务完成
    }

    // 5. 未完成，重新入队
    getDownloadEngine()->addCommand(std::unique_ptr<Command>(this));
    return false;
  }
};
```

### 3.2 模式 B：TimeBasedAsyncCommand（定时阻塞任务）

适用于自动保存、DHT 路由表持久化等定时执行的场景。

**已有基类**：`TimeBasedAsyncCommand`

```cpp
class MyPeriodicCommand : public TimeBasedAsyncCommand {
  void preProcess() override {
    // 主线程执行，每次 execute() 调用
    if (shouldStop()) enableExit();
  }

  void process() override {
    // 工作线程执行，仅在定时间隔到达时触发
    // 只做纯 I/O：写文件、fsync 等
  }

  void postProcess() override {
    // 主线程执行，每次 execute() 调用
  }
};
```

注意事项：
- `enableExit()` 是线程安全的（`exit_` 为 `std::atomic<bool>`），可在 `process()` 中调用
- `process()` 中的异常会被捕获并记录日志，然后 `enableExit()` 退出命令
- `process()` 中调用 `enableExit()` 后，`postProcess()` 会被跳过（与同步版 `TimeBasedCommand` 行为一致）

## 4. 关键规则

### 4.1 工作线程中禁止做的事

| 禁止操作 | 原因 |
|---------|------|
| 访问 `DownloadEngine` | 非线程安全，所有成员均假设单线程访问 |
| 访问 `RequestGroup` 的可变状态 | 主线程可能同时修改（RPC、信号处理） |
| 调用 `addCommand()` | 命令队列非线程安全 |
| 调用 `prepareForNextAction()` | 会创建命令对象并访问引擎状态 |
| 调用 `setNoWait()` | 非原子操作 |

### 4.2 工作线程中允许做的事

| 允许操作 | 说明 |
|---------|------|
| 文件 I/O（open/read/write/fsync/fallocate） | 这正是卸载的目的 |
| `A2_LOG_*` 日志 | Logger 已加锁 |
| 读取不可变配置（Option 值） | 运行期不会修改 |
| 操作独占的数据结构 | 如 `FileAllocationIterator`，分配期间不被其他命令访问 |

### 4.3 异常处理

`AsyncTask` 在工作线程中 `catch(...)` 捕获所有异常，主线程通过 `rethrowIfException()` 重抛。

**RealtimeCommand 子类**（如 FileAllocationCommand）：  
`RealtimeCommand::execute()` 仅 catch `RecoverableException`。必须将其他异常类型转换为 `DlAbortEx`（`RecoverableException` 的子类），否则会导致 `std::terminate`。

**TimeBasedAsyncCommand 子类**：  
基类 `execute()` 已内置 catch-all，异常会记录日志并调用 `enableExit()` 退出命令，不会崩溃。

### 4.4 析构安全

`AsyncTask` 析构函数自动调用 `waitForCompletion()`，通过 `yield` 自旋等待工作线程完成。这保证了 Command 析构时不会出现 use-after-free。

**注意**：如果阻塞操作可能耗时很长（如 `FallocFileAllocationIterator` 在大文件上可达数秒），应在 halt 路径中避免立即析构——改为重新入队等待下一轮检查。参见 `FileAllocationCommand::executeInternal()` 的 halt 处理。

### 4.5 halt 处理模式

```cpp
if (getRequestGroup()->isHaltRequested()) {
  // 异步任务仍在运行时，不直接 return true（会触发析构自旋）
  if (asyncTask_.isRunning() && !asyncTask_.checkFinished()) {
    getDownloadEngine()->addCommand(std::unique_ptr<Command>(this));
    return false;  // 下一轮再检查
  }
  return true;
}
```

## 5. 改造候选清单

以下 Command 当前在主线程执行阻塞操作，可按本指南卸载到 ThreadPool：

| Command | 阻塞操作 | 推荐模式 | 难度 |
|---------|---------|---------|------|
| `CheckIntegrityCommand` | 磁盘读取 + 哈希（最大 16MB/次） | 模式 A | 中 |
| `AutoSaveCommand` | N × fsync + 写控制文件 | 模式 B（改继承 TimeBasedAsyncCommand） | 低 |
| `SaveSessionCommand` | 序列化 + 写会话文件 | 模式 B（改继承 TimeBasedAsyncCommand） | 低 |
| `DHTAutoSaveCommand` | 写 DHT 路由表 | 模式 B（改继承 TimeBasedAsyncCommand） | 低 |

### CheckIntegrityCommand 改造要点

1. `validateChunk()` 在工作线程执行（纯磁盘读取 + 哈希计算）
2. `onDownloadFinished()` / `onDownloadIncomplete()` 必须在主线程（会创建命令、操作 RequestGroup）
3. 注意分配期间文件不能被并发写入——校验阶段下载尚未开始，天然满足

### AutoSaveCommand 改造要点

1. 将基类从 `TimeBasedCommand` 改为 `TimeBasedAsyncCommand`
2. `process()` 中执行 `RequestGroupMan::save()`（含 fsync）
3. `preProcess()` 中执行退出检查（已有逻辑）

## 6. 文件索引

| 文件 | 职责 |
|------|------|
| `src/util/AsyncTask.h` | 异步任务状态机（完成标记 + 异常 + 析构等待） |
| `src/util/AsyncTask.cc` | `submit()` 实现（enqueueDetached + wakeup） |
| `src/poll/WakeupPipe.h` | 跨平台唤醒管道（pipe / TCP socketpair） |
| `src/poll/WakeupPipe.cc` | 管道创建、signal、drain |
| `src/poll/EventPoll.h` | `wakeup()` 纯虚接口 |
| `src/poll/epoll/EpollEventPoll.cc` | epoll 唤醒实现 |
| `src/poll/kqueue/KqueueEventPoll.cc` | kqueue 唤醒实现 |
| `src/poll/poll/PollEventPoll.cc` | poll 唤醒实现 |
| `src/poll/port/PortEventPoll.cc` | Solaris port 唤醒实现 |
| `src/poll/select/SelectEventPoll.cc` | select 唤醒实现（Windows 主路径） |
| `src/poll/libuv/LibuvEventPoll.cc` | libuv uv_async_t 唤醒实现 |
| `src/core/DownloadEngine.h` | `wakeupPoll()` 方法 |
| `compat/ThreadPool.h` | `enqueueDetached()` 无返回值提交 |
| `src/storage/FileAllocationCommand.cc` | 模式 A 参考实现 |
| `src/util/TimeBasedAsyncCommand.cc` | 模式 B 基类实现 |
