/* <!-- copyright */
/*
 * aria2 - The high speed download utility
 *
 * Copyright (C) 2006 Tatsuhiro Tsujikawa
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
#ifndef D_PEER_INTERACTION_COMMAND_H
#define D_PEER_INTERACTION_COMMAND_H

#include "PeerAbstractCommand.h"

namespace aria2 {

class RequestGroup;
class BtInteractive;
class PeerConnection;
class BtRuntime;
class PeerStorage;
class PieceStorage;
class Option;

class PeerInteractionCommand : public PeerAbstractCommand {
  COMMAND_CLASSNAME(PeerInteractionCommand)
public:
  enum Seq {
    INITIATOR_SEND_HANDSHAKE,
    INITIATOR_WAIT_HANDSHAKE,
    RECEIVER_WAIT_HANDSHAKE,
    WIRED
  };

private:
  RequestGroup* requestGroup_;

  std::shared_ptr<BtRuntime> btRuntime_;

  std::shared_ptr<PieceStorage> pieceStorage_;

  std::shared_ptr<PeerStorage> peerStorage_;

  Seq sequence_;
  std::unique_ptr<BtInteractive> btInteractive_;

  const std::shared_ptr<Option>& getOption() const;

protected:
  virtual bool executeInternal() CXX11_OVERRIDE;
  virtual bool prepareForNextPeer(time_t wait) CXX11_OVERRIDE;
  virtual void onAbort() CXX11_OVERRIDE;
  virtual void onFailure(const Exception& err) CXX11_OVERRIDE;
  virtual bool exitBeforeExecute() CXX11_OVERRIDE;

public:
  PeerInteractionCommand(
      cuid_t cuid, RequestGroup* requestGroup,
      const std::shared_ptr<Peer>& peer, DownloadEngine* e,
      const std::shared_ptr<BtRuntime>& btRuntime,
      const std::shared_ptr<PieceStorage>& pieceStorage,
      const std::shared_ptr<PeerStorage>& peerStorage,
      const std::shared_ptr<SocketCore>& s, Seq sequence,
      std::unique_ptr<PeerConnection> peerConnection = nullptr);

  virtual ~PeerInteractionCommand();
};

} // namespace aria2

#endif // D_PEER_INTERACTION_COMMAND_H
