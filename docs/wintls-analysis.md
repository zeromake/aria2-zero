# WinTLS 实现分析与 Bug 报告

## 1. 文件概览

| 文件 | 行数 | 职责 |
|------|------|------|
| `WinTLSContext.h` | 103 | TLS 上下文定义：凭据管理、证书存储、协议版本 |
| `WinTLSContext.cc` | 281 | TLS 上下文实现：凭据获取、PKCS12 导入、验证配置 |
| `WinTLSSession.h` | 235 | TLS 会话定义：状态机、缓冲区、收发接口 |
| `WinTLSSession.cc` | 866 | TLS 会话实现：握手、加解密、连接关闭 |

## 2. 实现逻辑

### 2.1 WinTLSContext — TLS 上下文

**职责**: 管理 SChannel 凭据 (CredHandle) 和证书存储 (HCERTSTORE)。

**协议版本选择** (构造函数, 第 71-107 行):
使用 fall-through switch 累积启用的协议版本：
```
TLS_PROTO_TLS11 → 启用 TLS 1.1 + TLS 1.2 + TLS 1.3 (如果支持)
TLS_PROTO_TLS12 → 启用 TLS 1.2 + TLS 1.3 (如果支持)
TLS_PROTO_TLS13 → 仅启用 TLS 1.3 (如果支持)
```
`ver` 参数表示**最低**版本，fall-through 实现"最低版本及以上"的语义。

**凭据获取** (`getCredHandle()`, 第 165-234 行):
- 惰性初始化：首次调用时创建 CredHandle
- TLS 1.3 路径使用 `SCH_CREDENTIALS` 结构体 + `TLS_PARAMETERS` 指定禁用协议
- 旧版路径使用 `SCHANNEL_CRED` 结构体 + `grbitEnabledProtocols`
- 调用 `AcquireCredentialsHandleW()` 获取 SChannel 凭据

**证书导入** (`addCredentialFile()`, 第 236-272 行):
- 读取 PKCS12 (.pfx) 文件
- 尝试空密码 → 尝试无密码 → 导入证书存储
- 导入成功后替换旧 store_ 并重置凭据缓存

**验证配置** (`setVerifyPeer()`, 第 134-163 行):
- 客户端 + verify: 启用 `SCH_CRED_AUTO_CRED_VALIDATION` + 吊销检查
- 其他情况: 手动验证 + 忽略吊销 + 忽略服务器名检查

### 2.2 WinTLSSession — TLS 会话

**状态机**:
```
st_constructed → st_initialized → st_handshake_write → st_handshake_read
                                        ↓                    ↓
                               st_handshake_write_last ←─────┘
                                        ↓
                               st_handshake_done → st_connected
                                                        ↓
                                                  st_closing → st_closed
                                                        ↓
                                                    st_error
```

**握手流程** (`tlsConnect()`, 第 577-815 行):

客户端:
1. `st_initialized`: 调用 `InitializeSecurityContextA()` 生成 ClientHello → 写入 `writeBuf_`
2. `st_handshake_write`: 通过 `send()` 发送 `writeBuf_` 中的数据
3. `st_handshake_read`: 通过 `recv()` 读取服务器响应 → 调用 `InitializeSecurityContextA()` 处理
4. 如果 `SEC_I_CONTINUE_NEEDED`: 队列新消息 → 回到 step 2 (通过 `goto restart`)
5. 如果 `SEC_E_OK`: 进入 `st_handshake_done`

服务端:
1. 直接跳到 `read` 标签，等待客户端 ClientHello
2. 使用 `AcceptSecurityContext()` 处理
3. 后续流程类似客户端

**数据加密发送** (`writeData()`, 第 317-431 行):
1. 先发送上一次遗留的 TLS 记录 (`sendTLSRecord()`)
2. 处理 `writeBuffered_` (上次未完全发送的明文偏移)
3. 循环: 取最大 `cbMaximumMessage` 字节明文 → `EncryptMessage()` 加密 → `sendTLSRecord()` 发送
4. TLS 记录格式: [Header | Data | Trailer | Empty]

