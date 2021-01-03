#include <netdb.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdnoreturn.h>

#include "../libev/ev.h"
#include "ruby.h"
#include "ruby/io.h"
#include "../libev/ev.h"

ID ID_ivar_is_nonblocking;
ID ID_ivar_io;

// VALUE SYM_libev;

// Since we need to ensure that fd's are non-blocking before every I/O
// operation, here we improve upon Ruby's rb_io_set_nonblock by caching the
// "nonblock" state in an instance variable. Calling rb_ivar_get on every read
// is still much cheaper than doing a fcntl syscall on every read! Preliminary
// benchmarks (with a "hello world" HTTP server) show throughput is improved
// by 10-13%.
inline void io_set_nonblock(rb_io_t *fptr, VALUE io) {
  VALUE is_nonblocking = rb_ivar_get(io, ID_ivar_is_nonblocking);
  if (is_nonblocking == Qtrue) return;

  rb_ivar_set(io, ID_ivar_is_nonblocking, Qtrue);

#ifdef _WIN32
  rb_w32_set_nonblock(fptr->fd);
#elif defined(F_GETFL)
  int oflags = fcntl(fptr->fd, F_GETFL);
  if ((oflags == -1) && (oflags & O_NONBLOCK)) return;
  oflags |= O_NONBLOCK;
  fcntl(fptr->fd, F_SETFL, oflags);
#endif
}

typedef struct Scheduler_t {
  struct ev_loop *ev_loop;
  struct ev_async break_async;

  unsigned int currently_polling;
  VALUE ready; // holds ready fibers (used only while polling)
} Scheduler_t;

static size_t Scheduler_size(const void *ptr) {
  return sizeof(Scheduler_t);
}

static const rb_data_type_t Scheduler_type = {
    "LibevScheduler",
    {0, 0, Scheduler_size,},
    0, 0, RUBY_TYPED_FREE_IMMEDIATELY
};

static VALUE Scheduler_allocate(VALUE klass) {
  Scheduler_t *scheduler = ALLOC(Scheduler_t);

  return TypedData_Wrap_Struct(klass, &Scheduler_type, scheduler);
}

#define GetScheduler(obj, scheduler) \
  TypedData_Get_Struct((obj), Scheduler_t, &Scheduler_type, (scheduler))

void break_async_callback(struct ev_loop *ev_loop, struct ev_async *ev_async, int revents) {
  // This callback does nothing, the break async is used solely for breaking out
  // of a *blocking* event loop (waking it up) in a thread-safe, signal-safe manner
}

static VALUE Scheduler_initialize(VALUE self) {
  Scheduler_t *scheduler;
  VALUE thread = rb_thread_current();
  int is_main_thread = (thread == rb_thread_main());

  GetScheduler(self, scheduler);
  scheduler->ev_loop = is_main_thread ? EV_DEFAULT : ev_loop_new(EVFLAG_NOSIGMASK);

  ev_async_init(&scheduler->break_async, break_async_callback);
  ev_async_start(scheduler->ev_loop, &scheduler->break_async);
  ev_unref(scheduler->ev_loop); // don't count the break_async watcher

  scheduler->currently_polling = 0;

  return Qnil;
}

VALUE Scheduler_poll(VALUE self);

VALUE Scheduler_close(VALUE self) {
  Scheduler_t *scheduler;
  GetScheduler(self, scheduler);

  Scheduler_poll(self);

  ev_async_stop(scheduler->ev_loop, &scheduler->break_async);

  if (!ev_is_default_loop(scheduler->ev_loop)) ev_loop_destroy(scheduler->ev_loop);
  return self;
}

struct libev_timer {
  struct ev_timer timer;
  Scheduler_t *scheduler;
  VALUE fiber;
};

void Scheduler_timer_callback(EV_P_ ev_timer *w, int revents)
{
  struct libev_timer *timer = (struct libev_timer *)w;
  rb_ary_push(timer->scheduler->ready, timer->fiber);
}

VALUE Scheduler_sleep(VALUE self, VALUE duration) {
  Scheduler_t *scheduler;
  struct libev_timer timer;
  GetScheduler(self, scheduler);

  timer.scheduler = scheduler;
  timer.fiber = rb_fiber_current();
  ev_timer_init(&timer.timer, Scheduler_timer_callback, NUM2DBL(duration), 0.);
  ev_timer_start(scheduler->ev_loop, &timer.timer);
  VALUE nil = Qnil;
  VALUE ret = rb_fiber_yield(1, &nil);
  ev_timer_stop(scheduler->ev_loop, &timer.timer);
  return ret;
}

