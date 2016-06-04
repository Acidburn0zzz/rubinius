#include "memory.hpp"
#include "call_frame.hpp"
#include "environment.hpp"
#include "diagnostics.hpp"
#include "metrics.hpp"
#include "object_utils.hpp"
#include "on_stack.hpp"
#include "signal.hpp"
#include "thread_phase.hpp"

#include "builtin/channel.hpp"
#include "builtin/class.hpp"
#include "builtin/exception.hpp"
#include "builtin/fiber.hpp"
#include "builtin/fixnum.hpp"
#include "builtin/float.hpp"
#include "builtin/jit.hpp"
#include "builtin/location.hpp"
#include "builtin/lookup_table.hpp"
#include "builtin/native_method.hpp"
#include "builtin/string.hpp"
#include "builtin/symbol.hpp"
#include "builtin/thread.hpp"
#include "builtin/tuple.hpp"

#include "dtrace/dtrace.h"

#include "logger.hpp"

#include "missing/gettid.h"

/* HACK: returns a value that should identify a native thread
 * for debugging threading issues. The winpthreads library
 * defines pthread_t to be a structure not a pointer.
 */
intptr_t thread_debug_self() {
#ifdef RBX_WINDOWS
  return (intptr_t)(pthread_self()).p;
#else
  return (intptr_t)pthread_self();
#endif
}

static intptr_t thread_debug_id(pthread_t thr) {
#ifdef RBX_WINDOWS
    return (intptr_t)thr.p;
#else
    return (intptr_t)thr;
#endif
}


namespace rubinius {

  void Thread::bootstrap(STATE) {
    GO(thread).set(state->memory()->new_class<Class, Thread>(state, "Thread"));
  }

  Thread* Thread::create(STATE, VM* vm) {
    return Thread::create(state, G(thread), vm);
  }

  Thread* Thread::create(STATE, Class* klass, VM* vm) {
    Thread* thr = state->memory()->new_object_pinned<Thread>(state, klass);

    if(!vm) {
      Exception::raise_thread_error(state, "attempt to create Thread with NULL VM*");
    }

    thr->vm(vm);
    thr->thread_id(state, Fixnum::from(vm->thread_id()));

    vm->thread.set(thr);

    return thr;
  }

  Thread* Thread::create(STATE, VM* vm, ThreadFunction function) {
    return Thread::create(state, G(thread), vm, function);
  }

  Thread* Thread::create(STATE, Object* self, ThreadFunction function) {
    VM* vm = state->shared().thread_nexus()->new_vm(&state->shared());

    return Thread::create(state, self, vm, function);
  }

  Thread* Thread::create(STATE, Object* self, VM* vm, ThreadFunction function) {
    Thread* thr = Thread::create(state, as<Class>(self), vm);

    thr->function(function);

    state->memory()->native_finalizer(state, thr,
        (memory::FinalizerFunction)&Thread::finalize);

    state->vm()->metrics().system.threads_created++;

    return thr;
  }

  void Thread::finalize(STATE, Thread* thread) {
    thread->finalize_instance(state);
  }

  void Thread::finalize_instance(STATE) {
    if(vm() && vm()->zombie_p()) {
      VM::discard(state, vm());
      vm(NULL);
    }
  }

  Object* run_instance(STATE) {
    /* These are all referenced, so OnStack is not necessary. Additionally,
     * thread is pinned, so we do not need to worry about it moving.
     */
    Thread* thread = state->vm()->thread.get();
    Array* args = thread->args();
    Object* block = thread->block();

    if(thread->initialized()->false_p() || args->nil_p() || block->nil_p()) {
      return cNil;
    }

    Object* value = block->send(state, G(sym_call), args, block);

    /* We explicitly set the current CallFrame reference to NULL because we
     * are at the top of the stack in terms of managed code.
     */
    state->vm()->set_call_frame(NULL);

    thread->exception(state, state->vm()->thread_state()->current_exception());

    if(state->vm()->thread_state()->raise_reason() == cThreadKill) {
      thread->value(state, cNil);
    } else if(value) {
      thread->value(state, value);
    }

    Object* mirror = G(mirror)->send(state, state->symbol("reflect"),
        Array::from_tuple(state, Tuple::from(state, 1, thread)));
    mirror->send(state, state->symbol("finish"));

    return value;
  }

