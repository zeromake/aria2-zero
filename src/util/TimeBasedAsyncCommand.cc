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
#include "TimeBasedAsyncCommand.h"
#include "DownloadEngine.h"
#include "LogFactory.h"
#include "Logger.h"
#include "fmt.h"
#include "wallclock.h"

namespace aria2 {

TimeBasedAsyncCommand::TimeBasedAsyncCommand(cuid_t cuid, DownloadEngine* e,
                                             std::chrono::seconds interval,
                                             bool routineCommand)
    : Command(cuid),
      e_(e),
      checkPoint_(global::wallclock()),
      interval_(std::move(interval)),
      exit_(false),
      routineCommand_(routineCommand)
{
}

// AsyncTask 析构函数会自动等待工作线程完成
TimeBasedAsyncCommand::~TimeBasedAsyncCommand() = default;

bool TimeBasedAsyncCommand::execute()
{
  preProcess();
  if (exit_.load(std::memory_order_relaxed)) {
    return true;
  }

  // 定时间隔到达且无正在执行的任务时，提交 process() 到 ThreadPool
  if (!asyncTask_.isRunning() &&
      checkPoint_.difference(global::wallclock()) >= interval_) {
    checkPoint_ = global::wallclock();
    asyncTask_.submit(*e_->getThreadPool(), e_, [this]() { process(); });
  }

  // 主线程检查工作线程是否完成
  if (asyncTask_.checkFinished()) {
    // 捕获所有异常，防止逃逸到 DownloadEngine::run() 导致崩溃
    if (asyncTask_.hasException()) {
      try {
        asyncTask_.rethrowIfException();
      }
      catch (std::exception& ex) {
        A2_LOG_ERROR(fmt("async process error: %s", ex.what()));
      }
      catch (...) {
        A2_LOG_ERROR("unknown exception in async process");
      }
      enableExit();
      return true;
    }

    // process() 中调用 enableExit() 后跳过 postProcess()，与旧行为一致
    if (exit_.load(std::memory_order_relaxed)) {
      return true;
    }
  }

  postProcess();
  if (exit_.load(std::memory_order_relaxed)) {
    return true;
  }

  if (routineCommand_) {
    e_->addRoutineCommand(std::unique_ptr<Command>(this));
  }
  else {
    e_->addCommand(std::unique_ptr<Command>(this));
  }
  return false;
}

} // namespace aria2
