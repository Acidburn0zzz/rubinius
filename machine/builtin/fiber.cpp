#include "arguments.hpp"
#include "call_frame.hpp"
#include "memory.hpp"

#include "builtin/array.hpp"
#include "builtin/class.hpp"
#include "builtin/exception.hpp"
#include "builtin/fiber.hpp"
#include "builtin/lookup_table.hpp"
#include "builtin/object.hpp"

#include "memory/gc.hpp"

namespace rubinius {

  void Fiber::bootstrap(STATE) {
    GO(fiber).set(state->memory()->new_class<Class, Fiber>(
          state, G(rubinius), "Fiber"));

#ifdef RBX_FIBER_ENABLED
    G(fiber)->set_const(state, "ENABLED", cTrue);
#else
    G(fiber)->set_const(state, "ENABLED", cFalse);
#endif
  }

  Fiber* Fiber::current(STATE) {
#ifdef RBX_FIBER_ENABLED
    Fiber* fib = state->vm()->current_fiber.get();

    // Lazily allocate a root fiber.
    if(fib->nil_p()) {
      fib = state->memory()->new_object<Fiber>(state, G(fiber));
      fib->root_ = true;
      fib->status_ = Fiber::eRunning;

      fib->data_ = state->vm()->new_fiber_data(true);
      fib->data_->set_call_frame(state->vm()->call_frame());

      state->memory()->needs_finalization(fib,
          (memory::FinalizerFunction)&Fiber::finalize,
          memory::FinalizeObject::eUnmanaged);

      state->vm()->current_fiber.set(fib);
      state->vm()->root_fiber.set(fib);
    }

    return fib;
#else
    Exception::raise_not_implemented_error(state, "Fibers not supported on this platform");
#endif
  }

  void Fiber::start_on_stack() {
#ifdef RBX_FIBER_ENABLED
    VM* vm = VM::current();
    State state_obj(vm), *state = &state_obj;

    Fiber* fib = Fiber::current(state);

    // Reset the current fiber again to reset the stack limits so
    // we can properly detect stack overflows
    vm->set_current_fiber(fib);

    Array* result = nil<Array>();
    Object* obj = fib->starter()->send(state, G(sym_call), fib->value(), cNil, false);

    // GC has run! Don't use stack vars!

    fib = Fiber::current(state);
    fib->status_ = Fiber::eDead;
    fib->dead_ = cTrue;
    fib->set_call_frame(state, 0);

    Fiber* dest = fib->prev();

    // If this fiber has already been cleaned up, just ignore this
    if(!dest->data()) return;

    assert(!dest->nil_p());

    // Box this up so it's in a standard format at the point
    // of returning, so we can deal with it in the same way
    // as *args from #yield, #resume, and #transfer
    if(obj) {
      result = Array::create(state, 1);
      result->set(state, 0, obj);
    } else {
      if(state->vm()->thread_state()->raise_reason() == cException) {
        dest->exception(state, state->vm()->thread_state()->current_exception());
      }
    }

    dest->run(state);
    dest->value(state, result);

    dest->data_->switch_and_orphan(state, fib->data_);

    // TODO: CallFrame: return from this function

    assert(0 && "fatal start_on_stack error");
#else
    rubinius::bug("Fibers not supported on this platform");
#endif
  }

  Fiber* Fiber::create(STATE, Object* self, Object* callable) {
#ifdef RBX_FIBER_ENABLED
    Fiber* fib = state->memory()->new_object<Fiber>(state, as<Class>(self));
    fib->starter(state, callable);

    state->memory()->needs_finalization(fib,
        (memory::FinalizerFunction)&Fiber::finalize,
        memory::FinalizeObject::eUnmanaged);

    return fib;
#else
    Exception::raise_not_implemented_error(state, "Fibers not supported on this platform");
#endif
  }

