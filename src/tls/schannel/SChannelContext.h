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

#ifndef D_SCHANNEL_TLS_CONTEXT_H
#define D_SCHANNEL_TLS_CONTEXT_H

#include "common.h"
#include "config.h"

#include <string>

#include <windows.h>
#include <subauth.h>
#include <security.h>
#include <schnlsp.h>

#include "TLSContext.h"

namespace aria2 {

class SChannelContext : public TLSContext {
public:
  // side: 客户端或服务端（本实现仅支持客户端）
  // minVer: 允许的最低 TLS 版本，会启用该版本及以上所有协议
  SChannelContext(TLSSessionSide side, TLSVersion minVer);
  virtual ~SChannelContext();

  // 导入 PKCS12 (.pfx) 客户端证书。keyfile 参数在 SChannel 中不使用。
  virtual bool addCredentialFile(const std::string& certfile,
                                 const std::string& keyfile) CXX11_OVERRIDE;

  // SChannel 使用 Windows 系统证书存储，直接返回 true
  virtual bool addSystemTrustedCACerts() CXX11_OVERRIDE { return true; }

  // SChannel 不支持自定义 CA 文件，记录警告
  virtual bool addTrustedCACertFile(const std::string& certfile) CXX11_OVERRIDE;

  virtual bool good() const CXX11_OVERRIDE { return true; }

  virtual TLSSessionSide getSide() const CXX11_OVERRIDE { return side_; }

  virtual bool getVerifyPeer() const CXX11_OVERRIDE { return verifyPeer_; }

  virtual void setVerifyPeer(bool verify) CXX11_OVERRIDE;

  // ---- SChannelSession 调用的接口 ----

  // 获取 SSPI 凭据句柄（惰性初始化，首次调用时创建）
  CredHandle* getCredHandle();

  // 运行时检测当前 Windows 版本是否原生支持 TLS 1.3
  // (Windows Server 2022 / Windows 11, Build 20348+)
  static bool isTLS13Supported();

private:
  // 根据当前 verifyPeer_ 配置构建 SChannel 凭据标志
  DWORD buildCredFlags() const;

  TLSSessionSide side_;
  bool verifyPeer_;
  DWORD enabledProtocols_;  // SP_PROT_TLS1_X_CLIENT 掩码
  HCERTSTORE store_;        // PKCS12 证书存储（可为 nullptr）

  // SSPI 凭据句柄（由 getCredHandle() 惰性初始化）
  CredHandle credHandle_;
  bool credValid_;
};

} // namespace aria2

#endif // D_SCHANNEL_TLS_CONTEXT_H