  Thread* Thread::s_new(STATE, Object* self, Array* args, Object* stack_size, Object* block) {
    Thread* thread = Thread::create(state, self, run_instance);
    OnStack<1> os(state, thread);

    if(Fixnum* size = try_as<Fixnum>(stack_size)) {
      state->vm()->validate_stack_size(state, size->to_native());
      thread->stack_size(state, size);
    }

    CallFrame* call_frame = state->vm()->get_ruby_frame(1);

    logger::write("new thread: %s, %s:%d",
        thread->vm()->name().c_str(),
        call_frame->file(state)->cpp_str(state).c_str(),
        call_frame->line(state));

    if(!thread->send(state, state->symbol("initialize"), args, block, true)) {
      thread->vm()->set_zombie(state);
      return NULL;
    }

    thread->fork(state);

    return thread;
  }

  Thread* Thread::s_start(STATE, Object* self, Array* args, Object* stack_size, Object* block) {
    Thread* thread = Thread::create(state, self, run_instance);
    OnStack<1> os(state, thread);

    if(Fixnum* size = try_as<Fixnum>(stack_size)) {
      state->vm()->validate_stack_size(state, size->to_native());
      thread->stack_size(state, size);
    }

    CallFrame* call_frame = state->vm()->get_ruby_frame(1);

    logger::write("start thread: %s, %s:%d",
        thread->vm()->name().c_str(),
        call_frame->file(state)->cpp_str(state).c_str(),
        call_frame->line(state));

    if(!thread->send(state, state->symbol("__thread_initialize__"), args, block, true)) {
      thread->vm()->set_zombie(state);
      return NULL;
    }

    thread->fork(state);

    return thread;
  }

  Thread* Thread::current(STATE) {
    return state->vm()->thread.get();
  }

  void Thread::unlock_after_fork(STATE) {
    unlock_object_after_fork(state);

    memory::LockedObjects& los = vm()->locked_objects();
    for(memory::LockedObjects::iterator i = los.begin();
        i != los.end();
        ++i) {
      Object* obj = static_cast<Object*>(*i);
      if(obj && obj != this) {
        obj->unlock_object_after_fork(state);
      }
    }
    los.clear();
  }

  Object* Thread::locals_aref(STATE, Symbol* key) {
    /*
     * If we're not trying to set values on the current thread,
     * we will set thread locals anyway and not use fiber locals.
     */
    if(state->vm() != vm()) {
      return locals()->aref(state, key);
    }
    Fiber* fib = state->vm()->current_fiber.get();
    if(fib->nil_p() || fib->root_p()) {
      return locals()->aref(state, key);
    }
    if(try_as<LookupTable>(fib->locals())) {
      return fib->locals()->aref(state, key);
    }
    return cNil;
  }

  Object* Thread::locals_store(STATE, Symbol* key, Object* value) {
    /*
     * If we're not trying to set values on the current thread,
     * we will set thread locals anyway and not use fiber locals.
     */
    check_frozen(state);
    if(state->vm() != vm()) {
      return locals()->store(state, key, value);
    }
    Fiber* fib = state->vm()->current_fiber.get();
    if(fib->nil_p() || fib->root_p()) {
      return locals()->store(state, key, value);
    }
    if(fib->locals()->nil_p()) {
      fib->locals(state, LookupTable::create(state));
    }
    return fib->locals()->store(state, key, value);
  }

  Object* Thread::locals_remove(STATE, Symbol* key) {
    check_frozen(state);
    if(state->vm() != vm()) {
      return locals()->remove(state, key);
    }
    Fiber* fib = state->vm()->current_fiber.get();
    if(fib->nil_p() || fib->root_p()) {
      return locals()->remove(state, key);
    }
    if(fib->locals()->nil_p()) {
      return cNil;
    }
    return fib->locals()->remove(state, key);
  }

