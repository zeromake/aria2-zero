# CheckIntegrityCommand 线程卸载方案

## 1. 现状

`CheckIntegrityCommand` 继承 `RealtimeCommand`，每次 `executeInternal()` 同步调用 `entry_->validateChunk()`，读取一个完整 piece（256KB~16MB）并计算 SHA-1/MD5 哈希。这会阻塞事件循环数十至数百毫秒。

```cpp
// 当前代码 (src/core/CheckIntegrityCommand.cc:64-98)
bool CheckIntegrityCommand::executeInternal()
{
  if (getRequestGroup()->isHaltRequested()) {
    return true;                              // ← 无异步任务保护，可能析构自旋
  }
  entry_->validateChunk();                    // ← 阻塞：读磁盘 + 哈希（最大 16MB）
  if (entry_->finished()) {
    getRequestGroup()->enableSaveControlFile();
    if (getRequestGroup()->downloadFinished()) {
      entry_->onDownloadFinished(commands, e);  // ← 引擎状态
    } else {
      entry_->onDownloadIncomplete(commands, e); // ← 引擎状态
    }
    e->addCommand(std::move(commands));
    e->setNoWait(true);
    return true;
  }
  e->addCommand(std::unique_ptr<Command>(this));
  return false;
}
```

## 2. 阻塞操作分析

### 2.1 validateChunk — 阻塞 I/O + CPU

**IteratableChunkChecksumValidator::validateChunk()**（PieceHash 校验）：

```
validateChunk()
  → calculateActualChecksum()
    → digest(offset, pieceLength)
      → 循环 readDataDropCache(4KB) + ctx_->update()  ← 磁盘读取 + 哈希
      → ctx_->digest()                                ← 最终哈希
  → bitfield_->setBit/unsetBit(currentIndex_)          ← 私有 bitfield，非共享
  → (finished 时) pieceStorage_->setBitfield(...)       ← 写入共享 PieceStorage
```

| 操作 | 典型耗时 | 说明 |
|------|---------|------|
| `readDataDropCache` × N | 1-100ms | 读一个 piece（256KB~16MB），每次 4KB |
| SHA-1/MD5 哈希 | 0.1-10ms | CPU 密集，与 piece 大小成正比 |
| `setBitfield` (finished 时) | <0.01ms | 内存拷贝，仅在最后一次调用 |

**IteratableChecksumValidator::validateChunk()**（整文件 Checksum 校验）：

```
validateChunk()
  → readDataDropCache(4KB) + ctx_->update()  ← 每次仅读 4KB
  → (finished 时) markAllPiecesDone/setBitfield
```

每次调用仅读 4KB，阻塞极短。但需要调用数千次（大文件）。

### 2.2 onDownloadFinished / onDownloadIncomplete — 引擎状态

| 子类 | onDownloadFinished | onDownloadIncomplete |
|------|-------------------|---------------------|
| StreamCheckIntegrityEntry | 空操作 | `ps->onDownloadIncomplete()` + `proceedFileAllocation()` |
| BtCheckIntegrityEntry | hook 执行 + `proceedFileAllocation()` | `closeFile/openFile` + `proceedFileAllocation()` |
| ChecksumCheckIntegrityEntry | 空操作 | `proceedFileAllocation()` 或设错误码 |

这些方法访问 `DownloadEngine`、创建命令、调用 hook，**必须在主线程执行**。

### 2.3 线程安全性

与 FileAllocationCommand 同理：完整性校验在下载开始之前执行，该 RequestGroup 此时没有其他活跃命令。

| 操作 | 线程安全性 | 原因 |
|------|-----------|------|
| `readDataDropCache()` | 安全 | 无并发写入（下载未开始） |
| `bitfield_->setBit/unsetBit()` | 安全 | 验证器私有 bitfield，非 PieceStorage 共享 |
| `pieceStorage_->setBitfield()` | 安全 | 无并发修改（下载命令尚未创建） |
| `pieceStorage_->markAllPiecesDone()` | 安全 | 同上 |

## 3. 方案设计

与 FileAllocationCommand 采用完全相同的模式（`threadpool-offload-guide.md` 模式 A）：

```
┌────────────────────────────────────────────────┐
│  CheckIntegrityCommand 状态机                    │
│                                                 │
│  Worker thread:                                 │
│    entry_->validateChunk()                      │
│    （磁盘读取 + 哈希计算，每次一个 piece）          │
│                                                 │
│  Main thread:                                   │
│    finished? → enableSaveControlFile()           │
│             → onDownloadFinished/Incomplete()    │
│             → addCommand() + setNoWait()         │
│    not finished? → 提交下一次 validateChunk       │
└────────────────────────────────────────────────┘
```

### 3.1 CheckIntegrityCommand.h 变更

