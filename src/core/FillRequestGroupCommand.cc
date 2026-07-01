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
#include "FillRequestGroupCommand.h"
#include "DownloadEngine.h"
#include "RequestGroupMan.h"
#include "RequestGroup.h"
#include "DiskAdaptor.h"
#include "File.h"
#include "BufferedFile.h"
#include "RecoverableException.h"
#include "message.h"
#include "Logger.h"
#include "LogFactory.h"
#include "DownloadContext.h"
#include "fmt.h"
#include "wallclock.h"

namespace aria2 {

namespace {
void writeControlFileSnapshots(
    const std::vector<ControlFileSaveEntry>& entries)
{
  for (auto& cf : entries) {
    auto temp = cf.filePath + "__temp";
    {
      BufferedFile fp(temp.c_str(), IOFile::WRITE);
      if (!fp) {
        A2_LOG_ERROR(fmt(EX_SEGMENT_FILE_WRITE, cf.filePath.c_str()));
        continue;
      }
      if (fp.write(cf.data.data(), cf.data.size()) != cf.data.size()) {
        A2_LOG_ERROR(fmt(EX_SEGMENT_FILE_WRITE, cf.filePath.c_str()));
        File(temp).remove();
        continue;
      }
      if (fp.close() != 0) {
        A2_LOG_ERROR(fmt(EX_SEGMENT_FILE_WRITE, cf.filePath.c_str()));
        File(temp).remove();
        continue;
      }
    }
    if (!File(temp).renameTo(cf.filePath)) {
      A2_LOG_ERROR(fmt(EX_SEGMENT_FILE_WRITE, cf.filePath.c_str()));
    }
  }
}
} // namespace

FillRequestGroupCommand::FillRequestGroupCommand(cuid_t cuid, DownloadEngine* e)
    : Command(cuid), e_(e)
{
  setStatusRealtime();
}

FillRequestGroupCommand::~FillRequestGroupCommand() = default;

void FillRequestGroupCommand::handleAsyncCompletion()
{
  if (asyncTask_.hasException()) {
    try {
      asyncTask_.rethrowIfException();
    }
    catch (RecoverableException& ex) {
      A2_LOG_ERROR_EX(EX_EXCEPTION_CAUGHT, ex);
    }
    catch (std::exception& ex) {
      A2_LOG_ERROR(fmt("Async cleanup error: %s", ex.what()));
    }
    catch (...) {
      A2_LOG_ERROR("Unknown async cleanup error");
    }
  }
  for (auto& wg : activeGroupClears_) {
    if (auto g = wg.lock()) {
      g->setAsyncCleanupPending(false);
    }
  }
  activeGroupClears_.clear();
}

void FillRequestGroupCommand::drainPendingSynchronously()
{
  for (auto& a : pendingBatch_.adaptors) {
    try {
      a->flushOSBuffers();
    }
    catch (RecoverableException& ex) {
      A2_LOG_ERROR_EX(EX_EXCEPTION_CAUGHT, ex);
    }
    try {
      a->closeFile();
    }
    catch (RecoverableException& ex) {
      A2_LOG_ERROR_EX(EX_EXCEPTION_CAUGHT, ex);
    }
  }
  writeControlFileSnapshots(pendingBatch_.controlFilesToSave);
  for (auto& path : pendingBatch_.controlFilesToRemove) {
    File(path).remove();
  }
  for (auto& wg : pendingBatch_.groupsToClearPending) {
    if (auto g = wg.lock()) {
      g->setAsyncCleanupPending(false);
    }
  }
  pendingBatch_ = GroupCleanupBatch{};
}

void FillRequestGroupCommand::trySubmitPending()
{
  if (pendingBatch_.empty() || asyncTask_.isRunning()) {
    return;
  }
  activeGroupClears_ = std::move(pendingBatch_.groupsToClearPending);
  pendingBatch_.groupsToClearPending.clear();

  asyncTask_.submit(
      *e_->getThreadPool(), e_,
      [adaptors = std::move(pendingBatch_.adaptors),
       controlSaves = std::move(pendingBatch_.controlFilesToSave),
       controlFiles = std::move(pendingBatch_.controlFilesToRemove)] {
        for (auto& a : adaptors) {
          try {
            a->flushOSBuffers();
          }
          catch (RecoverableException& ex) {
            A2_LOG_ERROR_EX(EX_EXCEPTION_CAUGHT, ex);
          }
          try {
            a->closeFile();
          }
          catch (RecoverableException& ex) {
            A2_LOG_ERROR_EX(EX_EXCEPTION_CAUGHT, ex);
          }
        }
        writeControlFileSnapshots(controlSaves);
        for (auto& path : controlFiles) {
          File(path).remove();
        }
      });
  pendingBatch_.adaptors.clear();
  pendingBatch_.controlFilesToSave.clear();
  pendingBatch_.controlFilesToRemove.clear();
}

bool FillRequestGroupCommand::execute()
{
  if (e_->isHaltRequested()) {
    if (asyncTask_.isRunning() && !asyncTask_.checkFinished()) {
      e_->addRoutineCommand(std::unique_ptr<Command>(this));
      return false;
    }
    handleAsyncCompletion();
    drainPendingSynchronously();
    return true;
  }

  if (asyncTask_.isRunning() && asyncTask_.checkFinished()) {
    handleAsyncCompletion();
  }

  auto& rgman = e_->getRequestGroupMan();
  if (rgman->queueCheckRequested()) {
    GroupCleanupBatch batch;
    while (rgman->queueCheckRequested()) {
      try {
        // During adding RequestGroup,
        // RequestGroupMan::requestQueueCheck() might be called, so
        // first clear it here.
        rgman->clearQueueCheck();
        rgman->fillRequestGroupFromReserver(e_, batch);
      }
      catch (RecoverableException& ex) {
        A2_LOG_ERROR_EX(EX_EXCEPTION_CAUGHT, ex);
        // Re-request queue check to fulfill the requests of all
        // downloads, some might come after this exception.
        rgman->requestQueueCheck();
      }
    }
    pendingBatch_.mergeFrom(batch);
  }

  trySubmitPending();

  if (rgman->downloadFinished()) {
    if (asyncTask_.isRunning() && !asyncTask_.checkFinished()) {
      e_->addRoutineCommand(std::unique_ptr<Command>(this));
      return false;
    }
    handleAsyncCompletion();
    drainPendingSynchronously();
    return true;
  }

  e_->addRoutineCommand(std::unique_ptr<Command>(this));

  if (rgman->getOptimizeConcurrentDownloads()) {
    const auto& now = global::wallclock();
    if (std::chrono::duration_cast<std::chrono::seconds>(
            lastExecTime.difference(now)) >= 1_s) {
      lastExecTime = now;
      rgman->requestQueueCheck();
    }
  }

  return false;
}

} // namespace aria2