**数据解密接收** (`readData()`, 第 433-575 行):
1. 优先从 `decBuf_` (已解密缓冲) 返回数据
2. 从 socket `recv()` 读取密文到 `readBuf_`
3. 循环调用 `DecryptMessage()` 解密:
   - `SECBUFFER_DATA` → 写入 `decBuf_`
   - `SECBUFFER_EXTRA` → 保留未消费的密文
   - `SEC_I_RENEGOTIATE` → 重新握手
   - `SEC_I_CONTEXT_EXPIRED` → 优雅关闭
4. 从 `decBuf_` 复制到用户缓冲区

**连接关闭** (`closeConnection()`, 第 169-228 行):
1. 发送 `SCHANNEL_SHUTDOWN` 控制令牌
2. 调用 `InitializeSecurityContextA()`/`AcceptSecurityContext()` 生成关闭消息
3. 通过 `writeData()` 发送关闭消息
4. 设置状态为 `st_closed`

**TLS 记录发送** (`sendTLSRecord()`, 第 277-315 行):
- 使用 `WSASend()` + scatter-gather I/O 发送 [Header + Data + Trailer]
- 跟踪 `recordBytesSent_` 支持部分发送

**缓冲区管理** (`wintls::Buffer`):
- 简单的前置写入、前端消费缓冲区
- `write()` 追加数据到尾部
- `eat()` 从头部消费并 `memmove` 剩余数据
- `resize()` 仅增长不缩小

---

## 3. Bug 报告

### BUG-1 [严重] `setVerifyPeer()` 禁用验证时未赋值 `credentialsFlags_`

**文件**: `WinTLSContext.cc` 第 150-156 行

```cpp
if (side_ != TLS_CLIENT || !verify) {
    dwFlags |=
        SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_IGNORE_NO_REVOCATION_CHECK |
        SCH_CRED_IGNORE_REVOCATION_OFFLINE | SCH_CRED_NO_SERVERNAME_CHECK;
    return;  // ← 直接返回，未将 dwFlags 赋值给 credentialsFlags_
}
```

**问题**: 当 `verify=false` 或 `side_==TLS_SERVER` 时，函数在 `return` 之前没有执行 `credentialsFlags_ = dwFlags`。这意味着：
- `credentialsFlags_` 保留旧值（或初始化的 0）
- `getVerifyPeer()` 的返回值可能不正确：它检查 `credentialsFlags_ & SCH_CRED_AUTO_CRED_VALIDATION`，如果之前设置过 verify=true 再设置 verify=false，`getVerifyPeer()` 仍然返回 true
- **更关键的是**: `credentialsFlags_` 变量在 `getCredHandle()` 中**根本没有被使用**。`getCredHandle()` 中 TLS 1.3 路径硬编码了 `SCH_USE_STRONG_CRYPTO`，旧版路径没有设置任何 flags。`setVerifyPeer()` 中精心设置的标志实际上**完全无效**

**影响**: `setVerifyPeer(true)` 配置的 `SCH_CRED_AUTO_CRED_VALIDATION` 等标志**从未传递给 `AcquireCredentialsHandle()`**，证书验证行为完全取决于 SChannel 的默认行为，而非用户配置。

**修复建议**: 在 `getCredHandle()` 中将 `credentialsFlags_` 合并到 `credentials_.dwFlags` / `credentials13_.dwFlags`：
```cpp
// 旧版路径
credentials_.dwFlags = credentialsFlags_;
// TLS 1.3 路径
credentials13_.dwFlags = credentialsFlags_ | SCH_USE_STRONG_CRYPTO;
```
同时修复 `return` 前遗漏的赋值：
```cpp
if (side_ != TLS_CLIENT || !verify) {
    dwFlags |= ...;
    credentialsFlags_ = dwFlags;  // ← 添加
    return;
}
```

---

