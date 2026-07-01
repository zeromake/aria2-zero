/* <!-- copyright */
/*
 * aria2 - The high speed download utility
 *
 * Copyright (C) 2014 Tatsuhiro Tsujikawa
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
#include "OpenedFileCounter.h"

#include <cassert>

#include "RequestGroupMan.h"
#include "RequestGroup.h"
#include "PieceStorage.h"
#include "DiskAdaptor.h"
#include "SimpleRandomizer.h"

namespace aria2 {

OpenedFileCounter::OpenedFileCounter(RequestGroupMan* rgman,
                                     size_t maxOpenFiles)
    : rgman_(rgman), maxOpenFiles_(maxOpenFiles), numOpenFiles_(0)
{
}

void OpenedFileCounter::ensureMaxOpenFileLimit(size_t numNewFiles)
{
  if (!rgman_) {
    return;
  }

  size_t current = numOpenFiles_.load(std::memory_order_acquire);
  while (current + numNewFiles <= maxOpenFiles_) {
    if (numOpenFiles_.compare_exchange_weak(current, current + numNewFiles,
                                            std::memory_order_release,
                                            std::memory_order_acquire)) {
      return;
    }
  }
  assert(numNewFiles <= maxOpenFiles_);

  // Re-load after CAS loop to get a fresh value for eviction calculation.
  // Another thread may have decremented numOpenFiles_ since the loop exited.
  current = numOpenFiles_.load(std::memory_order_acquire);
  if (current + numNewFiles <= maxOpenFiles_) {
    // No longer need eviction — retry the fast path
    while (current + numNewFiles <= maxOpenFiles_) {
      if (numOpenFiles_.compare_exchange_weak(current, current + numNewFiles,
                                              std::memory_order_release,
                                              std::memory_order_acquire)) {
        return;
      }
    }
  }

  size_t numClose = current + numNewFiles - maxOpenFiles_;
  size_t left = numClose;

  auto& requestGroups = rgman_->getRequestGroups();

  auto mark = std::begin(requestGroups);
  std::advance(mark, SimpleRandomizer::getInstance()->getRandomNumber(
                         requestGroups.size()));

  auto closeFun = [&left](const std::shared_ptr<RequestGroup>& group) {
    auto& ps = group->getPieceStorage();

    if (!ps) {
      return;
    }

    auto diskAdaptor = ps->getDiskAdaptor();

    if (!diskAdaptor) {
      return;
    }

    left -= diskAdaptor->tryCloseFile(left);
  };

  for (auto i = mark; i != std::end(requestGroups) && left > 0; ++i) {
    closeFun(*i);
  }

  for (auto i = std::begin(requestGroups); i != mark && left > 0; ++i) {
    closeFun(*i);
  }

  assert(left == 0);
  numOpenFiles_.fetch_add(numNewFiles - numClose, std::memory_order_release);
}

void OpenedFileCounter::reduceNumOfOpenedFile(size_t numCloseFiles)
{
  if (!rgman_) {
    return;
  }

  assert(numOpenFiles_.load(std::memory_order_relaxed) >= numCloseFiles);
  numOpenFiles_.fetch_sub(numCloseFiles, std::memory_order_release);
}

void OpenedFileCounter::deactivate() { rgman_ = nullptr; }

} // namespace aria2
