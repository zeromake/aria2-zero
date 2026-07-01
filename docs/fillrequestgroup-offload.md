# FillRequestGroupCommand 线程卸载方案

## 1. 现状

`FillRequestGroupCommand` 是 routineCommand，每轮事件循环执行。当有下载完成/失败/暂停时，通过 `removeStoppedGroup()` → `ProcessStoppedRequestGroup` 清理已停止的任务组，执行大量阻塞文件 I/O。

```
FillRequestGroupCommand::execute()
  └─ rgman->fillRequestGroupFromReserver(e, batch)
       └─ removeStoppedGroup(e, batch)
            └─ requestGroups_.remove_if(ProcessStoppedRequestGroup)
                 └─ 对每个 numCommand==0 的 group:
                      ├─ group->closeFile()             ← fsync + 关闭文件
                      ├─ group->saveControlFile()       ← 写 .aria2 文件
                      ├─ group->applyLastModifiedTime() ← utime
                      ├─ group->removeControlFile()     ← 删除 .aria2
                      ├─ saveSignature()                ← 写 .sig 文件
                      └─ File::remove() × N             ← 删除 BT 未选择文件
```

## 2. 阻塞操作详细分析

### 2.1 关键发现：fsync 在 closeFile 内部

```cpp
// RequestGroup::closeFile() — src/core/RequestGroup.cc:210
void RequestGroup::closeFile() {
  if (pieceStorage_) {
    pieceStorage_->flushWrDiskCacheEntry(true);   // 刷写缓存 + 清除条目
    pieceStorage_->getDiskAdaptor()->flushOSBuffers(); // ← fsync (1-500ms)
    pieceStorage_->getDiskAdaptor()->closeFile();      // 关闭 fd
  }
}

// RequestGroup::saveControlFile() — src/core/RequestGroup.cc:1300
void RequestGroup::saveControlFile() const {
  if (saveControlFile_) {
    if (pieceStorage_) {
      pieceStorage_->flushWrDiskCacheEntry(false);         // ← closeFile 后 no-op
      pieceStorage_->getDiskAdaptor()->flushOSBuffers();   // ← closeFile 后 no-op (fd 已关闭)
    }
    progressInfoFile_->save();   // 写 .aria2 文件 (0.1-10ms)
  }
}
```

**实际阻塞链**：`closeFile()` 包含 fsync，是主要瓶颈（1-500ms）。之后 `saveControlFile()` 中的 fsync 已经是 no-op（fd 已关闭），只剩 `progressInfoFile_->save()` 写文件（0.1-10ms）。

### 2.2 耗时评估

| 操作 | 典型耗时 | 占比 |
|------|---------|------|
| **`flushOSBuffers()` (fsync)** | **1-500ms** | **>90%** |
| `closeFile()` on DiskAdaptor | 0.1-5ms | ~2% |
| `flushWrDiskCacheEntry(true)` | 0.1-5ms | ~2%（必须主线程） |
| `progressInfoFile_->save()` | 0.1-10ms | ~3% |
| `applyLastModifiedTime` / `removeControlFile` / `saveSignature` / `File::remove` | 各 <1ms | <3% |

**结论：卸载 fsync + close + 控制文件写入/删除即可获得 >95% 收益，其余 I/O 合计 <10ms，保留在主线程可接受。**

### 2.3 ProcessStoppedRequestGroup 操作分类

```
ProcessStoppedRequestGroup::operator()(group) {
  // ═══ 内存操作 ═══
  collectStat(group);                           // 读 PeerStats，更新 ServerStat
  decreaseNumActive();                          // 计数器
  resetDownloadStopTime();                      // 时间戳

  // ═══ 阻塞 I/O ═══
  group->closeFile();                           // flushCache + fsync + close ← 需要卸载
  //   在 pause/finished/incomplete 分支中:
  group->saveControlFile();                     // 写 .aria2 ← 快照后卸载
  group->applyLastModifiedTimeToLocalFiles();   // utime (<1ms)
  group->removeControlFile();                   // 删除 .aria2 ← 延迟到 fsync 后
  saveSignature(group);                         // 写 .sig (<5ms)
  File(path).remove();                          // 删除 BT 文件 × N (<1ms each)

  // ═══ 引擎状态操作 (必须主线程) ═══
  reportDownloadFinished();
  postDownloadProcessing(nextGroups);
  insertReservedGroup(nextGroups);
  releaseRuntimeResource(e);
  executeHookByOptName();
  notifyDownloadEvent();
  addDownloadResult();
  executeStopHook();
}
```