### BUG-2 [严重] `writeData()` 中将 `state_` 赋值给 `status_` (类型混淆)

**文件**: `WinTLSSession.cc` 第 351-352 行

```cpp
if (len < writeBuffered_) {
    status_ = SEC_E_INVALID_HANDLE;
    status_ = st_error;         // ← 应该是 state_ = st_error
    return TLS_ERR_ERROR;
}
```

**问题**: `status_` 是 `SECURITY_STATUS` (HRESULT) 类型，`st_error` 是枚举值 `state_t`。这里把 `st_error` (整数值 9) 赋给了 `status_`，同时覆盖了前一行设置的 `SEC_E_INVALID_HANDLE`。正确的写法应该是 `state_ = st_error`。

**影响**:
- `state_` 不会被设置为 `st_error`，后续操作不知道会话已进入错误状态
- `status_` 被设置为错误的值 (9)，`getLastErrorString()` 会返回无意义的错误信息
- 虽然此路径返回了 `TLS_ERR_ERROR`，但会话对象状态不正确，后续重试可能产生未定义行为

---

### BUG-3 [中等] `addCredentialFile()` 密码重试逻辑检查了错误的变量

**文件**: `WinTLSContext.cc` 第 247-252 行

```cpp
HCERTSTORE store =
    ::PFXImportCertStore(&blob, L"", CRYPT_EXPORTABLE | CRYPT_USER_KEYSET);
if (!store_) {  // ← 检查了成员变量 store_，而非局部变量 store
    store = ::PFXImportCertStore(&blob, nullptr,
                                 CRYPT_EXPORTABLE | CRYPT_USER_KEYSET);
}
```

**问题**: 空密码导入失败时的重试逻辑检查的是 `store_`（成员变量，旧的证书存储）而非 `store`（局部变量，本次导入结果）。

**影响**:
- 如果 `store_` 已有值（之前导入过证书），即使空密码导入失败 (`store == NULL`)，也不会尝试 nullptr 密码重试，直接进入 `if (!store)` 检查并返回失败
- 如果 `store_` 为空（首次导入），即使空密码导入成功，也会多余地再次调用 `PFXImportCertStore`，**覆盖已成功的 `store`**。如果 nullptr 密码导入也成功，不会有问题；如果 nullptr 密码导入失败，会把 `store` 覆盖为 NULL，导致后续检查失败

**修复**: 将 `if (!store_)` 改为 `if (!store)`。

---

### BUG-4 [中等] 握手 `recv()` 中 `read == 0` 的死代码

**文件**: `WinTLSSession.cc` 第 682-692 行

```cpp
if (read <= 0) {        // ← read == 0 在这里被捕获
    status_ = errno;
    state_ = st_error;
    return TLS_ERR_ERROR;
}
if (read == 0) {        // ← 永远不会执行到这里
    A2_LOG_DEBUG("WinTLS: Connection abruptly closed during handshake!");
    status_ = SEC_E_INCOMPLETE_MESSAGE;
    state_ = st_error;
    return TLS_ERR_ERROR;
}
```

**问题**: `read == 0` (对端关闭连接) 已被前面的 `read <= 0` 捕获。后面的 `read == 0` 分支是死代码，永远不会执行。

**影响**:
- 连接关闭时，`status_` 被设置为 `errno` (即 `WSAGetLastError()` 的值，此时可能是 0 或上一次 API 调用的残留值)，而非正确的 `SEC_E_INCOMPLETE_MESSAGE`
- 不会输出 "Connection abruptly closed during handshake!" 日志，调试困难

**修复**: 将 `read == 0` 检查移到 `read <= 0` 之前：
```cpp
if (read == 0) {
    A2_LOG_DEBUG("WinTLS: Connection abruptly closed during handshake!");
    status_ = SEC_E_INCOMPLETE_MESSAGE;
    state_ = st_error;
    return TLS_ERR_ERROR;
}
if (read < 0) {
    status_ = errno;
    state_ = st_error;
    return TLS_ERR_ERROR;
}
```

