# FillRequestGroupCommand 线程卸载方案

## 1. 现状

`FillRequestGroupCommand` 是 routineCommand，每轮事件循环执行。当有下载完成/失败/暂停时，通过 `removeStoppedGroup()` → `ProcessStoppedRequestGroup` 清理已停止的任务组，执行大量阻塞文件 I/O。

```
FillRequestGroupCommand::execute()
  └─ rgman->fillRequestGroupFromReserver(e)
       └─ removeStoppedGroup(e)                        ← 阻塞点
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

// RequestGroup::saveControlFile() — src/core/RequestGroup.cc:1292
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

**结论：只卸载 fsync + close 即可获得 >90% 收益，其余 I/O 合计 <15ms，保留在主线程可接受。**

### 2.3 ProcessStoppedRequestGroup 操作分类

```
ProcessStoppedRequestGroup::operator()(group) {
  // ═══ 内存操作 ═══
  collectStat(group);                           // 读 PeerStats，更新 ServerStat
  decreaseNumActive();                          // 计数器
  resetDownloadStopTime();                      // 时间戳

  // ═══ 阻塞 I/O ═══
  group->closeFile();                           // flushCache + fsync + close ← 唯一需要卸载的
  //   在 pause/finished/incomplete 分支中:
  group->saveControlFile();                     // 写 .aria2 (fsync 已 no-op，<10ms)
  group->applyLastModifiedTimeToLocalFiles();   // utime (<1ms)
  group->removeControlFile();                   // 删除 .aria2 (<1ms)
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

## 3. 方案设计：只卸载 fsync + close

### 3.1 核心思路

将 `closeFile()` 拆分为两步：

```
主线程 (快, <10ms):
  ├─ flushCacheOnly()                 ← 刷写 WrDiskCache（共享资源）
  ├─ saveControlFileNoSync()          ← 写 .aria2（跳过 fsync）
  ├─ applyLastModifiedTime / remove / sig  ← 保留同步（各 <5ms）
  ├─ 收集 shared_ptr<DiskAdaptor>
  └─ 所有引擎状态操作

工作线程 (慢, 异步):
  ├─ adaptor->flushOSBuffers()        ← fsync (1-500ms)
  └─ adaptor->closeFile()             ← 关闭 fd
```

不引入复杂的 `StoppedGroupIOItem` 结构体，不做快照序列化。只收集 `shared_ptr<DiskAdaptor>`。

### 3.2 RequestGroup 新增方法

```cpp
// src/core/RequestGroup.h

// 主线程调用：刷写 WrDiskCache（共享资源），不 fsync、不关闭文件
void flushCacheOnly();

// 主线程调用：写 .aria2 控制文件，跳过 flushOSBuffers
// 用于 closeFile 被异步化的场景（fsync 由工作线程负责）
void saveControlFileNoSync() const;
```

```cpp
// src/core/RequestGroup.cc

void RequestGroup::flushCacheOnly()
{
  if (pieceStorage_) {
    pieceStorage_->flushWrDiskCacheEntry(true);
  }
}

void RequestGroup::saveControlFileNoSync() const
{
  if (saveControlFile_) {
    progressInfoFile_->save();
  }
}
```

### 3.3 ProcessStoppedRequestGroup 变更

改动最小化：只替换 `group->closeFile()` 和 `group->saveControlFile()`。

```cpp
class ProcessStoppedRequestGroup {
  DownloadEngine* e_;
  RequestGroupList& reservedGroups_;
  // 新增：收集需要异步 fsync + close 的 DiskAdaptor
  std::vector<std::shared_ptr<DiskAdaptor>>& adaptorsToClose_;

  bool operator()(const RequestGroupList::value_type& group) {
    if (group->getNumCommand() == 0) {
      collectStat(group);
      // ...

      try {
        // ── 替换 group->closeFile() ──
        // 主线程：刷写 WrDiskCache（共享资源）
        group->flushCacheOnly();
        // 收集 DiskAdaptor，工作线程负责 fsync + close
        if (group->getPieceStorage()) {
          adaptorsToClose_.push_back(
              group->getPieceStorage()->getDiskAdaptor());
        }

        if (group->isPauseRequested()) {
          // ── 替换 group->saveControlFile() ──
          group->saveControlFileNoSync();
        }
        else if (group->downloadFinished() && ...) {
          group->applyLastModifiedTimeToLocalFiles();  // 保留同步
          group->reportDownloadFinished();
          if (group->allDownloadFinished() && ...) {
            group->removeControlFile();                // 保留同步
            saveSignature(group);                      // 保留同步
          }
          else {
            group->saveControlFileNoSync();            // 替换
          }
          // postDownloadProcessing / BT file removal 保持不变 ...
        }
        else {
          group->saveControlFileNoSync();              // 替换
        }
      }
      catch (RecoverableException& ex) { ... }

      // 引擎状态操作完全不变
      // ...
      return true;
    }
    return false;
  }
};
```

### 3.4 FillRequestGroupCommand 变更

```cpp
// src/core/FillRequestGroupCommand.h
class FillRequestGroupCommand : public Command {
  COMMAND_CLASSNAME(FillRequestGroupCommand)
private:
  DownloadEngine* e_;
  Timer lastExecTime;
  AsyncTask asyncTask_;
  // ...
};
```

```cpp
// src/core/FillRequestGroupCommand.cc
bool FillRequestGroupCommand::execute()
{
  if (e_->isHaltRequested()) {
    // 异步任务仍在运行时不直接返回，避免析构自旋
    if (asyncTask_.isRunning() && !asyncTask_.checkFinished()) {
      e_->addRoutineCommand(std::unique_ptr<Command>(this));
      return false;
    }
    return true;
  }

  // 清理上一轮异步 I/O
  if (asyncTask_.isRunning()) {
    asyncTask_.checkFinished();
  }

  auto& rgman = e_->getRequestGroupMan();
  if (rgman->queueCheckRequested()) {
    // 收集所有轮次的 DiskAdaptor
    std::vector<std::shared_ptr<DiskAdaptor>> adaptorsToClose;

    while (rgman->queueCheckRequested()) {
      try {
        rgman->clearQueueCheck();
        rgman->fillRequestGroupFromReserver(e_, adaptorsToClose);
      }
      catch (RecoverableException& ex) {
        A2_LOG_ERROR_EX(EX_EXCEPTION_CAUGHT, ex);
        rgman->requestQueueCheck();
      }
    }

    // 提交异步 fsync + close
    if (!adaptorsToClose.empty()) {
      // 上一轮还在跑则跳过（不阻塞），下次 execute 时再提交
      if (!asyncTask_.isRunning()) {
        asyncTask_.submit(*e_->getThreadPool(), e_,
          [adaptors = std::move(adaptorsToClose)] {
            for (auto& a : adaptors) {
              try {
                a->flushOSBuffers();
                a->closeFile();
              }
              catch (...) {
                // 最佳努力，错误已无法恢复
              }
            }
          });
      }
    }

    if (rgman->downloadFinished()) {
      // 非阻塞等待：不调用 waitForCompletion
      if (asyncTask_.isRunning() && !asyncTask_.checkFinished()) {
        e_->addRoutineCommand(std::unique_ptr<Command>(this));
        return false;
      }
      return true;
    }
  }

  e_->addRoutineCommand(std::unique_ptr<Command>(this));

  if (rgman->getOptimizeConcurrentDownloads()) {
    const auto& now = global::wallclock();
    if (std::chrono::duration_cast<std::chrono::seconds>(
            lastExecTime.difference(now)) >= 1_s) {
      lastExecTime = now;
      rgman->requestQueueCheck();
    }
  }

  return false;
}
```

### 3.5 生命周期保证

`shared_ptr<DiskAdaptor>` 保证工作线程期间 DiskAdaptor 不被析构。即使 `releaseRuntimeResource()` 在主线程执行后 RequestGroup 释放了 `pieceStorage_`，DiskAdaptor 对象仍然存活直到工作线程完成。

### 3.6 Unpause 竞态保护

**问题**：下载暂停后，工作线程正在 fsync + close 旧的 DiskAdaptor。用户通过 RPC unpause，主线程重新激活该 group 并打开同一文件。两个线程持有同一文件的不同 fd — Windows 上触发共享违规（`ERROR_SHARING_VIOLATION`）。

**解决**：在 RequestGroup 上新增 `asyncCleanupPending_` 标志：

```cpp
// RequestGroup.h
bool asyncCleanupPending_ = false;
void setAsyncCleanupPending(bool v) { asyncCleanupPending_ = v; }
bool isAsyncCleanupPending() const { return asyncCleanupPending_; }
```

- ProcessStoppedRequestGroup 中，收集 adaptor 后设置 `group->setAsyncCleanupPending(true)`
- `fillRequestGroupFromReserver` 中，跳过 `asyncCleanupPending_` 的 group（和 `isDependencyResolved()` 类似处理）
- FillRequestGroupCommand 中，`asyncTask_.checkFinished()` 返回 true 后，清除所有相关 group 的标志

## 4. 线程安全分析

| 操作 | 线程安全性 | 说明 |
|------|-----------|------|
| `flushWrDiskCacheEntry(true)` | 主线程 | 访问共享 WrDiskCache，不可卸载 |
| `flushOSBuffers()` (fsync) | 工作线程安全 | group 已停止，无并发写入同一 fd |
| `closeFile()` | 工作线程安全 | group 已停止，无并发访问 fd；unpause 竞态由 `asyncCleanupPending_` 保护 |
| `saveControlFileNoSync()` | 主线程 | 只写 .aria2，不访问 DiskAdaptor fd |
| `applyLastModifiedTime` | 主线程 | 保留同步，避免与新任务组的路径竞态 |
| `File::remove()` | 主线程 | 保留同步（各 <1ms） |

### 4.1 为何不卸载 applyLastModifiedTime / removeControlFile / saveSignature / File::remove

1. **耗时极低**：合计 <15ms，卸载带来的复杂度不值得
2. **路径竞态风险**：工作线程按路径操作文件时，主线程可能已为新任务组打开同一路径
3. **错误处理简单**：保留在主线程，错误可直接 catch 并记录

### 4.2 关键设计决策：禁止 waitForCompletion

所有等待路径（halt / downloadFinished / 上一轮未完成）均使用非阻塞的 `checkFinished()` + re-queue 模式。**禁止在 execute() 中调用 `waitForCompletion()`**，否则主线程阻塞，违背卸载目的。

## 5. 改动文件清单

| 文件 | 改动 | 行数估算 |
|------|------|---------|
| `src/core/RequestGroup.h` | 新增 `flushCacheOnly()` + `saveControlFileNoSync()` + `asyncCleanupPending_` | +8 |
| `src/core/RequestGroup.cc` | 实现 `flushCacheOnly()` + `saveControlFileNoSync()` | +10 |
| `src/core/RequestGroupMan.h` | 修改 `fillRequestGroupFromReserver` 签名 | +2 |
| `src/core/RequestGroupMan.cc` | ProcessStoppedRequestGroup 替换 closeFile/saveControlFile，fillRequestGroupFromReserver 跳过 asyncCleanupPending | +15, -5 |
| `src/core/FillRequestGroupCommand.h` | 新增 `AsyncTask asyncTask_` | +3 |
| `src/core/FillRequestGroupCommand.cc` | 异步提交 adaptors，非阻塞等待 | +25, -5 |
| **合计** | | **~50 行净改动** |

## 6. 与已有改造的对比

| 改造目标 | 模式 | 净改动 | 难度 |
|---------|------|--------|------|
| FileAllocationCommand | 拆分虚方法 (flushIOAfterAllocation) | ~50 行 | 低 |
| CheckIntegrityCommand | 直接卸载 (validateChunk → ThreadPool) | ~20 行 | 低 |
| **FillRequestGroupCommand** | **只卸载 fsync + close** | **~50 行** | **低-中** |

## 7. 注意事项

### 7.1 saveControlFileNoSync 中为何可以跳过 flushOSBuffers

原 `saveControlFile()` 中 `flushOSBuffers()` 的目的是确保数据文件的脏页落盘后再写 .aria2 控制文件，保证崩溃恢复时数据和进度一致。新方案中 `flushCacheOnly()` 已将 WrDiskCache 刷到内核缓冲区，工作线程的 fsync 将异步完成落盘。极端情况下（fsync 完成前进程崩溃），.aria2 控制文件可能比数据文件更"新"，但 aria2 恢复时会通过完整性校验（checksum/piece hash）发现不一致并重新下载受影响的 piece。这与 AutoSaveCommand 的行为一致 — AutoSave 也是先写 .aria2 再异步 fsync 数据文件。

### 7.2 ForceHalt 处理

普通 halt（`isHaltRequested()`）：re-queue 等待异步 I/O 完成。

强制 halt（`isForceHaltRequested()`）：直接 `return true`，放弃等待。AsyncTask 析构函数中的 `waitForCompletion()` 会在引擎最终析构时执行（主循环已退出）。由于 .aria2 使用 write-rename 原子写入，强制退出不会损坏已有的控制文件。

### 7.3 工作线程错误处理

工作线程中的 fsync/close 错误用 try/catch 捕获并静默忽略（已在 §3.4 代码中体现）。原因：
- group 已从活跃列表移除，无法重试
- fsync 失败意味着数据可能未完全落盘，但 OS 通常会在后续自动重试
- closeFile 失败意味着 fd 泄漏，但进程即将释放这些资源（group 已停止）
- 错误信息可通过 `A2_LOG_ERROR`（已线程安全）记录
