#include "memory.hpp"

#include "builtin/call_custom_cache.hpp"
#include "builtin/call_unit.hpp"
#include "builtin/class.hpp"

namespace rubinius {

  void CallCustomCache::bootstrap(STATE) {
    GO(call_custom_cache).set(state->memory()->new_class<Class, CallCustomCache>(
          state, G(call_site), G(rubinius), "CallCustomCache"));
  }

  CallCustomCache* CallCustomCache::create(STATE, CallSite* call_site, CallUnit* call_unit) {
    CallCustomCache* cache =
      state->memory()->new_object<CallCustomCache>(state, G(call_custom_cache));

    cache->name(state, call_site->name());
    cache->executable(state, call_site->executable());
    cache->ip(call_site->ip());
    cache->executor(check_cache);
    cache->fallback(call_site->fallback());
    cache->updater(NULL);
    cache->call_unit(state, call_unit);
    cache->hits(0);

    return cache;
  }

  Object* CallCustomCache::check_cache(STATE, CallSite* call_site, Arguments& args) {
    CallCustomCache* cache = static_cast<CallCustomCache*>(call_site);

    CallUnit* cu = cache->call_unit();
    return cu->execute(state, cu,
                       cu->executable(), cu->module(), args);
  }

  void CallCustomCache::Info::mark(Object* obj, memory::ObjectMark& mark) {
    auto_mark(obj, mark);
  }
}