---

### BUG-5 [中等] `getCredHandle()` 未使用 `credentialsFlags_` (与 BUG-1 关联)

**文件**: `WinTLSContext.cc` 第 165-234 行

`getCredHandle()` 中构建的凭据结构体没有引用 `credentialsFlags_`：

```cpp
// TLS 1.3 路径 (第 187 行):
credentials13_.dwFlags = SCH_USE_STRONG_CRYPTO;  // 硬编码，未合并 credentialsFlags_

// 旧版路径:
// 没有设置 credentials_.dwFlags，即 dwFlags = 0 (memset 的结果)
```

**问题**: `setVerifyPeer()` 设置的所有标志（`SCH_CRED_AUTO_CRED_VALIDATION`、`SCH_CRED_MANUAL_CRED_VALIDATION`、`SCH_CRED_NO_SERVERNAME_CHECK` 等）都不会生效。SChannel 使用默认行为。

**影响**: 证书验证配置实际上不起作用。对于客户端连接，SChannel 默认会执行某种程度的验证（取决于 Windows 版本），但行为不受 aria2 的 `--check-certificate` 选项控制。

---

### BUG-6 [低] `closeConnection()` 中有符号/无符号比较

**文件**: `WinTLSSession.cc` 第 209-220 行

```cpp
size_t len = ctx.cbBuffer;           // unsigned
ssize_t rv = writeData(...);         // signed (可能为负数错误码)
...
if (rv > 0 && rv - len != 0) {       // signed - unsigned → 隐式转换
```

**问题**: `rv` (ssize_t) 和 `len` (size_t) 相减时，`rv` 会被隐式转换为 `size_t`。虽然此处 `rv > 0` 确保了 `rv` 为正数，转换安全，但代码意图不清晰。`rv - len != 0` 等价于 `rv != len`（在 rv > 0 时），语义是"如果未完全写入则返回 WOULDBLOCK"。

**建议**: 改为 `static_cast<size_t>(rv) != len` 使意图更明确。

---

### BUG-7 [低] `isTLS13Supported()` 硬编码返回 `false`

**文件**: `WinTLSContext.cc` 第 109-112 行

```cpp
bool WinTLSContext::isTLS13Supported()
{
  return false;
}
```

**问题**: TLS 1.3 支持检测始终返回 false。Windows 10 20H1 (Build 19041) 及以上版本的 SChannel 已支持 TLS 1.3。项目中大量 TLS 1.3 相关代码（`SCH_CREDENTIALS` 路径、`SP_PROT_TLS1_3_CLIENT/SERVER`）永远不会被执行。

**影响**: 即使运行在支持 TLS 1.3 的 Windows 版本上，WinTLS 后端也只能使用 TLS 1.2。这可能是有意为之（SChannel 的 TLS 1.3 支持可能不够稳定），但如果是临时禁用则应有 TODO 标记。

**补充**: 如果确实要启用，可以通过运行时 Windows 版本检测实现：
```cpp
bool WinTLSContext::isTLS13Supported()
{
    // Windows 10 20H1 (Build 19041) 开始支持 TLS 1.3
    return IsWindowsVersionOrGreater(10, 0, 19041);
}
```

---

### BUG-8 [低] `readData()` 中 `recv()` 返回 0 时未关闭连接 (设计权衡)

**文件**: `WinTLSSession.cc` 第 496-503 行

```cpp
if (read == 0) {
    // closeConnection 的话 DecryptMessage 就会无法解析，所以这里不关闭连接，等 DecryptMessage 后再关闭
    A2_LOG_DEBUG(fmt("WinTLS: recv break buffered: %" PRIu64, ...));
    break;
}
```

**情况**: 当 `recv()` 返回 0（对端关闭）时，代码选择不关闭连接而是 break 出循环，让 `DecryptMessage` 处理剩余缓冲数据。

