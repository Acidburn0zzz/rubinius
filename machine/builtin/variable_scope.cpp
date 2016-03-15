#include "memory.hpp"
#include "call_frame.hpp"
#include "fiber_data.hpp"

#include "builtin/class.hpp"
#include "builtin/exception.hpp"
#include "builtin/fiber.hpp"
#include "builtin/object.hpp"
#include "builtin/system.hpp"
#include "builtin/tuple.hpp"
#include "builtin/variable_scope.hpp"

#include "memory/gc.hpp"

namespace rubinius {
  void VariableScope::bootstrap(STATE) {
    GO(variable_scope).set(state->memory()->new_class<Class, VariableScope>(
          state, G(rubinius), "VariableScope"));
  }

  void VariableScope::bootstrap_methods(STATE) {
    System::attach_primitive(state,
                             G(variable_scope), false,
                             state->symbol("method_visibility"),
                             state->symbol("variable_scope_method_visibility"));
  }

  VariableScope* VariableScope::of_sender(STATE) {
    if(CallFrame* frame = state->vm()->get_ruby_frame(1)) {
      return frame->promote_scope(state);
    }

    return nil<VariableScope>();
  }

  VariableScope* VariableScope::current(STATE) {
    if(CallFrame* call_frame = state->vm()->call_frame()) {
      if(!call_frame->native_method_p()) return call_frame->promote_scope(state);
    }

    return nil<VariableScope>();
  }

  VariableScope* VariableScope::allocate(STATE) {
    return state->memory()->new_object<VariableScope>(state, G(variable_scope));
  }

  VariableScope* VariableScope::synthesize(STATE, CompiledCode* method,
      Module* module, Object* parent, Object* self, Object* block, Tuple* locals)
  {
    VariableScope* scope =
      state->memory()->new_object<VariableScope>(state, G(variable_scope));

    scope->block(state, block);
    scope->module(state, module);
    scope->method(state, method);

    if(VariableScope* vs = try_as<VariableScope>(parent)) {
      scope->parent(state, vs);
    } else {
      scope->parent(state, nil<VariableScope>());
    }

    scope->heap_locals(state, locals);

    scope->self(state, self);
    scope->number_of_locals_ = locals->num_fields();

    return scope;
  }

  Tuple* VariableScope::locals(STATE) {
    Tuple* tup = state->memory()->new_fields<Tuple>(state, G(tuple), number_of_locals_);

    if(tup->young_object_p()) {
      for(int i = 0; i < number_of_locals_; i++) {
        tup->field[i] = get_local(state, i);
      }
    } else {
      for(int i = 0; i < number_of_locals_; i++) {
        tup->put(state, i, get_local(state, i));
      }
    }

    return tup;
  }

  Object* VariableScope::set_local_prim(STATE, Fixnum* number, Object* object) {
    int num = number->to_int();

    if(num < 0) {
      Exception::raise_argument_error(state, "negative local index");
    } else if(num >= number_of_locals_) {
      Exception::raise_argument_error(state, "index larger than number of locals");
    }

    set_local(state, num, object);
    return cNil;
  }

  // bootstrap method, replaced with an attr_accessor in core library.
  Object* VariableScope::method_visibility(STATE) {
    return cNil;
  }

  Object* VariableScope::locked(STATE) {
    return RBOOL(locked_p());
  }

  Object* VariableScope::set_locked(STATE) {
    flags_ |= CallFrame::cScopeLocked;
    VariableScope* parent = parent_;
    while(parent && !parent->nil_p()) {
      parent->set_locked(state);
      parent = parent->parent();
    }
    return cNil;
  }

  void VariableScope::set_local_internal(STATE, int pos, Object* val) {
     if(isolated_) {
       heap_locals_->put(state, pos, val);
     } else {
       set_local(pos, val);
     }
   }

  void VariableScope::set_local(STATE, int pos, Object* val) {
    if(unlikely(locked_p())) {
      utilities::thread::SpinLock::LockGuard guard(lock_);
      set_local_internal(state, pos, val);
    } else {
      set_local_internal(state, pos, val);
    }
  }

  void VariableScope::set_local(int pos, Object* val) {
    Object** ary = locals_;

    if(Fiber* fib = try_as<Fiber>(fiber_)) {
      FiberData* data = fib->data();
      if(data) {
        memory::AddressDisplacement dis(data->data_offset(),
            data->data_lower_bound(), data->data_upper_bound());

        ary = dis.displace(ary);
      }
    }
    ary[pos] = val;
  }

  Object* VariableScope::get_local_internal(STATE, int pos) {
     if(isolated_) {
       return heap_locals_->at(pos);
     } else {
       return get_local(pos);
     }
   }

  Object* VariableScope::get_local(STATE, int pos) {
    if(unlikely(locked_p())) {
      utilities::thread::SpinLock::LockGuard guard(lock_);
      return get_local_internal(state, pos);
    } else {
      return get_local_internal(state, pos);
    }
  }

  Object* VariableScope::get_local(int pos) {
    Object** ary = locals_;
    if(Fiber* fib = try_as<Fiber>(fiber_)) {
      FiberData* data = fib->data();
      if(data) {
        memory::AddressDisplacement dis(data->data_offset(),
            data->data_lower_bound(), data->data_upper_bound());

        ary = dis.displace(ary);
      }
    }
    return ary[pos];
  }

  Object* VariableScope::top_level_visibility(STATE) {
    return RBOOL(top_level_visibility_p());
  }

  Object* VariableScope::script(STATE) {
    return RBOOL(script_p());
  }

  void VariableScope::flush_to_heap_internal(STATE) {
    if(isolated_) return;

   Tuple* new_locals =
     state->memory()->new_fields<Tuple>(state, G(tuple), number_of_locals_);

   if(new_locals->young_object_p()) {
     for(int i = 0; i < number_of_locals_; i++) {
       new_locals->field[i] = locals_[i];
     }
   } else {
     for(int i = 0; i < number_of_locals_; i++) {
       new_locals->put(state, i, locals_[i]);
     }
   }

    heap_locals(state, new_locals);
    isolated_ = 1;
  }

  void VariableScope::flush_to_heap(STATE) {
    if(unlikely(locked_p())) {
      utilities::thread::SpinLock::LockGuard guard(lock_);
      flush_to_heap_internal(state);
      flags_ &= ~CallFrame::cScopeLocked;
    } else {
      flush_to_heap_internal(state);
    }
  }

  void VariableScope::Info::mark(Object* obj, memory::ObjectMark& mark) {
    auto_mark(obj, mark);

    VariableScope* vs = as<VariableScope>(obj);

    if(!vs->isolated()) {
      Object** ary = vs->locals_;

      if(Fiber* fib = try_as<Fiber>(vs->fiber())) {
        FiberData* data = fib->data();

        if(data) {
          memory::AddressDisplacement dis(data->data_offset(),
              data->data_lower_bound(), data->data_upper_bound());

          ary = dis.displace(ary);
        }
      }

      size_t locals = vs->number_of_locals();

      for(size_t i = 0; i < locals; i++) {
        if(Object* tmp = mark.call(ary[i])) {
          ary[i] = tmp;
        }
      }
    }
  }
}