## 3. 方案设计

### 3.1 核心思路

将阻塞 I/O 拆分为主线程快速路径和工作线程异步路径。通过 `GroupCleanupBatch` 收集所有需要异步处理的项目：

```
主线程 (快, <10ms):
  ├─ flushCacheOnly()                           ← 刷写 WrDiskCache（共享资源）
  ├─ adaptor->detachOpenedFileCounter()         ← 预递减计数器，防止驱逐不一致
  ├─ snapshotControlFile() → serializeToBuffer  ← 快照 .aria2 数据到内存 buffer
  ├─ applyLastModifiedTime / saveSignature      ← 保留同步（各 <5ms）
  ├─ 收集 shared_ptr<DiskAdaptor> 到 batch
  ├─ 收集 .aria2 快照和路径到 batch
  └─ 所有引擎状态操作

工作线程 (慢, 异步):
  ├─ adaptor->flushOSBuffers()                  ← fsync (1-500ms)
  ├─ adaptor->closeFile()                       ← 关闭 fd
  ├─ writeControlFileSnapshots()                ← 写 .aria2（fsync 之后，保证崩溃一致性）
  └─ File(path).remove()                        ← 删除 .aria2（仅已完成的下载）
```

### 3.2 GroupCleanupBatch 结构

```cpp
// src/core/RequestGroupMan.h

struct ControlFileSaveEntry {
  std::string data;     // serializeToBuffer() 快照的二进制数据
  std::string filePath; // .aria2 控制文件路径
};

struct GroupCleanupBatch {
  std::vector<std::shared_ptr<DiskAdaptor>> adaptors;          // fsync + close
  std::vector<ControlFileSaveEntry> controlFilesToSave;        // 快照写入
  std::vector<std::string> controlFilesToRemove;               // .aria2 删除
  std::vector<std::weak_ptr<RequestGroup>> groupsToClearPending; // 清除标志

  bool empty() const;
  void mergeFrom(GroupCleanupBatch& other);  // move-merge 另一个 batch
};
```

### 3.3 RequestGroup 新增方法

```cpp
// src/core/RequestGroup.h

// 主线程调用：刷写 WrDiskCache（共享资源），不 fsync、不关闭文件
void flushCacheOnly();

// asyncCleanupPending_ 标志：工作线程正在执行异步 fsync + close 期间不可重新激活
void setAsyncCleanupPending(bool v);
bool isAsyncCleanupPending() const;
```

`saveControlFileNoSync()` 仍保留但不再被 `ProcessStoppedRequestGroup` 使用——控制文件保存改为快照方式。

### 3.4 ProcessStoppedRequestGroup 变更

```cpp
bool operator()(const RequestGroupList::value_type& group) {
  if (group->getNumCommand() == 0) {
    collectStat(group);
    // ...

    try {
      // 主线程：刷写 WrDiskCache
      group->flushCacheOnly();

      if (group->getPieceStorage()) {
        auto adaptor = group->getPieceStorage()->getDiskAdaptor();
        // 预递减 OpenedFileCounter，防止 group 移除后驱逐路径找不到文件
        adaptor->detachOpenedFileCounter();
        batch_.adaptors.push_back(std::move(adaptor));
        group->setAsyncCleanupPending(true);
        batch_.groupsToClearPending.push_back(group);
      }

      if (group->isPauseRequested()) {
        snapshotControlFile(group);  // 快照 .aria2 到 batch
      }
      else if (group->downloadFinished() && ...) {
        group->applyLastModifiedTimeToLocalFiles();
        group->reportDownloadFinished();
        if (group->allDownloadFinished() && !forceSave) {
          // .aria2 删除延迟到 fsync 完成后
          batch_.controlFilesToRemove.push_back(
              group->getProgressInfoFile()->getFilename());
          saveSignature(group);
        }
        else {
          snapshotControlFile(group);
        }
        // postDownloadProcessing / BT file removal 保持不变 ...
      }
      else {
        snapshotControlFile(group);  // error/incomplete
      }
    }
    catch (RecoverableException& ex) { ... }

    // 引擎状态操作完全不变
    // ...
    return true;
  }
  return false;
}
```

