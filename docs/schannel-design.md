# SChannel TLS 实现文档

> **状态**: 已实现并验证通过
> **参考**: curl `lib/vtls/schannel.c`
> **最后更新**: 2025-06

---

## 1. 目标与原则

重新实现 `src/tls/schannel/` 模块，替换原有 `src/tls/wintls/`。

**设计原则**:
- 仅支持 TLS 客户端（`TLS_CLIENT`），不实现服务端
- 完整支持 TLS 1.2 / TLS 1.3（TLS 1.3 需 Windows 10 Build 20348+）
- 修复原 `wintls/` 实现中的所有已知 bug（见第 6 节）
- 非阻塞 I/O，与 aria2 事件循环无缝集成
- 对齐 curl 的成熟实现逻辑（EXTRA 处理、renegotiation、关闭流程）

---

## 2. 文件结构

```
src/tls/schannel/
├── SChannelContext.h       // TLS 上下文（凭据管理、证书存储、协议配置）
├── SChannelContext.cc
├── SChannelSession.h       // TLS 会话（握手、加解密、连接关闭）
└── SChannelSession.cc
```

### 构建集成（xmake.lua）

```lua
if is_plat("windows", "mingw") then
    add_files("src/tls/schannel/*.cc")   -- 原为 wintls/*.cc
    add_syslinks("crypt32", "secur32")
    set_configvar("SECURITY_WIN32", 1)
end
```

旧文件 `src/tls/wintls/` 保留不删除。

---

## 3. SChannelContext

### 3.1 职责

- 管理 SSPI 凭据句柄（`CredHandle`）的生命周期
- 根据 `minVer` 和 `verifyPeer` 构建凭据标志
- 导入 PKCS12 客户端证书（可选）
- TLS 1.3 运行时检测（一次性缓存）

### 3.2 成员变量

| 字段 | 类型 | 说明 |
|------|------|------|
| `side_` | `TLSSessionSide` | 固定为 `TLS_CLIENT` |
| `verifyPeer_` | `bool` | 是否验证服务器证书，默认 `true` |
| `enabledProtocols_` | `DWORD` | 已启用协议的位掩码（SP_PROT_TLS1_x_CLIENT 的组合） |
| `store_` | `HCERTSTORE` | PKCS12 证书存储，无客户端证书时为 `nullptr` |
| `credHandle_` | `CredHandle` | SSPI 凭据句柄（惰性初始化） |
| `credValid_` | `bool` | `credHandle_` 是否已有效初始化 |

### 3.3 协议版本选择

构造时依据 `minVer` 累加启用高版本（fall-through switch）：

```
TLS_PROTO_TLS11 → 启用 TLS 1.1 + TLS 1.2 + TLS 1.3（若支持）
TLS_PROTO_TLS12 → 启用 TLS 1.2 + TLS 1.3（若支持）
TLS_PROTO_TLS13 → 仅启用 TLS 1.3（若支持）
```

### 3.4 TLS 1.3 运行时检测

使用 `VerifyVersionInfoW()` 检测 Windows 10 Build 20348+（Server 2022 / Windows 11），结果静态缓存，全进程只检测一次。

### 3.5 凭据获取（惰性初始化）

`getCredHandle()` 在首次调用时创建凭据句柄：

- **支持 TLS 1.3**（Build 20348+）：使用 `SCH_CREDENTIALS`（新 API）
  - `grbitDisabledProtocols = ~enabledProtocols_`（**取反**，表示禁用的协议）
- **不支持 TLS 1.3**：使用 `SCHANNEL_CRED`（旧 API）
  - `grbitEnabledProtocols = enabledProtocols_`（**直接**传启用掩码）

`setVerifyPeer()` 或 `addCredentialFile()` 修改配置后重置 `credValid_ = false`，下次 `getCredHandle()` 时重新创建。

### 3.6 凭据标志构建

| `verifyPeer_` | 设置的标志 |
|---------------|-----------|
| `true` | `SCH_CRED_NO_DEFAULT_CREDS \| SCH_CRED_AUTO_CRED_VALIDATION \| SCH_CRED_REVOCATION_CHECK_CHAIN \| SCH_CRED_IGNORE_NO_REVOCATION_CHECK` |
| `false` | `SCH_CRED_NO_DEFAULT_CREDS \| SCH_CRED_MANUAL_CRED_VALIDATION \| SCH_CRED_IGNORE_NO_REVOCATION_CHECK \| SCH_CRED_IGNORE_REVOCATION_OFFLINE \| SCH_CRED_NO_SERVERNAME_CHECK` |

