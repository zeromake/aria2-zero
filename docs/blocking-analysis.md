# aria2-zero Command::execute() 阻塞分析报告

## 概述

aria2-zero 采用**单线程事件驱动架构**，所有 Command 的 `execute()` 方法在 `DownloadEngine::run()` 主循环中同步执行。任何阻塞操作都会卡住整个事件循环，影响所有下载任务的网络 I/O 响应。

本报告对所有 30 个 Command 类的 `execute()` 方法进行了系统性审查，识别出阻塞操作并按严重程度分级。

---

## 严重程度定义

| 级别 | 含义 |
|------|------|
| **CRITICAL** | 可阻塞数百毫秒至数秒，对事件循环影响严重 |
| **HIGH** | 可阻塞数十至数百毫秒，明显影响事件循环 |
| **MEDIUM** | 通常快速但在特定条件下可能阻塞 |
| **LOW** | 理论上存在阻塞可能，实际影响极小 |

---

## 一、确认存在阻塞的 Command（共 12 个）

### 1. CheckIntegrityCommand ⬤ CRITICAL

**文件**: `src/core/CheckIntegrityCommand.cc`  
**基类**: RealtimeCommand

| 阻塞操作 | 位置 | 说明 |
|----------|------|------|
| 同步磁盘读取 + 哈希计算 | `executeInternal()` → `entry_->validateChunk()` | 每次调用读取一个完整 piece（通常 256KB~16MB）并计算 SHA-1/MD5 |

**详细分析**: `validateChunk()` 调用 `IteratableChunkChecksumValidator::validateChunk()`，该方法通过 `readDataDropCache()` 以 4KB 为单位循环读取整个 piece 的数据并进行哈希计算。对于 16MB 的 piece，单次调用涉及约 4096 次磁盘读取 + 哈希运算。

**对比**: 同为 RealtimeCommand 子类的 `FileAllocationCommand` 已正确使用 `ThreadPool` + `future_` + `wait_for(100ns)` 模式，但 `CheckIntegrityCommand` 未做此迁移，是一个**遗漏**。

---

### 2. AutoSaveCommand ⬤ CRITICAL

**文件**: `src/core/AutoSaveCommand.cc`  
**基类**: TimeBasedCommand

| 阻塞操作 | 位置 | 说明 |
|----------|------|------|
| `fsync()` / `FlushFileBuffers()` | `process()` → `RequestGroupMan::save()` → `saveControlFile()` → `flushOSBuffers()` | 对**每个**活跃下载任务执行 fsync |
| 同步写入 .aria2 控制文件 | `saveControlFile()` → `progressInfoFile_->save()` | 序列化并写入进度信息 |
| 同步刷新磁盘缓存 | `saveControlFile()` → `flushWrDiskCacheEntry(false)` | 刷新写缓存到磁盘 |
| 同步删除控制文件 | `removeControlFile()` → `progressInfoFile_->removeFile()` | 已完成任务删除 .aria2 文件 |

**详细分析**: `RequestGroupMan::save()` 遍历所有 `requestGroups_`，对每个任务调用 `saveControlFile()` 或 `removeControlFile()`。`saveControlFile()` 的调用链：`flushWrDiskCacheEntry()` → `flushOSBuffers()`（即 `fsync()`）→ `progressInfoFile_->save()`。`fsync()` 是已知的高延迟操作，在机械硬盘上可达数十至数百毫秒。当有 N 个活跃下载时，此操作执行 N 次。

---

### 3. FillRequestGroupCommand ⬤ CRITICAL

**文件**: `src/core/FillRequestGroupCommand.cc`  
**基类**: Command

| 阻塞操作 | 位置 | 说明 |
|----------|------|------|
| 同步关闭文件 | `execute()` → `fillRequestGroupFromReserver()` → `removeStoppedGroup()` → `group->closeFile()` | 关闭所有已完成任务的文件句柄 |
| 同步写入控制文件 | 同上 → `group->saveControlFile()` | 含 fsync + 写 .aria2 文件 |
| 同步删除控制文件 | 同上 → `group->removeControlFile()` | 删除 .aria2 文件 |
| 同步写入签名文件 | 同上 → `sig->save()` | 保存 Metalink 签名 |
| 同步修改文件时间戳 | 同上 → `group->applyLastModifiedTimeToLocalFiles()` | utime/SetFileTime |
| 同步删除 BT 未选择文件 | 同上 → `File(file->getPath()).remove()` | 删除 BT padding/未选中文件 |