  Array* Thread::locals_keys(STATE) {
    /*
     * If we're not trying to set values on the current thread,
     * we will set thread locals anyway and not use fiber locals.
     */
    if(state->vm() != vm()) {
      return locals()->all_keys(state);
    }
    Fiber* fib = state->vm()->current_fiber.get();
    if(fib->nil_p() || fib->root_p()) {
      return locals()->all_keys(state);
    }
    if(try_as<LookupTable>(fib->locals())) {
      return fib->locals()->all_keys(state);
    }
    return Array::create(state, 0);
  }

  Object* Thread::locals_has_key(STATE, Symbol* key) {
    /*
     * If we're not trying to set values on the current thread,
     * we will set thread locals anyway and not use fiber locals.
     */
    if(state->vm() != vm()) {
      return locals()->has_key(state, key);
    }
    Fiber* fib = state->vm()->current_fiber.get();
    if(fib->nil_p() || fib->root_p()) {
      return locals()->has_key(state, key);
    }
    if(try_as<LookupTable>(fib->locals())) {
      return fib->locals()->has_key(state, key);
    }
    return cFalse;
  }

  int Thread::start_thread(STATE, void* (*function)(void*)) {
    Thread* self = this;
    OnStack<1> os(state, self);

    pthread_attr_t attrs;
    pthread_attr_init(&attrs);
    pthread_attr_setstacksize(&attrs, self->stack_size()->to_native());
    pthread_attr_setdetachstate(&attrs, PTHREAD_CREATE_DETACHED);

    int status = pthread_create(&self->vm()->os_thread(), &attrs,
        function, (void*)self->vm());

    pthread_attr_destroy(&attrs);

    return status;
  }

  Object* Thread::main_thread(STATE) {
    state->vm()->managed_phase();

    std::string& runtime = state->shared().env()->runtime_path();

    G(rubinius)->set_const(state, "Signature",
        Integer::from(state, state->shared().env()->signature()));

    G(rubinius)->set_const(state, "RUNTIME_PATH", String::create(state,
                           runtime.c_str(), runtime.size()));

    state->vm()->thread->pid(state, Fixnum::from(gettid()));

    state->shared().env()->load_core(state, runtime);

    state->vm()->thread_state()->clear();

    state->shared().start_console(state);
    state->shared().start_metrics(state);
    state->shared().start_diagnostics(state);
    state->shared().start_profiler(state);
    state->shared().start_jit(state);

    Object* klass = G(rubinius)->get_const(state, state->symbol("Loader"));
    if(klass->nil_p()) {
      rubinius::bug("unable to find class Rubinius::Loader");
    }

    Object* instance = 0;
    OnStack<1> os(state, instance);

    instance = klass->send(state, state->symbol("new"));
    if(instance) {
      state->shared().env()->set_loader(instance);
    } else {
      rubinius::bug("unable to instantiate Rubinius::Loader");
    }

    // Enable the JIT after the core library has loaded
    G(jit)->enable(state);

    Object* value = instance->send(state, state->symbol("main"));

    state->shared().signals()->system_exit(state->vm()->thread_state()->raise_value());

    return value;
  }

  void* Thread::run(void* ptr) {
    VM* vm = reinterpret_cast<VM*>(ptr);
    State state_obj(vm), *state = &state_obj;

    vm->set_stack_bounds(vm->thread->stack_size()->to_native());
    vm->set_current_thread();
    vm->set_start_time();

    RUBINIUS_THREAD_START(
        const_cast<RBX_DTRACE_CHAR_P>(vm->name().c_str()), vm->thread_id(), 0);

    vm->thread->pid(state, Fixnum::from(gettid()));

    logger::write("start thread: %s, %d, %#x",
        vm->name().c_str(), vm->thread->pid()->to_native(),
        (unsigned int)thread_debug_self());

    NativeMethod::init_thread(state);

    state->vm()->managed_phase();

    Object* value = vm->thread->function()(state);
    vm->set_call_frame(NULL);

    vm->thread->join_lock_.lock();
    vm->thread->stopped();

    state->shared().report_profile(state);

    memory::LockedObjects& locked_objects = state->vm()->locked_objects();
    for(memory::LockedObjects::iterator i = locked_objects.begin();
        i != locked_objects.end();
        ++i)
    {
      (*i)->unlock_for_terminate(state);
    }
    locked_objects.clear();

    vm->thread->join_cond_.broadcast();
    vm->thread->join_lock_.unlock();

    NativeMethod::cleanup_thread(state);

    logger::write("exit thread: %s %fs", vm->name().c_str(), vm->run_time());

    vm->unmanaged_phase();

    if(vm->main_thread_p() || (!value && vm->thread_state()->raise_reason() == cExit)) {
      state->shared().signals()->system_exit(vm->thread_state()->raise_value());
    }

    vm->set_zombie(state);

    RUBINIUS_THREAD_STOP(
        const_cast<RBX_DTRACE_CHAR_P>(vm->name().c_str()), vm->thread_id(), 0);

    return 0;
  }