两路径均附加 `SCH_USE_STRONG_CRYPTO`。

### 3.7 PKCS12 客户端证书导入

`addCredentialFile()` 流程：
1. 读取整个文件到内存
2. `PFXIsPFXBlob()` 验证格式
3. 先以空密码（`L""`）导入，失败再以无密码（`nullptr`）导入
4. `CertEnumCertificatesInStore()` 验证至少存在一个证书
5. 替换旧 `store_`，重置 `credValid_`

### 3.8 工厂方法

```cpp
TLSContext* TLSContext::make(TLSSessionSide side, TLSVersion ver)
{
  return new SChannelContext(side, ver);
}

const char* TLSContext::name() { return "SChannel"; }
```

---

## 4. SChannelSession

### 4.1 状态机

```
STATE_INITIAL
    │ init() + handshakeStep1()
    ▼
STATE_HANDSHAKE_SEND  ◄──────────────────────────────────┐
    │ flushOutBuffer() 完成                               │
    ▼                                                    │
STATE_HANDSHAKE_RECV                                     │ ISC 返回
    │ ISC 返回 SEC_I_CONTINUE_NEEDED 且有输出消息         │ SEC_I_CONTINUE_NEEDED
    └────────────────────────────────────────────────────┘
    │ ISC 返回 SEC_E_OK
    ▼
handshakeStep3() 完成
    │
    ▼
STATE_CONNECTED
    │ closeConnection()
    ▼
STATE_SHUTTING_DOWN
    │ close_notify 发送完成
    ▼
STATE_CLOSED

任意阶段出错 → STATE_ERROR
```

**重协商路径**（`readData` 中收到 `SEC_I_RENEGOTIATE`）：
```
STATE_CONNECTED
    │ DecryptMessage 返回 SEC_I_RENEGOTIATE
    │ 设 renegotiationPending_ = true
    ▼
STATE_HANDSHAKE_RECV
    │ handshakeStep2(): encBuf_ 为空，renegotiationPending_ 放行
    │ ISC 以 0 字节输入生成 ClientHello → outBuf_
    ▼
STATE_HANDSHAKE_SEND ←→ STATE_HANDSHAKE_RECV（循环收发握手消息）
    │ ISC 返回 SEC_E_OK
    ▼
handshakeStep3() → STATE_CONNECTED
```

### 4.2 缓冲区设计

所有缓冲区使用 `IoBuffer` 结构（定义于 `SChannelSession.h`）：

```cpp
struct IoBuffer {
  size_t length;                    // 已分配容量
  size_t offset;                    // 已使用字节数（数据末尾位置）
  std::vector<unsigned char> buffer;

  // 数据存储在 [0, offset)，[offset, length) 为空闲空间
  unsigned char* data()      { return buffer.data(); }
  unsigned char* writePtr()  { return buffer.data() + offset; }
  size_t size()        const { return offset; }
  size_t writeSpace()  const { return length - offset; }
  void   clear()             { offset = 0; }
  void   ensureCapacity(size_t minSize);
  void   consume(size_t n);   // 取出头部 n 字节，剩余数据前移
  void   append(const void* src, size_t n);
};
```

| 缓冲区 | 用途 |
|--------|------|
| `encBuf_` | 从 socket 读取的密文，等待 ISC / DecryptMessage 消费 |
| `decBuf_` | DecryptMessage 解密出的明文，等待 readData 调用方取走 |
| `outBuf_` | ISC / ApplyControlToken 生成的握手消息，等待 flushOutBuffer 发送 |
| `pendingSend_` | EncryptMessage 加密后发送途中被 WOULDBLOCK 打断的剩余密文 |
| `sendRecordBuf_` | EncryptMessage 使用的连续预分配缓冲区（Header + Data + Trailer） |

### 4.3 ISC 请求标志

