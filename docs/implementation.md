# aria2-zero 实现逻辑

## 1. 程序启动流程

```
main(argc, argv)                          [src/main.cc]
  │
  ├── Windows: 宽字符命令行转 UTF-8
  │
  ├── Context(true, argc, argv, KeyVals{})  [src/core/Context.cc]
  │     ├── 解析命令行选项 → Option 对象
  │     ├── 创建 RequestGroup (每个 URL 一个)
  │     ├── 初始化平台相关设置 (socket, TLS)
  │     └── 创建 MultiUrlRequestInfo
  │
  └── context.reqinfo->execute()            [src/core/MultiUrlRequestInfo.cc]
        ├── prepare()
        │     ├── 创建 DownloadEngine
        │     │     ├── 初始化 EventPoll (根据平台选择)
        │     │     ├── 初始化 ThreadPool (4 线程)
        │     │     └── 初始化 DNSCache
        │     ├── 通过 DownloadEngineFactory 配置:
        │     │     ├── RequestGroupMan (并发下载管理)
        │     │     ├── FileAllocationMan (文件预分配)
        │     │     ├── CheckIntegrityMan (完整性校验)
        │     │     ├── AuthConfigFactory (认证)
        │     │     ├── CookieStorage (Cookie)
        │     │     └── BtRegistry (BitTorrent)
        │     ├── 注册信号处理 (SIGINT/SIGTERM)
        │     └── 设置统计计算器 (StatCalc)
        │
        ├── e_->run()                       [src/core/DownloadEngine.cc]
        │     └── 进入主事件循环 (见下文)
        │
        └── getResult()
              ├── 保存会话 (.aria2 文件)
              ├── 保存 Cookie
              ├── 输出统计信息
              └── 返回退出码
```

## 2. 主事件循环实现

### 2.1 循环核心逻辑

```cpp
// DownloadEngine.cc (简化)
int DownloadEngine::run(bool oneshot) {
    while (!commands_.empty() || !routineCommands_.empty()) {
        // 1. 等待 I/O 事件
        waitData();   // → eventPoll_->poll(timeout)

        // 2. 更新统计
        calculateStatistics();

        // 3. 执行普通命令
        if (refreshInterval 已过) {
            executeCommand(commands_, Command::STATUS_ALL);
        } else {
            executeCommand(commands_, Command::STATUS_ACTIVE);
        }

        // 4. 执行例行命令
        executeCommand(routineCommands_, Command::STATUS_ALL);

        // 5. 迭代后处理
        afterEachIteration();

        // 6. oneshot 模式下单次退出
        if (oneshot && !noWait_) return 1;
    }
    onEndOfRun();
    return 0;
}
```

### 2.2 命令执行机制

```cpp
// DownloadEngine.cc (简化)
void DownloadEngine::executeCommand(deque<Command*>& commands, int statusFilter) {
    size_t count = commands.size();
    while (count--) {
        Command* cmd = commands.front();
        commands.pop_front();

        if (!cmd->statusMatch(statusFilter)) {
            commands.push_back(cmd);  // 状态不匹配, 放回队尾
            continue;
        }

        cmd->transitStatus();  // 更新命令状态

        try {
            if (cmd->execute()) {
                delete cmd;  // 命令完成, 销毁
            } else {
                cmd->clearIOEvents();  // 清除 I/O 标志
                commands.push_back(cmd);  // 继续执行, 放回队尾
            }
        } catch (...) {
            delete cmd;  // 异常时销毁
        }
    }
}
```

### 2.3 Command 状态转换

```
创建时: STATUS_INACTIVE
  │
  ├── socket 可读/可写事件触发 → STATUS_ACTIVE
  │
  ├── 刷新间隔到达 → 即使 INACTIVE 也会被执行
  │
  ├── Command 主动设置:
  │     setStatusActive()         → STATUS_ACTIVE
  │     setStatusRealtime()       → STATUS_REALTIME
  │     setStatusOneshot_Realtime → STATUS_ONESHOT_REALTIME
  │
  └── transitStatus():
        ONESHOT_REALTIME → INACTIVE (执行一次后降级)
        其他状态保持不变
```

### 2.4 EventPoll 集成