VALUE Scheduler_pause(VALUE self) {
  Scheduler_t *scheduler;
  GetScheduler(self, scheduler);

  ev_ref(scheduler->ev_loop);
  VALUE nil = Qnil;
  VALUE ret = rb_fiber_yield(1, &nil);
  ev_unref(scheduler->ev_loop);
  return ret;
}

VALUE Scheduler_block(int argc, VALUE *argv, VALUE self) {
  VALUE timeout = (argc == 2) ? argv[1] : Qnil;

  if (timeout != Qnil) return Scheduler_sleep(self, timeout);
  return Scheduler_pause(self);
}

VALUE Scheduler_unblock(VALUE self, VALUE blocker, VALUE fiber) {
  printf("unblock?\n");
  return self;
}





VALUE Scheduler_io_wait(VALUE self, VALUE io, VALUE events, VALUE duration) {
  return self;
}

VALUE Scheduler_process_wait(VALUE self, VALUE pid, VALUE flags) {
  return self;
}

VALUE Scheduler_poll(VALUE self) {
  Scheduler_t *scheduler;
  GetScheduler(self, scheduler);

  scheduler->ready = rb_ary_new();
  scheduler->currently_polling = 1;
  ev_run(scheduler->ev_loop, EVRUN_ONCE);
  scheduler->currently_polling = 0;

  unsigned int ready_count = RARRAY_LEN(scheduler->ready);
  VALUE nil = Qnil;
  for (unsigned int i = 0; i < ready_count; i++) {
    VALUE fiber = RARRAY_AREF(scheduler->ready, i);
    rb_fiber_resume(fiber, 1, &nil);
  }

  RB_GC_GUARD(scheduler->ready);

  return self;
}

VALUE Scheduler_wakeup(VALUE self) {
  Scheduler_t *backend;
  GetScheduler(self, backend);

  if (backend->currently_polling) {
    // Since the loop will run until at least one event has occurred, we signal
    // the selector's associated async watcher, which will cause the ev loop to
    // return. In contrast to using `ev_break` to break out of the loop, which
    // should be called from the same thread (from within the ev_loop), using an
    // `ev_async` allows us to interrupt the event loop across threads.
    ev_async_send(backend->ev_loop, &backend->break_async);
    return Qtrue;
  }

  return Qnil;
}

struct libev_io {
  struct ev_io io;
  VALUE fiber;
};

void Scheduler_io_callback(EV_P_ ev_io *w, int revents)
{
  struct libev_io *watcher = (struct libev_io *)w;
  Fiber_make_runnable(watcher->fiber, Qnil);
}

VALUE libev_wait_fd_with_watcher(Scheduler_t *backend, int fd, struct libev_io *watcher, int events) {
  VALUE switchpoint_result;

  if (watcher->fiber == Qnil) {
    watcher->fiber = rb_fiber_current();
    ev_io_init(&watcher->io, Scheduler_io_callback, fd, events);
  }
  ev_io_start(backend->ev_loop, &watcher->io);

  switchpoint_result = backend_await(backend);

  ev_io_stop(backend->ev_loop, &watcher->io);
  RB_GC_GUARD(switchpoint_result);
  return switchpoint_result;
}

VALUE libev_wait_fd(Scheduler_t *backend, int fd, int events, int raise_exception) {
  struct libev_io watcher;
  VALUE switchpoint_result = Qnil;
  watcher.fiber = Qnil;

  switchpoint_result = libev_wait_fd_with_watcher(backend, fd, &watcher, events);

  if (raise_exception) RAISE_IF_EXCEPTION(switchpoint_result);
  RB_GC_GUARD(switchpoint_result);
  return switchpoint_result;
}

VALUE Scheduler_wait_io(VALUE self, VALUE io, VALUE write) {
  Scheduler_t *backend;
  rb_io_t *fptr;
  int events = RTEST(write) ? EV_WRITE : EV_READ;
  VALUE underlying_io = rb_ivar_get(io, ID_ivar_io);
  if (underlying_io != Qnil) io = underlying_io;
  GetScheduler(self, backend);
  GetOpenFile(io, fptr);

  return libev_wait_fd(backend, fptr->fd, events, 1);
}

struct libev_child {
  struct ev_child child;
  VALUE fiber;
};