```cpp
const ULONG SChannelSession::kReqFlags =
    ISC_REQ_SEQUENCE_DETECT |   // 序列检测（防重放序号）
    ISC_REQ_REPLAY_DETECT   |   // 重放检测
    ISC_REQ_CONFIDENTIALITY |   // 机密性加密
    ISC_REQ_ALLOCATE_MEMORY |   // SChannel 负责分配输出缓冲区
    ISC_REQ_STREAM          |   // 流模式（TLS 记录分帧）
    ISC_REQ_USE_SUPPLIED_CREDS; // 使用调用方提供的凭据
```

`handshakeStep3` 会校验 `retFlags_` 与 `kReqFlags` 完全一致，否则返回错误。

### 4.4 关键常量

```cpp
static const int kMaxRenegotiations = 3;  // 单次 readData 调用允许的最大重协商次数
#define SCHANNEL_BUFFER_INIT_SIZE 4096    // encBuf_ 每次扩容的增量
bool renegotiationPending_;   // SEC_I_RENEGOTIATE 后需先调用 ISC 生成 ClientHello
bool handshakeFlushPending_;  // SEC_E_OK 但 flush 未完成，下次仅 flush 不重入 ISC
```

---

## 5. 核心函数详解

### 5.1 handshakeStep1() — 生成 ClientHello

1. 调用 `ctx_->getCredHandle()` 获取凭据
2. 以 `nullptr` 为输入上下文调用 `InitializeSecurityContextA()`（首次调用）
3. 期望返回 `SEC_I_CONTINUE_NEEDED`，否则报错
4. 将输出 Token 追加到 `outBuf_`，调用 `FreeContextBuffer()`
5. 进入 `STATE_HANDSHAKE_SEND`，调用 `flushOutBuffer()`
6. 返回 `TLS_ERR_WOULDBLOCK`（由调用方在事件循环中继续驱动）

### 5.2 handshakeStep2() — 循环握手

核心是一个 for(;;) 循环，对齐 curl 的 `schannel_connect_step2`：

```
loop:
  [发送阶段] state_ == STATE_HANDSHAKE_SEND:
    flushOutBuffer()
    WOULDBLOCK → 返回 WOULDBLOCK
    完成 且 handshakeFlushPending_ → 清除标志，返回 0（握手已完成）
    完成 → state_ = STATE_HANDSHAKE_RECV

  [接收阶段] state_ == STATE_HANDSHAKE_RECV:
    if encBuf_ 为空 或 needMoreData == true:
      needMoreData = false
      ensureCapacity()
      recv() → 追加到 encBuf_
      encBuf_ 仍为空:
        若 renegotiationPending_ → 清除标志，继续调用 ISC（生成重协商 ClientHello）
        否则 → 返回 WOULDBLOCK

    [调用 ISC]
    inbufs[0] = SECBUFFER_TOKEN（encBuf_ 全部数据）
    inbufs[1] = SECBUFFER_EMPTY（接收 EXTRA）
    outbufs[0..2] = TOKEN / ALERT / EMPTY

    status = InitializeSecurityContextA(cred, &ctxtHandle, host,
                kReqFlags, ..., &inbufDesc, ..., &outbufDesc, ...)

    [返回码处理顺序（对齐 curl）]

    1. SEC_E_INCOMPLETE_MESSAGE (0x80090318):
       → 释放所有 outbufs，设 needMoreData=true，continue
       → 下次循环强制 recv，避免用旧数据死循环

    2. 其他错误（不是 SEC_E_OK / SEC_I_CONTINUE_NEEDED / SEC_I_INCOMPLETE_CREDENTIALS）:
       → 必须在 EXTRA memmove 之前检查（防下溢死循环）
       → 释放 outbufs，设 STATE_ERROR，返回 TLS_ERR_ERROR

    3. 将输出 Token 追加到 outBuf_，FreeContextBuffer

    4. 处理 EXTRA（inbufs[1].BufferType == SECBUFFER_EXTRA）:
       if encBuf_.size() > EXTRA.cbBuffer:
         memmove(encBuf_ 头部 ← 末尾 EXTRA.cbBuffer 字节)
       encBuf_.offset = EXTRA.cbBuffer
       （若无 EXTRA → encBuf_.clear()）

    5. SEC_I_CONTINUE_NEEDED / SEC_I_INCOMPLETE_CREDENTIALS:
       有输出 → state_ = STATE_HANDSHAKE_SEND
       continue loop

    6. SEC_E_OK:
       有输出 → flushOutBuffer()
         WOULDBLOCK → 设 handshakeFlushPending_=true，返回 WOULDBLOCK
       返回 0（握手数据交换完成，进入 step3）
```

