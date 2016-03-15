#ifndef RBX_SHARED_STATE_H
#define RBX_SHARED_STATE_H

#include "config.h"

#include "memory/variable_buffer.hpp"
#include "memory/root_buffer.hpp"

#include "internal_threads.hpp"
#include "globals.hpp"
#include "symbol_table.hpp"
#include "thread_nexus.hpp"

#include "primitives.hpp"

#include "util/thread.hpp"

#include "capi/capi_constants.h"

#include <unistd.h>
#include <string>
#include <vector>

#include "missing/unordered_map.hpp"
#include "missing/unordered_set.hpp"

#ifdef RBX_WINDOWS
#include <winsock2.h>
#endif

namespace rubinius {
  namespace capi {
    class Handle;
    class Handles;
    class GlobalHandle;
  }

  namespace tooling {
    class ToolBroker;
  }

  namespace console {
    class Console;
  }

  namespace metrics {
    class Metrics;
  }

  namespace memory {
    class FinalizerThread;
    class ManagedThread;
  }

  class SignalThread;
  class Memory;
  class GlobalCache;
  class ConfigParser;
  class State;
  class VM;
  class Configuration;
  class LLVMState;
  class QueryAgent;
  class Environment;

  struct CallFrame;

  typedef std_unordered_set<std::string> CApiBlackList;
  typedef std::vector<utilities::thread::Mutex*> CApiLocks;
  typedef std_unordered_map<std::string, int> CApiLockMap;

  typedef std::vector<std::string> CApiConstantNameMap;
  typedef std_unordered_map<int, capi::Handle*> CApiConstantHandleMap;

  /**
   * SharedState represents the global shared state that needs to be shared
   * across all VM instances.
   *
   * Rubinius makes no use of global variables; instead, all shared state is
   * stored in a reference counted instance of this class. This makes it
   * possible in theory to have multiple independent Rubinius runtimes in a
   * single process.
   */

  class SharedState {
  private:
    ThreadNexus* thread_nexus_;
    InternalThreads* internal_threads_;
    SignalThread* signals_;
    memory::FinalizerThread* finalizer_thread_;
    console::Console* console_;
    metrics::Metrics* metrics_;

    CApiConstantNameMap capi_constant_name_map_;
    CApiConstantHandleMap capi_constant_handle_map_;

    uint64_t method_count_;
    unsigned int class_count_;
    int global_serial_;

    bool initialized_;
    bool check_global_interrupts_;
    bool check_gc_;

    VM* root_vm_;
    Environment* env_;
    tooling::ToolBroker* tool_broker_;

    utilities::thread::Mutex fork_exec_lock_;
    utilities::thread::Mutex codedb_lock_;

    utilities::thread::SpinLock capi_ds_lock_;
    utilities::thread::SpinLock capi_locks_lock_;
    utilities::thread::SpinLock capi_constant_lock_;
    utilities::thread::SpinLock global_capi_handle_lock_;
    utilities::thread::SpinLock capi_handle_cache_lock_;
    utilities::thread::SpinLock llvm_state_lock_;
    utilities::thread::SpinLock wait_lock_;
    utilities::thread::SpinLock type_info_lock_;
    utilities::thread::SpinLock code_resource_lock_;

    CApiBlackList capi_black_list_;
    CApiLocks capi_locks_;
    CApiLockMap capi_lock_map_;

    bool use_capi_lock_;
    int primitive_hits_[Primitives::cTotalPrimitives];

  public:
    Globals globals;
    Memory* om;
    GlobalCache* global_cache;
    Configuration& config;
    ConfigParser& user_variables;
    SymbolTable symbols;
    LLVMState* llvm_state;
    std::string username;
    std::string pid;
    uint32_t hash_seed;

  public:
    SharedState(Environment* env, Configuration& config, ConfigParser& cp);
    ~SharedState();

    int size();

    void set_initialized() {
      setup_capi_constant_names();
      initialized_ = true;
    }

    ThreadNexus* thread_nexus() {
      return thread_nexus_;
    }