**详细分析**: 当多个下载同时完成时，`ProcessStoppedRequestGroup` 会对每个已完成任务执行一系列文件操作。这是所有 Command 中阻塞操作**种类最多**的一个，且没有异步替代路径。

---

### 4. HttpRequestCommand ⬤ HIGH (Windows SChannel)

**文件**: `src/network/HttpRequestCommand.cc`  
**基类**: AbstractCommand

| 阻塞操作 | 位置 | 说明 |
|----------|------|------|
| **SChannel TLS 握手 — SSPI 内部阻塞** | `executeInternal()` 行 129 → `tlsConnect()` → `SChannelSession::handshakeStep2()` 行 427 → `InitializeSecurityContextA()` | **Windows SSPI API 内部执行同步网络操作** |
| 同步文件 stat（条件 GET） | `executeInternal()` 行 167-169 → `File::exists()` / `File::getModifiedTime()` | stat() 系统调用 |

#### SChannel 握手阻塞详细分析

`HttpRequestCommand::executeInternal()` 在 HTTPS 连接时调用 `getSocket()->tlsConnect()`:

```
HttpRequestCommand::executeInternal()  行 129
  → SocketCore::tlsConnect()           行 910
    → SocketCore::tlsHandshake()       行 915
      → SChannelSession::tlsConnect()  行 646
        → handshakeStep2()             行 338
          → InitializeSecurityContextA()  行 427  ← 这里阻塞
```

**socket send/recv 是非阻塞的** — SChannel 实现中所有 `::send()` 和 `::recv()` 调用都正确处理了 `WSAEWOULDBLOCK`，遇到时返回 `TLS_ERR_WOULDBLOCK`，由上层设置 `wantRead_`/`wantWrite_` 后 `addCommandSelf()` 重新入队。**这部分没有问题**。

**真正的阻塞点是 `InitializeSecurityContextA()` SSPI API 本身**。此 Windows API 在处理服务端证书时，会内部发起同步网络请求，完全绕过应用层的非阻塞 socket 模型：

| SSPI 内部操作 | 触发条件 | 典型耗时 |
|--------------|---------|---------|
| **CRL 下载** | Windows 需验证证书吊销状态，从 CDP (CRL Distribution Point) 下载 CRL | 500ms ~ 30s（取决于 CRL 服务器响应速度） |
| **OCSP 查询** | Windows 使用 OCSP 在线验证证书状态 | 200ms ~ 5s |
| **AIA 链构建** | 服务端未发送完整证书链，Windows 通过 AIA (Authority Information Access) 扩展下载中间证书 | 500ms ~ 10s |
| **证书信任验证** | 遍历 Windows 证书存储，校验签名链 | 通常 < 10ms，但大存储时可达 100ms |

这是 **SChannel 的已知缺陷**，curl 项目也有相同的文档记录。OpenSSL/QUICTLS 不受此影响，因为证书验证不涉及自动网络请求。

**重现条件**:
- 首次连接新主机（证书链未缓存）
- 连接 CRL/OCSP 服务器缓慢或不可达的站点
- 企业网络环境中有代理或防火墙拦截 CRL/OCSP 请求
- 服务端证书链不完整（需要 AIA 补全）

**事件循环影响**: `handshakeStep2()` 中 `for(;;)` 循环在收到 `SEC_I_CONTINUE_NEEDED` 时会立刻 `continue`，不会返回事件循环。如果 `InitializeSecurityContextA()` 在循环某次迭代中阻塞数秒做证书验证，整个事件循环在此期间完全停滞。

---

### 5. HttpResponseCommand ⬤ HIGH

**文件**: `src/network/HttpResponseCommand.cc`  
**基类**: AbstractCommand