**`needMoreData` 标志的必要性**：
`SEC_E_INCOMPLETE_MESSAGE` 表示 SChannel 未消费任何字节（encBuf_ 数据量不足一条完整 TLS 记录）。若不设此标志，下次循环 `encBuf_.size() > 0` 会跳过 recv 直接再跑 ISC，永远得到相同结果，形成死循环。

**`renegotiationPending_` 标志的必要性（BUG-13 修复）**：
服务端发起的 TLS 1.2 重协商协议流程为：Server 发送 HelloRequest（被 `DecryptMessage` 消费，返回 `SEC_I_RENEGOTIATE`）→ Client 必须发送新的 ClientHello → Server 才会响应 ServerHello 等。`handshakeStep2` 的默认逻辑是「先 recv 再跑 ISC」，但重协商时 Server 已经在等 ClientHello，不会再发送任何数据。若 `encBuf_` 为空时直接返回 WOULDBLOCK，事件循环等待 socket 可读，而 Server 等待 Client 发送 → **双方互等，形成死锁**。设置此标志后，`handshakeStep2` 在 `encBuf_` 为空时不返回 WOULDBLOCK，而是继续调用 ISC。ISC 内部感知到安全上下文刚收到 `SEC_I_RENEGOTIATE`，即使输入 0 字节也会生成 ClientHello 并返回 `SEC_I_CONTINUE_NEEDED`。ClientHello 写入 `outBuf_`，`state_` 变为 `STATE_HANDSHAKE_SEND`，`checkDirection()` 返回 `TLS_WANT_WRITE`，事件循环改为等写就绪，ClientHello 发出后 Server 响应，握手正常推进。标志在首次 ISC 调用前用完即清，不会造成后续忙循环。

**`handshakeFlushPending_` 标志的必要性（BUG-14 修复）**：
当 ISC 返回 `SEC_E_OK`（握手完成）时，步骤 4 的 EXTRA 处理在步骤 6 之前执行（代码结构），因此 `encBuf_` 中可能保存了 EXTRA 数据（此时为已加密的应用层数据，而非握手数据）。若步骤 6 的 `flushOutBuffer()` 因发送缓冲区满返回 WOULDBLOCK，函数设 `state_ = STATE_HANDSHAKE_SEND` 后返回。下次重入时 flush 成功后 `state_` 变为 `STATE_HANDSHAKE_RECV`，`encBuf_` 非空导致 ISC 被调用——但输入是应用层密文而非握手数据，ISC 无法解析，连接失败。设置此标志后，重入时 flush 完成后直接返回 0（握手已完成），不再进入 ISC 循环，`encBuf_` 中的 EXTRA 留给 `readData` 的解密循环正确处理。

### 5.3 handshakeStep3() — 查询连接参数

1. **校验 ISC 返回标志**：`retFlags_` 必须与 `kReqFlags` 完全一致，逐位检查并报告缺失项
2. **查询流大小**：`QueryContextAttributes(SECPKG_ATTR_STREAM_SIZES, &streamSizes_)`
3. **预分配发送缓冲区**：`sendRecordBuf_.resize(cbHeader + cbMaximumMessage + cbTrailer)`
4. **查询协议版本**：优先用 `SECPKG_ATTR_CONNECTION_INFO`（兼容性更好），失败时回退到 `SECPKG_ATTR_CIPHER_INFO` 的 `dwProtocol` 字段
5. **日志策略**：
   - 首次握手（`firstConnectDone_ == false`）：打 `INFO "SChannel: TLS 连接建立, 套件=XXX"`
   - renegotiate / Key Update 重调：打 `DEBUG "SChannel: TLS Key Update 完成, 套件=XXX"`
6. `state_ = STATE_CONNECTED`，`firstConnectDone_ = true`

### 5.4 writeData() — 加密发送