    InternalThreads* internal_threads() const {
      return internal_threads_;
    }

    memory::FinalizerThread* finalizer_handler() const {
      return finalizer_thread_;
    }

    void set_finalizer_handler(memory::FinalizerThread* thr) {
      finalizer_thread_ = thr;
    }

    Array* vm_threads(STATE);

    int global_serial() const {
      return global_serial_;
    }

    int inc_global_serial(STATE) {
      return atomic::fetch_and_add(&global_serial_, (int)1);
    }

    uint32_t new_thread_id();

    int* global_serial_address() {
      return &global_serial_;
    }

    uint32_t inc_class_count(STATE) {
      return atomic::fetch_and_add(&class_count_, (uint32_t)1);
    }

    uint64_t inc_method_count(STATE) {
      return atomic::fetch_and_add(&method_count_, (uint64_t)1);
    }

    int inc_primitive_hit(int primitive) {
      return ++primitive_hits_[primitive];
    }

    int& primitive_hits(int primitive) {
      return primitive_hits_[primitive];
    }

    SignalThread* signals() const {
      return signals_;
    }

    SignalThread* start_signals(STATE);

    console::Console* console() const {
      return console_;
    }

    console::Console* start_console(STATE);

    metrics::Metrics* metrics() const {
      return metrics_;
    }

    metrics::Metrics* start_metrics(STATE);
    void disable_metrics(STATE);

    Environment* env() const {
      return env_;
    }

    void set_root_vm(VM* vm) {
      root_vm_ = vm;
    }

    VM* root_vm() const {
      return root_vm_;
    }

    tooling::ToolBroker* tool_broker() const {
      return tool_broker_;
    }

    Memory* memory() const {
      return om;
    }

    bool check_gc_p() {
      bool c = check_gc_;
      if(unlikely(c)) {
        check_gc_ = false;
      }
      return c;
    }

    void gc_soon() {
      check_global_interrupts_ = true;
      check_gc_ = true;
      thread_nexus_->set_stop();
    }

    bool check_global_interrupts() const {
      return check_global_interrupts_;
    }

    void set_check_global_interrupts() {
      check_global_interrupts_ = true;
    }

    void clear_check_global_interrupts() {
      check_global_interrupts_ = false;
    }

    bool* check_global_interrupts_address() {
      return &check_global_interrupts_;
    }

    const unsigned int* object_memory_mark_address() const;

    utilities::thread::Mutex& fork_exec_lock() {
      return fork_exec_lock_;
    }

    utilities::thread::Mutex& codedb_lock() {
      return codedb_lock_;
    }

    void set_use_capi_lock(bool s) {
      use_capi_lock_ = s;
    }

    utilities::thread::SpinLock& capi_ds_lock() {
      return capi_ds_lock_;
    }

    utilities::thread::SpinLock& capi_constant_lock() {
      return capi_constant_lock_;
    }

    utilities::thread::SpinLock& global_capi_handle_lock() {
      return global_capi_handle_lock_;
    }

    utilities::thread::SpinLock& capi_handle_cache_lock() {
      return capi_handle_cache_lock_;
    }

    int capi_lock_index(std::string name);

    utilities::thread::SpinLock& llvm_state_lock() {
      return llvm_state_lock_;
    }

    utilities::thread::SpinLock& wait_lock() {
      return wait_lock_;
    }

    utilities::thread::SpinLock& type_info_lock() {
      return type_info_lock_;
    }

    utilities::thread::SpinLock& code_resource_lock() {
      return code_resource_lock_;
    }

    void scheduler_loop();

    void after_fork_child(STATE);

    void enter_capi(STATE, const char* file, int line);
    void leave_capi(STATE);

    void setup_capi_constant_names();
    CApiConstantNameMap& capi_constant_name_map() {
      return capi_constant_name_map_;
    }

    CApiConstantHandleMap& capi_constant_handle_map() {
      return capi_constant_handle_map_;
    }

    void initialize_capi_black_list();
  };
}

#endif