| 阻塞操作 | 位置 | 说明 |
|----------|------|------|
| **SChannel TLS 重协商 — SSPI 内部阻塞** | `receiveResponse()` → `readData()` → `SChannelSession::readData()` 行 969 → `handshakeStep2()` → `InitializeSecurityContextA()` | 当服务端发起 TLS renegotiation 或 TLS 1.3 Key Update 时 |
| 同步文件 stat/exists | `handleDefaultEncoding()` → `adjustFilename()` | 检查文件是否存在、获取文件大小 |
| 同步打开/创建文件 | `handleOtherEncoding()` → `initAndOpenFile()` | mkdirs + open(O_CREAT\|O_RDWR\|O_TRUNC) |
| 同步打开已有文件 | `handleDefaultEncoding()` → `openExistingFile()` | open() 系统调用 |
| 同步加载进度信息 | `createCheckIntegrityEntry()` → `progressInfoFile->load()` | 读取 .aria2 控制文件 |
| 同步 truncate | 已有 PieceStorage 路径 → `truncate(0)` | lseek + ftruncate |

#### SChannel 重协商/Key Update 阻塞分析

`HttpResponseCommand::executeInternal()` 调用 `httpConnection_->receiveResponse()` 接收响应头:

```
HttpResponseCommand::executeInternal()  行 146
  → HttpConnection::receiveResponse()   行 149
    → SocketRecvBuffer::recv()           行 59
      → SocketCore::readData()           行 883
        → SChannelSession::readData()    行 799
          → DecryptMessage()             行 892  ← 可能返回 SEC_I_RENEGOTIATE
          → handshakeStep2()             行 969  ← 触发新一轮握手
            → InitializeSecurityContextA()  ← 同上, SSPI 内部可能阻塞
```

`readData()` 解密循环中，当 `DecryptMessage()` 返回 `SEC_I_RENEGOTIATE`（行 932），代码直接内联调用 `handshakeStep2()` 完成重协商。此过程中 `InitializeSecurityContextA()` 同样可能因证书验证触发内部同步网络请求。

**注意**: TLS 1.3 中重协商已被废弃，取而代之的是 Key Update。SChannel 将 Key Update 映射为 `SEC_I_RENEGOTIATE` 返回值，但 Key Update 不涉及证书交换，因此不会触发 CRL/OCSP/AIA 请求。阻塞风险主要存在于 TLS 1.2 重协商场景。

#### 响应接收后的文件 I/O 阻塞

HTTP 响应头接收本身是非阻塞的（`receiveResponse()` 未完成时返回 null）。但一旦收到完整响应头，后续的文件准备工作（打开/创建文件、加载进度信息）全部同步执行。在网络文件系统或高负载磁盘上影响明显。

---

### 6. FtpNegotiationCommand ⬤ HIGH

**文件**: `src/network/FtpNegotiationCommand.cc`  
**基类**: AbstractCommand

| 阻塞操作 | 位置 | 说明 |
|----------|------|------|
| 同步 DNS 解析 | `preparePasvConnect()` 行 715 → `establishConnection()` → `getaddrinfo()` | PASV 数据连接建立 |
| 同步 DNS 解析 | `resolveProxy()` 行 734 → `establishConnection()` → `getaddrinfo()` | 代理连接建立 |
| 同步打开/创建文件 | `onFileSizeDetermined()` 行 445 → `initAndOpenFile()` | mkdirs + open |
| 同步打开已有文件 | `onFileSizeDetermined()` 行 428 → `openExistingFile()` | open() |
| 同步文件 stat | `onFileSizeDetermined()` 行 443 → `adjustFilename()` | File::exists() + File::size() |
| 同步加载进度信息 | `onFileSizeDetermined()` 行 486 → `createCheckIntegrityEntry()` | 读取 .aria2 文件 |

---

### 7. SftpNegotiationCommand ⬤ HIGH

**文件**: `src/protocol/sftp/SftpNegotiationCommand.cc`  
**基类**: AbstractCommand

| 阻塞操作 | 位置 | 说明 |
|----------|------|------|
| 同步打开/创建文件 | `onFileSizeDetermined()` 行 249 → `initAndOpenFile()` | mkdirs + open(O_CREAT\|O_RDWR\|O_TRUNC) |
| 同步打开已有文件 | `onFileSizeDetermined()` 行 232 → `openExistingFile()` | open() |
| 同步文件 stat | `onFileSizeDetermined()` → `adjustFilename()` | File::exists() 检查 |

