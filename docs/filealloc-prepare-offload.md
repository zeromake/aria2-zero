# FileAllocationCommand prepareForNextAction 线程卸载方案

## 1. 背景

commit `398b4c21` 将 `FileAllocationCommand::executeInternalImpl()` 中的 `prepareForNextAction()` 调用移入工作线程，与 `allocateChunk()` 一同在 ThreadPool 执行。分配完成后，工作线程构造 `commands` 向量并通过 `ExecuteResult` 传回主线程。

当前代码结构：

```cpp
// 工作线程 (executeInternalImpl)
fileAllocationEntry_->allocateChunk();
if (fileAllocationEntry_->finished()) {
  auto commands = make_unique<vector<unique_ptr<Command>>>();
  fileAllocationEntry_->prepareForNextAction(*commands, getDownloadEngine()); // ← 整体在工作线程
  return {std::move(commands), true};
}

// 主线程 (executeInternal)
if (asyncTask_.checkFinished()) {
  getDownloadEngine()->addCommand(std::move(*commandsPtr));   // 把 commands 加入引擎
  getDownloadEngine()->setNoWait(true);
}
```

**问题**：`prepareForNextAction()` 中混合了**阻塞 I/O**（适合工作线程）和**引擎状态操作**（必须主线程）。当前方案将两者全部放入工作线程，违反了 `threadpool-offload-guide.md` §4.1 规则。

---

## 2. prepareForNextAction 操作分类

### 2.1 StreamFileAllocationEntry::prepareForNextAction

**文件**: `src/stream/StreamFileAllocationEntry.cc:61-111`

| 行号 | 操作 | 类型 | 线程安全性 |
|------|------|------|-----------|
| 64-67 | 获取 rg/dctx/ps/diskAdaptor/option | 读取 | 安全（分配期间无并发修改） |
| 72 | `dctx->resetDownloadStartTime()` | 内存 | 引擎状态 |
| 73-77 | `diskAdaptor->enableMmap()` | 内存标记 | 安全 |
| 78-97 | `peerStat->downloadStart()` + 创建命令 | **引擎状态** | **不安全**：调用 `e->setNoWait(true)`, `rg->createNextCommandWithAdj()` |
| 99 | `rg->createNextCommandWithAdj(commands, e, 0)` | **引擎状态** | **不安全**：访问 `e->newCUID()`, `e->getRequestGroupMan()` |
| 102-110 | `rg->saveControlFile()` | **阻塞 I/O** | 见 §3 分析 |

### 2.2 BtFileAllocationEntry::prepareForNextAction

**文件**: `src/protocol/bt/BtFileAllocationEntry.cc:60-110`

| 行号 | 操作 | 类型 | 线程安全性 |
|------|------|------|-----------|
| 63-67 | 获取 rg/dctx/ps/diskAdaptor/option | 读取 | 安全 |
| 69 | `BtSetup().setup(commands, rg, e, option)` | **引擎状态** | **不安全**：访问 `e->getBtRegistry()`, `e->newCUID()` |
| 70-74 | `diskAdaptor->enableMmap()` | 内存标记 | 安全 |
| 75-83 | `dctx->resetDownloadStartTime()` + 创建命令 | **引擎状态** | **不安全** |
| 85-92 | `rg->saveControlFile()` | **阻塞 I/O** | 见 §3 分析 |
| 95-107 | `diskAdaptor->closeFile/enableReadOnly/openFile` (Windows) | **阻塞 I/O** | 见 §3 分析 |
| 108 | `rg->enableSeedOnly()` | 引擎状态 | 需要主线程 |

### 2.3 saveControlFile 调用链

