#include "config.h"
#include "configuration.hpp"
#include "environment.hpp"
#include "metrics.hpp"
#include "memory.hpp"
#include "state.hpp"
#include "thread_phase.hpp"
#include "vm.hpp"

#include "builtin/class.hpp"
#include "builtin/thread.hpp"

#include "memory/gc.hpp"
#include "memory/immix_collector.hpp"
#include "memory/immix_marker.hpp"

#include "dtrace/dtrace.h"
#include "instruments/timing.hpp"

namespace rubinius {
namespace memory {

  ImmixMarker::ImmixMarker(STATE, ImmixGC* immix, GCData* data)
    : InternalThread(state, "rbx.immix")
    , immix_(immix)
    , data_(data)
  {
    state->memory()->set_immix_marker(this);

    initialize(state);
    start_thread(state);
  }

  ImmixMarker::~ImmixMarker() {
    cleanup();
  }

  void ImmixMarker::initialize(STATE) {
    InternalThread::initialize(state);

    Thread::create(state, vm());
  }

  void ImmixMarker::after_fork_child(STATE) {
    cleanup();

    state->memory()->clear_mature_mark_in_progress();

    InternalThread::after_fork_child(state);
  }

  void ImmixMarker::cleanup() {
    if(data_) {
      delete data_;
      data_ = NULL;
    }
  }

  void ImmixMarker::stop(STATE) {
    InternalThread::stop(state);
  }

  void ImmixMarker::run(STATE) {
    state->vm()->become_managed();

    while(!thread_exit_) {
      timer::StopWatch<timer::milliseconds> timer(
          state->vm()->metrics().gc.immix_concurrent_ms);

      state->shared().thread_nexus()->blocking(state->vm());

      while(immix_->process_mark_stack(immix_->memory()->interrupt_p())) {
        if(thread_exit_ || immix_->memory()->collect_full_p()) {
          break;
        } else if(immix_->memory()->collect_young_p()) {
          state->shared().thread_nexus()->yielding(state->vm());
        } else if(immix_->memory()->interrupt_p()) {
          // We may be trying to fork or otherwise checkpoint
          state->shared().thread_nexus()->yielding(state->vm());
          immix_->memory()->reset_interrupt();
        }

        state->shared().thread_nexus()->blocking(state->vm());
      }

      if(thread_exit_) break;

      if(immix_->memory()->collect_full_p()) {
        timer::StopWatch<timer::milliseconds> timer(
            state->vm()->metrics().gc.immix_stop_ms);

        state->vm()->thread_nexus()->set_stop();

        LockPhase locked(state);

        state->memory()->collect_full_finish(state, data_);
        state->memory()->collect_full_restart(state, data_);

        if(state->shared().config.memory_collection_log.value) {
          // TODO: diagnostics
          // state->shared().env()->diagnostics()->log();
        }

        continue;
      }

      state->vm()->sleeping_suspend(state, state->vm()->metrics().gc.immix_suspend_ms);
    }

    state->memory()->clear_mature_mark_in_progress();
  }
}
}