```
AbstractCommand 注册 socket 事件:
  setReadCheckSocket(socket)
    → engine->addSocketForReadCheck(socket, this)
      → eventPoll->addEvents(socket.fd, this, EVENT_READ)

EventPoll::poll():
  epoll_wait() / kevent() / select() 返回就绪事件
    → 对每个就绪 socket:
       socketEntry.processEvents(events)
         → 对匹配的 CommandEvent:
            command->readEventReceived()   // 设置 readEvent_ = true
            command->writeEventReceived()  // 设置 writeEvent_ = true

下一轮 executeCommand():
  cmd->execute()
    → AbstractCommand::execute()
      → shouldProcess(): 检查 readEvent_/writeEvent_
      → 如果就绪: executeInternal() (子类具体逻辑)
```

## 3. 下载任务生命周期

### 3.1 HTTP 下载完整流程

```
1. 用户添加 URL
   → RequestGroup 创建, 加入 RequestGroupMan.reservedGroups_

2. RequestGroupMan::fillRequestGroupFromReserver()
   → 检查并发数限制 (maxConcurrentDownloads_)
   → 移动 RequestGroup 到 requestGroups_ (活跃列表)
   → 调用 RequestGroup::createInitialCommand()

3. createInitialCommand()
   → 创建 HttpInitiateConnectionCommand
   → 加入 DownloadEngine.commands_

4. HttpInitiateConnectionCommand::executeInternal()
   ├── resolveHostname() → DNS 解析
   │     同步: 直接查询
   │     异步: 通过 c-ares, 注册 ADNSEvent
   ├── 检查连接池是否有可复用 socket
   ├── 建立 TCP 连接 (非阻塞)
   ├── setWriteCheckSocket(socket)  → 等待连接完成
   └── 返回 HttpRequestCommand

5. HttpRequestCommand::executeInternal()
   ├── 创建 HttpRequest (包含 Range, Cookie, Auth 等头部)
   ├── httpConnection_->sendRequest(request)
   ├── setReadCheckSocket(socket)   → 等待响应
   └── 返回 HttpResponseCommand

6. HttpResponseCommand::executeInternal()
   ├── httpConnection_->receiveResponse() → 读取响应头
   ├── 解析状态码 (200/206/301/302/...)
   ├── 处理重定向: 创建新的 InitiateConnectionCommand
   ├── 检查服务器是否支持 Range
   ├── 分配下载分段: SegmentMan::getSegment()
   └── 返回 HttpDownloadCommand

7. HttpDownloadCommand::executeInternal() (继承自 DownloadCommand)
   ├── 从 socket 读取数据
   ├── 通过 StreamFilter 链处理:
   │     ChunkedDecodingStreamFilter → GZipDecodingStreamFilter
   ├── 写入 PieceStorage → DiskAdaptor
   ├── 更新 Segment.writtenLength
   ├── 检查分段是否完成
   │     如果完成: SegmentMan::completeSegment()
   │     尝试获取新分段: SegmentMan::getSegment()
   └── 循环直到所有分段完成

8. 下载完成
   → RequestGroup::downloadFinished() == true
   → RequestGroupMan::removeStoppedGroup()
   → 移入 downloadResults_
   → 如果有更多等待任务: fillRequestGroupFromReserver()
```

### 3.2 分段并行下载

```
文件大小: 100MB, minSplitSize: 20MB, maxConnections: 5

初始:
  Cmd A → SegmentMan::getSegment() → [0-20MB]
  自动触发更多连接:
    Cmd B → [20-40MB]
    Cmd C → [40-60MB]
    Cmd D → [60-80MB]
    Cmd E → [80-100MB]

连接失败时:
  Cmd C 失败
    → SegmentMan::cancelSegment(Cmd C.cuid)
    → [40-60MB] 分段释放回池
    → prepareForRetry() → 创建新 CreateRequestCommand
    → 新 Cmd F 获取 [40-60MB]

分段完成时:
  Cmd A 完成 [0-20MB]
    → SegmentMan::completeSegment()
    → 尝试 getSegment() 获取新分段
    → 如果所有分段已分配: Cmd A 结束
```

### 3.3 BitTorrent 下载流程