  Object* Fiber::resume(STATE, Arguments& args) {
#ifdef RBX_FIBER_ENABLED
    if(!data_) {
      data_ = state->vm()->new_fiber_data();
    }

    if(status_ == Fiber::eDead || data_->dead_p()) {
      Exception::raise_fiber_error(state, "dead fiber called");
    }

    if(!prev_->nil_p()) {
      Exception::raise_fiber_error(state, "double resume");
    }

    if(data_->thread() && data_->thread() != state->vm()) {
      Exception::raise_fiber_error(state, "cross thread fiber resuming is illegal");
    }

    Array* val = args.as_array(state);
    value(state, val);

    Fiber* cur = Fiber::current(state);
    prev(state, cur);

    cur->sleep(state);

    run(state);

    data_->switch_to(state, cur->data_);

    // Back here when someone yields back to us!
    // Beware here, because the GC has probably run so GC pointers on the C++ stack
    // can't be accessed.

    cur = Fiber::current(state);

    if(!cur->exception()->nil_p()) {
      state->raise_exception(cur->exception());
      cur->exception(state, nil<Exception>());
      return 0;
    }

    Array* ret = cur->value();

    if(ret->nil_p()) return cNil;

    switch(ret->size()) {
    case 0:  return cNil;
    case 1:  return ret->get(state, 0);
    default: return ret;
    }
#else
    Exception::raise_not_implemented_error(state, "Fibers not supported on this platform");
#endif
  }

  Object* Fiber::transfer(STATE, Arguments& args) {
#ifdef RBX_FIBER_ENABLED
    if(!data_) {
      data_ = state->vm()->new_fiber_data();
    }

    if(status_ == Fiber::eDead || data_->dead_p()) {
      Exception::raise_fiber_error(state, "dead fiber called");
    }

    if(data_->thread() && data_->thread() != state->vm()) {
      Exception::raise_fiber_error(state, "cross thread fiber resuming is illegal");
    }

    Array* val = args.as_array(state);
    value(state, val);

    Fiber* cur = Fiber::current(state);
    Fiber* root = state->vm()->root_fiber.get();
    assert(root);

    prev(state, root);

    cur->sleep(state);

    run(state);

    data_->switch_to(state, cur->data_);

    // Back here when someone transfers back to us!
    // Beware here, because the GC has probably run so GC pointers on the C++ stack
    // can't be accessed.

    cur = Fiber::current(state);

    if(!cur->exception()->nil_p()) {
      state->raise_exception(cur->exception());
      cur->exception(state, nil<Exception>());
      return 0;
    }

    Array* ret = cur->value();

    if(ret->nil_p()) return cNil;

    switch(ret->size()) {
    case 0:  return cNil;
    case 1:  return ret->get(state, 0);
    default: return ret;
    }
#else
    Exception::raise_not_implemented_error(state, "Fibers not supported on this platform");
#endif
  }

  Object* Fiber::s_yield(STATE, Arguments& args) {
#ifdef RBX_FIBER_ENABLED
    Fiber* cur = Fiber::current(state);
    Fiber* dest_fib = cur->prev();

    assert(cur != dest_fib);

    if(cur->root_) {
      Exception::raise_fiber_error(state, "can't yield from root fiber");
    }

    cur->prev(state, nil<Fiber>());

    Array* val = args.as_array(state);
    dest_fib->value(state, val);

    cur->sleep(state);

    dest_fib->run(state);

    dest_fib->data_->switch_to(state, cur->data_);

    // Back here when someone yields back to us!
    // Beware here, because the GC has probably run so GC pointers on the C++ stack
    // can't be accessed.

    cur = Fiber::current(state);

    Array* ret = cur->value();

    if(ret->nil_p()) return cNil;

    switch(ret->size()) {
    case 0:  return cNil;
    case 1:  return ret->get(state, 0);
    default: return ret;
    }
#else
    Exception::raise_not_implemented_error(state, "Fibers not supported on this platform");
#endif
  }


  void Fiber::finalize(STATE, Fiber* fib) {
#ifdef RBX_FIBER_ENABLED
    if(!fib->data_) return;
    fib->data_->orphan(state);

    delete fib->data_;
    fib->data_ = NULL;
#endif
  }

  void Fiber::Info::mark(Object* obj, memory::ObjectMark& mark) {
    auto_mark(obj, mark);
    Fiber* fib = force_as<Fiber>(obj);
    FiberData* data = fib->data_;
    if(!data || data->dead_p()) return;
    data->set_mark();
  }
}

