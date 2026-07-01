#include "AsyncTask.h"
#include "DownloadEngine.h"
#include "ThreadPool.h"

namespace aria2 {

void AsyncTask::submit(ThreadPool& pool, DownloadEngine* engine,
                       std::function<void()> work)
{
  finished_.store(false, std::memory_order_relaxed);
  exception_ = nullptr;

  pool.enqueueDetached([this, engine, work = std::move(work)]() {
    try {
      work();
    }
    catch (...) {
      exception_ = std::current_exception();
    }
    finished_.store(true, std::memory_order_release);
    engine->wakeupPoll();
  });
  running_ = true;
}

} // namespace aria2
