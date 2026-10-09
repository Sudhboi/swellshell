/* The daemon's event loop: poll() over registered fds, timerfd timers,
 * child processes (pidfd) and prepare hooks that run before every poll. */
#ifndef SWELL_LOOP_H
#define SWELL_LOOP_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

struct loop;
struct loop_watch;
struct loop_timer;

typedef void (*loop_fd_fn)(int fd, short revents, void *data);
typedef void (*loop_timer_fn)(struct loop_timer *t, void *data);
/* Runs before every poll(). Returns a poll timeout in ms, or -1 for none. */
typedef int (*loop_prepare_fn)(void *data);
/* A child started with loop_watch_child() exited with status (waitpid). */
typedef void (*loop_child_fn)(pid_t pid, int status, void *data);

struct loop *loop_create(void);
void loop_destroy(struct loop *loop);

/* Run until loop_quit() or SIGINT/SIGTERM. */
void loop_run(struct loop *loop);
void loop_quit(struct loop *loop);
bool loop_running(struct loop *loop);

/* Watch fd for events (POLLIN, ...). The callback also gets POLLERR/POLLHUP.
 * Removing a watch (also from inside a callback) is safe; the fd is not
 * closed. */
struct loop_watch *loop_add_fd(struct loop *loop, int fd, short events,
                               loop_fd_fn fn, void *data);
void loop_watch_set_events(struct loop_watch *w, short events);
void loop_remove_fd(struct loop_watch *w);

/* A timer on clock (CLOCK_MONOTONIC or CLOCK_REALTIME). Disarmed at first. */
struct loop_timer *loop_add_timer(struct loop *loop, int clock,
                                  loop_timer_fn fn, void *data);
/* Fire after ms (relative), then every interval_ms (0: once). ms <= 0
 * disarms. */
void loop_timer_arm(struct loop_timer *t, int64_t ms, int64_t interval_ms);
/* Arm with an absolute struct itimerspec on the timer's clock. */
void loop_timer_arm_abs(struct loop_timer *t, const struct itimerspec *its);
void loop_timer_disarm(struct loop_timer *t);
bool loop_timer_armed(struct loop_timer *t);
void loop_remove_timer(struct loop_timer *t);

/* Prepare hooks run in ascending priority order (wayland flushes at 100). */
void loop_add_prepare(struct loop *loop, int priority, loop_prepare_fn fn,
                      void *data);
void loop_remove_prepare(struct loop *loop, loop_prepare_fn fn, void *data);

/* Call fn once pid exits; the loop reaps it. Returns false if pidfd_open
 * failed (the caller should then waitpid itself). */
bool loop_watch_child(struct loop *loop, pid_t pid, loop_child_fn fn,
                      void *data);

#endif