```
1. 若 state_ 为 HANDSHAKE_SEND/RECV：先完成握手（重协商支持）
2. 若有 pendingSend_ 未发完：先 flushPendingSend()
3. len = min(len, streamSizes_.cbMaximumMessage)（不超过单条记录上限）
4. 在 sendRecordBuf_ 中构建 TLS 记录：
   [cbHeader 字节 Header | len 字节明文 | cbTrailer 字节 Trailer]
5. memcpy 明文到 DATA 区域
6. EncryptMessage(&ctxtHandle_, 0, &outbufDesc, 0)
7. 发送循环（totalLen = sum of 3 buffers）：
   - WSAEINTR → continue
   - WSAEWOULDBLOCK → 将剩余密文存入 pendingSend_，返回 len（明文已处理）
   - 其他错误 → STATE_ERROR，返回 TLS_ERR_ERROR
8. 返回 len
```

**设计要点**：加密后的 TLS 记录不可分割丢弃。WOULDBLOCK 时缓存剩余密文到 `pendingSend_`，下次 `writeData` 或 `readData` 入口处优先发送。

### 5.5 readData() — 解密接收

```
1. len == 0 → 返回 0

2. decBuf_ 已有 >= len 字节 → 直接拷贝返回（快路径）

3. state_ 为 CLOSED/ERROR/SHUTTING_DOWN → 返回 decBuf_ 剩余或 EOF/错误

4. state_ 为 HANDSHAKE_SEND/RECV → 先完成握手（重协商支持）

5. state_ != CONNECTED → TLS_ERR_ERROR

6. renegotiateCount_ = 0（每次 readData 重置计数器）

7. 若有 pendingSend_ → flushPendingSend()（即使 WOULDBLOCK 也继续）

8. 若 !recvConnectionClosed_：
   ensureCapacity(encBuf_.offset + len + 1024)
   recv() → 追加到 encBuf_（WOULDBLOCK 不报错，用已有数据解密）
   recv() == 0 → recvConnectionClosed_ = true

9. 解密循环（while encBuf_.size() > 0）：
   DecryptMessage(&ctxtHandle_, &inbufDesc, 0, nullptr)

   SEC_E_OK：
     提取 inbufs[1] SECBUFFER_DATA → 追加到 decBuf_
     处理 inbufs[3] SECBUFFER_EXTRA（同 handshakeStep2 的 memmove 逻辑）
     continue（继续解密下一条记录）

   SEC_E_INCOMPLETE_MESSAGE：
     break（等待更多密文）

   SEC_I_CONTEXT_EXPIRED：
     提取最后解密数据（若有）
     recvCloseNotify_ = true，recvConnectionClosed_ = true
     记录 DEBUG "SChannel: 收到服务器 close_notify"
     break

   SEC_I_RENEGOTIATE：
     提取已解密数据（若有）
     保留 EXTRA 到 encBuf_（供 handshakeStep2 消费）
     renegotiateCount_++ 超限 → 错误
     renegotiationPending_ = true（让 handshakeStep2 先调用 ISC 生成 ClientHello）
     state_ = STATE_HANDSHAKE_RECV
     handshakeStep2() → WOULDBLOCK 返回给调用方，错误返回错误
     handshakeStep3() → 若失败返回错误
     （step3 内部已设 state_ = STATE_CONNECTED）
     continue（继续解密循环，encBuf_ 可能还有应用数据）

   其他 → 记录 ERROR，STATE_ERROR，返回 TLS_ERR_ERROR

10. 从 decBuf_ 返回数据给调用方

11. 没有数据：
    recvConnectionClosed_ && !recvCloseNotify_ → WARN 截断攻击提示
    recvConnectionClosed_ → 返回 0（EOF）
    否则 → 返回 TLS_ERR_WOULDBLOCK
```

### 5.6 closeConnection() — 关闭 TLS 连接

```
守卫：state_ 不是 CONNECTED 且不是 SHUTTING_DOWN → 返回 TLS_ERR_ERROR

if !sentShutdown_ && ctxtValid_：
  记录 DEBUG "SChannel: 发送 close_notify"
  state_ = STATE_SHUTTING_DOWN

  步骤 1: ApplyControlToken(SCHANNEL_SHUTDOWN)
          失败 → STATE_ERROR，返回 TLS_ERR_ERROR

  步骤 2: InitializeSecurityContextA（无输入，生成 close_notify 报文）
          接受 SEC_E_OK 或 SEC_I_CONTEXT_EXPIRED

  步骤 3: 若有输出 Token → send()（尽力发送，忽略失败，参考 curl）

  总是 FreeContextBuffer(outbuf.pvBuffer)
  sentShutdown_ = true

state_ = STATE_CLOSED
记录 DEBUG "SChannel: TLS 连接已关闭"
返回 TLS_ERR_OK
```

