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

#include "SChannelSession.h"

#include <cassert>
#include <sstream>

#include "LogFactory.h"
#include "Logger.h"
#include "a2functional.h"
#include "fmt.h"
#include "util.h"

// ---- 旧版 SDK 可能缺少的常量 ----
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

// 缓冲区初始大小（与 curl 一致）
#define SCHANNEL_BUFFER_INIT_SIZE 4096

namespace {

using namespace aria2;

// ---- 密码套件信息查询结构体（旧 SDK 兼容） ----
struct WinCipherInfo {
  DWORD dwVersion;
  DWORD dwProtocol;
  DWORD dwCipherSuite;
  DWORD dwBaseCipherSuite;
  WCHAR szCipherSuite[SZ_ALG_MAX_SIZE];
  WCHAR szCipher[SZ_ALG_MAX_SIZE];
  DWORD dwCipherLen;
  DWORD dwCipherBlockLen;
  WCHAR szHash[SZ_ALG_MAX_SIZE];
  DWORD dwHashLen;
  WCHAR szExchange[SZ_ALG_MAX_SIZE];
  DWORD dwMinExchangeLen;
  DWORD dwMaxExchangeLen;
  WCHAR szCertificate[SZ_ALG_MAX_SIZE];
  DWORD dwKeyType;
};

// 初始化 SecBuffer
inline void initSecBuffer(SecBuffer* buf, ULONG type, void* data, ULONG size)
{
  buf->cbBuffer = size;
  buf->BufferType = type;
  buf->pvBuffer = data;
}

// 初始化 SecBufferDesc
inline void initSecBufferDesc(SecBufferDesc* desc, SecBuffer* bufArr,
                              ULONG count)
{
  desc->ulVersion = SECBUFFER_VERSION;
  desc->cBuffers = count;
  desc->pBuffers = bufArr;
}

// 查询连接的密码套件名称
std::string getCipherSuite(CtxtHandle* handle)
{
  WinCipherInfo info = {SECPKGCONTEXT_CIPHERINFO_V1};
  if (::QueryContextAttributes(handle, SECPKG_ATTR_CIPHER_INFO, &info) ==
      SEC_E_OK) {
    return wCharToUtf8(info.szCipherSuite);
  }
  return "Unknown";
}

// 查询连接使用的 TLS 协议版本号
uint32_t getProtocolVersion(CtxtHandle* handle)
{
  WinCipherInfo info = {SECPKGCONTEXT_CIPHERINFO_V1};
  if (::QueryContextAttributes(handle, SECPKG_ATTR_CIPHER_INFO, &info) ==
      SEC_E_OK) {
    return info.dwProtocol;
  }
  return 0;
}

} // namespace