**详细分析**: SSH/SFTP 网络操作本身已正确使用非阻塞模式（libssh2 EAGAIN 处理），但文件准备阶段的操作是同步的。与 HTTP/FTP 命令共享相同的文件初始化模式。

---

### 8. AbstractCommand (resolveHostname) ⬤ HIGH

**文件**: `src/core/AbstractCommand.cc`  
**基类**: Command

| 阻塞操作 | 位置 | 说明 |
|----------|------|------|
| 同步 DNS 解析 | `resolveHostname()` 行 810-813 → `NameResolver::resolve()` → `getaddrinfo()` | 当 `PREF_ASYNC_DNS=false` 时 |

**详细分析**: 当异步 DNS（c-ares）被禁用时，所有继承自 AbstractCommand 的协议命令（HTTP/FTP/SFTP 等）的 DNS 解析都会退化为同步 `getaddrinfo()` 调用。DNS 查询可能耗时数秒甚至超时（约 30 秒）。默认配置下 `PREF_ASYNC_DNS=true` 使用 c-ares 异步解析，此路径不会触发。

---

### 9. NameResolveCommand ⬤ HIGH

**文件**: `src/core/NameResolveCommand.cc`  
**基类**: Command

| 阻塞操作 | 位置 | 说明 |
|----------|------|------|
| 同步 DNS 解析 | `execute()` 行 106-117 → `NameResolver::resolve()` → `getaddrinfo()` | 当 `PREF_ASYNC_DNS=false` 时 |

**详细分析**: 用于 UDP tracker 的 DNS 解析。当异步 DNS 禁用时，与 AbstractCommand 相同的阻塞路径。启用异步 DNS 时通过 `AsyncNameResolverMan` 非阻塞处理。

---

### 10. DHTEntryPointNameResolveCommand ⬤ HIGH

**文件**: `src/protocol/bt/DHTEntryPointNameResolveCommand.cc`  
**基类**: Command

| 阻塞操作 | 位置 | 说明 |
|----------|------|------|
| 同步 DNS 解析（循环） | `execute()` 行 123-138 → `NameResolver::resolve()` → `getaddrinfo()` | 当 `PREF_ASYNC_DNS=false` 时 |

**详细分析**: 解析 DHT 入口点主机名。关键问题：`while (!entryPoints_.empty())` 循环意味着**多个** DNS 查询会在单次 `execute()` 调用中**串行执行**，阻塞时间叠加。

---

### 11. SaveSessionCommand ⬤ MEDIUM

**文件**: `src/core/SaveSessionCommand.cc`  
**基类**: TimeBasedCommand

| 阻塞操作 | 位置 | 说明 |
|----------|------|------|
| 同步文件写入 | `process()` → `SessionSerializer::save()` | 打开临时文件、写入所有下载状态、rename |
| 哈希计算 | `process()` → `calculateHash()` | 遍历所有下载结果计算哈希（CPU-bound） |

**详细分析**: 会话保存涉及将所有下载任务状态序列化写入文件。数据量与活跃/已完成下载数量成正比。定时触发（默认每 60 秒），单次执行。

---

### 12. DHTAutoSaveCommand ⬤ MEDIUM

**文件**: `src/protocol/bt/DHTAutoSaveCommand.cc`  
**基类**: TimeBasedCommand

| 阻塞操作 | 位置 | 说明 |
|----------|------|------|
| 同步删除临时文件 | `save()` 行 92 → `tempFile.remove()` | 删除旧的临时 DHT 路由表文件 |
| 同步创建目录 | `save()` 行 94 → `File::mkdirs()` | 递归创建目录 |
| 同步写入 DHT 路由表 | `save()` → `serializer.serialize(dhtFile)` | 打开 BufferedFile 写入路由表数据 |

---

## 二、潜在阻塞的 Command（共 4 个）

### 13. HttpServerBodyCommand ⬤ MEDIUM

**文件**: `src/network/HttpServerBodyCommand.cc`

| 潜在阻塞 | 位置 | 说明 |
|----------|------|------|
| RPC `aria2.saveSession` | `execute()` → `method->execute()` | 调用 `SessionSerializer::save()` 同步写文件 |
| JSON-RPC 批量请求 | `execute()` 行 288-304 | 大批量请求串行执行，累计 CPU 时间可观 |
| gzip 压缩 | `rpc::toJson()` / `rpc::toXml()` | 大响应体的 gzip 压缩是 CPU 密集操作 |

