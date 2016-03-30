#ifndef RBX_CALL_CUSTOM_CACHE_HPP
#define RBX_CALL_CUSTOM_CACHE_HPP

#include "builtin/call_site.hpp"
#include "builtin/call_unit.hpp"

namespace rubinius {
  struct CallFrame;
  class CallUnit;

  class CallCustomCache : public CallSite {
  public:
    const static object_type type = CallCustomCacheType;

    attr_accessor(call_unit, CallUnit);

  private:
    attr_field(hits, int);

  public:
    static void bootstrap(STATE);
    static void initialize(STATE, CallCustomCache* obj) {
      CallSite::initialize(state, obj);

      obj->call_unit(nil<CallUnit>());
      obj->hits(0);
    }

    static CallCustomCache* create(STATE, CallSite* call_site, CallUnit* call_unit);
    static CacheExecuteFunc check_cache;

  public: // Rubinius Type stuff
    class Info : public CallSite::Info {
    public:
      BASIC_TYPEINFO(CallSite::Info)
      virtual void mark(Object* t, memory::ObjectMark& mark);
    };

  };
}

#endif
