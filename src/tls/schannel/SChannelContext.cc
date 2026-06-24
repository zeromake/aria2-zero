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

#include "SChannelContext.h"

#include <cassert>
#include <sstream>

#include "BufferedFile.h"
#include "DlAbortEx.h"
#include "LogFactory.h"
#include "Logger.h"
#include "fmt.h"
#include "message.h"

// ---- 旧版 SDK 可能缺少的协议常量 ----
#ifndef SP_PROT_SSL2_CLIENT
#  define SP_PROT_SSL2_CLIENT 0x00000008
#endif
#ifndef SP_PROT_SSL3_CLIENT
#  define SP_PROT_SSL3_CLIENT 0x00000020
#endif
#ifndef SP_PROT_TLS1_0_CLIENT
#  define SP_PROT_TLS1_0_CLIENT 0x00000080
#endif
#ifndef SP_PROT_TLS1_1_CLIENT
#  define SP_PROT_TLS1_1_CLIENT 0x00000200
#endif
#ifndef SP_PROT_TLS1_2_CLIENT
#  define SP_PROT_TLS1_2_CLIENT 0x00000800
#endif
#ifndef SP_PROT_TLS1_3_CLIENT
#  define SP_PROT_TLS1_3_CLIENT 0x00002000
#endif
#ifndef SCH_USE_STRONG_CRYPTO
#  define SCH_USE_STRONG_CRYPTO 0x00400000
#endif
#ifndef PKCS12_NO_PERSIST_KEY
#  define PKCS12_NO_PERSIST_KEY 0x00008000
#endif
#ifndef CERT_FIND_HAS_PRIVATE_KEY
#  define CERT_FIND_HAS_PRIVATE_KEY 0x00150000
#endif

// 所有已知的旧版/弱协议掩码（SSLv2 + SSLv3 + TLS 1.0 + TLS 1.1）。
// 用于 SCH_CREDENTIALS.grbitDisabledProtocols，只禁用已知弱协议，
// 避免对保留位取反导致 SChannel 行为异常（0x80090301 SEC_E_INVALID_HANDLE）。
static const DWORD kLegacyProtocols =
    SP_PROT_SSL2_CLIENT | SP_PROT_SSL3_CLIENT | SP_PROT_TLS1_0_CLIENT |
    SP_PROT_TLS1_1_CLIENT;