```cpp
#include "RealtimeCommand.h"
#include "AsyncTask.h"

class CheckIntegrityCommand : public RealtimeCommand {
  COMMAND_CLASSNAME(CheckIntegrityCommand)

private:
  CheckIntegrityEntry* entry_;
  AsyncTask asyncTask_;
  // 工作线程写入，主线程读取
  bool validationFinished_ = false;

public:
  // ...
  virtual bool executeInternal() CXX11_OVERRIDE;
  virtual bool handleException(Exception& e) CXX11_OVERRIDE;
};
```

### 3.2 CheckIntegrityCommand.cc 变更

```cpp
bool CheckIntegrityCommand::executeInternal()
{
  if (getRequestGroup()->isHaltRequested()) {
    // 异步任务仍在执行时不直接返回，避免析构自旋阻塞主循环
    if (asyncTask_.isRunning() && !asyncTask_.checkFinished()) {
      getDownloadEngine()->addCommand(std::unique_ptr<Command>(this));
      return false;
    }
    return true;
  }

  // 提交 validateChunk 到 ThreadPool
  if (!asyncTask_.isRunning()) {
    validationFinished_ = false;
    asyncTask_.submit(*getDownloadEngine()->getThreadPool(),
      getDownloadEngine(),
      [this]{
        entry_->validateChunk();
        validationFinished_ = entry_->finished();
      });
  }

  // 主线程检查工作线程是否完成
  if (asyncTask_.checkFinished()) {
    if (asyncTask_.hasException()) {
      try { asyncTask_.rethrowIfException(); }
      catch (RecoverableException&) { throw; }
      catch (std::exception& e) { throw DL_ABORT_EX(e.what()); }
      catch (...) { throw DL_ABORT_EX("unknown error in integrity check"); }
    }

    if (getRequestGroup()->isHaltRequested()) {
      return true;
    }

    if (validationFinished_) {
      // 主线程：操作引擎状态
      getRequestGroup()->enableSaveControlFile();
      std::vector<std::unique_ptr<Command>> commands;
      if (getRequestGroup()->downloadFinished()) {
        A2_LOG_NOTICE(fmt(MSG_VERIFICATION_SUCCESSFUL, ...));
        entry_->onDownloadFinished(commands, getDownloadEngine());
      } else {
        A2_LOG_ERROR(fmt(MSG_VERIFICATION_FAILED, ...));
        entry_->onDownloadIncomplete(commands, getDownloadEngine());
      }
      getDownloadEngine()->addCommand(std::move(commands));
      getDownloadEngine()->setNoWait(true);
      return true;
    }
  }

  getDownloadEngine()->addCommand(std::unique_ptr<Command>(this));
  return false;
}
```

## 4. 与 FileAllocationCommand 的对比

| 维度 | FileAllocationCommand | CheckIntegrityCommand |
|------|----------------------|----------------------|
| 工作线程操作 | `allocateChunk()` + `flushIOAfterAllocation()` | `validateChunk()` |
| 主线程操作 | `prepareForNextAction()` | `enableSaveControlFile()` + `onDownloadFinished/Incomplete()` |
| 需要新增虚方法 | `flushIOAfterAllocation()` | 不需要 |
| 需要拆分 Entry | 是（提取 I/O） | 否（validateChunk 本身就是纯 I/O） |
| 循环调用 | `allocateChunk()` 多次 → finished | `validateChunk()` 多次 → finished |
| 额外修复 | — | halt 路径缺少异步任务保护 |

## 5. 改动文件清单

| 文件 | 改动 | 行数估算 |
|------|------|---------|
| `src/core/CheckIntegrityCommand.h` | 新增 `AsyncTask asyncTask_` + `bool validationFinished_` | +5 |
| `src/core/CheckIntegrityCommand.cc` | 异步化 `executeInternal()` | +30, -15 |
| **合计** | | **~20 行净改动** |

不需要修改任何 Entry 类。`validateChunk()` 本身就是纯磁盘 I/O + 哈希计算，天然适合工作线程。引擎状态操作全部在 `CheckIntegrityCommand::executeInternal()` 的主线程路径中完成。

## 6. 注意事项

### 6.1 halt 路径修复

原代码在 `isHaltRequested()` 时直接 `return true`，会触发析构函数中 `AsyncTask::waitForCompletion()` 自旋。需要像 FileAllocationCommand 一样先检查异步任务是否完成。

### 6.2 异常处理

`validateChunk()` 内部的 `RecoverableException` 被验证器自身 catch 并标记 piece 为无效（`unsetBit`），不会抛出。但 `readDataDropCache()` 可能抛 `DlAbortEx`（文件不存在等），需要在主线程转换异常类型。

### 6.3 IteratableChecksumValidator 的特殊性

Checksum 验证器每次只读 4KB（vs PieceHash 验证器读整个 piece），单次 `validateChunk()` 耗时极短。卸载到工作线程后单次 I/O 的收益很小，但 ThreadPool 提交/唤醒开销也很小（~微秒级），不会产生可观测的退化。同时避免了对两种验证器做区别处理的复杂性。