```
RequestGroup::saveControlFile()                          // src/core/RequestGroup.cc:1292
  ├─ pieceStorage_->flushWrDiskCacheEntry(false)          // 内存缓存 → 文件缓冲区
  ├─ pieceStorage_->getDiskAdaptor()->flushOSBuffers()    // fsync / FlushFileBuffers  ← 主要耗时
  └─ progressInfoFile_->save()                           // DefaultBtProgressInfoFile
       ├─ 序列化到 SHA1IOFile (内存计算)
       ├─ SHA1 对比 → 未变化则跳过
       └─ 有变化:
            ├─ BufferedFile 写临时文件 __temp
            └─ File::renameTo → .aria2
```

| 操作 | 典型耗时 |
|------|---------|
| `flushWrDiskCacheEntry(false)` | 0.1-5ms |
| **`flushOSBuffers()` (fsync)** | **1-500ms** (HDD) |
| SHA1 序列化 + 对比 | 0.01-1ms |
| 写临时文件 + rename | 0.1-10ms |

---

## 3. 线程安全分析

### 3.1 当前方案中工作线程访问引擎状态的问题

| 工作线程中的调用 | 风险 |
|----------------|------|
| `e->setNoWait(true)` | `noWait_` 非原子，主线程 `DownloadEngine::run()` 并发读写 |
| `e->getBtRegistry()` | 返回 `BtRegistry&`，主线程可能并发修改其他 group 的注册项 |
| `e->newCUID()` | 使用 `std::atomic`，**安全** |
| `rg->createNextCommandWithAdj()` | 内部调用 `e->newCUID()`（安全），但创建的 Command 对象需访问引擎指针 |
| `BtSetup().setup()` | 读取 `e->getBtRegistry()` + 创建多个命令对象 |

### 3.2 saveControlFile 在此场景下的线程安全性

**关键发现：FileAllocationCommand 执行期间，当前 RequestGroup 没有其他活跃命令。**

```
RequestGroup 生命周期:
  createInitialCommand()
    → 创建 FileAllocationCommand (唯一命令)
      → allocateChunk() (工作线程)
      → prepareForNextAction() 创建下载/BT 命令
        → 下载命令开始运行 (此后才有并发写入)
```

因此：
- `flushWrDiskCacheEntry()`: 此时 WrDiskCache 无脏页（下载未开始），**安全**
- `flushOSBuffers()`: fsync 对无并发写入的 fd 调用，**安全**
- `progressInfoFile_->save()`: 读取 bitfield/inFlightPieces，此时无并发修改，**安全**
- `closeFile/openFile` (BT Windows): 此时无其他命令访问 DiskAdaptor，**安全**

**结论：`saveControlFile()` 及 BT 的 Windows close/reopen 可安全在工作线程执行，但引擎状态操作不行。**

这与 AutoSaveCommand 场景有本质区别——AutoSaveCommand 在下载运行期间执行 save，存在并发写入，需要快照方案。

---

## 4. 方案设计

### 4.1 核心思路：拆分 I/O 与引擎操作

将 `prepareForNextAction` 拆分为两个虚方法：

```
┌────────────────────────────────────────────────┐
│  FileAllocationCommand 状态机                    │
│                                                 │
│  Stage 1 (Worker thread):                       │
│    allocateChunk()                              │
│    + flushIOAfterAllocation()  ← 新虚方法        │
│      saveControlFile / closeFile+openFile        │
│                                                 │
│  Stage 2 (Main thread):                         │
│    prepareForNextAction()     ← 删除 I/O 操作    │
│      BtSetup / createCommands / setNoWait        │
│      enableSeedOnly / enableMmap                 │
└────────────────────────────────────────────────┘
```

### 4.2 FileAllocationEntry 接口变更

```cpp
// src/storage/FileAllocationEntry.h
class FileAllocationEntry : public RequestGroupEntry, public ProgressAwareEntry {
public:
  // 工作线程调用：仅执行阻塞 I/O（saveControlFile、文件 close/reopen）
  // 分配完成时由 FileAllocationCommand 在工作线程调用
  virtual void flushIOAfterAllocation() = 0;

  // 主线程调用：创建命令、操作引擎状态（不含 I/O）
  virtual void prepareForNextAction(
      std::vector<std::unique_ptr<Command>>& commands,
      DownloadEngine* e) = 0;
};
```