---

### 14. WebSocketInteractionCommand ⬤ LOW

**文件**: `src/protocol/ws/WebSocketInteractionCommand.cc`

| 潜在阻塞 | 位置 | 说明 |
|----------|------|------|
| RPC 处理 | `onMsgRecvCallback` → `processJsonRpcRequest()` | 与 HttpServerBodyCommand 相同的 RPC 阻塞风险 |

---

### 15. LpdDispatchMessageCommand ⬤ LOW

**文件**: `src/core/LpdDispatchMessageCommand.cc`

| 潜在阻塞 | 位置 | 说明 |
|----------|------|------|
| getaddrinfo (数值 IP) | `sendMessage()` → `writeData()` → `callGetaddrinfo()` | 每次发送都对数值型多播地址调用 getaddrinfo |
| UDP sendto | 同上 | 内核缓冲区满时 sendto 可能阻塞 |

**实际影响**: 数值 IP 的 getaddrinfo 通常立即返回，小 UDP 包的 sendto 通常非阻塞。但缺少结果缓存，属于不必要的系统调用开销。

---

### 16. BackupIPv4ConnectCommand ⬤ LOW

**文件**: `src/core/BackupIPv4ConnectCommand.cc`

| 潜在阻塞 | 位置 | 说明 |
|----------|------|------|
| getaddrinfo (数值 IP) | `execute()` 行 125 → `establishConnection()` → `callGetaddrinfo()` | 对数值 IPv4 地址调用 getaddrinfo |

**实际影响**: 数值 IP 地址的 getaddrinfo 仅做格式转换，通常微秒级完成。connect() 已设置为非阻塞。

---

## 三、无阻塞的 Command（共 14 个）

| # | Command | 文件 | 说明 |
|---|---------|------|------|
| 1 | AbstractHttpServerResponseCommand | `src/core/AbstractHttpServerResponseCommand.cc` | 非阻塞 socket 写入，WOULDBLOCK 时重新入队 |
| 2 | ActivePeerConnectionCommand | `src/core/ActivePeerConnectionCommand.cc` | 纯内存操作：定时器检查 + 创建连接命令 |
| 3 | CreateRequestCommand | `src/core/CreateRequestCommand.cc` | 纯内存操作：查找文件条目 + 创建请求 |
| 4 | KeepRunningCommand | `src/core/KeepRunningCommand.cc` | 仅检查 halt 标志 |
| 5 | LpdReceiveMessageCommand | `src/core/LpdReceiveMessageCommand.cc` | 非阻塞 recvfrom，循环上限 20 次 |
| 6 | SeedCheckCommand | `src/core/SeedCheckCommand.cc` | 纯内存操作：检查做种条件 |
| 7 | TrackerWatcherCommand | `src/core/TrackerWatcherCommand.cc` | Tracker 响应使用 ByteArrayDiskWriter（内存） |
| 8 | FtpFinishDownloadCommand | `src/network/FtpFinishDownloadCommand.cc` | 非阻塞 FTP 响应接收 |
| 9 | HttpListenCommand | `src/network/HttpListenCommand.cc` | 非阻塞 accept（监听 socket 已设为非阻塞） |
| 10 | HttpServerCommand | `src/network/HttpServerCommand.cc` | 非阻塞请求接收 + TLS 握手 |
| 11 | DHTGetPeersCommand | `src/protocol/bt/DHTGetPeersCommand.cc` | 纯定时器 + 任务调度 |
| 12 | DHTInteractionCommand | `src/protocol/bt/DHTInteractionCommand.cc` | 非阻塞 UDP 收发，WOULDBLOCK 时中断 |
| 13 | PeerChokeCommand | `src/protocol/peer/PeerChokeCommand.cc` | CPU 排序（512 peer 上限，每 10 秒一次） |
| 14 | PeerListenCommand | `src/protocol/peer/PeerListenCommand.cc` | 非阻塞 accept 循环，上限 3 次 |
| 15 | SftpFinishDownloadCommand | `src/protocol/sftp/SftpFinishDownloadCommand.cc` | 非阻塞 libssh2 SFTP close（EAGAIN 处理） |
| 16 | FileAllocationCommand | `src/storage/FileAllocationCommand.cc` | ✅ 已正确异步化：ThreadPool + future + wait_for(100ns) |
| 17 | TimeBasedAsyncCommand | `src/util/TimeBasedAsyncCommand.cc` | ✅ 异步模板：ThreadPool + future（当前无子类使用） |
| 18 | PeerAbstractCommand | `src/protocol/peer/PeerAbstractCommand.cc` | 基类无阻塞，依赖子类 executeInternal() |