其中 `snapshotControlFile` 通过 `serializeToBuffer()` 在主线程将控制文件数据序列化到内存 buffer，存入 `batch_.controlFilesToSave`。工作线程 fsync 数据文件后再将 buffer 写入磁盘，保证 .aria2 内容不会超前于数据文件。

### 3.5 FillRequestGroupCommand 变更

```cpp
// src/core/FillRequestGroupCommand.h
class FillRequestGroupCommand : public Command {
private:
  DownloadEngine* e_;
  Timer lastExecTime;
  AsyncTask asyncTask_;
  std::vector<std::weak_ptr<RequestGroup>> activeGroupClears_;
  GroupCleanupBatch pendingBatch_;

  void handleAsyncCompletion();     // 清除 asyncCleanupPending 标志 + 记录异常
  void drainPendingSynchronously(); // halt/downloadFinished 时同步排空
  void trySubmitPending();          // 提交 pendingBatch_ 到线程池
};
```

```cpp
// src/core/FillRequestGroupCommand.cc
bool FillRequestGroupCommand::execute()
{
  if (e_->isHaltRequested()) {
    if (asyncTask_.isRunning() && !asyncTask_.checkFinished()) {
      e_->addRoutineCommand(std::unique_ptr<Command>(this));
      return false;
    }
    handleAsyncCompletion();
    drainPendingSynchronously();
    return true;
  }

  if (asyncTask_.isRunning() && asyncTask_.checkFinished()) {
    handleAsyncCompletion();
  }

  auto& rgman = e_->getRequestGroupMan();
  if (rgman->queueCheckRequested()) {
    GroupCleanupBatch batch;
    while (rgman->queueCheckRequested()) {
      try {
        rgman->clearQueueCheck();
        rgman->fillRequestGroupFromReserver(e_, batch);
      }
      catch (RecoverableException& ex) {
        A2_LOG_ERROR_EX(EX_EXCEPTION_CAUGHT, ex);
        rgman->requestQueueCheck();
      }
    }
    pendingBatch_.mergeFrom(batch);
  }

  trySubmitPending();

  if (rgman->downloadFinished()) {
    if (asyncTask_.isRunning() && !asyncTask_.checkFinished()) {
      e_->addRoutineCommand(std::unique_ptr<Command>(this));
      return false;
    }
    handleAsyncCompletion();
    drainPendingSynchronously();
    return true;
  }

  e_->addRoutineCommand(std::unique_ptr<Command>(this));
  // ...
  return false;
}
```

工作线程 lambda（在 `trySubmitPending()` 中提交）：

```cpp
[adaptors, controlSaves, controlFiles] {
  for (auto& a : adaptors) {
    try { a->flushOSBuffers(); } catch (RecoverableException&) { log; }
    try { a->closeFile(); }     catch (RecoverableException&) { log; }
  }
  writeControlFileSnapshots(controlSaves);  // fsync 后再写 .aria2
  for (auto& path : controlFiles) {
    File(path).remove();                     // 最后删除 .aria2
  }
}
```

### 3.6 DownloadEngine::onEndOfRun 同步关闭路径

引擎退出时（`onEndOfRun`）不走异步路径，直接同步执行全部操作：

