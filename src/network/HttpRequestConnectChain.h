/* <!-- copyright */
/*
 * aria2 - The high speed download utility
 *
 * Copyright (C) 2013 Tatsuhiro Tsujikawa
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
#ifndef HTTP_REQUEST_CONNECT_CHAIN_H
#define HTTP_REQUEST_CONNECT_CHAIN_H

#include "ControlChain.h"
#include "ConnectCommand.h"
#include "DownloadEngine.h"
#include "SocketRecvBuffer.h"
#include "HttpConnection.h"
#include "HttpRequestCommand.h"

namespace aria2 {

struct HttpRequestConnectChain : public ControlChain<ConnectCommand*> {
  HttpRequestConnectChain() {}
  virtual ~HttpRequestConnectChain() {}
  virtual int run(ConnectCommand* t, DownloadEngine* e) CXX11_OVERRIDE
  {
    auto b = std::make_shared<SocketRecvBuffer>(t->getSocket());
    auto k = std::make_shared<HttpConnection>(t->getCuid(), t->getSocket(), b);
    auto c = aria2::make_unique<HttpRequestCommand>(
        t->getCuid(), t->getRequest(), t->getFileEntry(), t->getRequestGroup(),
        k, e, t->getSocket());
    c->setProxyRequest(t->getProxyRequest());
    c->setStatus(Command::STATUS_ONESHOT_REALTIME);
    e->setNoWait(true);
    e->addCommand(std::move(c));
    return 0;
  }
};

} // namespace aria2

#endif // HTTP_REQUEST_CONNECT_CHAIN_H