### 4.3 StreamFileAllocationEntry 变更

```cpp
// src/stream/StreamFileAllocationEntry.cc

void StreamFileAllocationEntry::flushIOAfterAllocation()
{
  auto rg = getRequestGroup();
  auto& option = rg->getOption();
  if (option->getAsInt(PREF_AUTO_SAVE_INTERVAL) != 0 &&
      !rg->allDownloadFinished()) {
    try {
      rg->saveControlFile();
    }
    catch (RecoverableException& e) {
      A2_LOG_ERROR_EX(EX_EXCEPTION_CAUGHT, e);
    }
  }
}

void StreamFileAllocationEntry::prepareForNextAction(
    std::vector<std::unique_ptr<Command>>& commands, DownloadEngine* e)
{
  auto rg = getRequestGroup();
  auto& dctx = rg->getDownloadContext();
  auto& ps = rg->getPieceStorage();
  auto diskAdaptor = ps->getDiskAdaptor();
  auto& option = rg->getOption();

  dctx->resetDownloadStartTime();
  if (option->getAsBool(PREF_ENABLE_MMAP) &&
      option->get(PREF_FILE_ALLOCATION) != V_NONE &&
      diskAdaptor->size() <= option->getAsLLInt(PREF_MAX_MMAP_LIMIT)) {
    diskAdaptor->enableMmap();
  }
  if (getNextCommand()) {
    const auto& fileEntries = dctx->getFileEntries();
    for (auto& f : fileEntries) {
      const auto& reqs = f->getInFlightRequests();
      for (auto& req : reqs) {
        const auto& peerStat = req->getPeerStat();
        if (peerStat) {
          peerStat->downloadStart();
        }
      }
    }
    getNextCommand()->setStatus(Command::STATUS_ONESHOT_REALTIME);
    e->setNoWait(true);
    commands.push_back(popNextCommand());
    rg->createNextCommandWithAdj(commands, e, -1);
  }
  else {
    rg->createNextCommandWithAdj(commands, e, 0);
  }
  // saveControlFile 已移至 flushIOAfterAllocation()
}
```

### 4.4 BtFileAllocationEntry 变更

```cpp
// src/protocol/bt/BtFileAllocationEntry.cc

void BtFileAllocationEntry::flushIOAfterAllocation()
{
  auto rg = getRequestGroup();
  auto& option = rg->getOption();

  if (!rg->downloadFinished()) {
    if (option->getAsInt(PREF_AUTO_SAVE_INTERVAL) != 0) {
      try {
        rg->saveControlFile();
      }
      catch (RecoverableException& e) {
        A2_LOG_ERROR_EX(EX_EXCEPTION_CAUGHT, e);
      }
    }
  }
  else {
#ifdef _WIN32
    auto& ps = rg->getPieceStorage();
    auto diskAdaptor = ps->getDiskAdaptor();
    if (!diskAdaptor->isReadOnlyEnabled()) {
      A2_LOG_INFO("Closing files and re-open them with read-only mode enabled.");
      diskAdaptor->closeFile();
      diskAdaptor->enableReadOnly();
      diskAdaptor->openFile();
    }
#endif // _WIN32
  }
}

void BtFileAllocationEntry::prepareForNextAction(
    std::vector<std::unique_ptr<Command>>& commands, DownloadEngine* e)
{
  auto rg = getRequestGroup();
  auto& dctx = rg->getDownloadContext();
  auto& ps = rg->getPieceStorage();
  auto diskAdaptor = ps->getDiskAdaptor();
  auto& option = rg->getOption();

  BtSetup().setup(commands, rg, e, option.get());
  if (option->getAsBool(PREF_ENABLE_MMAP) &&
      option->get(PREF_FILE_ALLOCATION) != V_NONE &&
      diskAdaptor->size() <= option->getAsLLInt(PREF_MAX_MMAP_LIMIT)) {
    diskAdaptor->enableMmap();
  }
  if (!rg->downloadFinished()) {
    dctx->resetDownloadStartTime();
    const auto& fileEntries = dctx->getFileEntries();
    if (isUriSuppliedForRequsetFileEntry(std::begin(fileEntries),
                                         std::end(fileEntries))) {
      rg->createNextCommandWithAdj(commands, e, 0);
    }
    // saveControlFile 已移至 flushIOAfterAllocation()
  }
  else {
    // closeFile/openFile 已移至 flushIOAfterAllocation()
    rg->enableSeedOnly();
  }
}
```

