#include "builtin/dir.hpp"
#include "builtin/memorypointer.hpp"
#include "builtin/string.hpp"

#include <cstdio>
#include <sys/stat.h>
#include <cxxtest/TestSuite.h>

using namespace rubinius;

class TestDir : public CxxTest::TestSuite {
  public:

  VM *state;
  Dir *d;
  void setUp() {
    state = new VM(1024);
    d = Dir::create(state);
  }

  void tearDown() {
    if(!d->closed_p(state)->true_p()) d->close(state);
    delete state;
  }

  void test_create() {
    TS_ASSERT_EQUALS(d->obj_type, DirType);
    TS_ASSERT(d->data->nil_p());
  }

  void test_open() {
    String* path = String::create(state, ".");
    TS_ASSERT_EQUALS(d->open(state, path), Qnil);
    TS_ASSERT(!d->data->nil_p());
  }

  /*
  void _test_open_nonexistent() {
    String* path = String::create(state, "nonexistent");
    // TODO: change to TS_ASSERT_RAISES(d->open(state, path), {Ruby IOError});
  }
  */

  /*
  void _test_close_closed() {
    // TODO: TS_ASSERT_RAISES(d->close(state), {Ruby IOError});
  }
  */

  void test_close() {
    String* path = String::create(state, ".");
    d->open(state, path);
    TS_ASSERT_EQUALS(d->close(state), Qtrue);
    TS_ASSERT(d->data->nil_p());
  }

  void test_closed_p() {
    TS_ASSERT_EQUALS(d->closed_p(state), Qtrue);
    String* path = String::create(state, ".");
    d->open(state, path);
    TS_ASSERT_EQUALS(d->closed_p(state), Qfalse);
  }

  char* make_directory() {
    char *dir = tmpnam(NULL);
    mkdir(dir, S_IRWXU);
    return dir;
  }

  void remove_directory(const char *dir) {
    rmdir(dir);
  }

  void test_read() {
    char *dir = make_directory();
    String* path = String::create(state, dir);
    d->open(state, path);
    String* name = (String*)d->read(state);
    TS_ASSERT_EQUALS(name->byte_address(state)[0], '.');
    remove_directory(dir);
  }

  void test_read_returns_nil_when_no_more_entries() {
    char *dir = make_directory();
    String* path = String::create(state, dir);
    d->open(state, path);
    d->read(state);
    d->read(state);
    TS_ASSERT(d->read(state)->nil_p());
    remove_directory(dir);
  }

  /*
  void _test_read_raises_exception_if_closed() {
    // TODO: TS_ASSERT_RAISES(d->read(state), {Ruby IOError});
  }
  */

  void test_control_tells_current_position() {
    char *dir = make_directory();
    String* path = String::create(state, dir);
    d->open(state, path);
    FIXNUM pos = (FIXNUM)d->control(state, Fixnum::from(2), Fixnum::from(0));
    TS_ASSERT_EQUALS(pos->to_native(), 0);
    d->read(state);
    pos = (FIXNUM)d->control(state, Fixnum::from(2), Fixnum::from(0));
    TS_ASSERT_LESS_THAN(0, pos->to_native());
    remove_directory(dir);
  }

  void test_control_rewinds_read_location() {
    char *dir = make_directory();
    String* path = String::create(state, dir);
    d->open(state, path);
    d->read(state);
    d->read(state);
    TS_ASSERT(d->read(state)->nil_p());
    d->control(state, Fixnum::from(1), Fixnum::from(0));
    String* name = (String*)d->read(state);
    TS_ASSERT_EQUALS(name->byte_address()[0], '.');
    remove_directory(dir);
  }

  void test_control_seeks_to_a_known_position() {
    char *dir = make_directory();
    String* path = String::create(state, dir);
    d->open(state, path);
    d->read(state);
    FIXNUM pos = (FIXNUM)d->control(state, Fixnum::from(2), Fixnum::from(0));
    String* first = (String*)d->read(state);

    d->control(state, Fixnum::from(0), pos);
    String* second = (String*)d->read(state);
    TS_ASSERT_EQUALS(first->size(), second->size());
    TS_ASSERT_SAME_DATA(first, second, first->size());
    remove_directory(dir);
  }

  /*
  void _test_control_closed() {
    // TODO: TS_ASSERT_RAISES(d->control(state, Fixnum::from(0), Fixnum::from(0)), {Ruby IOError});
  }
  */

};
