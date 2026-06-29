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
#include "DownloadContext.h"
#include "a2functional.h"
#include "RecoverableException.h"
#include "DlAbortEx.h"
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
  // AsyncTask 析构函数会自动等待工作线程完成
  getDownloadEngine()->getFileAllocationMan()->dropPickedEntry();
}

bool FileAllocationCommand::executeInternal()
{
  if (getRequestGroup()->isHaltRequested()) {
    // 异步任务仍在执行时不直接返回，避免析构自旋阻塞主循环
    if (asyncTask_.isRunning() && !asyncTask_.checkFinished()) {
      getDownloadEngine()->addCommand(std::unique_ptr<Command>(this));
      return false;
    }
    return true;
  }

  // 提交 allocateChunk + flushIOAfterAllocation 到 ThreadPool
  if (!asyncTask_.isRunning()) {
    asyncTask_.submit(*getDownloadEngine()->getThreadPool(),
      getDownloadEngine(),
      [this]{ executeInWorkerThread(); });
  }

  // 主线程检查工作线程是否完成
  if (asyncTask_.checkFinished()) {
    // 将非 RecoverableException 转换为 DlAbortEx，
    // 确保 RealtimeCommand::execute() 的 catch 能捕获
    if (asyncTask_.hasException()) {
      try {
        asyncTask_.rethrowIfException();
      }
      catch (RecoverableException&) {
        throw;
      }
      catch (std::exception& e) {
        throw DL_ABORT_EX(e.what());
      }
      catch (...) {
        throw DL_ABORT_EX("unknown error in file allocation");
      }
    }

    // 异步完成后再次检查 halt，避免为已取消的下载创建后续命令
    if (getRequestGroup()->isHaltRequested()) {
      return true;
    }

    if (allocationFinished_) {
      // 主线程：创建后续命令、操作引擎状态（无阻塞 I/O）
      std::vector<std::unique_ptr<Command>> commands;
      fileAllocationEntry_->prepareForNextAction(commands,
                                                 getDownloadEngine());
      getDownloadEngine()->addCommand(std::move(commands));
      getDownloadEngine()->setNoWait(true);
      return true;
    }
  }

  getDownloadEngine()->addCommand(std::unique_ptr<Command>(this));
  return false;
}

// 工作线程执行：纯磁盘 I/O，不访问 DownloadEngine 共享状态
void FileAllocationCommand::executeInWorkerThread()
{
  if (getRequestGroup()->isHaltRequested()) {
    allocationFinished_ = true;
    return;
  }
  fileAllocationEntry_->allocateChunk();
  if (fileAllocationEntry_->finished()) {
    A2_LOG_DEBUG(fmt(
        MSG_ALLOCATION_COMPLETED,
        static_cast<long int>(std::chrono::duration_cast<std::chrono::seconds>(
                                  timer_.difference(global::wallclock()))
                                  .count()),
        getRequestGroup()->getTotalLength()));
    // 分配完成后执行阻塞 I/O（saveControlFile、文件 close/reopen）
    fileAllocationEntry_->flushIOAfterAllocation();
    allocationFinished_ = true;
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