**关于重复日志**：日志中出现两次"发送 close_notify / TLS 连接已关闭"是**正常现象**，表示两条独立的 HTTPS 连接恰好在同一秒关闭（aria2 并发多连接）。`sentShutdown_` + `state_ = STATE_CLOSED` 两重保护保证同一 `SChannelSession` 实例不会重复执行关闭逻辑。

### 5.7 checkDirection()

| `state_` | 返回值 |
|----------|--------|
| `STATE_HANDSHAKE_SEND` | `TLS_WANT_WRITE` |
| `STATE_SHUTTING_DOWN` | `TLS_WANT_WRITE` |
| `STATE_HANDSHAKE_RECV` | `TLS_WANT_READ` |
| `STATE_CONNECTED` 且有 `pendingSend_` 或 `outBuf_` | `TLS_WANT_WRITE` |
| `STATE_CONNECTED` 其他 | `TLS_WANT_READ` |
| 其他（INITIAL/CLOSED/ERROR） | `TLS_WANT_READ` |

### 5.8 底层 I/O 辅助

**`flushOutBuffer()`**：循环 `send(outBuf_)`，`WSAEINTR` 重试，`WSAEWOULDBLOCK` 返回 `TLS_ERR_WOULDBLOCK`，其他错误设 `STATE_ERROR`。

**`flushPendingSend()`**：循环 `send(pendingSend_ + pendingSendOffset_)`，语义同上。发送完毕后 `pendingSend_.clear()`，`pendingSendOffset_ = 0`。

---

## 6. 修复的原 wintls/ Bug

| Bug | 现象 | 修复 |
|-----|------|------|
| BUG-1 | `setVerifyPeer(false)` 后 `credentialsFlags_` 未更新，验证仍然开启 | `buildCredFlags()` 在每次 `getCredHandle()` 时动态构建，`setVerifyPeer` 重置 `credValid_` |
| BUG-2 | `status_` 既存状态码又存枚举值，类型混淆 | 分为 `state_`（枚举）和 `lastStatus_`（`SECURITY_STATUS`） |
| BUG-3 | `addCredentialFile()` 判断 `store_`（成员）而非 `store`（局部变量），永远为 true | 改为检查局部变量 `store` |
| BUG-4 | `recv()` 结果 `read <= 0` 优先于 `read == 0` 处理，错误码被覆盖 | 分别处理 `== 0` 和 `< 0` |
| BUG-5 | `getCredHandle()` 未将 `credentialsFlags_` 传给 `AcquireCredentialsHandle` | `buildCredFlags()` 结果直接写入 `dwFlags` |
| BUG-6 | 有符号/无符号比较产生编译警告和潜在错误 | 统一类型 |
| BUG-7 | `isTLS13Supported()` 硬编码返回 `false` | `VerifyVersionInfoW()` 运行时检测 Build 20348+ |
| BUG-8 | `recv()` 返回 0 后没有标记，下次调用继续 recv 可能忙循环 | `recvConnectionClosed_` 标志，此后跳过 recv |
| BUG-9 | `closeConnection()` 某些路径泄漏 `ctx.pvBuffer` | 所有路径统一 `FreeContextBuffer()` |
| BUG-10 | 初始化列表顺序与声明顺序不一致（UB 风险） | 对齐 |
| BUG-11（新发现）| `grbitDisabledProtocols` 未取反，实际禁用了想启用的协议 | `~enabledProtocols_` |
| BUG-12（新发现）| ISC 返回 `SEC_E_INCOMPLETE_MESSAGE` 时先做 EXTRA memmove，cbBuffer 值无效导致下溢，encBuf_ 不变形成死循环（463711 行日志） | 错误检查移到 EXTRA 处理之前；对 `SEC_E_INCOMPLETE_MESSAGE` 单独处理并设 `needMoreData=true` |
| BUG-13（新发现）| 服务端发起 TLS 1.2 重协商时，`readData` 设 `state_=HANDSHAKE_RECV` 后调用 `handshakeStep2`，但 step2 先 recv 再跑 ISC——Server 在等 ClientHello 不会发数据，recv 永远 WOULDBLOCK，`checkDirection` 返回 `TLS_WANT_READ`，双方互等形成死锁 | `renegotiationPending_` 标志让 step2 在 `encBuf_` 为空时仍调用 ISC 生成 ClientHello |
| BUG-14（新发现）| ISC 返回 `SEC_E_OK` 且有 EXTRA 时，若 `flushOutBuffer` 返回 WOULDBLOCK，重入 step2 后 `encBuf_` 中的应用层密文被当作握手数据送给 ISC，导致连接失败 | `handshakeFlushPending_` 标志让重入时 flush 完成后直接返回 0，不重入 ISC 循环 |

