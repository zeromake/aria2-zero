# AutoSaveCommand ThreadPool 卸载分析

## 1. 现状

`AutoSaveCommand` 继承 `TimeBasedCommand`（同步），定时调用 `RequestGroupMan::save()`。

```
AutoSaveCommand::process()                    [主线程，阻塞事件循环]
  └─ RequestGroupMan::save()
       └─ 遍历 requestGroups_，对每个 RequestGroup:
            ├─ 已完成 → removeControlFile()        [删除 .aria2 文件]
            └─ 未完成 → saveControlFile()
                 ├─ flushWrDiskCacheEntry(false)    [内存缓存 → 文件缓冲区]
                 ├─ flushOSBuffers()                [fsync / FlushFileBuffers]
                 └─ progressInfoFile_->save()
                      ├─ 序列化到 SHA1IOFile (内存)  [计算 SHA1 判断是否变化]
                      ├─ 内容未变 → 跳过写盘
                      └─ 内容有变:
                           ├─ 写临时文件 __temp      [磁盘 I/O]
                           └─ rename → .aria2       [磁盘 I/O]
```

### 1.1 耗时分析

| 操作 | 典型耗时 | 说明 |
|------|---------|------|
| `flushWrDiskCacheEntry(false)` | 0.1-5ms | 将 WrDiskCache 中的脏页写入文件缓冲区（用户态 → 内核态） |
| **`flushOSBuffers()` (fsync)** | **1-500ms** | **等待内核将所有脏页刷到物理磁盘，HDD 上尤其慢** |
| SHA1 序列化 | 0.01-1ms | 内存计算，取决于 bitfield 大小 |
| 写临时文件 + rename | 0.1-10ms | 小文件写入 |
| `removeControlFile()` | 0.1-1ms | 删除文件 |

**N 个活跃下载 = N 次 fsync**。10 个下载在 HDD 上可阻塞 1-5 秒。

### 1.2 竞态分析

`save()` 访问的共享状态全部被主线程并发修改：

| 共享数据 | save() 中的访问 | 主线程并发修改者 |
|---------|---------------|----------------|
| `requestGroups_` 容器 | 遍历 | `FillRequestGroupCommand`（添加/删除） |
| `PieceStorage::getBitfield()` | 读取 bitfield | `DownloadCommand`（标记完成分片） |
| `PieceStorage::getInFlightPieces()` | 读取正在下载的分片 | `DownloadCommand`（获取/释放分片） |
| `WrDiskCache` | `flushWrDiskCacheEntry` 遍历并写入 | `DownloadCommand`（添加新缓存条目） |
| `DiskAdaptor` (fd) | `flushOSBuffers` 调用 fsync | `DownloadCommand`（写入数据） |
| `btRuntime_->uploadLength` | 读取上传量 | BT peer 命令（更新统计） |

**直接将整个 `save()` 卸载到工作线程会导致严重数据竞争。**

---

## 2. 方案：主线程快照 + 工作线程写盘

### 2.1 核心思路

将 `saveControlFile()` 拆分为两个阶段：

```
阶段 1（主线程，快速）:
  ├─ flushWrDiskCacheEntry(false)     [内存 → 文件缓冲区，无竞态]
  ├─ 序列化 → SHA1IOFile              [读取 PieceStorage 状态，内存计算]
  ├─ SHA1 比对 → 内容未变则跳过
  └─ 内容有变 → 保存序列化数据到 std::string

阶段 2（工作线程，慢）:
  ├─ flushOSBuffers()                 [fsync，最慢的操作]
  ├─ 写临时文件 __temp                 [磁盘 I/O]
  └─ rename → .aria2                  [磁盘 I/O]
```

阶段 1 在主线程完成，所有共享状态的读取在单线程内安全进行。耗时约 0.1-5ms（远小于 fsync 的 1-500ms）。

阶段 2 仅操作独立的文件（`.aria2` 控制文件和下载数据文件的 fd），不读取任何共享数据结构。

### 2.2 数据流

```
主线程 (prepareProcess)                   工作线程 (process)
──────────────────────                    ────────────────────
collectSaveSnapshot():
  rg1: flushCache + serializeToBuffer → buf1
  rg2: 已完成 → 记录删除路径
  rg3: flushCache + serializeToBuffer → buf3
  ...
pendingSnapshot_ = unique_ptr<SaveSnapshot>:
  saveItems = [{shared_ptr<adaptor>, buf1, path1}, ...]
  removeItems = [path2, ...]
                    │
                    ▼ submit(AsyncTask)
                                          writeSaveSnapshot(pendingSnapshot_):
                                            for item in saveItems:
                                              adaptor->flushOSBuffers()
                                              write(path + "__temp", data)
                                              rename(path + "__temp", path)
                                            for path in removeItems:
                                              remove(path)
                    ◄── wakeup ───────────
```

### 2.3 需要修改的类

#### 2.3.1 RequestGroup 新增快照方法

