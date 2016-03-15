#ifndef RBX_RESPOND_TO_CACHE_HPP
#define RBX_RESPOND_TO_CACHE_HPP

#include "object_utils.hpp"

#include "builtin/call_site.hpp"
#include "builtin/class.hpp"
#include "builtin/object.hpp"

namespace rubinius {
  struct CallFrame;
  class Arguments;
  class Module;

  class RespondToCache : public CallSite {
  public:
    const static object_type type = RespondToCacheType;

  private:
    ClassData receiver_;
    Class*  receiver_class_;       // slot
    Object* message_;              // slot
    Object* visibility_;           // slot
    Object* responds_;             // slot
    CallSite* fallback_call_site_; // slot
    int hits_;

  public:
    attr_accessor(receiver_class, Class);
    attr_accessor(message, Object);
    attr_accessor(visibility, Object);
    attr_accessor(responds, Object);
    attr_accessor(fallback_call_site, CallSite);

    void clear_receiver_data() {
      receiver_.raw = 0;
    }

    void set_receiver_data(uint64_t data) {
      receiver_.raw = data;
    }

    ClassData receiver_data() {
      return receiver_;
    }

    uint64_t receiver_data_raw() {
      return receiver_.raw;
    }

    uint32_t receiver_class_id() {
      return receiver_.f.class_id;
    }

    uint32_t receiver_serial_id() {
      return receiver_.f.serial_id;
    }

    void hit() {
      ++hits_;
    }

    int hits() {
      return hits_;
    }

    // Rubinius.primitive+ :respond_to_cache_hits
    Integer* hits_prim(STATE);

  public:
    static void bootstrap(STATE);
    static void initialize(STATE, RespondToCache* obj) {
      CallSite::initialize(state, obj);

      obj->receiver_.raw = 0;
      obj->receiver_class_ = nil<Class>();
      obj->message_ = nil<Object>();
      obj->visibility_ = nil<Object>();
      obj->responds_ = nil<Object>();
      obj->fallback_call_site_ = nil<CallSite>();
      obj->hits_ = 0;
    }

    static RespondToCache* create(STATE, CallSite* fallback, Object* recv,
                                  Symbol* msg, Object* priv, Object* res, int hits);

    static Object* check_cache(STATE, CallSite* call_site,
                               Arguments& args);

  public: // Rubinius Type stuff
    class Info : public CallSite::Info {
    public:
      BASIC_TYPEINFO(CallSite::Info)
      virtual void mark(Object* t, memory::ObjectMark& mark);
    };

  };
}

#endif