```cpp
void DownloadEngine::onEndOfRun()
{
  GroupCleanupBatch batch;
  requestGroupMan_->removeStoppedGroup(this, batch);
  for (auto& a : batch.adaptors) {
    try { a->flushOSBuffers(); } catch (RecoverableException&) { log; }
    try { a->closeFile(); }     catch (RecoverableException&) { log; }
  }
  // fsync 之后写控制文件快照
  for (auto& cf : batch.controlFilesToSave) {
    auto temp = cf.filePath + "__temp";
    { BufferedFile fp(temp.c_str(), IOFile::WRITE); if (fp) fp.write(...); }
    File(temp).renameTo(cf.filePath);
  }
  for (auto& path : batch.controlFilesToRemove) {
    File(path).remove();
  }
  for (auto& wg : batch.groupsToClearPending) {
    if (auto g = wg.lock()) g->setAsyncCleanupPending(false);
  }
  requestGroupMan_->closeFile();
  requestGroupMan_->save();
}
```

### 3.7 生命周期保证

`shared_ptr<DiskAdaptor>` 保证工作线程期间 DiskAdaptor 不被析构。即使 `releaseRuntimeResource()` 在主线程执行后 RequestGroup 释放了 `pieceStorage_`，DiskAdaptor 对象仍然存活直到工作线程完成。

### 3.8 Unpause 竞态保护

**问题**：下载暂停后，工作线程正在 fsync + close 旧的 DiskAdaptor。用户通过 RPC unpause，主线程重新激活该 group 并打开同一文件。两个线程持有同一文件的不同 fd — Windows 上触发共享违规（`ERROR_SHARING_VIOLATION`）。

**解决**：在 RequestGroup 上新增 `asyncCleanupPending_` 标志：

- `ProcessStoppedRequestGroup` 中，收集 adaptor 后设置 `group->setAsyncCleanupPending(true)`
- `fillRequestGroupFromReserver` 中，跳过 `asyncCleanupPending_` 的 group（和 `isDependencyResolved()` 类似处理）
- `FillRequestGroupCommand::handleAsyncCompletion()` 中，`asyncTask_.checkFinished()` 返回 true 后，清除所有相关 group 的标志

### 3.9 OpenedFileCounter 计数器一致性

**问题**：`ProcessStoppedRequestGroup` 通过 `remove_if` 将 group 从 `requestGroups_` 中移除，但 `numOpenFiles_` 仍计入该 group 的打开文件数。此时若 `ensureMaxOpenFileLimit` 的驱逐路径运行，它只遍历 `requestGroups_`（已不含已停止 group），找不到足够的文件可驱逐 → debug 构建中 `assert(left == 0)` 触发。

**解决**：在收集 DiskAdaptor 到 batch 之前，调用 `adaptor->detachOpenedFileCounter()`：

```cpp
// DiskAdaptor.h — 基类默认实现
virtual void detachOpenedFileCounter() { openedFileCounter_.reset(); }

// MultiDiskAdaptor.cc — 预递减后清除引用
void MultiDiskAdaptor::detachOpenedFileCounter() {
  auto& counter = getOpenedFileCounter();
  if (counter) {
    counter->reduceNumOfOpenedFile(openedDiskWriterEntries_.size());
  }
  DiskAdaptor::detachOpenedFileCounter();
}
```

这样 `numOpenFiles_` 在 group 被移除前就已递减，驱逐路径不会看到虚高的计数。工作线程后续 `closeFile()` 时 `openedFileCounter_` 已为空，不会重复递减。

### 3.10 MultiDiskAdaptor 互斥锁

**问题**：`AutoSaveCommand` 的工作线程通过 `ControlFileSaveItem::adaptor` 持有活跃下载的 `shared_ptr<DiskAdaptor>` 并调用 `flushOSBuffers()`。如果该下载在 AutoSave worker 执行期间停止，`FillRequestGroupCommand` 的 worker 也持有同一 `DiskAdaptor` 并调用 `flushOSBuffers()` + `closeFile()`。两个 worker 并发访问 `openedDiskWriterEntries_`（vector 遍历 vs clear）——未定义行为。

