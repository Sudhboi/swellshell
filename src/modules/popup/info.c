/* System information for the popup: sysfs/proc reads, plus wpctl, nmcli
 * and playerctl run asynchronously with their pipes on the daemon loop. */
#include "info.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "battery.h"
#include "util.h"

/* --- small file helpers --- */

static bool read_line(const char *path, char *buf, size_t n) {
  FILE *f = fopen(path, "re");
  if (!f)
    return false;
  bool ok = fgets(buf, (int)n, f) != NULL;
  fclose(f);
  if (ok)
    buf[strcspn(buf, "\n")] = '\0';
  return ok;
}

static bool read_long(const char *path, long *v) {
  char buf[64];
  if (!read_line(path, buf, sizeof(buf)))
    return false;
  char *end;
  *v = strtol(buf, &end, 10);
  return end != buf;
}

static void copy_str(char *dst, size_t n, const char *src) {
  snprintf(dst, n, "%s", src);
}

/* --- sysfs / proc --- */

static void read_battery(struct info *in) {
  in->has_battery = battery_read(&in->battery, &in->charging);
  in->has_power =
      in->has_battery && battery_power(&in->power_w, &in->power_charging);
}

static void read_brightness(struct info *in) {
  DIR *d = opendir("/sys/class/backlight");
  in->has_brightness = false;
  if (!d)
    return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.')
      continue;
    char path[300];
    long cur, max;
    snprintf(path, sizeof(path), "/sys/class/backlight/%s/brightness",
             e->d_name);
    if (!read_long(path, &cur))
      continue;
    snprintf(path, sizeof(path), "/sys/class/backlight/%s/max_brightness",
             e->d_name);
    if (!read_long(path, &max) || max <= 0)
      continue;
    in->has_brightness = true;
    in->brightness = (int)((cur * 100 + max / 2) / max);
    break;
  }
  closedir(d);
}

static void read_cpu(struct info *in, int refresh_ms) {
  static unsigned long long prev_busy, prev_total;
  static struct timespec prev_ts;
  unsigned long long v[8] = {0};
  char buf[256];

  if (!read_line("/proc/stat", buf, sizeof(buf)) ||
      sscanf(buf, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1],
             &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) < 4)
    return;
  unsigned long long idle = v[3] + v[4], total = 0;
  for (int i = 0; i < 8; i++)
    total += v[i];
  unsigned long long busy = total - idle;

  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  double age =
      (ts.tv_sec - prev_ts.tv_sec) + (ts.tv_nsec - prev_ts.tv_nsec) / 1e9;
  /* A stale baseline (popup was hidden) would give a long-term average;
   * keep the last value and just take a new baseline. */
  if (prev_total && total > prev_total && age < 3.0 * refresh_ms / 1000)
    in->cpu = (int)((busy - prev_busy) * 100 / (total - prev_total));
  prev_busy = busy;
  prev_total = total;
  prev_ts = ts;
}

static void read_mem(struct info *in) {
  FILE *f = fopen("/proc/meminfo", "re");
  if (!f)
    return;
  char line[128];
  long total = 0, avail = 0, v;
  while (fgets(line, sizeof(line), f)) {
    if (sscanf(line, "MemTotal: %ld", &v) == 1)
      total = v;
    else if (sscanf(line, "MemAvailable: %ld", &v) == 1)
      avail = v;
  }
  fclose(f);
  in->mem_total_gib = total / 1048576.0;
  in->mem_used_gib = (total - avail) / 1048576.0;
}

static void read_uptime(struct info *in) {
  char buf[64];
  if (read_line("/proc/uptime", buf, sizeof(buf)))
    in->uptime_s = atol(buf);
}

/* --- async commands --- */

enum { CMD_VOLUME, CMD_NETDEV, CMD_WIFI, CMD_COUNT };

/* A command still running at the next tick isn't re-spawned; after this
 * long it is killed. */

struct cmd {
  const char *const *argv;
  pid_t pid;    /* > 0 until reaped */
  bool watched; /* the loop reaps pid (pidfd); else we waitpid */
  int fd;       /* read end of its stdout, -1 once read to EOF */
  struct loop_watch *watch;
  struct timespec started;
  char buf[1024];
  size_t len;
};

