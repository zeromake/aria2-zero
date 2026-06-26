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
主线程                                    工作线程
──────                                    ────────
遍历 requestGroups_:
  rg1: flushCache + 序列化 → buf1
  rg2: 已完成 → 记录删除路径
  rg3: flushCache + 序列化 → buf3
  ...
收集结果:
  saveItems = [{fd, buf1, path1}, ...]   
  removeItems = [path2, ...]             
                    │
                    ▼ submit(AsyncTask)
                                          for item in saveItems:
                                            fsync(item.fd)
                                            write(item.path + "__temp", item.buf)
                                            rename(item.path + "__temp", item.path)
                                          for path in removeItems:
                                            remove(path)
                    ◄── wakeup ───────────
```

### 2.3 需要修改的类

#### 2.3.1 RequestGroup 新增快照方法

```cpp
// RequestGroup.h
struct ControlFileSaveData {
  std::string serializedData;  // 序列化后的控制文件内容
  std::string filePath;        // .aria2 文件路径
  DiskAdaptor* diskAdaptor;    // 用于 fsync 的 fd（指针在 save 期间稳定）
  bool needFsync;              // 是否需要 fsync 下载数据文件
};

// RequestGroup.cc
// 主线程调用：flush 缓存 + 序列化到内存（不写盘）
std::unique_ptr<ControlFileSaveData> RequestGroup::snapshotControlFile();

// 工作线程调用：fsync + 写盘 + rename
static void RequestGroup::writeControlFile(const ControlFileSaveData& data);
```

#### 2.3.2 DefaultBtProgressInfoFile 新增序列化到 buffer 的方法

当前 `save()` 直接写文件。需要一个新方法将序列化结果输出到 `std::string`：

```cpp
// 已有 save(IOFile& fp) 可以序列化到任意 IOFile
// SHA1IOFile 已经是内存 IOFile，可复用
// 新增：序列化到 string 并返回（含 SHA1 变化检测）
bool DefaultBtProgressInfoFile::snapshotToBuffer(std::string& out);
```

#### 2.3.3 AutoSaveCommand 改继承 TimeBasedAsyncCommand

```cpp
class AutoSaveCommand : public TimeBasedAsyncCommand {
  // 主线程快照结果，传递给工作线程
  struct SaveSnapshot {
    struct Item {
      std::string data;        // 序列化数据
      std::string filePath;    // .aria2 路径
      DiskAdaptor* adaptor;    // fsync 用
    };
    std::vector<Item> saveItems;
    std::vector<std::string> removeItems;
  };
  std::unique_ptr<SaveSnapshot> pendingSnapshot_;

  void preProcess() override;   // 退出检查 + 快照收集
  void process() override;      // 工作线程：fsync + 写盘
};
```

### 2.4 fsync 安全性

**关键问题**：工作线程调用 `fsync(fd)` 时，主线程的 `DownloadCommand` 可能正在对同一个 fd 调用 `write()`。这是否安全？

**回答：安全。** POSIX 和 Windows 保证：
- `fsync()` 是线程安全的系统调用，可以并发于 `write()`
- `fsync()` 确保调用时刻之前的所有 `write()` 数据刷到磁盘
- `FlushFileBuffers()` 同理

`fsync` 和 `write` 对同一 fd 并发是操作系统级别支持的，不需要应用层加锁。

### 2.5 DiskAdaptor 指针稳定性

`SaveSnapshot::Item::adaptor` 是裸指针，工作线程通过它调用 `flushOSBuffers()`。需要确认该指针在工作线程执行期间不会失效。

`DiskAdaptor` 的生命周期绑定到 `PieceStorage`，后者绑定到 `RequestGroup`。`RequestGroup` 在下载活跃期间不会被销毁（由 `RequestGroupMan` 持有）。`AutoSaveCommand` 只在活跃下载上调用 `saveControlFile()`，因此指针在工作线程执行期间稳定。

但存在一个边界情况：如果在快照采集后、工作线程 fsync 前，该 `RequestGroup` 完成下载并被 `FillRequestGroupCommand` 移除（`removeStoppedGroup` 会 `closeFile()`），则 fd 已关闭，fsync 会操作无效 fd。

**缓解**：`flushOSBuffers()` 内部检查 `fd_ == A2_BAD_FD` 则跳过（已有此检查）。所以即使 fd 被关闭，也不会崩溃，只是 fsync 被跳过（可接受，因为文件已关闭意味着数据已写完）。

---

## 3. 复杂度评估

| 改动 | 说明 | 行数 |
|------|------|------|
| `DefaultBtProgressInfoFile` 新增 `snapshotToBuffer()` | 复用已有 `save(IOFile&)`，用 `StringIOFile` 适配器 | ~30 |
| `RequestGroup` 新增 `snapshotControlFile()` | 拆分 `saveControlFile()` 为快照+写盘 | ~30 |
| `RequestGroupMan` 新增 `snapshotForSave()` | 遍历收集快照 | ~25 |
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

如果选择完整方案，需要先确认是否有现成的 `StringIOFile` 类（将 IOFile 接口适配到 `std::string`），否则需要新增一个。