### 4.5 FileAllocationCommand 变更

```cpp
// src/storage/FileAllocationCommand.cc

FileAllocationCommand::ExecuteResult
FileAllocationCommand::executeInternalImpl()
{
  if (getRequestGroup()->isHaltRequested()) {
    return {true};
  }
  fileAllocationEntry_->allocateChunk();
  if (fileAllocationEntry_->finished()) {
    A2_LOG_DEBUG(fmt(MSG_ALLOCATION_COMPLETED, ...));
    // 工作线程：仅执行阻塞 I/O
    fileAllocationEntry_->flushIOAfterAllocation();
    return {true};
  }
  return {false};
}

bool FileAllocationCommand::executeInternal()
{
  if (getRequestGroup()->isHaltRequested()) {
    if (asyncTask_.isRunning() && !asyncTask_.checkFinished()) {
      getDownloadEngine()->addCommand(std::unique_ptr<Command>(this));
      return false;
    }
    return true;
  }

  if (!asyncTask_.isRunning()) {
    asyncTask_.submit(*getDownloadEngine()->getThreadPool(),
      getDownloadEngine(),
      [this]{
        executeResult_ = executeInternalImpl();
      });
  }

  if (asyncTask_.checkFinished()) {
    if (asyncTask_.hasException()) {
      try { asyncTask_.rethrowIfException(); }
      catch (RecoverableException&) { throw; }
      catch (std::exception& e) { throw DL_ABORT_EX(e.what()); }
      catch (...) { throw DL_ABORT_EX("unknown error in file allocation"); }
    }

    if (getRequestGroup()->isHaltRequested()) {
      return true;
    }

    if (executeResult_.finished) {
      // 主线程：创建命令 + 操作引擎状态
      std::vector<std::unique_ptr<Command>> commands;
      fileAllocationEntry_->prepareForNextAction(commands, getDownloadEngine());
      getDownloadEngine()->addCommand(std::move(commands));
      getDownloadEngine()->setNoWait(true);
      return true;
    }
  }

  getDownloadEngine()->addCommand(std::unique_ptr<Command>(this));
  return false;
}
```

```cpp
// src/storage/FileAllocationCommand.h
class FileAllocationCommand : public RealtimeCommand {
  COMMAND_CLASSNAME(FileAllocationCommand)
  struct ExecuteResult {
    bool finished = false;
  };

private:
  FileAllocationEntry* fileAllocationEntry_;
  Timer timer_;
  AsyncTask asyncTask_;
  ExecuteResult executeResult_;
  ExecuteResult executeInternalImpl();
  // ...
};
```

---

## 5. 数据流对比

### 5.1 改造前（commit 398b4c21）

```
工作线程                              主线程
────────                              ──────
allocateChunk()
prepareForNextAction()     ← 引擎状态访问，有竞态风险
  ├─ BtSetup().setup()     ← e->getBtRegistry()
  ├─ createNextCommandWithAdj() ← e->newCUID()
  ├─ e->setNoWait(true)    ← noWait_ 非原子
  └─ saveControlFile()     ← fsync (安全但与上面混合)
  return {commands, true}
                                      addCommand(commands)
```

### 5.2 改造后