```cpp
// RequestGroupMan.h
struct ControlFileSaveItem {
  std::string data;
  std::string filePath;
  // 持有 shared_ptr 保证工作线程 fsync 期间 DiskAdaptor 不被析构
  std::shared_ptr<DiskAdaptor> adaptor;
};

struct SaveSnapshot {
  std::vector<ControlFileSaveItem> saveItems;
  std::vector<std::string> removeItems;
};
```

#### 2.3.2 DefaultBtProgressInfoFile 新增序列化到 buffer 的方法

当前 `save()` 直接写文件。新增 `serializeToBuffer()` 将序列化结果输出到 `std::string`：

```cpp
// 序列化一次到 StringIOFile，再对 buffer 计算 SHA1 检测变化（避免双重序列化）
bool DefaultBtProgressInfoFile::serializeToBuffer(std::string& out);
```

#### 2.3.3 AutoSaveCommand 改继承 TimeBasedAsyncCommand

```cpp
class AutoSaveCommand : public TimeBasedAsyncCommand {
  std::unique_ptr<SaveSnapshot> pendingSnapshot_;

  void preProcess() override;       // 退出检查
  void prepareProcess() override;   // 主线程采集快照
  void process() override;          // 工作线程：fsync + 写盘
};

// RequestGroupMan
std::unique_ptr<SaveSnapshot> collectSaveSnapshot();
static void writeSaveSnapshot(const std::unique_ptr<SaveSnapshot>& snapshot);
```

### 2.4 fsync 安全性

**关键问题**：工作线程调用 `fsync(fd)` 时，主线程的 `DownloadCommand` 可能正在对同一个 fd 调用 `write()`。这是否安全？

**回答：安全。** POSIX 和 Windows 保证：
- `fsync()` 是线程安全的系统调用，可以并发于 `write()`
- `fsync()` 确保调用时刻之前的所有 `write()` 数据刷到磁盘
- `FlushFileBuffers()` 同理

`fsync` 和 `write` 对同一 fd 并发是操作系统级别支持的，不需要应用层加锁。

### 2.5 DiskAdaptor 生命周期

`ControlFileSaveItem::adaptor` 使用 `shared_ptr<DiskAdaptor>` 持有引用，保证工作线程 fsync 期间对象不被析构。

如果快照采集后 `RequestGroup` 完成下载并被 `removeStoppedGroup` 移除（`closeFile()` 关闭 fd），`shared_ptr` 保证 `DiskAdaptor` 对象存活，`flushOSBuffers()` 内部检查 `fd_ == A2_BAD_FD` 跳过（已有此检查）。fsync 被跳过是可接受的——文件已关闭意味着数据已写完。

---

## 3. 复杂度评估

| 改动 | 说明 | 行数 |
|------|------|------|
| `DefaultBtProgressInfoFile` 新增 `serializeToBuffer()` | 复用已有 `save(IOFile&)`，单次序列化 + SHA1 校验 | ~20 |
| `RequestGroup` 新增访问器 | `isSaveControlFileEnabled()` + `getProgressInfoFile()` | ~5 |
| `RequestGroupMan` 新增 `collectSaveSnapshot()` | 遍历收集快照 | ~25 |
| `AutoSaveCommand` 改继承 + 实现 | 从 `TimeBasedCommand` → `TimeBasedAsyncCommand` | ~40 |
| `StringIOFile` 或类似内存 IOFile | 如果没有现成的则需要新增 | ~40 |
| **总计** | | **~165** |

### 对比 FileAllocationCommand 的改造

| 维度 | FileAllocationCommand | AutoSaveCommand |
|------|----------------------|-----------------|
| 竞态复杂度 | 低（分配期间无并发访问） | 高（遍历活跃下载的共享状态） |
| 方案模式 | 直接卸载 | 快照 + 卸载 |
| 需要新增类 | 无 | StringIOFile / 快照结构体 |
| 改动范围 | 2 个文件 | 5+ 个文件 |

---

## 4. 替代方案（简化版）

如果上述方案改动量过大，还有一个更简单的折中：

**只卸载 fsync**：`saveControlFile()` 整体仍在主线程执行（序列化 + 写文件），但将所有 `flushOSBuffers()` 调用收集起来，批量提交到工作线程。

```
主线程:
  for rg in requestGroups_:
    flushWrDiskCacheEntry(false)    [快速]
    progressInfoFile_->save()       [序列化+写文件，几ms]
    fdsToSync.push_back(fd)         [收集 fd]

工作线程:
  for fd in fdsToSync:
    fsync(fd)                       [慢操作，卸载到此处]
```

优点：改动极小（~50 行），收益大（fsync 占总耗时 90%+）。
缺点：序列化和写临时文件仍在主线程（但通常 < 5ms，可接受）。

---

## 5. 建议

推荐**简化版（只卸载 fsync）**作为第一步：

1. 改动量小，风险低
2. fsync 占阻塞耗时 90%+，收益最大化
3. 后续如果需要进一步优化，再做完整快照方案

完整方案已实现，新增 `StringIOFile`（`src/storage/StringIOFile.h`）将 IOFile 接口适配到 `std::string`，支持 `write` 和 `vprintf`。
