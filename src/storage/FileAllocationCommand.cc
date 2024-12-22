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
#include "FileAllocationCommand.h"
#include "FileAllocationMan.h"
#include "FileAllocationEntry.h"
#include "DownloadEngine.h"
#include "RequestGroup.h"
#include "Logger.h"
#include "LogFactory.h"
#include "message.h"
#include "prefs.h"
#include "util.h"
#include "DownloadEngine.h"
#include "DownloadContext.h"
#include "a2functional.h"
#include "RecoverableException.h"
#include "wallclock.h"
#include "RequestGroupMan.h"
#include "fmt.h"

namespace aria2 {

FileAllocationCommand::FileAllocationCommand(
    cuid_t cuid, RequestGroup* requestGroup, DownloadEngine* e,
    FileAllocationEntry* fileAllocationEntry)
    : RealtimeCommand{cuid, requestGroup, e},
      fileAllocationEntry_{fileAllocationEntry}
{
}

FileAllocationCommand::~FileAllocationCommand()
{
  getDownloadEngine()->getFileAllocationMan()->dropPickedEntry();
}

bool FileAllocationCommand::executeInternal()
{
  bool result = false;
  if (future_ == nullptr) {
    auto f = getDownloadEngine()->getThreadPool()->enqueue(
        &FileAllocationCommand::executeInternalImpl, this);
    future_ =
        aria2::make_unique<std::future<FileAllocationCommand::ExecuteResult>>(
            std::move(f));
  }
  if (future_->wait_for(100_ns) == std::future_status::ready) {
    auto execResult = future_->get();
    future_ = nullptr;
    result = std::get<1>(execResult);
    auto commands = std::move(std::get<0>(execResult));
    if (result && commands != nullptr) {
      getDownloadEngine()->addCommand(std::move(*commands));
      commands.release();
      getDownloadEngine()->setNoWait(true);
    }
  }
  if (!result) {
    getDownloadEngine()->addCommand(std::unique_ptr<Command>(this));
  }
  return result;
}

FileAllocationCommand::ExecuteResult
FileAllocationCommand::executeInternalImpl()
{
  if (getRequestGroup()->isHaltRequested()) {
    return {nullptr, true};
  }
  fileAllocationEntry_->allocateChunk();
  if (fileAllocationEntry_->finished()) {
    A2_LOG_DEBUG(fmt(
        MSG_ALLOCATION_COMPLETED,
        static_cast<long int>(std::chrono::duration_cast<std::chrono::seconds>(
                                  timer_.difference(global::wallclock()))
                                  .count()),
        getRequestGroup()->getTotalLength()));
    auto commands = aria2::make_unique<std::vector<std::unique_ptr<Command>>>();
    fileAllocationEntry_->prepareForNextAction(*commands, getDownloadEngine());
    return {std::move(commands), true};
  }
  else {
    return {nullptr, false};
  }
}

bool FileAllocationCommand::handleException(Exception& e)
{
  getRequestGroup()->setLastErrorCode(e.getErrorCode(), e.what());
  A2_LOG_ERROR_EX(fmt(MSG_FILE_ALLOCATION_FAILURE, getCuid()), e);
  A2_LOG_ERROR(
      fmt(MSG_DOWNLOAD_NOT_COMPLETE, getCuid(),
          getRequestGroup()->getDownloadContext()->getBasePath().c_str()));
  return true;
}

} // namespace aria2