```
工作线程                              主线程
────────                              ──────
allocateChunk()
flushIOAfterAllocation()   ← 纯 I/O，无引擎状态访问
  ├─ saveControlFile()     ← fsync + 写文件
  └─ closeFile/openFile    ← (BT Windows)
return {finished: true}
                                      prepareForNextAction()  ← 纯引擎操作，无 I/O
                                        ├─ BtSetup().setup()
                                        ├─ createNextCommandWithAdj()
                                        └─ e->setNoWait(true)
                                      addCommand(commands)
```

---

## 6. 与 AutoSaveCommand 方案的对比

| 维度 | FileAllocationCommand | AutoSaveCommand |
|------|----------------------|-----------------|
| saveControlFile 时的并发写入 | **无**（下载命令未创建） | **有**（下载命令正在运行） |
| 方案复杂度 | 拆分虚方法 | 快照 + 异步写盘 |
| 是否需要 snapshot | **否**（无竞态） | **是**（避免读取竞态） |
| flushWrDiskCacheEntry 安全性 | 安全（缓存为空） | 不安全（需要主线程执行） |
| flushOSBuffers (fsync) 安全性 | 安全（无并发写同一 fd） | 安全（POSIX/Win32 保证） |
| progressInfoFile_->save() | 安全（bitfield 无并发修改） | 不安全（需要 snapshot） |

---

## 7. 改动文件清单

| 文件 | 改动 | 行数估算 |
|------|------|---------|
| `src/storage/FileAllocationEntry.h` | 新增 `flushIOAfterAllocation()` 纯虚声明 | +2 |
| `src/stream/StreamFileAllocationEntry.h` | 新增 `flushIOAfterAllocation()` 声明 | +1 |
| `src/stream/StreamFileAllocationEntry.cc` | 提取 saveControlFile 到 `flushIOAfterAllocation()` | +12, -8 |
| `src/protocol/bt/BtFileAllocationEntry.h` | 新增 `flushIOAfterAllocation()` 声明 | +1 |
| `src/protocol/bt/BtFileAllocationEntry.cc` | 提取 saveControlFile + close/reopen 到 `flushIOAfterAllocation()` | +20, -15 |
| `src/storage/FileAllocationCommand.h` | 简化 `ExecuteResult` 为 `struct {bool finished}` | +3, -3 |
| `src/storage/FileAllocationCommand.cc` | 工作线程调 `flushIOAfterAllocation`，主线程调 `prepareForNextAction` | +10, -15 |
| **合计** | | **~50 行净改动** |

---

## 8. 注意事项

### 8.1 异常处理

`flushIOAfterAllocation()` 中的 `saveControlFile()` 已用 `try-catch(RecoverableException)` 包裹，异常仅记录日志不影响后续流程。工作线程中的 `RecoverableException` 会被 `AsyncTask` 捕获并传回主线程，由 `FileAllocationCommand::executeInternal()` 的异常转换逻辑处理。

### 8.2 enableMmap 的位置

`enableMmap()` 仅设置内存标记 (`enableMmap_ = true`)，不涉及 I/O。保留在主线程的 `prepareForNextAction()` 中。它必须在工作线程的文件操作完成后执行，当前流程天然保证这一点（工作线程完成 → 主线程调用）。

### 8.3 BT downloadFinished 路径

`BtFileAllocationEntry` 的 `downloadFinished()` 分支（文件已完整，仅做种）中：
- `closeFile/enableReadOnly/openFile` → `flushIOAfterAllocation()`（工作线程，I/O）
- `enableSeedOnly()` → `prepareForNextAction()`（主线程，状态变更）

两者无数据依赖：`enableSeedOnly()` 仅设置 `seedOnly_` 标记，不访问 DiskAdaptor 文件句柄。

### 8.4 后续可扩展

`flushIOAfterAllocation()` 接口为未来其他 `FileAllocationEntry` 子类提供统一的工作线程 I/O 钩子。若后续新增子类，只需实现此虚方法即可享受线程卸载。
