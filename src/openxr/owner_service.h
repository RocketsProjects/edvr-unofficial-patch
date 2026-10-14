#pragma once

#include <condition_variable>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include "owner_invoke_stats.h"

namespace edvr::openxr {

// CPU-only executor for the native runtime owner. It deliberately knows
// nothing about OpenXR or Win32. start and stop are explicit: the destructor
// terminates if the caller forgot to complete an external stop, so it can
// never detach a callback or silently run user code during destruction.
class OwnerService final {
 public:
  static constexpr std::size_t kQueueCapacity = 16;
  using IdleCallback = std::function<void()>;
  using CompletionCallback = std::function<void(bool)>;

  OwnerService() = default;
  OwnerService(const OwnerService&) = delete;
  OwnerService& operator=(const OwnerService&) = delete;
  ~OwnerService() { if (worker_.joinable()||joining_) std::terminate(); }

  bool start(IdleCallback idle = {}) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_ || worker_.joinable() || stopping_ || stopCallers_ != 0) return false;
    const bool previousOutcome=finalOutcome_;
    idle_ = std::move(idle);
    stopping_ = false;
    cancellationReady_ = false; idleFailed_ = false; finalOutcome_ = true; finalizer_ = {};
    running_ = true;
    try { worker_ = std::thread(&OwnerService::run, this); }
    catch (...) { running_ = false; idle_ = {}; finalOutcome_=previousOutcome;return false; }
    return true;
  }

  // Returns false for a rejected, cancelled, or throwing invocation. An
  // executing callback always completes before this method returns. Queued
  // requests can be cancelled. Constructing the by-value std::function argument
  // may throw before entry; allocations made inside invoke are contained.
  bool invoke(std::function<void()> callback, OwnerCallKind kind = OwnerCallKind::Other) {
    if (!callback) return false;
    if (isOwner()) {
      { std::lock_guard<std::mutex> lock(mutex_); if (!running_ || stopping_) return false; }
      try { callback(); } catch (...) { return false; }
      return true;
    }
    std::shared_ptr<Request> request;
    try { request = std::make_shared<Request>(std::move(callback)); }
    catch (...) { return false; }
    request->kind = kind;
    // The stats object belongs to whoever set it (the host), and a callback may destroy that host (a fixture's cleanup runs
    // on the owner), so it is read under the lock here and read again, not trusted, after the request is done.
    OwnerInvokeStats* stats = nullptr;
    uint64_t (*now)() = nullptr;
    { std::lock_guard<std::mutex> lock(mutex_); stats = invokeStats_; now = invokeNow_; }
    const bool timed = stats && now;
    request->now = timed ? now : nullptr;
    if (timed) request->enqueuedUs = now();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!running_ || stopping_ || queue_.size() >= kQueueCapacity) return false;
      try { queue_.push_back(request); } catch (...) { return false; }
      request->behindWait = currentKind_ == uint8_t(OwnerCallKind::WaitPoses);
    }
    cv_.notify_one();
    std::unique_lock<std::mutex> waitLock(request->mutex);
    request->cv.wait(waitLock, [&] { return request->done; });
    if (timed && request->startedUs) {
      OwnerInvokeStats* current = nullptr;
      { std::lock_guard<std::mutex> lock(mutex_); current = invokeStats_; }
      if (current != stats) return request->success;   // the host went away (or was replaced) during the call
      // Written by the owner before it set done, read here under the same request mutex.
      OwnerInvokeStats::Sample sample;
      sample.kind = kind;
      sample.behindWait = request->behindWait;
      sample.queueUs = int64_t(request->startedUs) - int64_t(request->enqueuedUs);
      sample.runUs = int64_t(request->finishedUs) - int64_t(request->startedUs);
      current->note(sample);
    }
    return request->success;
  }
  // Optional per-kind accounting of invoke (owner_invoke_stats.h). Set before start(); now is the clock the instants use.
  // Without it invoke does not read the clock.
  void setInvokeStats(OwnerInvokeStats* stats, uint64_t (*now)()) {
    std::lock_guard<std::mutex> lock(mutex_);
    invokeStats_ = stats;
    invokeNow_ = now;
  }

  // Only accepted requests call completion, exactly once, after callback
  // captures are released. Cancellation reports false; rejection returns false
  // without calling completion. Completion runs outside service/request locks
  // and must not wait for this owner's progress.
  bool submit(std::function<void()> callback, CompletionCallback completion = {}) {
    if (!callback) return false;
    std::shared_ptr<Request> request;
    try { request = std::make_shared<Request>(std::move(callback), std::move(completion)); }
    catch (...) { return false; }
    bool accepted = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (running_ && !stopping_ && queue_.size() < kQueueCapacity) {
        try { queue_.push_back(request); accepted = true; } catch (...) {}
      }
    }
    if (accepted) { cv_.notify_one(); return true; }
    return false;
  }

  // First stop closes admission and owns the finalizer; later calls cannot
  // replace it. Queued requests are cancelled before finalization. Active
  // work finishes, then the finalizer runs on the owner before the join.
  // Owner-thread calls request stop but return false; an external caller must
  // still join. No callback is interrupted, detached or timed out.
  bool stop(std::function<void()> finalizer = {}) {
    std::unique_lock<std::mutex> lock(mutex_);
    const bool owner=isOwnerLocked();
    if(!owner)++stopCallers_;
    std::deque<std::shared_ptr<Request>> cancelled;
    if((running_||worker_.joinable())&&!stopping_) {
      stopping_=true;finalizer_=std::move(finalizer);cancelled.swap(queue_);
      // Captures may have destructors. Release them without the service mutex,
      // before reporting cancellation and allowing final resource cleanup.
      lock.unlock();
      for(auto& request:cancelled){request->callback={};complete(request,false);finish(request,false);}
      cancelled.clear();
      lock.lock();cancellationReady_=true;cv_.notify_all();
    }
    if(owner)return false;
    auto done = [&] { --stopCallers_; cv_.notify_all(); };
    if (!worker_.joinable() && !running_) { const bool outcome=finalOutcome_; done(); return outcome; }
    if (joining_) {
      cv_.wait(lock, [&] { return !joining_ && !worker_.joinable(); });
      const bool outcome=finalOutcome_; done(); return outcome;
    }
    joining_ = true;
    std::thread joined=std::move(worker_);
    cv_.notify_all();
    lock.unlock();
    if (joined.joinable()) joined.join();
    lock.lock();
    running_ = false; stopping_ = false; joining_ = false; ownerId_ = {};
    const bool outcome=finalOutcome_;done();return outcome;
  }

  bool isOwner() const { std::lock_guard<std::mutex> lock(mutex_); return isOwnerLocked(); }
  bool running() const { std::lock_guard<std::mutex> lock(mutex_); return running_ && !stopping_; }
  std::size_t pending() const { std::lock_guard<std::mutex> lock(mutex_); return queue_.size(); }

 private:
  struct Request {
    explicit Request(std::function<void()> fn, CompletionCallback done = {})
        : callback(std::move(fn)), completion(std::move(done)) {}
    std::function<void()> callback; CompletionCallback completion;
    std::mutex mutex; std::condition_variable cv;
    bool done = false, success = false;
    // Timing for invoke's stats: the kind and the caller's enqueue instant are set before the push; the owner sets
    // startedUs and finishedUs before finish(). behindWait is the caller's, set under the service mutex at the push.
    OwnerCallKind kind = OwnerCallKind::Other;
    uint64_t enqueuedUs = 0, startedUs = 0, finishedUs = 0;
    uint64_t (*now)() = nullptr;   // set by invoke before the push; null when this request is not timed
    bool behindWait = false;
  };
  static constexpr uint8_t kIdleKind = 0xFF;

  bool isOwnerLocked() const { return ownerId_ == std::this_thread::get_id(); }
  static void finish(const std::shared_ptr<Request>& request, bool success) {
    std::lock_guard<std::mutex> lock(request->mutex);
    request->success = success; request->done = true; request->cv.notify_one();
  }
  static void complete(const std::shared_ptr<Request>& request, bool success) {
    auto completion = std::move(request->completion);
    if (completion) { try { completion(success); } catch (...) {} }
  }
  void run() {
    { std::lock_guard<std::mutex> lock(mutex_); ownerId_ = std::this_thread::get_id(); }
    auto lastIdle = std::chrono::steady_clock::now();
    for (;;) {
      std::shared_ptr<Request> request;
      bool idle=false;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(5), [&] { return stopping_ || !queue_.empty(); });
        if(stopping_) {
          cv_.wait(lock,[&]{return cancellationReady_;});
          auto finalizer = std::move(finalizer_);
          lock.unlock();
          bool outcome = true; if (finalizer) { try { finalizer(); } catch (...) { outcome = false; } }
          finalizer={};
          // Clear idle captures on this owner, outside the service mutex.
          idle_={};
          lock.lock();finalOutcome_=outcome&&!idleFailed_;ownerId_={};cv_.notify_all();break;
        }
        if (!queue_.empty()) { request = queue_.front(); queue_.pop_front(); currentKind_ = uint8_t(request->kind); }
        else idle=bool(idle_);
      }
      if (request) {
        const bool timed = request->now != nullptr;
        if (timed) request->startedUs = request->now();
        bool success = true; try { request->callback(); } catch (...) { success = false; }
        request->callback={};
        if (timed) request->finishedUs = request->now();
        { std::lock_guard<std::mutex> lock(mutex_); currentKind_ = kIdleKind; }
        complete(request, success);
        finish(request, success);
        auto now = std::chrono::steady_clock::now();
        if (idle_ && now - lastIdle >= std::chrono::milliseconds(5)) {
          bool between=false;
          { std::lock_guard<std::mutex> lock(mutex_); between=!stopping_; }
          if (between) { try { idle_(); } catch (...) {idleFailed_=true;} lastIdle = std::chrono::steady_clock::now(); }
        }
      } else if (idle) {
        try { idle_(); } catch (...) {idleFailed_=true;}
        lastIdle = std::chrono::steady_clock::now();
      }
    }
  }

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::shared_ptr<Request>> queue_;
  std::thread worker_;
  IdleCallback idle_;
  std::thread::id ownerId_{};
  bool running_ = false, stopping_ = false, joining_ = false;
  bool cancellationReady_ = false, idleFailed_ = false, finalOutcome_ = true;
  std::function<void()> finalizer_;
  unsigned stopCallers_ = 0;
  OwnerInvokeStats* invokeStats_ = nullptr;
  uint64_t (*invokeNow_)() = nullptr;
  uint8_t currentKind_ = kIdleKind; // the kind of the job the owner is running, under mutex_
};

} // namespace edvr::openxr
