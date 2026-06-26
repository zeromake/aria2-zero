# aria2-zero

aria2 的 fork 分支，支持 MSVC 编译，使用 xmake 替代原有构建系统实现全平台编译。

## 项目概述

aria2-zero 是一个高性能多协议下载工具，支持 HTTP/HTTPS、FTP、SFTP、BitTorrent 和 Metalink。核心架构为**单线程事件驱动主循环**，通过 Command 模式派发所有任务，ThreadPool (4 线程) 仅用于阻塞 I/O 操作。

## 构建系统

使用 xmake 构建，不再使用 autotools/CMake。

```bash
# 编译共享库 (Release)
xmake f -m release && xmake

# 编译静态库 (Debug)
xmake f -m debug -k static && xmake

# 编译
xmake f -c && xmake
```

### 构建选项

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `uv` | false | 使用 libuv 事件循环 |
| `ssl_external` | false (Linux 为 true) | 使用外部 SSL 库 |
| `use_quictls` | true | 使用 QUICTLS (否则 LibreSSL) |
| `with_breakpad` | false | 启用崩溃转储 |
| `with_static` | false | 静态链接 glibc |
| `unit` | false | 编译单元测试 |
| `merge_staticlib` | false | 合并静态库 |

### 核心依赖

expat, zlib, sqlite3, c-ares, libssh2, boost.intl, quictls/libressl

## 项目结构

```
aria2-zero/
├── xmake.lua              # 主构建配置
├── package.lua             # 依赖包配置
├── merge_staticlib.lua     # 静态库合并规则
├── config.h.in             # 构建配置模板
├── include/aria2/aria2.h   # 公共库 API
├── src/
│   ├── main.cc             # CLI 入口
│   ├── core/               # 核心引擎 (DownloadEngine, Command, RequestGroup)
│   ├── network/            # 网络协议 (HTTP, FTP, Socket)
│   ├── protocol/
│   │   ├── bt/             # BitTorrent 协议
│   │   ├── metalink/       # Metalink 解析
│   │   ├── lpd/            # 本地对等发现
│   │   ├── peer/           # 对等连接管理
│   │   ├── piece/          # 分片管理
│   │   ├── ws/             # WebSocket
│   │   └── sftp/           # SFTP 协议
│   ├── poll/               # 事件轮询 (epoll/kqueue/select/libuv)
│   ├── rpc/                # RPC 接口 (JSON-RPC/XML-RPC)
│   ├── storage/            # 磁盘 I/O 和文件管理
│   ├── stream/             # 流过滤 (chunked/gzip)
│   ├── tls/                # TLS 实现 (OpenSSL/WinTLS/Apple)
│   ├── crypto/             # 加密算法
│   ├── parser/             # XML/JSON 解析
│   ├── util/               # 工具函数
│   └── win32/              # Windows 特定代码
├── compat/                 # 兼容层 (ThreadPool)
├── deps/                   # 内嵌依赖 (wslay)
├── test/                   # 单元测试
└── examples/               # 示例代码
```

## 关键架构概念

- **单线程主循环**: `DownloadEngine::run()` 是唯一的事件循环，所有网络 I/O 通过 EventPoll 多路复用
- **Command 模式**: 所有操作（HTTP 请求、BT 对等通信、RPC 处理）都是 Command 对象，由主循环按状态派发执行
- **Command 状态**: INACTIVE (等待事件) → ACTIVE (就绪) → REALTIME (高优先级)
- **RequestGroup**: 代表一个下载任务，包含 DownloadContext、SegmentMan、PieceStorage
- **三个命令队列**: routineCommands (每轮执行)、commands (状态过滤执行)、priorityCommands (高优先级)
- **ThreadPool**: 4 线程，仅用于阻塞 I/O (文件分配、DNS 解析)，不处理网络事件

## 编码规范

- C++14 标准
- 使用 `.clang-format` 格式化
- Windows 平台使用 `/EHsc` 异常处理
- 类型命名: 大驼峰 (DownloadEngine, RequestGroup)
- 文件命名: 大驼峰 (DownloadEngine.h, RequestGroup.cc)

## 测试

> windows 下需要在 pwsh 执行

```bash
rm -rf build && xmake f -c -y && xmake
```

## 特性 (相对于原版 aria2 的改进)

- MSVC 编译支持
- xmake 全平台构建
- Windows 长路径支持 (`\\?\` 前缀, MAX_PATH > 260)
- HTTP 头部排序
- 统一 LibreSSL 用于 SFTP
- Metalink v3 命名空间支持
- BT padding 文件跳过
- UTF-8 BOM 支持
- 分类目录支持
- TLS 1.3 支持
- 异步文件操作 (ThreadPool)
- 跨平台 MO 翻译文件加载