```
1. 种子文件解析
   → BtPostDownloadHandler 解析 .torrent
   → 创建 DownloadContext (文件列表, 分片哈希)
   → 注册到 BtRegistry

2. Tracker 通告
   → BtAnnounce 发送 announce 请求
   → 获取 peer 列表
   → PeerStorage 存储 peer 信息

3. 对等连接建立
   ├── 主动连接: ActivePeerConnectionCommand
   │     → 从 PeerStorage 选择 peer
   │     → BtInitiateConnectionCommand → TCP 连接
   │     → PeerInteractionCommand
   │
   └── 被动接受: PeerListenCommand
         → 监听端口
         → 收到连接 → PeerInteractionCommand

4. PeerInteractionCommand (每个 peer 一个)
   循环执行:
   ├── BtInteractive::receiveMessages()
   │     解析 BT 消息 (choke/unchoke/have/bitfield/request/piece)
   │     更新 peer 状态
   │
   ├── BtInteractive::doInteractionProcessing()
   │     ├── 发送 interested/not-interested
   │     ├── 请求分片 (通过 BtMessageDispatcher)
   │     ├── 上传分片 (响应 request 消息)
   │     └── keep-alive
   │
   └── BtMessageDispatcher::sendMessages()
         刷新消息队列到 socket

5. 分片完成
   → PieceStorage 标记分片完成
   → 校验分片哈希 (SHA-1)
   → 广播 have 消息给所有 peer
   → 所有分片完成 → 进入做种模式
```

## 4. 连接复用与 Socket 管理

### 4.1 Socket 连接池

```cpp
// DownloadEngine 维护 socket 连接池
multimap<string, SocketPoolEntry> socketPool_;
// key = "protocol://host:port"

// 连接复用流程:
HttpInitiateConnectionCommand::executeInternal() {
    // 1. 先查连接池
    socket = engine->popPooledSocket(host, port, protocol);
    if (socket) {
        // 复用已有连接, 跳过 DNS+TCP 握手
        return createHttpRequestCommand(socket);
    }
    // 2. 无可用连接, 新建
    socket = createNewConnection();
}

// 请求完成后归还连接 (HTTP Keep-Alive):
HttpResponseCommand::afterComplete() {
    if (response.isKeepAlive()) {
        engine->poolSocket(host, port, protocol, socket);
    }
}
```

### 4.2 Socket 核心抽象

**文件**: `src/network/SocketCore.h`

统一的 socket 接口，封装平台差异：
- 非阻塞 I/O
- TLS 透明集成 (通过 TLSContext)
- IPv4/IPv6 双栈
- Winsock 兼容

## 5. 流过滤链

### 5.1 StreamFilter 架构

```
网络数据 → [ChunkedDecoding] → [GZipDecoding] → [ContentEncoding] → PieceStorage

DownloadCommand 持有 StreamFilter 链:
  streamFilter_ → next_ → next_ → ... → sinkFilter_ (写入存储)
```

实现:
- `ChunkedDecodingStreamFilter` — HTTP chunked 传输解码
- `GZipDecodingStreamFilter` — gzip/deflate 解压
- 链式调用: 每个 filter 处理完数据后传递给 `next_`

### 5.2 分片选择策略

**文件**: `src/stream/StreamPieceSelector.h`

| 策略 | 类 | 使用场景 |
|------|-----|---------|
| 默认 | `DefaultStreamPieceSelector` | 顺序下载 |
| 随机 | `RandomStreamPieceSelector` | BT 分散下载 |
| 几何 | `GeomStreamPieceSelector` | 几何分布 |
| 顺序 | `InorderStreamPieceSelector` | 严格顺序 |

## 6. 文件分配策略

### 6.1 分配流程

```
RequestGroup 创建
  → FileAllocationEntry 加入 FileAllocationMan 队列
  → FileAllocationCommand 在主循环中增量执行

FileAllocationCommand::execute():
  while (!allocator_->finished()) {
      allocator_->allocateChunk();
      if (已用时间 > 阈值) break;  // 避免阻塞主循环过久
  }
  if (finished) {
      → 创建下载命令
      return true;
  }
  return false;  // 继续下次迭代
```

### 6.2 分配方式

```
AdaptiveFileAllocationIterator:
  尝试 fallocate()
    成功 → 使用 fallocate (最快, 无实际写入)
    失败 → 回退到 truncate() (设置文件大小)
             或 写零填充 (最慢但最可靠)
```

## 7. 完整性校验

### 7.1 校验流程

```
下载完成
  → CheckIntegrityEntry 加入 CheckIntegrityMan 队列
  → CheckIntegrityCommand 增量校验

CheckIntegrityCommand::execute():
  entry_->validateChunk()
    → 读取文件数据 (DiskAdaptor)
    → 计算分片哈希 (MD5/SHA-1/SHA-256)
    → 与期望值比较

  if (finished) {
      if (校验通过) {
          entry_->onDownloadFinished()
            → BT: 进入做种模式
            → HTTP: 完成
      } else {
          entry_->onDownloadIncomplete()
            → 重新下载损坏分片
      }
  }
```