**解决**：在 `MultiDiskAdaptor` 和 `AbstractSingleDiskAdaptor` 中各新增 `std::mutex fileIoMutex_`，在 `flushOSBuffers()` 和 `closeFile()` 中加锁。`MultiDiskAdaptor::tryCloseFile()` 也需要加锁（驱逐路径会修改 `openedDiskWriterEntries_`）：

```cpp
// MultiDiskAdaptor.h / AbstractSingleDiskAdaptor.h
std::mutex fileIoMutex_;

// MultiDiskAdaptor.cc
void MultiDiskAdaptor::flushOSBuffers() {
  std::lock_guard<std::mutex> lock(fileIoMutex_);
  for (auto& dwent : openedDiskWriterEntries_) { ... }
}

void MultiDiskAdaptor::closeFile() {
  std::lock_guard<std::mutex> lock(fileIoMutex_);
  for (auto& dwent : openedDiskWriterEntries_) { dw->closeFile(); }
  openedDiskWriterEntries_.clear();
}

size_t MultiDiskAdaptor::tryCloseFile(size_t numClose) {
  std::lock_guard<std::mutex> lock(fileIoMutex_);
  // ... swap-and-pop eviction ...
}

// AbstractSingleDiskAdaptor.cc
void AbstractSingleDiskAdaptor::flushOSBuffers() {
  std::lock_guard<std::mutex> lock(fileIoMutex_);
  diskWriter_->flushOSBuffers();
}

void AbstractSingleDiskAdaptor::closeFile() {
  std::lock_guard<std::mutex> lock(fileIoMutex_);
  diskWriter_->closeFile();
}
```

**竞争场景分析**：
- AutoSave worker 持锁 fsync → FillRequestGroupCommand worker 等锁 → AutoSave 完成 → FillRequestGroupCommand 获锁做 close → 正确序列化
- FillRequestGroupCommand worker 先获锁 close（清空 vector）→ AutoSave worker 获锁时遍历空 vector → no-op → 安全
- 主线程 `tryCloseFile()`（驱逐路径）持锁修改 vector → AutoSave worker 等锁 → 驱逐完成后 worker 遍历更新后的 vector → 安全
- 主线程调用（`onEndOfRun` / `drainPendingSynchronously`）→ 此时无 worker 在跑（已等待完成），无竞争

**为何 `tryCloseFile` 也需要加锁**：`ensureMaxOpenFileLimit` 的驱逐路径在主线程通过 `tryCloseFile()` 修改 `openedDiskWriterEntries_`（swap-and-pop）。AutoSave worker 可能同时持有同一 `MultiDiskAdaptor` 的 `shared_ptr` 并在 `flushOSBuffers()` 中遍历该 vector。无锁时为并发读写 `std::vector` 的未定义行为。

**为何 `AbstractSingleDiskAdaptor` 也需要互斥锁**：单文件下载（HTTP/FTP）使用 `AbstractSingleDiskAdaptor`。AutoSave worker 和 FillRequestGroupCommand worker 可能并发调用同一 adaptor 的 `flushOSBuffers()`（fsync）和 `closeFile()`（close fd），导致 use-after-close。

**为何不移除 AutoSave 的 fsync**：AutoSave 的 `flushOSBuffers()` 保证 `.aria2` bitfield 不超前于磁盘数据。移除会导致掉电时 `.aria2` 声称已完成的 piece 实际未持久化，对无 piece hash 的 HTTP 下载造成静默数据损坏。

### 3.11 collectSaveSnapshot 过滤已停止 group

**问题**：`AutoSaveCommand` 和 `FillRequestGroupCommand` 各有独立的 `AsyncTask`，两个 worker 可在线程池中并发执行。若 `collectSaveSnapshot()` 捕获了 `numCommand==0` 的 group（已停止但尚未被 `removeStoppedGroup` 移除），而同一迭代中 `ProcessStoppedRequestGroup` 也通过 `snapshotControlFile()` 捕获同一 group，两个 worker 会并发写入同一 `filePath + "__temp"` 文件。