namespace aria2 {

// ============================================================
// TLSContext 工厂方法 — 编译时只有一个 TLS 后端被链接
// ============================================================
TLSContext* TLSContext::make(TLSSessionSide side, TLSVersion ver)
{
  return new SChannelContext(side, ver);
}

const char* TLSContext::name() { return "SChannel"; }

// ============================================================
// 构造 / 析构
// ============================================================

SChannelContext::SChannelContext(TLSSessionSide side, TLSVersion minVer)
    : side_(side),
      verifyPeer_(side == TLS_CLIENT),
      enabledProtocols_(0),
      store_(nullptr),
      credValid_(false)
{
  memset(&credHandle_, 0, sizeof(credHandle_));

  // minVer 是最低版本，通过 fall-through 累积启用该版本及以上所有协议。
  // 例如 TLS_PROTO_TLS11 → 启用 TLS 1.1 + 1.2 + 1.3
  switch (minVer) {
  case TLS_PROTO_TLS11:
    enabledProtocols_ |= SP_PROT_TLS1_1_CLIENT;
    // fall through
  case TLS_PROTO_TLS12:
    enabledProtocols_ |= SP_PROT_TLS1_2_CLIENT;
    // fall through
  case TLS_PROTO_TLS13:
    if (isTLS13Supported()) {
      enabledProtocols_ |= SP_PROT_TLS1_3_CLIENT;
    }
    break;
  default:
    break;
  }
}

SChannelContext::~SChannelContext()
{
  if (credValid_) {
    ::FreeCredentialsHandle(&credHandle_);
    credValid_ = false;
  }
  if (store_) {
    ::CertCloseStore(store_, 0);
    store_ = nullptr;
  }
}

// ============================================================
// TLS 1.3 运行时检测
// ============================================================

bool SChannelContext::isTLS13Supported()
{
  // Windows Server 2022 / Windows 11 (Build 20348+) 原生支持 TLS 1.3。
  // 早期 Windows 10 1809 的 TLS 1.3 实现不稳定，不启用。
  // 参考 curl: curlx_verify_windows_version(10, 0, 20348, ...)
  static int cached = -1;
  if (cached >= 0) {
    return cached != 0;
  }

  OSVERSIONINFOEXW osvi = {};
  osvi.dwOSVersionInfoSize = sizeof(osvi);
  osvi.dwMajorVersion = 10;
  osvi.dwMinorVersion = 0;
  osvi.dwBuildNumber = 20348;

  DWORDLONG condMask = 0;
  VER_SET_CONDITION(condMask, VER_MAJORVERSION, VER_GREATER_EQUAL);
  VER_SET_CONDITION(condMask, VER_MINORVERSION, VER_GREATER_EQUAL);
  VER_SET_CONDITION(condMask, VER_BUILDNUMBER, VER_GREATER_EQUAL);

  cached = ::VerifyVersionInfoW(
               &osvi,
               VER_MAJORVERSION | VER_MINORVERSION | VER_BUILDNUMBER,
               condMask)
               ? 1
               : 0;

  if (cached) {
    A2_LOG_INFO("SChannel: TLS 1.3 is supported on this system");
  }
  else {
    A2_LOG_DEBUG("SChannel: TLS 1.3 is not supported"
                 " (requires Build 20348+)");
  }

  return cached != 0;
}

// ============================================================
// 验证配置
// ============================================================

void SChannelContext::setVerifyPeer(bool verify)
{
  if (verifyPeer_ == verify) {
    return;
  }
  verifyPeer_ = verify;

  // 验证配置变更后必须重新创建凭据
  if (credValid_) {
    ::FreeCredentialsHandle(&credHandle_);
    credValid_ = false;
  }
}

DWORD SChannelContext::buildCredFlags() const
{
  // 不自动使用客户端证书（由 addCredentialFile 手动导入）
  DWORD flags = SCH_CRED_NO_DEFAULT_CREDS;

  if (verifyPeer_) {
    // 客户端 + 验证模式:
    //   - 自动验证服务端证书链
    //   - 检查吊销状态（容忍无法联系吊销服务器的情况）
    flags |= SCH_CRED_AUTO_CRED_VALIDATION | SCH_CRED_REVOCATION_CHECK_CHAIN |
             SCH_CRED_IGNORE_NO_REVOCATION_CHECK;
  }
  else {
    // 不验证模式: 手动验证 + 忽略所有检查
    flags |= SCH_CRED_MANUAL_CRED_VALIDATION |
             SCH_CRED_IGNORE_NO_REVOCATION_CHECK |
             SCH_CRED_IGNORE_REVOCATION_OFFLINE |
             SCH_CRED_NO_SERVERNAME_CHECK;
  }

  return flags;
}

// ============================================================
// 凭据获取（惰性初始化）
// ============================================================

CredHandle* SChannelContext::getCredHandle()
{
  if (credValid_) {
    return &credHandle_;
  }

  DWORD flags = buildCredFlags();
  TimeStamp ts;
  SECURITY_STATUS status;

  // 准备客户端证书（如果有）— 优先查找带私钥的证书（Windows 8+），
  // 避免在含多个证书的 PKCS12 中误选 CA 证书。参考 curl get_client_cert()。
  const CERT_CONTEXT* clientCert = nullptr;
  if (store_) {
    clientCert = ::CertFindCertificateInStore(
        store_, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
        CERT_FIND_HAS_PRIVATE_KEY, nullptr, nullptr);
    if (!clientCert) {
      clientCert = ::CertFindCertificateInStore(
          store_, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
          CERT_FIND_ANY, nullptr, nullptr);
    }
    if (!clientCert) {
      throw DL_ABORT_EX("SChannel: failed to get certificate from store");
    }
  }

  // 根据是否支持 TLS 1.3 选择不同的凭据结构体。
  // 参考 curl acquire_sspi_handle():
  //   SCH_CREDENTIALS (新 API): 支持 TLS 1.3，用 grbitDisabledProtocols 指定要禁用的协议
  //   SCHANNEL_CRED (旧 API): 不支持 TLS 1.3，用 grbitEnabledProtocols 直接掩码
  if (isTLS13Supported()) {
    // ---- 新版路径 (Build 20348+) ----
    A2_LOG_DEBUG(fmt("SChannel: using SCH_CREDENTIALS (TLS 1.3 capable),"
                     " enabledProtocols=0x%lx, disabledProtocols=0x%lx,"
                     " flags=0x%lx",
                     enabledProtocols_,
                     kLegacyProtocols & ~enabledProtocols_,
                     flags | SCH_USE_STRONG_CRYPTO));

    CRYPTO_SETTINGS cryptoSettings = {};
    TLS_PARAMETERS tlsParams = {};
    SCH_CREDENTIALS cred13 = {};

    tlsParams.pDisabledCrypto = &cryptoSettings;
    tlsParams.cDisabledCrypto = 0;
    // 只禁用已知的旧版弱协议中「不在启用列表里」的那些。
    // 不能对 enabledProtocols_ 直接取反：DWORD 高位有保留位，
    // 全取反后 SChannel 会拒绝使用该凭据（DecryptMessage 返回 0x80090301）。
    tlsParams.grbitDisabledProtocols = kLegacyProtocols & ~enabledProtocols_;

    cred13.dwVersion = SCH_CREDENTIALS_VERSION;
    cred13.dwFlags = flags | SCH_USE_STRONG_CRYPTO;
    cred13.pTlsParameters = &tlsParams;
    cred13.cTlsParameters = 1;

    if (clientCert) {
      cred13.cCreds = 1;
      cred13.paCred = &clientCert;
    }

    status = ::AcquireCredentialsHandleW(
        nullptr, (SEC_WCHAR*)UNISP_NAME_W, SECPKG_CRED_OUTBOUND, nullptr,
        &cred13, nullptr, nullptr, &credHandle_, &ts);
  }
  else {
    // ---- 旧版路径 (不支持 TLS 1.3) ----
    A2_LOG_DEBUG(fmt("SChannel: using SCHANNEL_CRED (no TLS 1.3),"
                     " enabledProtocols=0x%lx, flags=0x%lx",
                     enabledProtocols_, flags | SCH_USE_STRONG_CRYPTO));

    SCHANNEL_CRED cred = {};
    cred.dwVersion = SCHANNEL_CRED_VERSION;
    cred.dwFlags = flags | SCH_USE_STRONG_CRYPTO;
    cred.grbitEnabledProtocols = enabledProtocols_;

    if (clientCert) {
      cred.cCreds = 1;
      cred.paCred = &clientCert;
    }

    status = ::AcquireCredentialsHandleW(
        nullptr, (SEC_WCHAR*)UNISP_NAME_W, SECPKG_CRED_OUTBOUND, nullptr,
        &cred, nullptr, nullptr, &credHandle_, &ts);
  }

  // 释放证书上下文（不影响 store_ 中的证书）
  if (clientCert) {
    ::CertFreeCertificateContext(clientCert);
  }

  if (status != SEC_E_OK) {
    A2_LOG_ERROR(fmt("SChannel: AcquireCredentialsHandle failed,"
                     " status=0x%lx",
                     status));
    throw DL_ABORT_EX("SChannel: failed to initialize credential handle");
  }

  credValid_ = true;
  return &credHandle_;
}

// ============================================================
// 证书管理
// ============================================================

bool SChannelContext::addCredentialFile(const std::string& certfile,
                                       const std::string& keyfile)
{
  // SChannel 使用 PKCS12 (.pfx) 格式，keyfile 参数不使用。
  // 将证书文件整体读入内存后调用 PFXImportCertStore。
  (void)keyfile;

  std::stringstream ss;
  BufferedFile(certfile.c_str(), "rb").transfer(ss);
  auto data = ss.str();

  CRYPT_DATA_BLOB blob = {static_cast<DWORD>(data.length()),
                          reinterpret_cast<BYTE*>(const_cast<char*>(data.c_str()))};

  if (!::PFXIsPFXBlob(&blob)) {
    A2_LOG_ERROR("SChannel: not a valid PKCS12 file");
    return false;
  }

  // 尝试空密码导入
  HCERTSTORE store =
      ::PFXImportCertStore(&blob, L"", PKCS12_NO_PERSIST_KEY);
  if (!store) {
    // 空密码失败，尝试无密码导入
    store = ::PFXImportCertStore(&blob, nullptr,
                                 PKCS12_NO_PERSIST_KEY);
  }
  if (!store) {
    A2_LOG_ERROR("SChannel: failed to import PKCS12 cert store");
    return false;
  }

  // 验证存储中至少有一个证书
  auto ctx = ::CertEnumCertificatesInStore(store, nullptr);
  if (!ctx) {
    A2_LOG_ERROR("SChannel: PKCS12 file contains no certificate");
    ::CertCloseStore(store, 0);
    return false;
  }
  ::CertFreeCertificateContext(ctx);

  // 替换旧的证书存储
  if (store_) {
    ::CertCloseStore(store_, 0);
  }
  store_ = store;

  // 证书变更后需要重新创建凭据
  if (credValid_) {
    ::FreeCredentialsHandle(&credHandle_);
    credValid_ = false;
  }

  A2_LOG_INFO("SChannel: client certificate imported successfully");
  return true;
}

bool SChannelContext::addTrustedCACertFile(const std::string& certfile)
{
  (void)certfile;
  A2_LOG_WARN("SChannel: custom CA certificate files are not supported,"
               " using system certificate store");
  return false;
}

} // namespace aria2