void Scheduler_child_callback(EV_P_ ev_child *w, int revents)
{
  struct libev_child *watcher = (struct libev_child *)w;
  int exit_status = WEXITSTATUS(w->rstatus);
  VALUE status;

  status = rb_ary_new_from_args(2, INT2NUM(w->rpid), INT2NUM(exit_status));
  Fiber_make_runnable(watcher->fiber, status);
}

VALUE Scheduler_waitpid(VALUE self, VALUE pid) {
  Scheduler_t *backend;
  struct libev_child watcher;
  VALUE switchpoint_result = Qnil;
  GetScheduler(self, backend);

  watcher.fiber = rb_fiber_current();
  ev_child_init(&watcher.child, Scheduler_child_callback, NUM2INT(pid), 0);
  ev_child_start(backend->ev_loop, &watcher.child);

  switchpoint_result = backend_await(backend);

  ev_child_stop(backend->ev_loop, &watcher.child);
  RAISE_IF_EXCEPTION(switchpoint_result);
  RB_GC_GUARD(watcher.fiber);
  RB_GC_GUARD(switchpoint_result);
  return switchpoint_result;
}

void Scheduler_async_callback(EV_P_ ev_async *w, int revents) { }

VALUE Scheduler_wait_event(VALUE self, VALUE raise) {
  Scheduler_t *backend;
  VALUE switchpoint_result = Qnil;
  GetScheduler(self, backend);

  struct ev_async async;

  ev_async_init(&async, Scheduler_async_callback);
  ev_async_start(backend->ev_loop, &async);

  switchpoint_result = backend_await(backend);

  ev_async_stop(backend->ev_loop, &async);
  if (RTEST(raise)) RAISE_IF_EXCEPTION(switchpoint_result);
  RB_GC_GUARD(switchpoint_result);
  return switchpoint_result;
}

// VALUE Scheduler_kind(VALUE self) {
//   return SYM_libev;
// }

void Init_Scheduler() {
  ev_set_allocator(xrealloc);

  VALUE mLibev = rb_define_module("Libev");
  VALUE cScheduler = rb_define_class_under(mLibev, "Scheduler", rb_cData);
  rb_define_alloc_func(cScheduler, Scheduler_allocate);

  rb_define_method(cScheduler, "initialize", Scheduler_initialize, 0);

  // fiber scheduler interface
  rb_define_method(cScheduler, "close", Scheduler_close, 0);
  rb_define_method(cScheduler, "io_wait", Scheduler_io_wait, 3);
  rb_define_method(cScheduler, "process_wait", Scheduler_process_wait, 2);
  rb_define_method(cScheduler, "block", Scheduler_block, -1);
  rb_define_method(cScheduler, "unblock", Scheduler_unblock, 2);

  // rb_define_method(cScheduler, "finalize", Scheduler_finalize, 0);
  // rb_define_method(cScheduler, "post_fork", Scheduler_post_fork, 0);

  // rb_define_method(cScheduler, "poll", Scheduler_poll, 3);
  // rb_define_method(cScheduler, "break", Scheduler_wakeup, 0);

  // rb_define_method(cScheduler, "read", Scheduler_read, 4);
  // rb_define_method(cScheduler, "read_loop", Scheduler_read_loop, 1);
  // rb_define_method(cScheduler, "write", Scheduler_write_m, -1);
  // rb_define_method(cScheduler, "accept", Scheduler_accept, 2);
  // rb_define_method(cScheduler, "accept_loop", Scheduler_accept_loop, 2);
  // rb_define_method(cScheduler, "connect", Scheduler_connect, 3);
  // rb_define_method(cScheduler, "recv", Scheduler_recv, 3);
  // rb_define_method(cScheduler, "recv_loop", Scheduler_read_loop, 1);
  // rb_define_method(cScheduler, "send", Scheduler_write, 2);
  // rb_define_method(cScheduler, "wait_io", Scheduler_wait_io, 2);
  // rb_define_method(cScheduler, "sleep", Scheduler_sleep, 1);
  // rb_define_method(cScheduler, "timer_loop", Scheduler_timer_loop, 1);
  // rb_define_method(cScheduler, "timeout", Scheduler_timeout, -1);
  // rb_define_method(cScheduler, "waitpid", Scheduler_waitpid, 1);
  // rb_define_method(cScheduler, "wait_event", Scheduler_wait_event, 1);

  // rb_define_method(cScheduler, "kind", Scheduler_kind, 0);

  ID_ivar_is_nonblocking = rb_intern("@is_nonblocking");
  ID_ivar_io             = rb_intern("@io");

  // SYM_libev = ID2SYM(rb_intern("libev"));
}