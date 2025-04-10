/* <!-- copyright */
/*
 * aria2 - The high speed download utility
 *
 * Copyright (C) 2010 Tatsuhiro Tsujikawa
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
#include "AdaptiveFileAllocationIterator.h"
#include "BinaryStream.h"
#include "SingleFileAllocationIterator.h"
#include "RecoverableException.h"
#include "LogFactory.h"
#include "Logger.h"
#include "a2functional.h"
#ifdef HAVE_FALLOCATE
#  include "FallocFileAllocationIterator.h"
#endif // HAVE_FALLOCATE

namespace aria2 {

AdaptiveFileAllocationIterator::AdaptiveFileAllocationIterator(
    BinaryStream* stream, int64_t offset, int64_t totalLength)
    : stream_(stream), offset_(offset), totalLength_(totalLength)
{
}

AdaptiveFileAllocationIterator::~AdaptiveFileAllocationIterator() = default;

void AdaptiveFileAllocationIterator::allocateChunk()
{
  if (!allocator_) {
#ifdef HAVE_FALLOCATE
    try {
      A2_LOG_DEBUG("Testing file system supports fallocate.");
      if (offset_ < totalLength_) {
        int64_t len =
            std::min(totalLength_ - offset_, static_cast<int64_t>(4_k));
        stream_->allocate(offset_, len, false);
        offset_ += len;
      }
      A2_LOG_DEBUG("File system supports fallocate.");
      allocator_ = aria2::make_unique<FallocFileAllocationIterator>(
          stream_, offset_, totalLength_);
    }
    catch (RecoverableException& e) {
      A2_LOG_DEBUG("File system does not support fallocate.");
      auto salloc = aria2::make_unique<SingleFileAllocationIterator>(
          stream_, offset_, totalLength_);
      salloc->init();
      allocator_ = std::move(salloc);
    }
#else  // !HAVE_FALLOCATE
    auto salloc = aria2::make_unique<SingleFileAllocationIterator>(
        stream_, offset_, totalLength_);
    salloc->init();
    allocator_ = std::move(salloc);
#endif // !HAVE_FALLOCATE
    allocator_->allocateChunk();
  }
  else {
    allocator_->allocateChunk();
  }
}

bool AdaptiveFileAllocationIterator::finished()
{
  if (!allocator_) {
    return offset_ == totalLength_;
  }
  else {
    return allocator_->finished();
  }
}

int64_t AdaptiveFileAllocationIterator::getCurrentLength()
{
  if (!allocator_) {
    return offset_;
  }
  else {
    return allocator_->getCurrentLength();
  }
}

int64_t AdaptiveFileAllocationIterator::getTotalLength()
{
  return totalLength_;
}

} // namespace aria2
