#include "bug.hpp"
#include "configuration.hpp"
#include "fiber_stack.hpp"
#include "fiber_data.hpp"
#include "memory.hpp"
#include "state.hpp"
#include "vm.hpp"

#include "memory/gc.hpp"

#include <stdlib.h>
#ifdef HAVE_VALGRIND_H
#include <valgrind/valgrind.h>
#endif

namespace rubinius {

  FiberStack::FiberStack(size_t size)
    : address_(0)
    , size_(size)
    , refs_(0)
    , user_(0)
#ifdef HAVE_VALGRIND_H
    , valgrind_id_(0)
#endif
  {}

  void FiberStack::allocate() {
    assert(!address_);
    address_ = malloc(size_);
    if(!address_) rubinius::abort();

#ifdef HAVE_VALGRIND_H
    valgrind_id_ = VALGRIND_STACK_REGISTER(address_, (char *)address_ + size_);
#endif
  }

  void FiberStack::free() {
    if(!address_) return;
#ifdef HAVE_VALGRIND_H
    VALGRIND_STACK_DEREGISTER(valgrind_id_);
#endif
    ::free(address_);
    address_ = 0;
  }

  void FiberStack::flush(STATE) {
    if(!user_) return;

    // TODO assumes higher to lower stack growth.
    user_->copy_to_heap(state);
  }

  void FiberStack::orphan(STATE, FiberData* user) {
    if(user == user_) {
      user_ = 0;
    }

    dec_ref();
  }

  FiberStacks::FiberStacks(VM* thread, SharedState& shared)
    : max_stacks_(shared.config.machine_fiber_stacks)
    , thread_(thread)
    , trampoline_(0)
  {
    lock_.init();
  }

  FiberStacks::~FiberStacks() {
    for(Datas::iterator i = datas_.begin(); i != datas_.end(); ++i) {
      (*i)->die();
    }

    for(Stacks::iterator i = stacks_.begin(); i != stacks_.end(); ++i) {
      i->free();
    }

    if(trampoline_) free(trampoline_);
  }

  void FiberStacks::gc_scan(memory::GarbageCollector* gc, bool marked_only) {
    for(Datas::iterator i = datas_.begin(); i != datas_.end(); ++i) {
      FiberData* data = *i;
      if(data->dead_p()) continue;
      if(marked_only && !data->marked_p()) {
        data->status_ = FiberData::eDead;
        continue;
      }

      memory::AddressDisplacement dis(data->data_offset(),
          data->data_lower_bound(), data->data_upper_bound());

      if(CallFrame* cf = data->call_frame()) {
        gc->walk_call_frame(cf, &dis);
      }

      gc->scan(data->variable_root_buffers(), false, &dis);
    }
  }

  FiberData* FiberStacks::new_data(size_t stack_size, bool root) {
    utilities::thread::SpinLock::LockGuard guard(lock_);
    FiberData* data = new FiberData(thread_, stack_size, root);
    datas_.insert(data);
    return data;
  }

  void FiberStacks::remove_data(FiberData* data) {
    utilities::thread::SpinLock::LockGuard guard(lock_);
    datas_.erase(data);
  }

  FiberStack* FiberStacks::allocate(size_t stack_size) {
    for(Stacks::iterator i = stacks_.begin();
        i != stacks_.end();
        ++i)
    {
      if(i->unused_p() && i->size() >= stack_size) {
        i->inc_ref();
        return &*i;
      }
    }

    FiberStack* stack = 0;

    if(stacks_.size() < max_stacks_) {
      stacks_.push_back(FiberStack(stack_size));
      stack = &stacks_.back();

      stack->allocate();
    } else {
      for(Stacks::iterator i = stacks_.begin();
          i != stacks_.end();
          ++i)
      {
        if(!stack || i->refs() < stack->refs()) {
          stack = &*i;
        }
      }

      assert(stack);
    }

    assert(stack);

    stack->inc_ref();

    return stack;
  }

  void* FiberStacks::trampoline() {
    if(trampoline_ == 0) {
      trampoline_ = malloc(cTrampolineSize);
      if(!trampoline_) rubinius::abort();
    }

    return trampoline_;
  }

  void FiberStacks::gc_clear_mark() {
    utilities::thread::SpinLock::LockGuard guard(lock_);
    for(Datas::iterator i = datas_.begin(); i != datas_.end(); ++i) {
      (*i)->clear_mark();
    }
  }
}
