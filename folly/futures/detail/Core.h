/*
 * Copyright 2015 Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <atomic>
#include <mutex>
#include <stdexcept>
#include <vector>

#include <folly/Optional.h>
#include <folly/SmallLocks.h>

#include <folly/futures/Try.h>
#include <folly/futures/Promise.h>
#include <folly/futures/Future.h>
#include <folly/Executor.h>
#include <folly/futures/detail/FSM.h>

#include <folly/io/async/Request.h>

namespace folly { namespace detail {

/*
        OnlyCallback
       /            \
  Start              Armed - Done
       \            /
         OnlyResult

This state machine is fairly self-explanatory. The most important bit is
that the callback is only executed on the transition from Armed to Done,
and that transition can happen immediately after transitioning from Only*
to Armed, if it is active (the usual case).
*/
enum class State : uint8_t {
  Start,
  OnlyResult,
  OnlyCallback,
  Armed,
  Done,
};

/// The shared state object for Future and Promise.
/// Some methods must only be called by either the Future thread or the
/// Promise thread. The Future thread is the thread that currently "owns" the
/// Future and its callback-related operations, and the Promise thread is
/// likewise the thread that currently "owns" the Promise and its
/// result-related operations. Also, Futures own interruption, Promises own
/// interrupt handlers. Unfortunately, there are things that users can do to
/// break this, and we can't detect that. However if they follow move
/// semantics religiously wrt threading, they should be ok.
///
/// It's worth pointing out that Futures and/or Promises can and usually will
/// migrate between threads, though this usually happens within the API code.
/// For example, an async operation will probably make a Promise, grab its
/// Future, then move the Promise into another thread that will eventually
/// fulfill it. With executors and via, this gets slightly more complicated at
/// first blush, but it's the same principle. In general, as long as the user
/// doesn't access a Future or Promise object from more than one thread at a
/// time there won't be any problems.
template<typename T>
class Core {
 public:
  /// This must be heap-constructed. There's probably a way to enforce that in
  /// code but since this is just internal detail code and I don't know how
  /// off-hand, I'm punting.
  Core() {}

  explicit Core(Try<T>&& t)
    : fsm_(State::OnlyResult),
      attached_(1),
      result_(std::move(t)) {}

  ~Core() {
    assert(attached_ == 0);
  }

  // not copyable
  Core(Core const&) = delete;
  Core& operator=(Core const&) = delete;

  // not movable (see comment in the implementation of Future::then)
  Core(Core&&) noexcept = delete;
  Core& operator=(Core&&) = delete;

  /// May call from any thread
  bool hasResult() const {
    switch (fsm_.getState()) {
      case State::OnlyResult:
      case State::Armed:
      case State::Done:
        assert(!!result_);
        return true;

      default:
        return false;
    }
  }

  /// May call from any thread
  bool ready() const {
    return hasResult();
  }

  /// May call from any thread
  Try<T>& getTry() {
    if (ready()) {
      return *result_;
    } else {
      throw FutureNotReady();
    }
  }

  template <typename F>
  class LambdaBufHelper {
   public:
    explicit LambdaBufHelper(F&& func) : func_(std::forward<F>(func)) {}
    void operator()(Try<T>&& t) {
      SCOPE_EXIT { this->~LambdaBufHelper(); };
      func_(std::move(t));
    }
   private:
    F func_;
  };

  /// Call only from Future thread.
  template <typename F>
  void setCallback(F func) {
    bool transitionToArmed = false;
    auto setCallback_ = [&]{
      context_ = RequestContext::saveContext();

      // Move the lambda into the Core if it fits
      if (sizeof(LambdaBufHelper<F>) <= lambdaBufSize) {
        auto funcLoc = static_cast<LambdaBufHelper<F>*>((void*)lambdaBuf_);
        new (funcLoc) LambdaBufHelper<F>(std::forward<F>(func));
        callback_ = std::ref(*funcLoc);
      } else {
        callback_ = std::move(func);
      }
    };

    FSM_START(fsm_)
      case State::Start:
        FSM_UPDATE(fsm_, State::OnlyCallback, setCallback_);
        break;

      case State::OnlyResult:
        FSM_UPDATE(fsm_, State::Armed, setCallback_);
        transitionToArmed = true;
        break;

      case State::OnlyCallback:
      case State::Armed:
      case State::Done:
        throw std::logic_error("setCallback called twice");
    FSM_END

    // we could always call this, it is an optimization to only call it when
    // it might be needed.
    if (transitionToArmed) {
      maybeCallback();
    }
  }

  /// Call only from Promise thread
  void setResult(Try<T>&& t) {
    bool transitionToArmed = false;
    auto setResult_ = [&]{ result_ = std::move(t); };
    FSM_START(fsm_)
      case State::Start:
        FSM_UPDATE(fsm_, State::OnlyResult, setResult_);
        break;

      case State::OnlyCallback:
        FSM_UPDATE(fsm_, State::Armed, setResult_);
        transitionToArmed = true;
        break;

      case State::OnlyResult:
      case State::Armed:
      case State::Done:
        throw std::logic_error("setResult called twice");
    FSM_END

    if (transitionToArmed) {
      maybeCallback();
    }
  }

  /// Called by a destructing Future (in the Future thread, by definition)
  void detachFuture() {
    activate();
    detachOne();
  }

  /// Called by a destructing Promise (in the Promise thread, by definition)
  void detachPromise() {
    // detachPromise() and setResult() should never be called in parallel
    // so we don't need to protect this.
    if (!result_) {
      setResult(Try<T>(exception_wrapper(BrokenPromise())));
    }
    detachOne();
  }