static const char *const argv_volume[] = {"wpctl", "get-volume",
                                          "@DEFAULT_AUDIO_SINK@", NULL};
static const char *const argv_netdev[] = {
    "nmcli", "-t", "-f", "TYPE,STATE,CONNECTION", "device", NULL};
static const char *const argv_wifi[] = {
    "nmcli",    "-t", "-f", "IN-USE,SIGNAL", "device", "wifi", "list",
    "--rescan", "no", NULL};

static struct cmd cmds[CMD_COUNT] = {
    [CMD_VOLUME] = {.argv = argv_volume, .fd = -1},
    [CMD_NETDEV] = {.argv = argv_netdev, .fd = -1},
    [CMD_WIFI] = {.argv = argv_wifi, .fd = -1},
};

static struct loop *loop;
static struct info *info;
static info_changed_fn changed_fn;
static void *changed_data;

static void parse(struct info *in, int which, char *out);

static void cmd_close(struct cmd *c) {
  loop_remove_fd(c->watch);
  c->watch = NULL;
  if (c->fd >= 0)
    close(c->fd);
  c->fd = -1;
}

/* Forget the process: the loop reaps a watched one; otherwise wait for it
 * here (as the original did). */
static void cmd_release(struct cmd *c) {
  if (c->pid > 0 && !c->watched)
    waitpid(c->pid, NULL, 0);
  c->pid = 0;
}

static void on_child(pid_t pid, int status, void *data) {
  struct cmd *c = data;
  if (c->pid == pid)
    c->pid = 0;
}

static void on_output(int fd, short revents, void *data) {
  struct cmd *c = data;
  ssize_t r;
  while ((r = read(c->fd, c->buf + c->len, sizeof(c->buf) - 1 - c->len)) > 0)
    c->len += (size_t)r;
  if (r == 0 || (r < 0 && errno != EAGAIN) || c->len == sizeof(c->buf) - 1) {
    c->buf[c->len] = '\0';
    cmd_close(c);
    if (!c->watched)
      cmd_release(c);
    parse(info, (int)(c - cmds), c->buf);
    if (changed_fn)
      changed_fn(changed_data);
  }
}

static void cmd_start(struct cmd *c, int kill_ms) {
  if (c->fd >= 0 || c->pid > 0) {
    /* Still running from last tick: give it kill_ms, then kill. */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long long ran = (ts.tv_sec - c->started.tv_sec) * 1000LL +
                    (ts.tv_nsec - c->started.tv_nsec) / 1000000;
    if (ran < kill_ms)
      return;
    if (c->pid > 0)
      kill(c->pid, SIGKILL);
    cmd_close(c);
    cmd_release(c);
  }
  int p[2];
  if (pipe2(p, O_CLOEXEC) < 0)
    return;
  pid_t pid = fork();
  if (pid < 0) {
    close(p[0]);
    close(p[1]);
    return;
  }
  if (pid == 0) {
    /* The daemon blocks signals for its signalfd and ignores SIGPIPE;
     * don't pass that on. */
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
    signal(SIGPIPE, SIG_DFL);
    dup2(p[1], STDOUT_FILENO);
    int null = open("/dev/null", O_RDWR);
    if (null >= 0) {
      dup2(null, STDIN_FILENO);
      dup2(null, STDERR_FILENO);
    }
    execvp(c->argv[0], (char *const *)c->argv);
    _exit(127);
  }
  close(p[1]);
  fcntl(p[0], F_SETFL, O_NONBLOCK);
  c->pid = pid;
  c->watched = loop_watch_child(loop, pid, on_child, c);
  c->fd = p[0];
  c->len = 0;
  c->watch = loop_add_fd(loop, c->fd, POLLIN, on_output, c);
  clock_gettime(CLOCK_MONOTONIC, &c->started);
}

/* nmcli -t escapes ':' and '\' in values with a backslash. */
static void nm_unescape(char *s) {
  char *o = s;
  for (; *s; s++) {
    if (*s == '\\' && s[1])
      s++;
    *o++ = *s;
  }
  *o = '\0';
}