虽然在同一迭代内 `FillRequestGroupCommand` 始终先于 `AutoSaveCommand` 执行（`routineCommands_` 添加顺序保证），使得同一迭代内不会冲突，但跨迭代场景仍有风险：AutoSave 在迭代 N 捕获活跃 group X 的快照并提交 worker，group X 在迭代 N+M 停止，FillRequestGroupCommand 提交新 worker——若 AutoSave worker 仍在运行（慢 fsync），两个 worker 并发写同一 `__temp` 文件。

**解决**：在 `collectSaveSnapshot()` 入口跳过已停止的 group：

```cpp
for (auto& rg : requestGroups_) {
  if (rg->getNumCommand() == 0) {
    continue;  // ProcessStoppedRequestGroup 负责处理
  }
  // ...
}
```

这样 AutoSave 不会捕获即将被 `ProcessStoppedRequestGroup` 处理的 group，也不会捕获已停止但因异步任务繁忙尚未提交的 group。FillRequestGroupCommand 的 worker 独占这些 group 的控制文件写入。

## 4. 线程安全分析

| 操作 | 线程安全性 | 说明 |
|------|-----------|------|
| `flushWrDiskCacheEntry(true)` | 主线程 | 访问共享 WrDiskCache，不可卸载 |
| `flushOSBuffers()` (fsync) | 工作线程安全 | `fileIoMutex_` 序列化 AutoSave / FillRequestGroupCommand 的并发访问（MultiDiskAdaptor + AbstractSingleDiskAdaptor） |
| `closeFile()` | 工作线程安全 | `fileIoMutex_` 保护；unpause 竞态由 `asyncCleanupPending_` 保护 |
| `tryCloseFile()` | 主线程 | `fileIoMutex_` 保护，防止与 AutoSave worker 的 `flushOSBuffers()` 并发访问 `openedDiskWriterEntries_` |
| `writeControlFileSnapshots()` | 工作线程安全 | 数据已快照到独立 buffer，不访问 group 状态 |
| `File(path).remove()` | 工作线程安全 | 路径为 string 拷贝，不与主线程路径竞态 |
| `applyLastModifiedTime` | 主线程 | 保留同步，避免与新任务组的路径竞态 |
| `saveSignature` | 主线程 | 保留同步（<5ms） |
| `detachOpenedFileCounter()` | 主线程 | 预递减 atomic 计数器，清除 shared_ptr 引用 |
| `collectSaveSnapshot()` | 主线程 | 跳过 `numCommand==0` 的 group，避免与 FillRequestGroupCommand worker 写同一 `__temp` 文件 |

### 4.1 为何不卸载 applyLastModifiedTime / saveSignature

1. **耗时极低**：合计 <10ms，卸载带来的复杂度不值得
2. **路径竞态风险**：工作线程按路径操作文件时，主线程可能已为新任务组打开同一路径
3. **错误处理简单**：保留在主线程，错误可直接 catch 并记录

### 4.2 关键设计决策：禁止 waitForCompletion

所有等待路径（halt / downloadFinished / 上一轮未完成）均使用非阻塞的 `checkFinished()` + re-queue 模式。**禁止在 execute() 中调用 `waitForCompletion()`**，否则主线程阻塞，违背卸载目的。

### 4.3 pendingBatch_ 累积机制

当 `asyncTask_` 正忙时，新一轮 `removeStoppedGroup` 产生的 batch 通过 `mergeFrom()` 累积到 `pendingBatch_`。下一轮 `execute()` 中 `trySubmitPending()` 检查 `asyncTask_` 是否已完成，完成后提交累积的 batch。`pendingBatch_` 中的 items 和正在运行的 async task 中的 items 来自不同轮次的不同 group，不会重叠。

## 5. 改动文件清单