---

## 7. 与 curl 实现的对比

| 方面 | curl schannel.c | 本实现 |
|------|-----------------|--------|
| 凭据生命周期 | refcount + session cache（跨连接复用） | `SChannelContext` 单例管理，同一下载任务复用 |
| 握手状态机 | 同步循环，CF（connection filter）层驱动 | 非阻塞，`state_` 驱动，事件循环多次调用 `tlsConnect` |
| 加密发送 | 每次 `malloc/free` | 预分配 `sendRecordBuf_`，避免碎片 |
| EXTRA 缓冲区处理 | 同逻辑：memmove 至头部，更新 offset | 完全对齐 |
| `SEC_E_INCOMPLETE_MESSAGE` | 标记 `doread=TRUE`，下次循环强制 recv | `needMoreData=true`，语义一致 |
| 重协商启动 | `io_need=SEND` + `doread=FALSE` 跳过 recv 直接跑 ISC | `renegotiationPending_` 标志让 step2 在 `encBuf_` 为空时仍调用 ISC |
| SEC_E_OK + flush 中断 | `break` 跳出循环，EXTRA 留给 `schannel_recv` 解密 | `handshakeFlushPending_` 标志阻止重入 ISC |
| renegotiation 限制 | `MAX_RENEG_BLOCK_TIME`（时间限制） | `kMaxRenegotiations = 3`（次数限制） |
| 关闭流程 | `ApplyControlToken` + ISC + send，尽力发送 | 完全对齐 |
| ALPN | 支持（`SCH_CRED_ALPN`） | 未实现（`TLSSession` 基类无此接口） |
| 服务端 TLS | 不支持（SChannel 可以，但 curl 只做客户端） | 不支持（`tlsAccept` 直接返回错误） |
| 密码套件 | 可自定义列表 | 使用系统默认（`SCH_USE_STRONG_CRYPTO`） |
| 证书验证 | 手动验证 + 公钥固定 | SChannel 自动验证（系统证书存储） |

---

## 8. 日志策略

| 级别 | 场景 |
|------|------|
| `INFO` | 首次 TLS 握手完成（`SChannel: TLS 连接建立, 套件=XXX`） |
| `DEBUG` | renegotiate / Key Update 后重握手完成；收到 close_notify；发送 close_notify；连接已关闭；对端关闭连接 |
| `WARN` | 对端关闭 TCP 但未发 close_notify（可能的截断攻击） |
| `ERROR` | 所有 SSPI API 失败；协议协商失败；ISC 返回标志不满足；renegotiation 超限 |

**重要**：相同秒内出现两次"发送 close_notify"+"TLS 连接已关闭"属正常现象（两条并发连接同时关闭），不是 bug。

---

## 9. 旧 SDK 兼容宏

定义于 `SChannelSession.cc` 顶部：

```cpp
#ifndef SP_PROT_TLS1_1_CLIENT
#  define SP_PROT_TLS1_1_CLIENT 0x00000200
#endif
#ifndef SP_PROT_TLS1_2_CLIENT
#  define SP_PROT_TLS1_2_CLIENT 0x00000800
#endif
#ifndef SP_PROT_TLS1_3_CLIENT
#  define SP_PROT_TLS1_3_CLIENT 0x00002000
#endif
#ifndef SECBUFFER_ALERT
#  define SECBUFFER_ALERT 17
#endif
#ifndef SZ_ALG_MAX_SIZE
#  define SZ_ALG_MAX_SIZE 64
#endif
#ifndef SECPKGCONTEXT_CIPHERINFO_V1
#  define SECPKGCONTEXT_CIPHERINFO_V1 1
#endif
#ifndef SECPKG_ATTR_CIPHER_INFO
#  define SECPKG_ATTR_CIPHER_INFO 0x64
#endif
```