  void Thread::fork(STATE) {
    if(int error = start_thread(state, Thread::run)) {
      char buf[RBX_STRERROR_BUFSIZE];
      char* err = RBX_STRERROR(error, buf, RBX_STRERROR_BUFSIZE);
      Exception::raise_thread_error(state, err);
    }
  }

  Object* Thread::pass(STATE) {
    atomic::pause();
    return cNil;
  }

  Array* Thread::list(STATE) {
    return state->shared().vm_threads(state);
  }

  Object* Thread::set_priority(STATE, Fixnum* new_priority) {
    priority(state, new_priority);
    return new_priority;
  }

  Object* Thread::get_priority(STATE) {
    return priority();
  }

  Object* Thread::raise(STATE, Exception* exc) {
    utilities::thread::SpinLock::LockGuard guard(init_lock_);

    if(!vm()) return cNil;

    vm()->register_raise(state, exc);
    vm()->wakeup(state);

    return exc;
  }

  Object* Thread::kill(STATE) {
    utilities::thread::SpinLock::LockGuard guard(init_lock_);

    if(!vm()) return cNil;

    if(state->vm()->thread.get() == this) {
      vm()->thread_state_.raise_thread_kill();
      return NULL;
    } else {
      vm()->register_kill(state);
      vm()->wakeup(state);
      return this;
    }
  }

  Thread* Thread::wakeup(STATE) {
    utilities::thread::SpinLock::LockGuard guard(init_lock_);

    if(!CBOOL(alive()) || !vm()) {
      return force_as<Thread>(Primitives::failure());
    }

    vm()->wakeup(state);

    return this;
  }

  Tuple* Thread::context(STATE) {
    utilities::thread::SpinLock::LockGuard guard(init_lock_);

    if(!vm()) return nil<Tuple>();

    CallFrame* call_frame = vm()->get_ruby_frame();
    VariableScope* scope = call_frame->promote_scope(state);

    return Tuple::from(state, 3, Fixnum::from(call_frame->ip()),
        call_frame->compiled_code, scope);
  }

  Array* Thread::mri_backtrace(STATE) {
    utilities::thread::SpinLock::LockGuard guard(init_lock_);

    if(!vm()) return nil<Array>();

    return Location::mri_backtrace(state);
  }

  void Thread::stopped() {
    alive(cFalse);
  }

  void Thread::init_lock() {
    init_lock_.init();
  }

  Thread* Thread::join(STATE, Object* timeout) {
    if(!vm()) return nil<Thread>();

    Thread* self = this;
    OnStack<2> os(state, self, timeout);

    state->vm()->unmanaged_phase();

    {
      utilities::thread::Mutex::LockGuard guard(self->join_lock_);
      state->vm()->managed_phase();
      atomic::memory_barrier();

      if(self->alive()->true_p()) {
        UnmanagedPhase unmanaged(state);

        if(timeout->nil_p()) {
          self->join_cond_.wait(self->join_lock_);
        } else {
          struct timespec ts = {0,0};
          self->join_cond_.offset(&ts, as<Float>(timeout)->value());

          if(self->join_cond_.wait_until(self->join_lock_, &ts)
                == utilities::thread::cTimedOut) {
            return nil<Thread>();
          }
        }
      }
    }

    return self;
  }
}