| 文件 | 改动 | 行数估算 |
|------|------|---------|
| `src/storage/DiskAdaptor.h` | 新增 `detachOpenedFileCounter()` 虚方法 | +2 |
| `src/storage/MultiDiskAdaptor.h` | 声明 `detachOpenedFileCounter()` override + `std::mutex fileIoMutex_` | +4 |
| `src/storage/MultiDiskAdaptor.cc` | 实现预递减 + 清除引用 + `flushOSBuffers`/`closeFile`/`tryCloseFile` 加锁 | +14 |
| `src/storage/AbstractSingleDiskAdaptor.h` | `std::mutex fileIoMutex_` | +3 |
| `src/storage/AbstractSingleDiskAdaptor.cc` | `flushOSBuffers`/`closeFile` 加锁 | +4 |
| `src/storage/OpenedFileCounter.h` | `numOpenFiles_` 改为 `std::atomic<size_t>` | +1, -1 |
| `src/storage/OpenedFileCounter.cc` | CAS 快速路径 + atomic fetch_add/fetch_sub | +25, -5 |
| `src/core/RequestGroup.h` | 新增 `flushCacheOnly()` + `asyncCleanupPending_` | +8 |
| `src/core/RequestGroup.cc` | 实现 `flushCacheOnly()` + `saveControlFileNoSync()` | +10 |
| `src/core/RequestGroupMan.h` | `GroupCleanupBatch` + `ControlFileSaveEntry` + 签名变更 | +45 |
| `src/core/RequestGroupMan.cc` | ProcessStoppedRequestGroup: detach + snapshot + batch; collectSaveSnapshot 过滤 numCommand==0 | +28, -5 |
| `src/core/FillRequestGroupCommand.h` | `AsyncTask` + `activeGroupClears_` + `pendingBatch_` | +10 |
| `src/core/FillRequestGroupCommand.cc` | 异步提交、非阻塞等待、快照写入、异常日志 | +80, -10 |
| `src/core/DownloadEngine.cc` | `onEndOfRun` 同步路径适配 | +20, -1 |
| **合计** | | **~200 行净改动** |

## 6. 与已有改造的对比

| 改造目标 | 模式 | 净改动 | 难度 |
|---------|------|--------|------|
| FileAllocationCommand | 拆分虚方法 (flushIOAfterAllocation) | ~50 行 | 低 |
| CheckIntegrityCommand | 直接卸载 (validateChunk → ThreadPool) | ~20 行 | 低 |
| AutoSaveCommand | 快照序列化 (collectSaveSnapshot) | ~150 行 | 中 |
| **FillRequestGroupCommand** | **fsync + close + 控制文件快照** | **~200 行** | **中** |

## 7. 注意事项

### 7.1 downloadFinished 检查必须在 queueCheckRequested 块外面

原代码中 `downloadFinished()` 在 `if (queueCheckRequested())` 块内部。异步化后，当工作线程 fsync 尚未完成时 `execute()` 通过 re-queue 返回。下一轮调用时 `queueCheckRequested()` 已为 false，整个 if 块被跳过，`downloadFinished()` 不会再被检查 → 进程永远无法退出。

**修复**：将 `downloadFinished()` 检查移到 `queueCheckRequested()` 块外面，确保每轮 `execute()` 都能检测到下载完成。

### 7.2 控制文件快照保证崩溃一致性

原 `saveControlFile()` 先 fsync 数据文件再写 .aria2，保证 .aria2 的 bitfield 不会超前于磁盘上的数据。新方案通过快照实现同等保证：

1. 主线程：`serializeToBuffer()` 将 .aria2 数据序列化到内存 buffer
2. 工作线程：先 `flushOSBuffers()` fsync 数据文件，再 `writeControlFileSnapshots()` 将 buffer 写入 .aria2

这样 .aria2 写入始终发生在数据 fsync 之后。即使进程在 fsync 后、写 .aria2 前崩溃，也只是丢失最后一次 .aria2 更新（数据文件已安全），恢复时回退到上一次 AutoSave 的 .aria2 状态。

