#ifndef D_ASYNC_TASK_H
#define D_ASYNC_TASK_H

#include "common.h"

#include <atomic>
#include <exception>
#include <functional>
#include <thread>

class ThreadPool;

namespace aria2 {

class DownloadEngine;

// ThreadPool 异步任务封装。
// 管理 工作线程→主线程 的状态同步，包括完成标记、异常传递和析构等待。
// 所有公开方法（submit 除外）仅由主线程调用。
class AsyncTask {
  bool running_ = false;
  std::atomic<bool> finished_{false};
  std::exception_ptr exception_;

public:
  AsyncTask() = default;
  ~AsyncTask() { waitForCompletion(); }

  AsyncTask(const AsyncTask&) = delete;
  AsyncTask& operator=(const AsyncTask&) = delete;

  bool isRunning() const { return running_; }

  // 提交任务到线程池，完成后自动唤醒主线程 EventPoll
  void submit(ThreadPool& pool, DownloadEngine* engine,
              std::function<void()> work);

  // 检查任务是否完成。返回 true 时重置内部状态，可安全发起下一次 submit。
  bool checkFinished()
  {
    if (!finished_.load(std::memory_order_acquire)) {
      return false;
    }
    running_ = false;
    finished_.store(false, std::memory_order_relaxed);
    return true;
  }

  bool hasException() const { return exception_ != nullptr; }

  // 有异常则重抛。仅在 checkFinished() 返回 true 后调用。
  void rethrowIfException()
  {
    if (exception_) {
      auto ex = exception_;
      exception_ = nullptr;
      std::rethrow_exception(ex);
    }
  }

  // 阻塞等待工作线程完成。仅用于析构路径。
  void waitForCompletion()
  {
    if (running_) {
      while (!finished_.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      running_ = false;
    }
  }
};

} // namespace aria2

#endif // D_ASYNC_TASK_H