### 7.2 断点续传校验

aria2 在启动下载前，如果存在 `.aria2` 控制文件:
1. 加载已下载分片位图
2. 对已下载数据进行完整性校验
3. 仅下载缺失/损坏的分片

## 8. RPC 请求处理

### 8.1 JSON-RPC over HTTP

```
客户端发送 POST /jsonrpc
  ↓
HttpServerCommand (监听命令, 在主循环中)
  → 接收 HTTP 请求
  → 解析 JSON-RPC 请求体
  ↓
RpcMethodFactory::getMethod(methodName)
  → 返回对应的 RpcMethod 实例 (单例复用)
  ↓
RpcMethod::process(RpcRequest, DownloadEngine*)
  → 执行操作 (如 addUri: 创建 RequestGroup)
  → 返回 RpcResponse
  ↓
HttpServerCommand 发送 HTTP 响应
```

### 8.2 WebSocket RPC

```
WebSocket 握手完成
  ↓
WebSocketSessionMan 管理活跃连接
  ↓
接收 WebSocket 帧
  → 解析 JSON-RPC
  → 同上处理
  ↓
推送通知 (下载开始/完成/错误):
  → WebSocketSessionMan 遍历所有活跃连接
  → 发送事件通知
```

## 9. 错误恢复机制

### 9.1 连接失败处理

```
AbstractCommand::checkIfConnectionEstablished():
  socket 连接失败
    → 将失败 IP 标记为 bad
    → 尝试下一个 IP 地址 (DNS 返回多个)
    → 所有 IP 失败: prepareForRetry()
      → 创建 CreateRequestCommand
      → 等待重试间隔后重新开始
```

### 9.2 超时处理

```
AbstractCommand::execute():
  if (当前时间 - checkPoint_ > timeout) {
      // 连接/读取超时
      标记服务器错误
      清除 DNS 缓存中的坏地址
      prepareForRetry()
  }
```

### 9.3 服务器错误重试

```
HTTP 5xx 错误
  → 等待递增间隔 (backoff)
  → 重新创建 InitiateConnectionCommand
  → 最大重试次数检查

HTTP 503 (Service Unavailable)
  → 检查 Retry-After 头
  → 按服务器指示的时间等待
```

### 9.4 分段失败恢复

```
某个分段的连接失败:
  1. SegmentMan::cancelSegment(cuid) → 释放分段
  2. 其他活跃连接继续工作
  3. 重试逻辑获取空闲分段并重新下载
  4. 如果是最后一个连接失败: 整个 RequestGroup 进入重试
```

## 10. 平台适配

### 10.1 Windows 特殊处理

- **长路径**: 使用 `\\?\` 前缀支持超过 260 字符的路径
- **Winsock**: 启动时调用 `WSAStartup()`，socket 使用 `SOCKET` 类型
- **TLS**: 可使用 WinTLS (SChannel) 或 OpenSSL
- **文件锁**: 使用 `LockFileEx()` 替代 `flock()`
- **控制台**: 使用 `SetConsoleOutputCP(CP_UTF8)` 设置 UTF-8 输出

### 10.2 xmake 平台检测

```lua
-- xmake.lua 中的平台适配
if is_plat("windows") then
    add_cxflags("/EHsc")      -- MSVC 异常处理
    add_syslinks("ws2_32", "iphlpapi", "crypt32")
elseif is_plat("linux") then
    add_defines("_GNU_SOURCE=1")
    -- 使用 epoll
elseif is_plat("macosx") then
    -- 使用 kqueue + Apple Security
end
```

## 11. 关键数据结构

### 11.1 全局 ID (A2Gid)

```cpp
typedef uint64_t a2_gid_t;  // 全局唯一下载标识
// 通过 GroupId::generateGid() 生成
// RPC 接口中用 16 进制字符串表示
```

### 11.2 命令 ID (cuid_t)

```cpp
typedef int64_t cuid_t;  // 命令唯一标识
// 每个 Command 实例分配唯一 ID
// 用于 SegmentMan 追踪分段所有权
```

### 11.3 选项系统

```cpp
class Option {
    // key-value 存储, 支持全局和 per-download 选项
    // 在 prefs.h 中定义所有合法选项名
};

// 选项继承: 全局选项 → RequestGroup 选项 → 命令行覆盖
```