/* Split an nmcli -t line on unescaped ':'; returns the field count. */
static int nm_split(char *line, char **f, int max) {
  int n = 0;
  f[n++] = line;
  for (char *s = line; *s && n < max; s++) {
    if (*s == '\\' && s[1])
      s++;
    else if (*s == ':') {
      *s = '\0';
      f[n++] = s + 1;
    }
  }
  for (int i = 0; i < n; i++)
    nm_unescape(f[i]);
  return n;
}

static void parse(struct info *in, int which, char *out) {
  switch (which) {
  case CMD_VOLUME: {
    double v;
    in->has_volume = sscanf(out, "Volume: %lf", &v) == 1;
    if (in->has_volume) {
      in->volume = (int)(v * 100 + 0.5);
      in->muted = strstr(out, "MUTED") != NULL;
    }
    break;
  }
  case CMD_NETDEV: {
    enum net_kind kind = NET_OFFLINE;
    char *save, *line;
    in->net_name[0] = '\0';
    for (line = strtok_r(out, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
      char *f[3];
      if (nm_split(line, f, 3) < 3 || strcmp(f[1], "connected"))
        continue;
      if (!strcmp(f[0], "wifi"))
        kind = NET_WIFI;
      else if (!strcmp(f[0], "ethernet"))
        kind = NET_WIRED;
      else
        continue;
      copy_str(in->net_name, sizeof(in->net_name), f[2]);
      for (size_t l = strlen(in->net_name); l && in->net_name[l - 1] == ' ';)
        in->net_name[--l] = '\0';
      break;
    }
    in->net = kind;
    break;
  }
  case CMD_WIFI: {
    char *save, *line;
    in->net_signal = -1;
    for (line = strtok_r(out, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
      char *f[2];
      if (nm_split(line, f, 2) == 2 && !strcmp(f[0], "*")) {
        in->net_signal = atoi(f[1]);
        break;
      }
    }
    break;
  }
  }
}

/* --- workspaces --- */

static int ws_cmp(const void *a, const void *b) {
  const struct workspace *x = a, *y = b;
  if ((x->num < 0) != (y->num < 0))
    return x->num < 0 ? 1 : -1;
  if (x->num != y->num)
    return x->num < y->num ? -1 : 1;
  return strcmp(x->name, y->name);
}

bool info_set_workspaces(struct info *in, const struct wm_workspace *ws,
                         int n) {
  struct workspace out[WS_MAX];
  int count = 0;
  memset(out, 0, sizeof(out));
  for (int i = 0; i < n && count < WS_MAX; i++) {
    struct workspace *w = &out[count++];
    size_t len = strnlen(ws[i].name, sizeof(ws[i].name));
    if (len >= sizeof(w->name))
      len = sizeof(w->name) - 1;
    memcpy(w->name, ws[i].name, len);
    w->num = ws[i].num;
    w->focused = ws[i].focused;
    w->urgent = ws[i].urgent;
  }
  qsort(out, (size_t)count, sizeof(out[0]), ws_cmp);
  if (count == in->ws_count &&
      !memcmp(out, in->ws, (size_t)count * sizeof(out[0])))
    return false;
  memset(in->ws, 0, sizeof(in->ws));
  memcpy(in->ws, out, (size_t)count * sizeof(out[0]));
  in->ws_count = count;
  return true;
}

/* --- public --- */

void info_init(struct loop *l, struct info *in, info_changed_fn fn,
               void *data) {
  loop = l;
  info = in;
  changed_fn = fn;
  changed_data = data;
}

void info_finish(void) {
  for (int i = 0; i < CMD_COUNT; i++) {
    struct cmd *c = &cmds[i];
    if (c->pid > 0)
      kill(c->pid, SIGKILL);
    cmd_close(c);
    cmd_release(c);
  }
  changed_fn = NULL;
}

void info_refresh(struct info *in, int refresh_ms, int kill_ms) {
  in->now = time(NULL);
  read_battery(in);
  read_brightness(in);
  read_cpu(in, refresh_ms);
  read_mem(in);
  read_uptime(in);
  for (int i = 0; i < CMD_COUNT; i++)
    cmd_start(&cmds[i], kill_ms);
}