**潜在问题**: 如果 `readBuf_` 中没有完整的 TLS 记录，`DecryptMessage` 会返回 `SEC_E_INCOMPLETE_MESSAGE`，代码 break 出解密循环。然后 `decBuf_` 为空，`len` 为 0，函数返回 `TLS_ERR_WOULDBLOCK`。调用方会再次调用 `readData()`，`recv()` 再次返回 0，形成无限循环（虽然主循环的事件驱动机制通常会打断这个循环，因为 socket 不再有读事件）。

**建议**: 在 `recv()` 返回 0 后设置一个标志，下次进入时如果 `readBuf_` 仍然无法解密完整记录则进入关闭流程。

---

### BUG-9 [低] `closeConnection()` 在 `writeData` 失败时可能泄漏 `ctx.pvBuffer`

**文件**: `WinTLSSession.cc` 第 207-222 行

```cpp
if ((status_ == SEC_E_OK || status_ == SEC_I_CONTEXT_EXPIRED) &&
    getLeftTLSRecordSize() == 0) {
    size_t len = ctx.cbBuffer;
    ssize_t rv = writeData(ctx.pvBuffer, ctx.cbBuffer);
    ::FreeContextBuffer(ctx.pvBuffer);
```

**情况**: 如果 `status_` 不满足条件（既不是 `SEC_E_OK` 也不是 `SEC_I_CONTEXT_EXPIRED`），或者 `getLeftTLSRecordSize() != 0`，整个 if 块不执行，`ctx.pvBuffer` **不会被释放**。

**影响**: 如果 `InitializeSecurityContextA()`/`AcceptSecurityContext()` 成功分配了 `ctx.pvBuffer` 但返回了其他成功状态码，会有内存泄漏。实际上这种情况很少发生，因为关闭握手通常返回 `SEC_E_OK`，但 `getLeftTLSRecordSize() != 0` 时泄漏是确定的。

---

### BUG-10 [低] 初始化列表顺序与声明顺序不一致

**文件**: `WinTLSContext.cc` 第 72 行

```cpp
WinTLSContext::WinTLSContext(TLSSessionSide side, TLSVersion ver)
    : side_(side), store_(0), credentialsFlags_(0), enabled_protocols_(0)
```

**文件**: `WinTLSContext.h` 第 94-98 行 (声明顺序)

```cpp
TLSSessionSide side_;
DWORD credentialsFlags_;
int enabled_protocols_;
HCERTSTORE store_;
wintls::CredPtr cred_;
```

**问题**: 初始化列表顺序 (`side_, store_, credentialsFlags_, enabled_protocols_`) 与成员声明顺序 (`side_, credentialsFlags_, enabled_protocols_, store_`) 不一致。C++ 按声明顺序初始化成员，而非初始化列表顺序。这里所有值都是常量初始化所以不会导致问题，但部分编译器会发出警告 (`-Wreorder`)。

---

## 4. 严重程度总结

| Bug | 严重程度 | 类型 | 影响 |
|-----|---------|------|------|
| BUG-1 | **严重** | 逻辑错误 | 证书验证配置完全无效 |
| BUG-2 | **严重** | 类型混淆 | 错误状态未正确设置，status_ 值被覆盖 |
| BUG-3 | **中等** | 变量名错误 | PKCS12 密码重试逻辑行为错误 |
| BUG-4 | **中等** | 死代码 | 握手期间连接关闭的错误处理不正确 |
| BUG-5 | **中等** | 逻辑遗漏 | `credentialsFlags_` 未传递到凭据创建 (BUG-1 关联) |
| BUG-6 | **低** | 类型安全 | 有符号/无符号比较，当前安全但不清晰 |
| BUG-7 | **低** | 功能缺失 | TLS 1.3 支持被硬编码禁用 |
| BUG-8 | **低** | 设计权衡 | recv() 返回 0 后可能导致忙循环 |
| BUG-9 | **低** | 资源泄漏 | 关闭流程中可能泄漏 SChannel 缓冲区 |
| BUG-10 | **低** | 代码规范 | 初始化列表顺序不一致 |
