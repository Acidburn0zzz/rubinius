#ifndef RBX_BUILTIN_CLASS_HPP
#define RBX_BUILTIN_CLASS_HPP

#include "object_utils.hpp"
#include "memory.hpp"

#include "builtin/object.hpp"
#include "builtin/fixnum.hpp"
#include "builtin/module.hpp"
#include "builtin/weakref.hpp"

#include "type_info.hpp"

namespace rubinius {
  class LookupTable;
  class Symbol;

  struct ClassFlags {
    uint32_t class_id;
    uint32_t serial_id;
  };

  union ClassData {
    struct ClassFlags f;
    uint64_t raw;
  };

  class Class : public Module {
  public:
    const static object_type type = ClassType;

  private:
    Fixnum* instance_type_;   // slot
    LookupTable* packed_ivar_info_; // slot

    TypeInfo* type_info_;

    ClassData data_;
    uint32_t packed_size_;

  public:
    /* accessors */

    attr_accessor(packed_ivar_info, LookupTable);
    attr_accessor(instance_type, Fixnum);

    TypeInfo* type_info() const {
      return type_info_;
    }

    void set_type_info(TypeInfo* ti) {
      type_info_ = ti;
    }

    ClassData data() const {
      return data_;
    }

    uint64_t data_raw() const {
      return data_.raw;
    }

    uint32_t class_id() const {
      return data_.f.class_id;
    }

    uint32_t serial_id() const {
      return data_.f.serial_id;
    }

    void increment_serial() {
      atomic::fetch_and_add(&data_.f.serial_id, 1U);
    }

    void set_class_id(uint32_t id) {
      data_.f.class_id = id;
    }

    uint32_t packed_size() const {
      return packed_size_;
    }

    void set_packed_size(uint32_t s) {
      packed_size_ = s;
    }

    /* interface */
    static void bootstrap(STATE);
    static Class* bootstrap_class(STATE, Class* super, object_type type);
    static void bootstrap_initialize(STATE, Class* klass, Class* super, object_type type);

    static void initialize_data(STATE, Class* klass);
    static void initialize_type(STATE, Class* klass, Class* super);

    static void initialize(STATE, Class* klass);
    static void initialize(STATE, Class* klass, Class* super);
    static void initialize(STATE, Class* klass, Class* super, const char* name);
    static void initialize(STATE, Class* klass, Class* super,
        Module* under, const char* name);
    static void initialize(STATE, Class* klass, Class* super,
        Module* under, Symbol* name);
    static void initialize(STATE, Class* klass, Class* super,
        Module* under, Symbol* name, object_type type);

    /** Returns actual superclass, skipping over IncludedModules */
    Class* true_superclass(STATE);

    static Class* real_class(STATE, Class* klass);

    void set_object_type(STATE, size_t type);

    static Class* create(STATE, Class* super);
    static Class* create(STATE, Class* super, Module* under, Symbol* name);

    // Rubinius.primitive :class_s_allocate
    static Class* s_allocate(STATE);

    // Rubinius.primitive+ :class_allocate
    Object* allocate(STATE);

    // Rubinius.primitive :class_set_superclass
    Object* set_superclass(STATE, Object* sup);

    void auto_pack(STATE);

    class Info : public Module::Info {
    public:
      BASIC_TYPEINFO(Module::Info)
    };
  };

  class CompiledCode;
  class ConstantScope;

  class SingletonClass : public Class {
  public:
    const static object_type type = SingletonClassType;

  private:
    WeakRef* object_reference_; // slot

  public:
    /* accessors */

    attr_accessor(object_reference, WeakRef);

    /* interface */
    static void initialize(STATE, SingletonClass* obj);

    static SingletonClass* create(STATE, Object* obj);
    static SingletonClass* attach(STATE, Object* obj, Class* sup);

    Object* singleton() {
      return object_reference_->object();
    }

    class Info : public Class::Info {
    public:
      BASIC_TYPEINFO(Class::Info)
      virtual void show(STATE, Object* self, int level);
    };
  };
};

#endif