namespace aria2 {

// ============================================================
// ISC 请求标志
// ============================================================
const ULONG SChannelSession::kReqFlags =
    ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
    ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM | ISC_REQ_USE_SUPPLIED_CREDS;

const int SChannelSession::kMaxRenegotiations = 3;

// ============================================================
// TLSSession 工厂方法
// ============================================================
TLSSession* TLSSession::make(TLSContext* ctx)
{
  return new SChannelSession(static_cast<SChannelContext*>(ctx));
}

// ============================================================
// 构造 / 析构
// ============================================================

SChannelSession::SChannelSession(SChannelContext* ctx)
    : sockfd_(0),
      ctx_(ctx),
      ctxtValid_(false),
      retFlags_(0),
      state_(STATE_INITIAL),
      lastStatus_(SEC_E_OK),
      renegotiateCount_(0),
      firstConnectDone_(false),
      pendingSendOffset_(0),
      recvCloseNotify_(false),
      recvConnectionClosed_(false),
      sentShutdown_(false),
      renegotiationPending_(false),
      handshakeFlushPending_(false)
{
  memset(&ctxtHandle_, 0, sizeof(ctxtHandle_));
  memset(&streamSizes_, 0, sizeof(streamSizes_));
}

SChannelSession::~SChannelSession()
{
  if (ctxtValid_) {
    ::DeleteSecurityContext(&ctxtHandle_);
  }
}

// ============================================================
// 初始化与配置
// ============================================================

int SChannelSession::init(sock_t sockfd)
{
  if (state_ != STATE_INITIAL) {
    lastStatus_ = SEC_E_INVALID_HANDLE;
    return TLS_ERR_ERROR;
  }
  sockfd_ = sockfd;
  return TLS_ERR_OK;
}

int SChannelSession::setSNIHostname(const std::string& hostname)
{
  if (state_ != STATE_INITIAL) {
    lastStatus_ = SEC_E_INVALID_HANDLE;
    return TLS_ERR_ERROR;
  }
  hostname_ = hostname;
  return TLS_ERR_OK;
}

// ============================================================
// 底层 I/O: 发送 outBuf_ 中的握手数据
// ============================================================

int SChannelSession::flushOutBuffer()
{
  while (outBuf_.size()) {
    int rv = ::send(sockfd_, reinterpret_cast<const char*>(outBuf_.data()),
                    static_cast<int>(outBuf_.size()), 0);
    if (rv == SOCKET_ERROR) {
      int err = ::WSAGetLastError();
      if (err == WSAEINTR) {
        continue;
      }
      if (err == WSAEWOULDBLOCK) {
        return TLS_ERR_WOULDBLOCK;
      }
      A2_LOG_ERROR(fmt("SChannel: send failed, WSA error=%d", err));
      lastStatus_ = SEC_E_INTERNAL_ERROR;
      state_ = STATE_ERROR;
      return TLS_ERR_ERROR;
    }
    outBuf_.consume(rv);
  }
  return 0;
}

// ============================================================
// 底层 I/O: 发送 pendingSend_ 中缓存的已加密 TLS 记录
// ============================================================

int SChannelSession::flushPendingSend()
{
  while (pendingSendOffset_ < pendingSend_.size()) {
    int rv = ::send(
        sockfd_,
        reinterpret_cast<const char*>(pendingSend_.data() + pendingSendOffset_),
        static_cast<int>(pendingSend_.size() - pendingSendOffset_), 0);
    if (rv == SOCKET_ERROR) {
      int err = ::WSAGetLastError();
      if (err == WSAEINTR) {
        continue;
      }
      if (err == WSAEWOULDBLOCK) {
        return TLS_ERR_WOULDBLOCK;
      }
      A2_LOG_ERROR(fmt("SChannel: failed to send buffered data, WSA error=%d", err));
      lastStatus_ = SEC_E_INTERNAL_ERROR;
      state_ = STATE_ERROR;
      return TLS_ERR_ERROR;
    }
    pendingSendOffset_ += rv;
  }
  // 全部发送完毕，清理
  pendingSend_.clear();
  pendingSendOffset_ = 0;
  return 0;
}

// ============================================================
// 握手步骤 1: 生成 ClientHello
// ============================================================

int SChannelSession::handshakeStep1()
{
  CredHandle* cred = ctx_->getCredHandle();
  A2_LOG_DEBUG(fmt("SChannel: handshake step 1 - generating ClientHello,"
                   " host=%s, cred=%p",
                   hostname_.c_str(), (void*)cred));

  // 输出缓冲区（SChannel 分配内存）
  SecBuffer outbuf;
  SecBufferDesc outbufDesc;
  initSecBuffer(&outbuf, SECBUFFER_EMPTY, nullptr, 0);
  initSecBufferDesc(&outbufDesc, &outbuf, 1);

  // hostname 转为 SEC_CHAR* 用于 SNI
  SEC_CHAR* host =
      hostname_.empty() ? nullptr : const_cast<SEC_CHAR*>(hostname_.c_str());

  retFlags_ = 0;
  lastStatus_ = ::InitializeSecurityContextA(
      cred,
      nullptr,  // 首次调用无已有上下文
      host, kReqFlags, 0, 0,
      nullptr,  // 首次调用无输入
      0,
      &ctxtHandle_,  // 输出: 新建安全上下文
      &outbufDesc, &retFlags_, nullptr);

  if (lastStatus_ != SEC_I_CONTINUE_NEEDED) {
    if (lastStatus_ == SEC_E_WRONG_PRINCIPAL) {
      A2_LOG_ERROR(fmt("SChannel: SNI or certificate check failed,"
                       " status=0x%lx",
                       lastStatus_));
    }
    else {
      A2_LOG_ERROR(fmt("SChannel: initial InitializeSecurityContext failed,"
                       " status=0x%lx",
                       lastStatus_));
    }
    state_ = STATE_ERROR;
    return TLS_ERR_ERROR;
  }
  ctxtValid_ = true;

  // 将 ClientHello 消息复制到 outBuf_ 并释放 SChannel 分配的内存
  if (outbuf.cbBuffer > 0 && outbuf.pvBuffer) {
    outBuf_.append(outbuf.pvBuffer, outbuf.cbBuffer);
    ::FreeContextBuffer(outbuf.pvBuffer);
  }

  // 开始发送 ClientHello
  state_ = STATE_HANDSHAKE_SEND;
  int rv = flushOutBuffer();
  if (rv == 0) {
    // 全部发送完毕，转入接收阶段
    state_ = STATE_HANDSHAKE_RECV;
  }
  // rv == TLS_ERR_WOULDBLOCK: 保持 STATE_HANDSHAKE_SEND
  return rv < 0 ? rv : TLS_ERR_WOULDBLOCK;
}

// ============================================================
// 握手步骤 2: 循环收发握手消息
// ============================================================

int SChannelSession::handshakeStep2()
{
  // needMoreData：标记上次 ISC 返回"数据不够"，下次循环必须强制 recv，
  // 不能因为 encBuf_ 非空就跳过（否则会用旧数据反复跑 ISC 形成死循环）。
  bool needMoreData = false;

  for (;;) {
    // ---- 发送阶段 ----
    if (state_ == STATE_HANDSHAKE_SEND) {
      int rv = flushOutBuffer();
      if (rv == TLS_ERR_WOULDBLOCK) {
        return TLS_ERR_WOULDBLOCK;
      }
      if (rv != 0) {
        return rv;
      }
      if (handshakeFlushPending_) {
        handshakeFlushPending_ = false;
        return 0;
      }
      // 发送完毕，转入接收
      state_ = STATE_HANDSHAKE_RECV;
    }

    // ---- 接收阶段 ----
    if (state_ != STATE_HANDSHAKE_RECV) {
      break;
    }

    // 对齐 curl 的 doread 语义：
    // - encBuf_ 为空 或 needMoreData=true（上次 ISC 数据不足）→ 先 recv
    // - 否则 encBuf_ 里已有数据（renegotiation 时 EXTRA 被保留）→ 直接跑 ISC
    if (encBuf_.size() == 0 || needMoreData) {
      needMoreData = false;
      // 确保加密缓冲区有空间，然后从 socket 读取
      encBuf_.ensureCapacity(encBuf_.offset + SCHANNEL_BUFFER_INIT_SIZE);
      while (encBuf_.writeSpace()) {
        int nread = ::recv(sockfd_, reinterpret_cast<char*>(encBuf_.writePtr()),
                           static_cast<int>(encBuf_.writeSpace()), 0);
        if (nread == SOCKET_ERROR) {
          int err = ::WSAGetLastError();
          if (err == WSAEINTR) {
            continue;
          }
          if (err == WSAEWOULDBLOCK) {
            break;
          }
          A2_LOG_ERROR(fmt("SChannel: handshake recv failed, WSA error=%d", err));
          lastStatus_ = SEC_E_INTERNAL_ERROR;
          state_ = STATE_ERROR;
          return TLS_ERR_ERROR;
        }
        if (nread == 0) {
          A2_LOG_ERROR("SChannel: server closed the connection during handshake");
          lastStatus_ = SEC_E_INCOMPLETE_MESSAGE;
          state_ = STATE_ERROR;
          return TLS_ERR_ERROR;
        }
        encBuf_.offset += nread;
        break;  // 非阻塞: 读到数据就去处理
      }

      if (encBuf_.size() == 0) {
        if (renegotiationPending_) {
          renegotiationPending_ = false;
        }
        else {
          return TLS_ERR_WOULDBLOCK;
        }
      }
    }

    // ---- 调用 InitializeSecurityContext 处理接收到的握手数据 ----
    SecBuffer inbufs[2];
    SecBufferDesc inbufDesc;
    initSecBuffer(&inbufs[0], SECBUFFER_TOKEN, encBuf_.data(),
                  static_cast<ULONG>(encBuf_.size()));
    initSecBuffer(&inbufs[1], SECBUFFER_EMPTY, nullptr, 0);
    initSecBufferDesc(&inbufDesc, inbufs, 2);

    SecBuffer outbufs[3];
    SecBufferDesc outbufDesc;
    initSecBuffer(&outbufs[0], SECBUFFER_TOKEN, nullptr, 0);
    initSecBuffer(&outbufs[1], SECBUFFER_ALERT, nullptr, 0);
    initSecBuffer(&outbufs[2], SECBUFFER_EMPTY, nullptr, 0);
    initSecBufferDesc(&outbufDesc, outbufs, 3);

    SEC_CHAR* host =
        hostname_.empty() ? nullptr : const_cast<SEC_CHAR*>(hostname_.c_str());
    lastStatus_ = ::InitializeSecurityContextA(
        ctx_->getCredHandle(), &ctxtHandle_, host, kReqFlags, 0, 0, &inbufDesc,
        0, &ctxtHandle_, &outbufDesc, &retFlags_, nullptr);

    // ---- 对齐 curl 的 switch→EXTRA 处理顺序 ----
    // 1. SEC_E_INCOMPLETE_MESSAGE (0x80090318)：握手数据不足一条完整 TLS 记录。
    //    SChannel 未消费任何字节，保留 encBuf_ 原样，标记 needMoreData 后
    //    continue 回顶部强制 recv 追加数据；若 socket 暂无数据则返回 WOULDBLOCK。
    //    非阻塞模式下大证书链（多 TCP 分片）会触发多次，属正常现象。
    if (lastStatus_ == SEC_E_INCOMPLETE_MESSAGE) {
      for (int i = 0; i < 3; ++i) {
        if (outbufs[i].pvBuffer) {
          ::FreeContextBuffer(outbufs[i].pvBuffer);
          outbufs[i].pvBuffer = nullptr;
        }
      }
      needMoreData = true;
      continue;
    }

    // 2. 其他错误：不处理 EXTRA，直接报错。
    //    必须在 EXTRA memmove 之前检查：错误时 inbufs[1].cb 可能大于实际数据量，
    //    若先做 memmove 会产生下溢，破坏 encBuf_ 并形成死循环（已有前车之鉴）。
    if (lastStatus_ != SEC_E_OK &&
        lastStatus_ != SEC_I_CONTINUE_NEEDED &&
        lastStatus_ != SEC_I_INCOMPLETE_CREDENTIALS) {
      if (lastStatus_ == SEC_E_WRONG_PRINCIPAL) {
        A2_LOG_ERROR(fmt("SChannel: SNI or certificate check failed,"
                         " status=0x%lx",
                         lastStatus_));
      }
      else if (lastStatus_ == SEC_E_UNTRUSTED_ROOT) {
        A2_LOG_ERROR(fmt("SChannel: server certificate is not trusted,"
                         " status=0x%lx",
                         lastStatus_));
      }
      else {
        A2_LOG_ERROR(
            fmt("SChannel: handshake failed, status=0x%lx", lastStatus_));
      }
      for (int i = 0; i < 3; ++i) {
        if (outbufs[i].pvBuffer) {
          ::FreeContextBuffer(outbufs[i].pvBuffer);
        }
      }
      state_ = STATE_ERROR;
      return TLS_ERR_ERROR;
    }

    // 3. 成功或可继续时，先发出 SChannel 生成的握手输出消息。
    for (int i = 0; i < 3; ++i) {
      if (outbufs[i].BufferType == SECBUFFER_TOKEN && outbufs[i].cbBuffer > 0) {
        outBuf_.append(outbufs[i].pvBuffer, outbufs[i].cbBuffer);
        ::FreeContextBuffer(outbufs[i].pvBuffer);
        outbufs[i].pvBuffer = nullptr;
      }
    }

    // 4. 处理未消费的剩余密文（EXTRA）。
    //    对齐 curl：只在 encBuf_.size() > EXTRA.cb 时（SChannel 确实消费了一些
    //    数据）才做 memmove；否则说明 EXTRA 就是全部输入，不需要移动。
    if (inbufs[1].BufferType == SECBUFFER_EXTRA && inbufs[1].cbBuffer > 0) {
      if (encBuf_.size() > inbufs[1].cbBuffer) {
        memmove(encBuf_.data(),
                encBuf_.data() + encBuf_.size() - inbufs[1].cbBuffer,
                inbufs[1].cbBuffer);
      }
      // 否则 EXTRA == 全部输入，数据已在 encBuf_ 头部，只需更新 offset
      encBuf_.offset = inbufs[1].cbBuffer;
    }
    else {
      encBuf_.clear();
    }

    // 5. 根据状态决定下一步。
    if (lastStatus_ == SEC_I_CONTINUE_NEEDED ||
        lastStatus_ == SEC_I_INCOMPLETE_CREDENTIALS) {
      if (lastStatus_ == SEC_I_INCOMPLETE_CREDENTIALS) {
        A2_LOG_DEBUG("SChannel: server requested client certificate,"
                     " continuing handshake");
      }
      if (outBuf_.size() > 0) {
        state_ = STATE_HANDSHAKE_SEND;
      }
      continue;
    }

    // lastStatus_ == SEC_E_OK
    if (outBuf_.size() > 0) {
      state_ = STATE_HANDSHAKE_SEND;
      int rv = flushOutBuffer();
      if (rv == TLS_ERR_WOULDBLOCK) {
        handshakeFlushPending_ = true;
        return TLS_ERR_WOULDBLOCK;
      }
      if (rv != 0) {
        return rv;
      }
    }
    return 0;
  }

  return TLS_ERR_WOULDBLOCK;
}

// ============================================================
// 握手步骤 3: 查询连接参数
// ============================================================

int SChannelSession::handshakeStep3(TLSVersion& version)
{
  // 校验 ISC 返回标志是否满足所有请求的安全特性（参考 curl schannel_connect_step3）
  if (retFlags_ != kReqFlags) {
    if (!(retFlags_ & ISC_RET_SEQUENCE_DETECT)) {
      A2_LOG_ERROR("SChannel: failed to setup sequence detection");
    }
    if (!(retFlags_ & ISC_RET_REPLAY_DETECT)) {
      A2_LOG_ERROR("SChannel: failed to setup replay detection");
    }
    if (!(retFlags_ & ISC_RET_CONFIDENTIALITY)) {
      A2_LOG_ERROR("SChannel: failed to setup confidentiality");
    }
    if (!(retFlags_ & ISC_RET_STREAM)) {
      A2_LOG_ERROR("SChannel: failed to setup stream orientation");
    }
    lastStatus_ = SEC_E_INTERNAL_ERROR;
    state_ = STATE_ERROR;
    return TLS_ERR_ERROR;
  }

  // 查询流大小限制
  lastStatus_ = ::QueryContextAttributes(&ctxtHandle_,
                                         SECPKG_ATTR_STREAM_SIZES,
                                         &streamSizes_);
  if (lastStatus_ != SEC_E_OK || streamSizes_.cbMaximumMessage == 0) {
    A2_LOG_ERROR("SChannel: failed to query stream sizes");
    state_ = STATE_ERROR;
    return TLS_ERR_ERROR;
  }

  // 预分配加密发送缓冲区: Header + MaxMessage + Trailer
  size_t sendBufSize = streamSizes_.cbHeader +
                       streamSizes_.cbMaximumMessage + streamSizes_.cbTrailer;
  sendRecordBuf_.resize(sendBufSize);

  // 查询协议版本 — 优先使用 SECPKG_ATTR_CONNECTION_INFO (兼容性更好)，
  // 再用 SECPKG_ATTR_CIPHER_INFO 获取密码套件名称。
  SecPkgContext_ConnectionInfo connInfo = {};
  lastStatus_ = ::QueryContextAttributes(&ctxtHandle_,
                                         SECPKG_ATTR_CONNECTION_INFO,
                                         &connInfo);
  if (lastStatus_ == SEC_E_OK) {
    if (connInfo.dwProtocol & SP_PROT_TLS1_3_CLIENT) {
      version = TLS_PROTO_TLS13;
    }
    else if (connInfo.dwProtocol & SP_PROT_TLS1_2_CLIENT) {
      version = TLS_PROTO_TLS12;
    }
    else if (connInfo.dwProtocol & SP_PROT_TLS1_1_CLIENT) {
      version = TLS_PROTO_TLS11;
    }
    else {
      A2_LOG_ERROR(fmt("SChannel: unsupported protocol negotiated 0x%lx",
                       connInfo.dwProtocol));
      state_ = STATE_ERROR;
      return TLS_ERR_ERROR;
    }
  }
  else {
    // 回退: 使用 CIPHER_INFO 中的 dwProtocol 字段
    auto proto = getProtocolVersion(&ctxtHandle_);
    switch (proto) {
    case 0x0302:
      version = TLS_PROTO_TLS11;
      break;
    case 0x0303:
      version = TLS_PROTO_TLS12;
      break;
    case 0x0304:
      version = TLS_PROTO_TLS13;
      break;
    default:
      A2_LOG_ERROR(fmt("SChannel: unknown protocol version 0x%x", proto));
      state_ = STATE_ERROR;
      return TLS_ERR_ERROR;
    }
  }

  auto suite = getCipherSuite(&ctxtHandle_);
  // 仅在首次握手完成时打 INFO；renegotiate / Key Update 后重调时打 DEBUG，
  // 避免日志里出现多次"TLS 连接建立"令人困惑。
  state_ = STATE_CONNECTED;
  if (!firstConnectDone_) {
    firstConnectDone_ = true;
    A2_LOG_INFO(fmt("SChannel: TLS connection established, cipher=%s",
                    suite.c_str()));
  }
  else {
    A2_LOG_DEBUG(fmt("SChannel: TLS Key Update complete, cipher=%s",
                     suite.c_str()));
  }

  return TLS_ERR_OK;
}

// ============================================================
// tlsConnect — 握手入口（可多次调用）
// ============================================================

int SChannelSession::tlsConnect(const std::string& hostname,
                                TLSVersion& version,
                                std::string& handshakeErr)
{
  // 首次调用: 设置 SNI 并启动握手
  if (state_ == STATE_INITIAL) {
    if (!hostname.empty() && hostname_.empty()) {
      hostname_ = hostname;
    }
    int rv = handshakeStep1();
    if (rv != 0) {
      if (rv == TLS_ERR_ERROR) {
        handshakeErr = getLastErrorString();
      }
      return rv;
    }
  }

  // 继续握手循环
  if (state_ == STATE_HANDSHAKE_SEND || state_ == STATE_HANDSHAKE_RECV) {
    int rv = handshakeStep2();
    if (rv != 0) {
      if (rv == TLS_ERR_ERROR) {
        handshakeErr = getLastErrorString();
      }
      return rv;
    }
  }

  // 握手完成，查询连接参数
  if (state_ != STATE_CONNECTED) {
    int rv = handshakeStep3(version);
    if (rv != TLS_ERR_OK) {
      handshakeErr = getLastErrorString();
    }
    return rv;
  }

  return TLS_ERR_OK;
}

int SChannelSession::tlsAccept(TLSVersion& version)
{
  // 本实现仅支持 TLS 客户端
  (void)version;
  A2_LOG_ERROR("SChannel: server-side TLS is not supported");
  lastStatus_ = SEC_E_UNSUPPORTED_FUNCTION;
  state_ = STATE_ERROR;
  return TLS_ERR_ERROR;
}

// ============================================================
// writeData — 加密并发送应用数据
// ============================================================

ssize_t SChannelSession::writeData(const void* data, size_t len)
{
  // 重协商支持
  if (state_ == STATE_HANDSHAKE_SEND || state_ == STATE_HANDSHAKE_RECV) {
    std::string hn, err;
    TLSVersion ver;
    int rv = tlsConnect(hn, ver, err);
    if (rv != TLS_ERR_OK) {
      return rv;
    }
  }

  if (state_ != STATE_CONNECTED && state_ != STATE_SHUTTING_DOWN) {
    lastStatus_ = SEC_E_INVALID_HANDLE;
    return TLS_ERR_ERROR;
  }

  // 先发送之前缓存的加密数据（如果有）
  if (pendingSend_.size() > pendingSendOffset_) {
    int rv = flushPendingSend();
    if (rv == TLS_ERR_WOULDBLOCK) {
      return TLS_ERR_WOULDBLOCK;
    }
    if (rv != 0) {
      return rv;
    }
  }

  // 限制单次写入大小为 SChannel 允许的最大消息长度
  if (len > streamSizes_.cbMaximumMessage) {
    len = streamSizes_.cbMaximumMessage;
  }

  // 在预分配的 sendRecordBuf_ 中构建 TLS 记录:
  //   [Header | Plaintext | Trailer]
  SecBuffer outbufs[4];
  SecBufferDesc outbufDesc;
  initSecBuffer(&outbufs[0], SECBUFFER_STREAM_HEADER, sendRecordBuf_.data(),
                streamSizes_.cbHeader);
  initSecBuffer(&outbufs[1], SECBUFFER_DATA,
                sendRecordBuf_.data() + streamSizes_.cbHeader,
                static_cast<ULONG>(len));
  initSecBuffer(&outbufs[2], SECBUFFER_STREAM_TRAILER,
                sendRecordBuf_.data() + streamSizes_.cbHeader + len,
                streamSizes_.cbTrailer);
  initSecBuffer(&outbufs[3], SECBUFFER_EMPTY, nullptr, 0);
  initSecBufferDesc(&outbufDesc, outbufs, 4);

  // 复制明文到 DATA 区域
  memcpy(outbufs[1].pvBuffer, data, len);

  // 加密
  lastStatus_ = ::EncryptMessage(&ctxtHandle_, 0, &outbufDesc, 0);
  if (lastStatus_ != SEC_E_OK) {
    A2_LOG_ERROR(
        fmt("SChannel: EncryptMessage failed, status=0x%lx", lastStatus_));
    state_ = STATE_ERROR;
    return TLS_ERR_ERROR;
  }

  // 加密后的总长度
  size_t totalLen =
      outbufs[0].cbBuffer + outbufs[1].cbBuffer + outbufs[2].cbBuffer;

  // 尝试发送完整的 TLS 记录
  size_t sent = 0;
  while (sent < totalLen) {
    int rv =
        ::send(sockfd_, reinterpret_cast<const char*>(sendRecordBuf_.data() + sent),
               static_cast<int>(totalLen - sent), 0);
    if (rv == SOCKET_ERROR) {
      int err = ::WSAGetLastError();
      if (err == WSAEINTR) {
        continue;
      }
      if (err == WSAEWOULDBLOCK) {
        // 已加密的数据不能丢弃，缓存剩余部分
        pendingSend_.append(sendRecordBuf_.data() + sent, totalLen - sent);
        pendingSendOffset_ = 0;
        return static_cast<ssize_t>(len);
      }
      A2_LOG_ERROR(fmt("SChannel: failed to send encrypted data,"
                       " WSA error=%d",
                       err));
      lastStatus_ = SEC_E_INTERNAL_ERROR;
      state_ = STATE_ERROR;
      return TLS_ERR_ERROR;
    }
    sent += rv;
  }

  return static_cast<ssize_t>(len);
}

// ============================================================
// readData — 接收并解密应用数据
// ============================================================

ssize_t SChannelSession::readData(void* data, size_t len)
{
  if (len == 0) {
    return 0;
  }

  // 1. 优先从已解密缓冲区返回数据
  if (decBuf_.size() >= len) {
    memcpy(data, decBuf_.data(), len);
    decBuf_.consume(len);
    return static_cast<ssize_t>(len);
  }

  // 2. 连接已关闭或出错时，返回剩余解密数据
  if (state_ == STATE_CLOSED || state_ == STATE_ERROR ||
      state_ == STATE_SHUTTING_DOWN) {
    if (decBuf_.size() > 0) {
      size_t n = std::min(decBuf_.size(), len);
      memcpy(data, decBuf_.data(), n);
      decBuf_.consume(n);
      return static_cast<ssize_t>(n);
    }
    return state_ == STATE_ERROR ? TLS_ERR_ERROR : 0;
  }

  // 3. 重协商支持
  if (state_ == STATE_HANDSHAKE_SEND || state_ == STATE_HANDSHAKE_RECV) {
    std::string hn, err;
    TLSVersion ver;
    int rv = tlsConnect(hn, ver, err);
    if (rv != TLS_ERR_OK) {
      return rv;
    }
  }

  if (state_ != STATE_CONNECTED) {
    lastStatus_ = SEC_E_INVALID_HANDLE;
    return TLS_ERR_ERROR;
  }

  renegotiateCount_ = 0;

  // 4. 先发送缓存的加密数据（如果有）
  if (pendingSend_.size() > pendingSendOffset_) {
    flushPendingSend();
    // 即使 WOULDBLOCK 也继续解密已有数据
  }

  // 5. 从 socket 非阻塞读取密文
  if (!recvConnectionClosed_) {
    encBuf_.ensureCapacity(encBuf_.offset + len + 1024);
    while (encBuf_.writeSpace()) {
      int nread = ::recv(sockfd_, reinterpret_cast<char*>(encBuf_.writePtr()),
                         static_cast<int>(encBuf_.writeSpace()), 0);
      if (nread == SOCKET_ERROR) {
        int err = ::WSAGetLastError();
        if (err == WSAEINTR) {
          continue;
        }
        if (err == WSAEWOULDBLOCK) {
          break;
        }
        A2_LOG_ERROR(fmt("SChannel: recv failed, WSA error=%d", err));
        lastStatus_ = SEC_E_INTERNAL_ERROR;
        state_ = STATE_ERROR;
        return TLS_ERR_ERROR;
      }
      if (nread == 0) {
        // 对端关闭了 TCP 连接
        recvConnectionClosed_ = true;
        A2_LOG_DEBUG("SChannel: server closed the connection");
        break;
      }
      encBuf_.offset += nread;
      break;  // 非阻塞：读到数据就去解密
    }
  }

  // 6. 解密循环 — 处理 encBuf_ 中的所有完整 TLS 记录
  while (encBuf_.size() > 0) {
    SecBuffer inbufs[4];
    SecBufferDesc inbufDesc;
    initSecBuffer(&inbufs[0], SECBUFFER_DATA, encBuf_.data(),
                  static_cast<ULONG>(encBuf_.size()));
    initSecBuffer(&inbufs[1], SECBUFFER_EMPTY, nullptr, 0);
    initSecBuffer(&inbufs[2], SECBUFFER_EMPTY, nullptr, 0);
    initSecBuffer(&inbufs[3], SECBUFFER_EMPTY, nullptr, 0);
    initSecBufferDesc(&inbufDesc, inbufs, 4);

    lastStatus_ =
        ::DecryptMessage(&ctxtHandle_, &inbufDesc, 0, nullptr);

    if (lastStatus_ == SEC_E_OK) {
      // 提取解密后的明文数据 (inbufs[1])
      if (inbufs[1].BufferType == SECBUFFER_DATA && inbufs[1].cbBuffer > 0) {
        decBuf_.append(inbufs[1].pvBuffer, inbufs[1].cbBuffer);
      }

      // 处理剩余未消费的密文 (inbufs[3] EXTRA)
      if (inbufs[3].BufferType == SECBUFFER_EXTRA && inbufs[3].cbBuffer > 0) {
        if (encBuf_.size() > inbufs[3].cbBuffer) {
          memmove(encBuf_.data(),
                  encBuf_.data() + encBuf_.size() - inbufs[3].cbBuffer,
                  inbufs[3].cbBuffer);
        }
        encBuf_.offset = inbufs[3].cbBuffer;
      }
      else {
        encBuf_.clear();
      }
      continue;
    }

    if (lastStatus_ == SEC_E_INCOMPLETE_MESSAGE) {
      // TLS 记录不完整，需要更多密文
      break;
    }

    if (lastStatus_ == SEC_I_CONTEXT_EXPIRED) {
      // 服务器发送了 close_notify
      A2_LOG_DEBUG("SChannel: server close notification received"
                   " (close_notify)");
      if (inbufs[1].BufferType == SECBUFFER_DATA && inbufs[1].cbBuffer > 0) {
        decBuf_.append(inbufs[1].pvBuffer, inbufs[1].cbBuffer);
      }
      recvCloseNotify_ = true;
      recvConnectionClosed_ = true;
      break;
    }

    if (lastStatus_ == SEC_I_RENEGOTIATE) {
      // SChannel 要求完成一次新握手（TLS 1.2 重协商 或 TLS 1.3 Key Update）。
      // inbufs[3] 里的 EXTRA 数据是下一条记录的开头，需先保留到 encBuf_，
      // 然后通过 handshakeStep2 让 SChannel 发送应答消息，完成握手交换，
      // 之后再继续解密剩余数据。

      if (inbufs[1].BufferType == SECBUFFER_DATA && inbufs[1].cbBuffer > 0) {
        decBuf_.append(inbufs[1].pvBuffer, inbufs[1].cbBuffer);
      }
      // 保留 EXTRA 数据供 handshakeStep2 使用
      if (inbufs[3].BufferType == SECBUFFER_EXTRA && inbufs[3].cbBuffer > 0) {
        if (encBuf_.size() > inbufs[3].cbBuffer) {
          memmove(encBuf_.data(),
                  encBuf_.data() + encBuf_.size() - inbufs[3].cbBuffer,
                  inbufs[3].cbBuffer);
        }
        encBuf_.offset = inbufs[3].cbBuffer;
      }
      else {
        encBuf_.clear();
      }

      // Fix #6: 防止无限 renegotiation 循环（参考 curl MAX_RENEG_BLOCK_TIME）
      if (++renegotiateCount_ > kMaxRenegotiations) {
        A2_LOG_ERROR("SChannel: exceeded maximum renegotiation count");
        lastStatus_ = SEC_E_INTERNAL_ERROR;
        state_ = STATE_ERROR;
        return TLS_ERR_ERROR;
      }

      // 进入握手状态，让 handshakeStep2 完成 Key Update 应答。
      // 设置 renegotiationPending_ 标志：服务端发起的重协商需要客户端先调用 ISC
      // 生成新的 ClientHello，而 handshakeStep2 默认会先等待接收数据。此标志让
      // handshakeStep2 在 encBuf_ 为空时仍调用 ISC，避免双方互等造成死锁。
      A2_LOG_DEBUG("SChannel: remote party requests renegotiation");
      renegotiationPending_ = true;
      state_ = STATE_HANDSHAKE_RECV;
      int rv = handshakeStep2();
      if (rv != 0) {
        // WOULDBLOCK: 需要更多 socket 数据，等下次 readData 再试
        return rv == TLS_ERR_WOULDBLOCK ? TLS_ERR_WOULDBLOCK : TLS_ERR_ERROR;
      }
      // 握手完成，重新查询流参数（Key Update 后 stream sizes 可能变化）
      // 注意：handshakeStep3 失败时会把 state_ 设为 STATE_ERROR 并返回错误码，
      // 必须检查返回值，不能用后续的 state_ = STATE_CONNECTED 覆盖。
      TLSVersion dummyVer;
      {
        int rv3 = handshakeStep3(dummyVer);
        if (rv3 != TLS_ERR_OK) {
          A2_LOG_ERROR(fmt("SChannel: handshakeStep3 failed after"
                         " renegotiation, rv=%d",
                         rv3));
          return TLS_ERR_ERROR;
        }
      }
      // handshakeStep3 内部已将 state_ = STATE_CONNECTED
      // encBuf_ 里可能还有应用数据，继续解密循环
      continue;
    }

    // 其他错误
    A2_LOG_ERROR(fmt("SChannel: failed to read data from server,"
                     " status=0x%lx",
                     lastStatus_));
    state_ = STATE_ERROR;
    return TLS_ERR_ERROR;
  }

  // 7. 从 decBuf_ 返回数据给调用方
  size_t nread = std::min(decBuf_.size(), len);
  if (nread > 0) {
    memcpy(data, decBuf_.data(), nread);
    decBuf_.consume(nread);
    return static_cast<ssize_t>(nread);
  }

  // 8. 没有解密数据可返回
  if (recvConnectionClosed_) {
    if (!recvCloseNotify_) {
      A2_LOG_WARN("SChannel: server closed abruptly"
                   " (missing close_notify)");
    }
    return 0;  // EOF
  }

  return TLS_ERR_WOULDBLOCK;
}

// ============================================================
// closeConnection — 关闭 TLS 连接
// ============================================================

int SChannelSession::closeConnection()
{
  if (state_ != STATE_CONNECTED && state_ != STATE_SHUTTING_DOWN) {
    if (state_ != STATE_ERROR && state_ != STATE_CLOSED) {
      lastStatus_ = SEC_E_INVALID_HANDLE;
      state_ = STATE_ERROR;
    }
    return TLS_ERR_ERROR;
  }

  if (!sentShutdown_ && ctxtValid_) {
    A2_LOG_DEBUG("SChannel: sending close_notify");
    state_ = STATE_SHUTTING_DOWN;

    // 步骤 1: 发送 SCHANNEL_SHUTDOWN 控制令牌
    DWORD dwShut = SCHANNEL_SHUTDOWN;
    SecBuffer shutBuf;
    SecBufferDesc shutBufDesc;
    initSecBuffer(&shutBuf, SECBUFFER_TOKEN, &dwShut, sizeof(dwShut));
    initSecBufferDesc(&shutBufDesc, &shutBuf, 1);

    lastStatus_ = ::ApplyControlToken(&ctxtHandle_, &shutBufDesc);
    if (lastStatus_ != SEC_E_OK) {
      A2_LOG_ERROR(fmt("SChannel: ApplyControlToken failure,"
                       " status=0x%lx",
                       lastStatus_));
      state_ = STATE_ERROR;
      return TLS_ERR_ERROR;
    }

    // 步骤 2: 生成 close_notify 消息
    SecBuffer outbuf;
    SecBufferDesc outbufDesc;
    initSecBuffer(&outbuf, SECBUFFER_EMPTY, nullptr, 0);
    initSecBufferDesc(&outbufDesc, &outbuf, 1);

    SEC_CHAR* host =
        hostname_.empty() ? nullptr : const_cast<SEC_CHAR*>(hostname_.c_str());
    ULONG shutRetFlags = 0;
    lastStatus_ = ::InitializeSecurityContextA(
        ctx_->getCredHandle(), &ctxtHandle_, host, kReqFlags, 0, 0, nullptr, 0,
        &ctxtHandle_, &outbufDesc, &shutRetFlags, nullptr);

    if (lastStatus_ == SEC_E_OK || lastStatus_ == SEC_I_CONTEXT_EXPIRED) {
      // 步骤 3: 发送 close_notify
      if (outbuf.pvBuffer && outbuf.cbBuffer > 0) {
        // 尽力发送，忽略失败（参考 curl 的处理）
        int rv = ::send(sockfd_,
                        reinterpret_cast<const char*>(outbuf.pvBuffer),
                        outbuf.cbBuffer, 0);
        (void)rv;
      }
    }

    // 总是释放 SChannel 分配的缓冲区
    if (outbuf.pvBuffer) {
      ::FreeContextBuffer(outbuf.pvBuffer);
    }

    sentShutdown_ = true;
  }

  state_ = STATE_CLOSED;
  A2_LOG_DEBUG("SChannel: TLS connection closed");
  return TLS_ERR_OK;
}

// ============================================================
// checkDirection — 报告当前需要的 I/O 方向
// ============================================================

int SChannelSession::checkDirection()
{
  switch (state_) {
  case STATE_HANDSHAKE_SEND:
  case STATE_SHUTTING_DOWN:
    return TLS_WANT_WRITE;

  case STATE_HANDSHAKE_RECV:
    return TLS_WANT_READ;

  case STATE_CONNECTED:
    // 有待发送的缓存加密数据
    if (pendingSend_.size() > pendingSendOffset_) {
      return TLS_WANT_WRITE;
    }
    // 有握手数据待发送
    if (outBuf_.size() > 0) {
      return TLS_WANT_WRITE;
    }
    // 有已解密数据可读
    if (decBuf_.size() > 0) {
      return TLS_WANT_READ;
    }
    return TLS_WANT_READ;

  default:
    return TLS_WANT_READ;
  }
}

// ============================================================
// getRecvBufferedLength — 返回已解密缓冲区中的数据量
// ============================================================

size_t SChannelSession::getRecvBufferedLength() { return decBuf_.size(); }

// ============================================================
// getLastErrorString — 格式化最后一次 SSPI 错误
// ============================================================

std::string SChannelSession::getLastErrorString()
{
  wchar_t* buf = nullptr;
  auto rv = ::FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, lastStatus_, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<LPWSTR>(&buf), 0, nullptr);

  std::stringstream ss;
  if (rv && buf) {
    ss << wCharToUtf8(buf) << " (0x" << std::hex << lastStatus_ << ")";
    ::LocalFree(buf);
  }
  else {
    ss << "SSPI error 0x" << std::hex << lastStatus_;
  }
  return ss.str();
}

} // namespace aria2
