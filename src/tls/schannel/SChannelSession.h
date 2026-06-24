/* <!-- copyright */
/*
 * aria2 - The high speed download utility
 *
 * Copyright (C) 2013 Nils Maier
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of portions of this program with the
 * OpenSSL library under certain conditions as described in each
 * individual source file, and distribute linked combinations
 * including the two.
 * You must obey the GNU General Public License in all respects
 * for all of the code used other than OpenSSL.  If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so.  If you
 * do not wish to do so, delete this exception statement from your
 * version.  If you delete this exception statement from all source
 * files in the program, then also delete it here.
 */
/* copyright --> */

#ifndef D_SCHANNEL_TLS_SESSION_H
#define D_SCHANNEL_TLS_SESSION_H

#include "common.h"
#include "TLSSession.h"
#include "SChannelContext.h"

#include <vector>

namespace aria2 {

// ============================================================
// IoBuffer — 线性 I/O 缓冲区
// ============================================================
// 数据存储在 [0, offset) 区间。[offset, length) 为可写空间。
// 参考 curl schannel 的 sbuffer 模式，语义清晰。
struct IoBuffer {
  size_t length;  // 已分配容量
  size_t offset;  // 已使用字节数（= 数据末尾位置）
  std::vector<unsigned char> buf;

  IoBuffer() : length(0), offset(0) {}

  // 确保总容量至少为 minSize 字节
  void ensureCapacity(size_t minSize)
  {
    if (length >= minSize) {
      return;
    }
    buf.resize(minSize);
    length = minSize;
  }

  // 从头部消费 n 字节，将剩余数据前移
  void consume(size_t n)
  {
    if (n >= offset) {
      offset = 0;
      return;
    }
    memmove(buf.data(), buf.data() + n, offset - n);
    offset -= n;
  }

  // 追加写入数据
  void append(const void* data, size_t len)
  {
    if (!len) {
      return;
    }
    ensureCapacity(offset + len);
    memcpy(buf.data() + offset, data, len);
    offset += len;
  }

  unsigned char* data() { return buf.data(); }
  const unsigned char* data() const { return buf.data(); }
  unsigned char* writePtr() { return buf.data() + offset; }
  size_t writeSpace() const { return length - offset; }
  size_t size() const { return offset; }
  void clear() { offset = 0; }
};

// ============================================================
// SChannelSession — TLS 会话
// ============================================================
class SChannelSession : public TLSSession {
  // 握手 / 连接状态机
  enum State {
    STATE_INITIAL,         // 等待 init() / 首次 tlsConnect()
    STATE_HANDSHAKE_SEND,  // 有握手消息待发送
    STATE_HANDSHAKE_RECV,  // 等待接收握手消息
    STATE_CONNECTED,       // 握手完成，可收发应用数据
    STATE_SHUTTING_DOWN,   // 正在发送 close_notify
    STATE_CLOSED,          // TLS 连接已关闭
    STATE_ERROR            // 不可恢复的错误
  };

public:
  SChannelSession(SChannelContext* ctx);
  virtual ~SChannelSession();

  // ---- TLSSession 接口 ----
  virtual int init(sock_t sockfd) CXX11_OVERRIDE;
  virtual int setSNIHostname(const std::string& hostname) CXX11_OVERRIDE;
  virtual int closeConnection() CXX11_OVERRIDE;
  virtual int checkDirection() CXX11_OVERRIDE;
  virtual ssize_t writeData(const void* data, size_t len) CXX11_OVERRIDE;
  virtual ssize_t readData(void* data, size_t len) CXX11_OVERRIDE;
  virtual int tlsConnect(const std::string& hostname, TLSVersion& version,
                         std::string& handshakeErr) CXX11_OVERRIDE;
  virtual int tlsAccept(TLSVersion& version) CXX11_OVERRIDE;
  virtual std::string getLastErrorString() CXX11_OVERRIDE;
  virtual size_t getRecvBufferedLength() CXX11_OVERRIDE;

private:
  // ---- 握手三步 ----
  // Step 1: 生成 ClientHello 并开始发送
  int handshakeStep1();
  // Step 2: 循环收发握手消息直到握手完成
  int handshakeStep2();
  // Step 3: 查询连接参数（流大小、协议版本、密码套件）
  int handshakeStep3(TLSVersion& version);

  // ---- 底层 I/O 辅助 ----
  // 将 outBuf_ 中的数据发送到 socket（非阻塞）。
  // 返回 0=全部发完, TLS_ERR_WOULDBLOCK=部分发送, TLS_ERR_ERROR=错误
  int flushOutBuffer();

  // 尝试发送 pendingSend_ 中缓存的已加密 TLS 记录。
  // 返回 0=无待发/发完, TLS_ERR_WOULDBLOCK=部分, TLS_ERR_ERROR=错误
  int flushPendingSend();

  // ---- 连接参数 ----
  sock_t sockfd_;
  std::string hostname_;
  SChannelContext* ctx_;   // 所属上下文（不拥有所有权，每次通过 getCredHandle() 获取凭据）
  CtxtHandle ctxtHandle_;  // SSPI 安全上下文句柄
  bool ctxtValid_;         // ctxtHandle_ 是否已初始化
  ULONG retFlags_;         // ISC 实际返回的安全特性标志

  // ---- 状态 ----
  State state_;
  SECURITY_STATUS lastStatus_;
  int renegotiateCount_;   // 连续 renegotiation 计数（防止无限循环）
  bool firstConnectDone_;  // 首次握手完成标记（区分初始连接和 Key Update）

  // ---- 缓冲区 ----
  IoBuffer encBuf_;   // 从 socket 读取的密文
  IoBuffer decBuf_;   // 已解密的明文（待交给调用方）
  IoBuffer outBuf_;   // 握手阶段的待发送数据

  // 已加密但未完全发送的应用数据 TLS 记录
  IoBuffer pendingSend_;
  size_t pendingSendOffset_;  // pendingSend_ 中已发送的字节偏移

  // ---- 流参数（握手完成后有效） ----
  SecPkgContext_StreamSizes streamSizes_;

  // ---- EncryptMessage 使用的连续内存（预分配） ----
  std::vector<unsigned char> sendRecordBuf_;

  // ---- 连接关闭状态 ----
  bool recvCloseNotify_;       // 收到服务器 close_notify
  bool recvConnectionClosed_;  // recv() 返回 0（对端关闭 TCP）
  bool sentShutdown_;          // 已发送己方 close_notify
  bool renegotiationPending_;  // SEC_I_RENEGOTIATE 后需要先调用 ISC 生成 ClientHello
  bool handshakeFlushPending_; // ISC 返回 SEC_E_OK 但 flush 未完成，下次仅 flush 不重入 ISC

  // ---- ISC 请求标志 ----
  static const ULONG kReqFlags;
  // 单次 readData 中允许的最大连续 renegotiation 次数
  static const int kMaxRenegotiations;
};

} // namespace aria2

#endif // D_SCHANNEL_TLS_SESSION_H