### TimeBasedCommand 无阻塞子类

| # | Command | 文件 | 说明 |
|---|---------|------|------|
| 1 | HaveEraseCommand | `src/core/HaveEraseCommand.cc` | 内存中移除过期 piece 广告 |
| 2 | EvictSocketPoolCommand | `src/core/EvictSocketPoolCommand.cc` | 内存中清理连接池 |
| 3 | BtStopDownloadCommand | `src/protocol/bt/BtStopDownloadCommand.cc` | 计算下载速度（内存） |
| 4 | DHTBucketRefreshCommand | `src/protocol/bt/DHTBucketRefreshCommand.cc` | 入队刷新任务（内存） |
| 5 | DHTPeerAnnounceCommand | `src/protocol/bt/DHTPeerAnnounceCommand.cc` | 超时处理（内存） |
| 6 | DHTTokenUpdateCommand | `src/protocol/bt/DHTTokenUpdateCommand.cc` | Token 更新（内存） |
| 7 | DelayedCommand | `src/core/DelayedCommand.h` | 延迟入队命令（内存） |
| 8 | TimedHaltCommand | `src/util/TimedHaltCommand.cc` | 设置 halt 标志 |
| 9 | WatchProcessCommand | `src/core/WatchProcessCommand.cc` | 进程存活检查（0 超时，实际非阻塞） |

---

## 四、阻塞类型统计

### 按阻塞类型分类

| 阻塞类型 | 涉及 Command 数量 | 典型耗时 |
|----------|-------------------|----------|
| **SChannel SSPI 内部网络请求** | 2 个（HttpRequestCommand, HttpResponseCommand） | 200ms ~ 30s（CRL/OCSP/AIA） |
| **同步 DNS (getaddrinfo)** | 5 个（AbstractCommand, NameResolveCommand, DHTEntryPointNameResolveCommand, FtpNegotiationCommand, LpdDispatchMessageCommand） | 1ms ~ 30s |
| **同步文件 I/O (open/stat/write/delete)** | 7 个（FillRequestGroupCommand, HttpResponseCommand, HttpRequestCommand, FtpNegotiationCommand, SftpNegotiationCommand, SaveSessionCommand, DHTAutoSaveCommand） | 0.1ms ~ 100ms |
| **fsync / FlushFileBuffers** | 2 个（AutoSaveCommand, FillRequestGroupCommand） | 1ms ~ 500ms |
| **同步磁盘读取 + 哈希** | 1 个（CheckIntegrityCommand） | 1ms ~ 1000ms |
| **CPU 密集（压缩/序列化）** | 1 个（HttpServerBodyCommand） | 1ms ~ 100ms |

### 按触发频率分类

| 频率 | Command | 影响 |
|------|---------|------|
| **每次 HTTPS 连接** | HttpRequestCommand (SChannel TLS 握手) | SSPI 内部 CRL/OCSP/AIA 同步网络请求，首次连接或证书缓存过期时触发 |
| **每次下载启动** | HttpResponseCommand, FtpNegotiationCommand, SftpNegotiationCommand, HttpRequestCommand | 新连接文件准备时一次性阻塞 |
| **每次下载完成** | FillRequestGroupCommand | 清理时阻塞，批量完成时叠加 |
| **定时循环（~60s）** | AutoSaveCommand, SaveSessionCommand | 周期性阻塞，影响随活跃下载数量线性增长 |
| **定时循环（~30min）** | DHTAutoSaveCommand | 低频，单次影响有限 |
| **完整性校验期间** | CheckIntegrityCommand | 校验期间持续阻塞，每次读一个 piece |
| **连接建立时** | AbstractCommand (DNS), NameResolveCommand, DHTEntryPointNameResolveCommand | 仅在 async DNS 禁用时 |

