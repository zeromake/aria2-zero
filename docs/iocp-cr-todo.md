# IOCP EventPoll CR 待办

三轮对抗 CR 中标记为"不修"的发现，按优先级排列。

## TODO-1: fd_set 栈溢出 ~512KB

**严重性**: CR-A Major / CR-B Critical / CR-C Minor

**问题**: `pollConnectingSockets()` 栈上声明两个 `fd_set`，`FD_SETSIZE=32768` 时各约 256KB，共 ~512KB，接近 Windows 默认 1MB 栈限制。

**修复方案**: 改为类成员变量 `wfds_`/`efds_`（与 `SelectEventPoll` 的 `rfdset_`/`wfdset_` 模式一致），避免栈分配。

**状态**: 已修复 ✅

---

## TODO-2: CancelIoEx + pending 标志立即重置（OVERLAPPED 并发引用）

**严重性**: CR-A Major / CR-B Major / CR-C Major

**问题**: `addEvents` 对延迟清理条目调用 `CancelIoEx` 后立即重置 `readPending`/`writePending` 并 rearm，理论上旧操作的取消完成尚未到达时新操作与旧操作并发引用同一 OVERLAPPED 结构。

**修复方案**: `addEvents` 不再重置 pending 标志也不再调用 `CancelIoEx`；`poll()` 的 `STATUS_CANCELLED` 处理中追加 `rearmEvents` 调用，让取消完成自然到达后重新提交操作。`postZeroByteRecv`/`postZeroByteSend` 的 `if (pending) return` 守卫自动跳过已挂起方向。

**状态**: 已修复 ✅

---

## TODO-3: 监听 socket 无 IOCP 通知（RPC 延迟）

**严重性**: CR-B Major

**问题**: `WSARecv` 在监听 socket 上立即失败（`WSAENOTCONN`/`WSAEINVAL`），无法获得 IOCP 就绪通知。其它 EventPoll 实现（epoll/kqueue/select）均能原生通知监听 socket，IOCP 是唯一例外。虽然当前监听 socket 均为 routineCommand 不受影响，但若未来有非 routine 的 Command 使用监听 socket 会出问题。

**修复方案**: 将 `connectingSockets_`（仅写方向）泛化为 `fallbackSockets_`（`map<sock_t, int>` 方向掩码），同时支持读/写回退。`postZeroByteRecv` 失败时按错误码加入读回退（`WSAENOTCONN`/`WSAEINVAL`），`postZeroByteSend` 失败时加入写回退。`pollFallbackSockets()` 使用 `rfds_`/`wfds_`/`efds_` 三个 fd_set，按方向分别检测。零字节操作成功挂起时自动移除对应方向的回退标记。

**状态**: 已修复 ✅

---

## TODO-4: KSocketEntry move 构造的潜在风险

**严重性**: CR-C Critical→Latent

**问题**: 默认 move 构造会 memcpy `OverlappedEntry`，若在有挂起操作时移动，内核持有旧地址指针。

**修复方案**: 删除 move 构造函数（及 copy 赋值运算符），改用 `emplace_hint` + `piecewise_construct` 原地构造，避免 move。

**状态**: 已修复 ✅

---

## TODO-5: accumulateEvent 非匿名命名空间

**严重性**: CR-A Nit / CR-B Nit

**问题**: 自由函数 `accumulateEvent` 未放入匿名命名空间，与 `EpollEventPoll.cc` 同名函数共存。

**状态**: 放弃修复 ❌

**放弃原因**: `accumulateEvent` 被 `IocpEventPoll.h` 中的 `friend int accumulateEvent(...)` 声明引用。匿名命名空间中的函数无法被外部 `friend` 声明匹配（它们是不同实体）。改为 `static` 同理不兼容 `friend`。改为 `private static` 成员函数可行但偏离 `EpollEventPoll` 的既有模式（同样使用 `friend` free function 且未放入匿名命名空间）。两者在平台互斥的编译单元中，函数签名不同（参数类型分别为 `EpollEventPoll::KEvent&` 和 `IocpEventPoll::KEvent&`），不构成 ODR 违规。