  /// May call from any thread
  void deactivate() {
    active_ = false;
  }

  /// May call from any thread
  void activate() {
    active_ = true;
    maybeCallback();
  }

  /// May call from any thread
  bool isActive() { return active_; }

  /// Call only from Future thread
  void setExecutor(Executor* x, int8_t priority) {
    folly::MSLGuard g(executorLock_);
    executor_ = x;
    priority_ = priority;
  }

  Executor* getExecutor() {
    folly::MSLGuard g(executorLock_);
    return executor_;
  }

  /// Call only from Future thread
  void raise(exception_wrapper e) {
    folly::MSLGuard guard(interruptLock_);
    if (!interrupt_ && !hasResult()) {
      interrupt_ = folly::make_unique<exception_wrapper>(std::move(e));
      if (interruptHandler_) {
        interruptHandler_(*interrupt_);
      }
    }
  }

  std::function<void(exception_wrapper const&)> getInterruptHandler() {
    folly::MSLGuard guard(interruptLock_);
    return interruptHandler_;
  }

  /// Call only from Promise thread
  void setInterruptHandler(std::function<void(exception_wrapper const&)> fn) {
    folly::MSLGuard guard(interruptLock_);
    if (!hasResult()) {
      if (interrupt_) {
        fn(*interrupt_);
      } else {
        interruptHandler_ = std::move(fn);
      }
    }
  }

 protected:
  void maybeCallback() {
    FSM_START(fsm_)
      case State::Armed:
        if (active_) {
          FSM_UPDATE2(fsm_, State::Done, []{}, [this]{ this->doCallback(); });
        }
        FSM_BREAK

      default:
        FSM_BREAK
    FSM_END
  }

  void doCallback() {
    RequestContext::setContext(context_);

    // TODO(6115514) semantic race on reading executor_ and setExecutor()
    Executor* x;
    int8_t priority;
    {
      folly::MSLGuard g(executorLock_);
      x = executor_;
      priority = priority_;
    }

    if (x) {
      ++attached_; // keep Core alive until executor did its thing
      try {
        if (LIKELY(x->getNumPriorities() == 1)) {
          x->add([this]() mutable {
            SCOPE_EXIT { detachOne(); };
            callback_(std::move(*result_));
          });
        } else {
          x->addWithPriority([this]() mutable {
            SCOPE_EXIT { detachOne(); };
            callback_(std::move(*result_));
          }, priority);
        }
      } catch (...) {
        result_ = Try<T>(exception_wrapper(std::current_exception()));
        callback_(std::move(*result_));
      }
    } else {
      callback_(std::move(*result_));
    }
  }

  void detachOne() {
    auto a = --attached_;
    assert(a >= 0);
    assert(a <= 2);
    if (a == 0) {
      delete this;
    }
  }

  FSM<State> fsm_ {State::Start};
  std::atomic<unsigned char> attached_ {2};
  std::atomic<bool> active_ {true};
  folly::MicroSpinLock interruptLock_ {0};
  folly::MicroSpinLock executorLock_ {0};
  int8_t priority_ {-1};
  Executor* executor_ {nullptr};
  folly::Optional<Try<T>> result_ {};
  std::function<void(Try<T>&&)> callback_ {nullptr};
  static constexpr size_t lambdaBufSize = 8 * sizeof(void*);
  char lambdaBuf_[lambdaBufSize];
  std::shared_ptr<RequestContext> context_ {nullptr};
  std::unique_ptr<exception_wrapper> interrupt_ {};
  std::function<void(exception_wrapper const&)> interruptHandler_ {nullptr};
};

template <typename... Ts>
struct CollectAllVariadicContext {
  CollectAllVariadicContext() {}
  template <typename T, size_t I>
  inline void setPartialResult(Try<T>& t) {
    std::get<I>(results) = std::move(t);
  }
  ~CollectAllVariadicContext() {
    p.setValue(std::move(results));
  }
  Promise<std::tuple<Try<Ts>...>> p;
  std::tuple<Try<Ts>...> results;
  typedef Future<std::tuple<Try<Ts>...>> type;
};

template <typename... Ts>
struct CollectVariadicContext {
  CollectVariadicContext() {}
  template <typename T, size_t I>
  inline void setPartialResult(Try<T>& t) {
    if (t.hasException()) {
       if (!threw.exchange(true)) {
         p.setException(std::move(t.exception()));
       }
     } else if (!threw) {
       std::get<I>(results) = std::move(t.value());
     }
  }
  ~CollectVariadicContext() {
    if (!threw.exchange(true)) {
      p.setValue(std::move(results));
    }
  }
  Promise<std::tuple<Ts...>> p;
  std::tuple<Ts...> results;
  std::atomic<bool> threw;
  typedef Future<std::tuple<Ts...>> type;
};

template <template <typename ...> class T, typename... Ts>
void collectVariadicHelper(const std::shared_ptr<T<Ts...>>& ctx) {
  // base case
}

template <template <typename ...> class T, typename... Ts,
          typename THead, typename... TTail>
void collectVariadicHelper(const std::shared_ptr<T<Ts...>>& ctx,
                           THead&& head, TTail&&... tail) {
  head.setCallback_([ctx](Try<typename THead::value_type>&& t) {
    ctx->template setPartialResult<typename THead::value_type,
                                   sizeof...(Ts) - sizeof...(TTail) - 1>(t);
  });
  // template tail-recursion
  collectVariadicHelper(ctx, std::forward<TTail>(tail)...);
}

}} // folly::detail