### 7.3 detachOpenedFileCounter 为何必需

`OpenedFileCounter::ensureMaxOpenFileLimit` 的驱逐路径遍历 `requestGroups_` 关闭文件以腾出配额。如果 group 已从 `requestGroups_` 移除但 `numOpenFiles_` 仍计入其打开文件数，驱逐路径会发现找不到足够文件可关闭，导致 `assert(left == 0)` 失败。

`detachOpenedFileCounter()` 在 group 被 `remove_if` 移除前执行：
- `MultiDiskAdaptor::detachOpenedFileCounter()` 将 `openedDiskWriterEntries_.size()` 从 `numOpenFiles_` 中减去
- 然后清除 `openedFileCounter_` 引用，使工作线程后续的 `closeFile()` 不会重复递减

### 7.4 ForceHalt 处理

普通 halt（`isHaltRequested()`）：re-queue 等待异步 I/O 完成后，`handleAsyncCompletion()` + `drainPendingSynchronously()` 清理剩余项，然后 `return true`。

强制 halt（`isForceHaltRequested()`）：当前实现与普通 halt 相同，等待异步 I/O 完成。`AsyncTask` 析构函数中的 `waitForCompletion()` 会在引擎最终析构时执行（主循环已退出）。由于 .aria2 使用 write-rename 原子写入，强制退出不会损坏已有的控制文件。

### 7.5 工作线程错误处理

工作线程中的 fsync/close/write 错误用 try/catch 捕获并通过 `A2_LOG_ERROR` 记录（已线程安全）。`handleAsyncCompletion()` 检查 `asyncTask_.hasException()` 并记录逃逸异常。原因：

- group 已从活跃列表移除，无法重试
- fsync 失败意味着数据可能未完全落盘，但 OS 通常会在后续自动重试
- closeFile 失败意味着 fd 泄漏，但进程即将释放这些资源（group 已停止）

### 7.6 writeControlFileSnapshots 错误处理

`writeControlFileSnapshots()` 使用 write-rename 模式写入 `.aria2` 快照。错误处理与 `writeSaveSnapshot()`（AutoSave 路径）和 `onEndOfRun()` 保持一致：

1. `BufferedFile` 打开失败 → 记录错误，`continue` 跳过（保留原 `.aria2` 不动）
2. `fp.write()` 短写（磁盘满/I/O 错误）→ 记录错误，删除 temp 文件，`continue`
3. `fp.close()` 失败（fflush/fsync/fclose 错误）→ 记录错误，删除 temp 文件，`continue`（防止未持久化的 temp 文件覆盖正常 `.aria2`）
4. `renameTo` 失败 → 记录错误（temp 文件留在磁盘，下次写入时覆盖，无害）

**为何必须显式调用 `fp.close()` 并检查返回值**：`BufferedFile::onClose()` 执行 `fflush → fsync → fclose`。如果仅依赖析构器隐式调用 `close()`，析构器会丢弃返回值。当 fsync 失败时（数据未持久化到磁盘），代码仍会执行 `renameTo`，将可能损坏的 temp 文件覆盖正常 `.aria2`。显式调用 `fp.close()` 可以捕获此错误，在 rename 前中止并清理 temp 文件。此模式在三处保持一致：`writeControlFileSnapshots`、`onEndOfRun`、`writeSaveSnapshot`。

### 7.7 AsyncTask::submit 中 running_ 的赋值位置

`running_` 是非原子 `bool`，仅在主线程读写（worker 线程不访问）。当前放在 `pool.enqueueDetached()` 之后。这是安全的：

- `submit()` 在 `execute()` 内调用，`execute()` 返回前 `running_ = true` 必然执行
- 单线程事件循环保证 `isRunning()` 的下一次检查发生在下一轮 `execute()`，此时 `running_` 已为 `true`
- 即使 worker 线程在 `running_ = true` 之前完成（设置 `finished_ = true`），也不影响正确性——`isRunning()` 和 `checkFinished()` 总是在主线程的后续迭代中配合检查