---

## 五、关键发现与建议

### 发现 1：CheckIntegrityCommand 未异步化（遗漏）

`FileAllocationCommand` 和 `CheckIntegrityCommand` 同为 `RealtimeCommand` 子类，执行类似的重 I/O 操作。`FileAllocationCommand` 已正确迁移到 ThreadPool 异步模式：

```cpp
// FileAllocationCommand - 正确的异步模式
future_ = e_->getThreadPool()->enqueue([this]() { executeInternalImpl(); });
// ...
if (future_->wait_for(100_ns) == std::future_status::ready) {
    future_->get();
}
```

但 `CheckIntegrityCommand` 仍然直接在事件循环线程上执行磁盘读取和哈希计算。建议参照 `FileAllocationCommand` 进行异步化改造。

### 发现 2：AutoSaveCommand 的 fsync 风暴

`AutoSaveCommand::process()` 对**每个活跃下载任务**调用 `saveControlFile()`，其中包含 `flushOSBuffers()`（即 `fsync()`）。当有 N 个活跃下载时，单次 `process()` 会触发 N 次 fsync。在 HDD 上每次 fsync 可达 10-100ms，10 个活跃下载就可能阻塞 1 秒。

建议：将 `AutoSaveCommand` 迁移到 `TimeBasedAsyncCommand` 模式（该基类已存在但当前无子类使用），或在 ThreadPool 中执行 save 操作。

### 发现 3：文件初始化是共性问题

HTTP、FTP、SFTP 三种协议的下载命令在收到服务端文件大小信息后，都会走入相同的文件初始化路径：`adjustFilename()` → `initAndOpenFile()` / `openExistingFile()` → `loadAndOpenFile()`。这些操作全部同步执行。

这是架构性问题 —— 文件准备阶段缺少异步化。可考虑将文件打开/创建操作卸载到 ThreadPool，或引入文件操作的 Command 状态机。

### 发现 4：TimeBasedAsyncCommand 基础设施闲置

`TimeBasedAsyncCommand` 已实现正确的 ThreadPool 异步模式，但**当前没有任何子类**。`AutoSaveCommand`、`SaveSessionCommand`、`DHTAutoSaveCommand` 这些需要定时执行文件 I/O 的命令都继承自同步的 `TimeBasedCommand`，未利用已有的异步基础设施。

### 发现 5：同步 DNS 有条件触发

三处同步 DNS 解析（AbstractCommand、NameResolveCommand、DHTEntryPointNameResolveCommand）均仅在 `PREF_ASYNC_DNS=false` 或未编译 `ENABLE_ASYNC_DNS` 时触发。默认配置下使用 c-ares 异步解析，不受影响。但 `FtpNegotiationCommand` 中 `establishConnection()` 路径的 `getaddrinfo()` 调用不受此开关控制（因为它在 SocketCore 底层直接调用）。

### 发现 6：Windows SChannel TLS 握手的隐性阻塞（CRITICAL）

**这是 Windows 平台上最严重的 socket 级阻塞问题。**

SChannel 实现（`src/tls/schannel/SChannelSession.cc`）的 socket I/O（`::send()`/`::recv()`）已正确处理 `WSAEWOULDBLOCK`，表面上完全非阻塞。但 `InitializeSecurityContextA()` SSPI API 本身会在证书验证阶段**内部发起同步网络请求**，完全绕过应用层的非阻塞模型：

```
事件循环线程
  → HttpRequestCommand::executeInternal()
    → SocketCore::tlsHandshake()
      → SChannelSession::handshakeStep2()
        → InitializeSecurityContextA()     ← 用户空间调用
          → [SChannel 内部]
            → CryptoAPI 证书验证
              → HTTP GET http://crl.xxx.com/xxx.crl    ← 内部同步网络请求!
              → HTTP GET http://ocsp.xxx.com/           ← 内部同步网络请求!
              → HTTP GET http://aia.xxx.com/xxx.cer     ← 内部同步网络请求!
        ← 返回（可能已过去数秒）
```

这些内部网络请求使用 Windows 自己的 HTTP 栈（WinHTTP），与应用的非阻塞 socket 完全独立。即使应用 socket 设置了非阻塞模式，也无法避免 SSPI 内部的阻塞。

