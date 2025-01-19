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

#include "WinTLSContext.h"

#include <cassert>
#include <sstream>
#include <versionhelpers.h>

#include "BufferedFile.h"
#include "LogFactory.h"
#include "Logger.h"
#include "fmt.h"
#include "message.h"
#include "util.h"

#ifndef SP_PROT_TLS1_1_CLIENT
#  define SP_PROT_TLS1_1_CLIENT 0x00000200
#endif
#ifndef SP_PROT_TLS1_1_SERVER
#  define SP_PROT_TLS1_1_SERVER 0x00000100
#endif
#ifndef SP_PROT_TLS1_2_CLIENT
#  define SP_PROT_TLS1_2_CLIENT 0x00000800
#endif
#ifndef SP_PROT_TLS1_2_SERVER
#  define SP_PROT_TLS1_2_SERVER 0x00000400
#endif

#ifndef SCH_USE_STRONG_CRYPTO
#  define SCH_USE_STRONG_CRYPTO 0x00400000
#endif

#define WEAK_CIPHER_BITS 56
#define STRONG_CIPHER_BITS 128

namespace aria2 {

WinTLSContext::WinTLSContext(TLSSessionSide side, TLSVersion ver)
    : side_(side), store_(0), credentialsFlags_(0), enabled_protocols_(0)
{
  if (side_ == TLS_CLIENT) {
    switch (ver) {
    case TLS_PROTO_TLS11:
      enabled_protocols_ |= SP_PROT_TLS1_1_CLIENT;
    // fall through
    case TLS_PROTO_TLS12:
      enabled_protocols_ |= SP_PROT_TLS1_2_CLIENT;
    // fall through
    case TLS_PROTO_TLS13:
      if (isTLS13Supported()) enabled_protocols_ |= SP_PROT_TLS1_3_CLIENT;
      break;
    default:
      assert(0);
      abort();
    }
  }
  else {
    switch (ver) {
    case TLS_PROTO_TLS11:
      enabled_protocols_ |= SP_PROT_TLS1_1_SERVER;
    // fall through
    case TLS_PROTO_TLS12:
      enabled_protocols_ |= SP_PROT_TLS1_2_SERVER;
      // fall through
    case TLS_PROTO_TLS13:
      if (isTLS13Supported()) enabled_protocols_ |= SP_PROT_TLS1_3_SERVER;
      break;
    default:
      assert(0);
      abort();
    }
  }
  setVerifyPeer(side_ == TLS_CLIENT);
}

bool WinTLSContext::isTLS13Supported()
{
  return false;
}

TLSContext* TLSContext::make(TLSSessionSide side, TLSVersion ver)
{
  return new WinTLSContext(side, ver);
}

const char* TLSContext::name() { return "WinTLS"; }

WinTLSContext::~WinTLSContext()
{
  if (store_) {
    ::CertCloseStore(store_, 0);
    store_ = 0;
  }
}

bool WinTLSContext::getVerifyPeer() const
{
  return credentialsFlags_ & SCH_CRED_AUTO_CRED_VALIDATION;
}

void WinTLSContext::setVerifyPeer(bool verify)
{
  cred_.reset();

  // Never automatically push any client or server certs. We'll do cert setup
  // ourselves.
  DWORD dwFlags = 0;
  if (isTLS13Supported()) {
    dwFlags = SCH_CRED_NO_DEFAULT_CREDS | SCH_USE_STRONG_CRYPTO;
  } else {
    dwFlags = SCH_CRED_NO_DEFAULT_CREDS;
      // Enable strong crypto if we already set a minimum cipher streams.
      // This might actually require even stronger algorithms, which is a good
      // thing.
      // dwFlags |= SCH_USE_STRONG_CRYPTO;
  }
  if (side_ != TLS_CLIENT || !verify) {
    // No verification for servers and if user explicitly requested it
    dwFlags |=
        SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_IGNORE_NO_REVOCATION_CHECK |
        SCH_CRED_IGNORE_REVOCATION_OFFLINE | SCH_CRED_NO_SERVERNAME_CHECK;
    return;
  }

  // Verify other side's cert chain.
  dwFlags |= SCH_CRED_AUTO_CRED_VALIDATION |
                          SCH_CRED_REVOCATION_CHECK_CHAIN |
                          SCH_CRED_IGNORE_NO_REVOCATION_CHECK;
  credentialsFlags_ = dwFlags;
}

CredHandle* WinTLSContext::getCredHandle()
{
  if (cred_) {
    return cred_.get();
  }

  TimeStamp ts;
  cred_.reset(new CredHandle());
  CRYPTO_SETTINGS crypto_settings_[1];
  TLS_PARAMETERS tls_parameters_;
  SCH_CREDENTIALS credentials13_;

  SCHANNEL_CRED credentials_;

  if (isTLS13Supported()) {
    A2_LOG_DEBUG("WinTLS: TLS 1.3 is supported");
    memset(&tls_parameters_, 0, sizeof(TLS_PARAMETERS));
    memset(&crypto_settings_, 0, sizeof(CRYPTO_SETTINGS));
    memset(&credentials13_, 0, sizeof(SCH_CREDENTIALS));
    tls_parameters_.pDisabledCrypto = crypto_settings_;
    tls_parameters_.cDisabledCrypto = (DWORD)0;
    credentials13_.dwVersion = SCH_CREDENTIALS_VERSION;
    credentials13_.dwFlags = SCH_USE_STRONG_CRYPTO;
    credentials13_.pTlsParameters = &tls_parameters_;
    credentials13_.cTlsParameters = 1;
    credentials13_.pTlsParameters->grbitDisabledProtocols = (DWORD)~enabled_protocols_;
  } else {
    memset(&credentials_, 0, sizeof(credentials_));
    credentials_.dwVersion = SCHANNEL_CRED_VERSION;
    credentials_.grbitEnabledProtocols = 0;
    credentials_.grbitEnabledProtocols = enabled_protocols_;
  }

  const CERT_CONTEXT* ctx = nullptr;
  if (store_) {
    ctx = ::CertEnumCertificatesInStore(store_, nullptr);
    if (!ctx) {
      throw DL_ABORT_EX("Failed to load certificate");
    }
    if (isTLS13Supported()) {
      credentials13_.cCreds = 1;
      credentials13_.paCred = &ctx;
    } else {
      credentials_.cCreds = 1;
      credentials_.paCred = &ctx;
    }
  }
  else {
    if (isTLS13Supported()) {
      credentials13_.cCreds = 0;
      credentials13_.paCred = nullptr;
    } else {
      credentials_.cCreds = 0;
      credentials_.paCred = nullptr;
    }
  }
  void* credentials = isTLS13Supported() ? (void*)&credentials13_ : (void*)&credentials_;
  SECURITY_STATUS status = ::AcquireCredentialsHandleW(
      nullptr, (SEC_WCHAR*)UNISP_NAME_W,
      side_ == TLS_CLIENT ? SECPKG_CRED_OUTBOUND : SECPKG_CRED_INBOUND, nullptr,
      credentials, nullptr, nullptr, cred_.get(), &ts);
  if (ctx) {
    ::CertFreeCertificateContext(ctx);
  }
  if (status != SEC_E_OK) {
    cred_.reset();
    throw DL_ABORT_EX("Failed to initialize WinTLS context handle");
  }
  return cred_.get();
}

bool WinTLSContext::addCredentialFile(const std::string& certfile,
                                      const std::string& keyfile)
{
  std::stringstream ss;
  BufferedFile(certfile.c_str(), "rb").transfer(ss);
  auto data = ss.str();
  CRYPT_DATA_BLOB blob = {(DWORD)data.length(), (BYTE*)data.c_str()};
  if (!::PFXIsPFXBlob(&blob)) {
    A2_LOG_ERROR("Not a valid PKCS12 file");
    return false;
  }
  HCERTSTORE store =
      ::PFXImportCertStore(&blob, L"", CRYPT_EXPORTABLE | CRYPT_USER_KEYSET);
  if (!store_) {
    store = ::PFXImportCertStore(&blob, nullptr,
                                 CRYPT_EXPORTABLE | CRYPT_USER_KEYSET);
  }
  if (!store) {
    A2_LOG_ERROR("Failed to import PKCS12 store");
    return false;
  }
  auto ctx = ::CertEnumCertificatesInStore(store, nullptr);
  if (!ctx) {
    A2_LOG_ERROR("PKCS12 file does not contain certificates");
    ::CertCloseStore(store, 0);
    return false;
  }
  ::CertFreeCertificateContext(ctx);

  if (store_) {
    ::CertCloseStore(store_, 0);
  }
  store_ = store;
  cred_.reset();

  return true;
}

bool WinTLSContext::addTrustedCACertFile(const std::string& certfile)
{
  A2_LOG_WARN("TLS CA bundle files are not supported. "
              "The system trust store will be used.");
  return false;
}

} // namespace aria2