**影响范围**:
- `HttpRequestCommand`: TLS 握手阶段（行 129 `tlsConnect()`）
- `HttpResponseCommand`: TLS 重协商阶段（readData 中 `SEC_I_RENEGOTIATE` 触发）
- 所有使用 SChannel 的 HTTPS 连接

**对比**: OpenSSL/QUICTLS/LibreSSL 的证书验证是纯计算操作，不涉及网络请求，不存在此问题。

**缓解措施**:
1. 将 TLS 握手卸载到 ThreadPool（需要较大架构改动）
2. 通过 Windows 注册表或 `CRYPT_VERIFY_CERT_SIGN_DISABLE_MD2_MD4_FLAG` 等策略禁用 CRL/OCSP 自动检查（降低安全性）
3. 使用 OpenSSL/QUICTLS 替代 SChannel（当前默认配置已支持 `use_quictls=true`）
4. 在 SChannel 凭证创建时设置 `SCH_CRED_REVOCATION_CHECK_CHAIN` 策略控制吊销检查行为

---

## 六、总览表

| # | Command | 文件 | 阻塞? | 严重度 | 主要阻塞操作 |
|---|---------|------|-------|--------|-------------|
| 1 | CheckIntegrityCommand | `src/core/CheckIntegrityCommand.cc` | **YES** | **CRITICAL** | 同步读 piece + 哈希（最大 16MB/次） |
| 2 | AutoSaveCommand | `src/core/AutoSaveCommand.cc` | **YES** | **CRITICAL** | N × fsync + 写控制文件 |
| 3 | FillRequestGroupCommand | `src/core/FillRequestGroupCommand.cc` | **YES** | **CRITICAL** | 关闭/写入/删除多个文件 |
| 4 | HttpRequestCommand | `src/network/HttpRequestCommand.cc` | **YES** | **HIGH** | SChannel `InitializeSecurityContextA()` 内部同步 CRL/OCSP/AIA 网络请求；stat 文件（条件 GET） |
| 5 | HttpResponseCommand | `src/network/HttpResponseCommand.cc` | **YES** | **HIGH** | SChannel 重协商时同 #4；打开/创建/stat 文件 + 加载进度 |
| 6 | FtpNegotiationCommand | `src/network/FtpNegotiationCommand.cc` | **YES** | **HIGH** | getaddrinfo + 打开/创建文件 |
| 7 | SftpNegotiationCommand | `src/protocol/sftp/SftpNegotiationCommand.cc` | **YES** | **HIGH** | 打开/创建文件 |
| 8 | AbstractCommand | `src/core/AbstractCommand.cc` | **YES** | **HIGH** | getaddrinfo（async DNS 禁用时） |
| 9 | NameResolveCommand | `src/core/NameResolveCommand.cc` | **YES** | **HIGH** | getaddrinfo（async DNS 禁用时） |
| 10 | DHTEntryPointNameResolveCommand | `src/protocol/bt/DHTEntryPointNameResolveCommand.cc` | **YES** | **HIGH** | 循环内多次 getaddrinfo（async DNS 禁用时） |
| 11 | SaveSessionCommand | `src/core/SaveSessionCommand.cc` | **YES** | **MEDIUM** | 同步写会话文件 |
| 12 | DHTAutoSaveCommand | `src/protocol/bt/DHTAutoSaveCommand.cc` | **YES** | **MEDIUM** | 同步写 DHT 路由表 |
| 13 | HttpServerBodyCommand | `src/network/HttpServerBodyCommand.cc` | POTENTIAL | **MEDIUM** | RPC saveSession + 批量请求 + gzip |
| 14 | WebSocketInteractionCommand | `src/protocol/ws/WebSocketInteractionCommand.cc` | POTENTIAL | **LOW** | RPC 处理同步执行 |
| 15 | LpdDispatchMessageCommand | `src/core/LpdDispatchMessageCommand.cc` | POTENTIAL | **LOW** | getaddrinfo（数值 IP，通常即时） |
| 16 | BackupIPv4ConnectCommand | `src/core/BackupIPv4ConnectCommand.cc` | POTENTIAL | **LOW** | getaddrinfo（数值 IP，通常即时） |
| 17-30 | 其余 14 个 Command | — | **NO** | — | 非阻塞 |
