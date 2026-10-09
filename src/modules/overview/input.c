#include "input.h"

#include <linux/input-event-codes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cursor-shape-v1-client-protocol.h"
#include "layout.h"
#include "model.h"
#include "util.h"
#include "zoom.h"

void ov_input_activate(struct ov *ov, struct target t) {
  if (t.win)
    ov_model_focus_window(t.win);
  else if (t.ws)
    ov_model_focus_workspace(t.ws);
  else
    return;
  ov_request_close(ov);
}

static void set_cursor(struct ov *ov, uint32_t shape) {
  wl_set_cursor(ov->wl, shape);
}

/* ---- pointer --------------------------------------------------------- */

void ov_input_update_hover(struct ov *ov, long prev_con) {
  struct target t;
  /* the grown preview covers other windows but still belongs to its own */
  if (ov_zoom_contains(ov, ov->ptr_output, ov->px, ov->py))
    t = (struct target){ov->zoom.win, ov->zoom.win->ws};
  else
    t = ov_layout_hit(ov->ptr_output, ov->px, ov->py);
  if (!ov_target_eq(t, ov->hover)) {
    ov->hover = t;
    ov_redraw(ov);
  }
  if ((t.win ? t.win->con_id : -1) != prev_con)
    ov_zoom_hover_changed(ov);
}

static void update_hover(struct ov *ov) {
  ov_input_update_hover(ov, ov->hover.win ? ov->hover.win->con_id : -1);
}

static void pointer_enter(void *data, struct wl_surface *s, double x,
                          double y) {
  struct output *out = data;
  struct ov *ov = out->ov;
  ov->ptr_output = out;
  ov->px = x;
  ov->py = y;
  set_cursor(ov, ov->drag.active ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING
                                 : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
  update_hover(ov);
}

static void pointer_leave(void *data, struct wl_surface *s) {
  struct output *out = data;
  struct ov *ov = out->ov;
  if (ov->ptr_output != out)
    return; /* already entered another output */
  ov->ptr_output = NULL;
  ov->hover = (struct target){0};
  ov_zoom_hover_changed(ov);
  ov_redraw(ov);
}

static void pointer_motion(void *data, double x, double y) {
  struct output *out = data;
  struct ov *ov = out->ov;
  ov->px = x;
  ov->py = y;

  if (ov->drag.pressed && !ov->drag.active &&
      (ov->drag.press.win || ov_layout_reorderable(ov->drag.press.ws)) &&
      hypot(ov->px - ov->drag.start_x, ov->py - ov->drag.start_y) >
          ov->cfg.drag_threshold) {
    ov->drag.active = true;
    set_cursor(ov, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING);
  }
  update_hover(ov);
  if (ov->drag.active)
    ov_redraw(ov);
}

/* Move ws to index slot among the reorderable workspaces of its output. */
static void move_workspace(struct workspace *ws, int slot) {
  struct workspace **list;
  int n = ov_layout_reorder_list(ws, &list);
  int from = -1;
  for (int i = 0; i < n; ++i)
    if (list[i] == ws)
      from = i;
  int *nums = xcalloc(n, sizeof(*nums));
  int *sorted = xcalloc(n, sizeof(*sorted));
  for (int i = 0; i < n; ++i)
    nums[i] = list[i]->num;
  if (ov_reorder_plan((void **)list, nums, n, from, slot, sorted))
    ov_model_renumber_workspaces(list, sorted, n);
  free(sorted);
  free(nums);
  free(list);
}

static void drop(struct ov *ov) {
  if (!ov->drag.press.win) {
    struct workspace *ws = ov->drag.press.ws;
    int slot = ov_layout_drop_slot(ws, ov->hover.ws);
    if (slot >= 0)
      move_workspace(ws, slot);
    return;
  }
  struct window *w = ov->drag.press.win;
  struct workspace *ws = ov->hover.ws;
  if (w && ws && ws != w->ws) {
    ov_model_move_window(w, ws);
    ov->selected = (struct target){w, ws};
    /* the model reload triggered by sway's events fixes up the layout */
  }
}

static void pointer_button(void *data, uint32_t button, bool pressed) {
  struct output *out = data;
  struct ov *ov = out->ov;
  if (ov->close_pending)
    return;

  if (button == BTN_LEFT) {
    if (pressed) {
      ov->drag.pressed = true;
      ov->drag.active = false;
      ov->drag.press = ov->hover;
      ov->drag.output = ov->ptr_output;
      ov->drag.start_x = ov->px;
      ov->drag.start_y = ov->py;
      /* where inside the window it was grabbed, in tile coordinates */
      struct window *w = ov->hover.win;
      if (w && ov_zoom_contains(ov, ov->ptr_output, ov->px, ov->py)) {
        struct box z = ov->zoom.big;
        ov->drag.grab_dx =
            (ov->px - z.x) / z.w * w->cbox.w + (w->cbox.x - w->box.x);
        ov->drag.grab_dy =
            (ov->py - z.y) / z.h * w->cbox.h + (w->cbox.y - w->box.y);
      } else if (w) {
        ov->drag.grab_dx = ov->px - w->box.x;
        ov->drag.grab_dy = ov->py - w->box.y;
      } else if (ov->hover.ws) {
        ov->drag.grab_dx = ov->px - ov->hover.ws->box.x;
        ov->drag.grab_dy = ov->py - ov->hover.ws->box.y;
      }
      return;
    }
    if (!ov->drag.pressed)
      return;
    if (ov->drag.active) {
      drop(ov);
      set_cursor(ov, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
    } else if (!ov->hover.ws && !ov->drag.press.ws) {
      ov_request_close(ov); /* click on the background closes */
    } else if (ov_target_eq(ov->hover, ov->drag.press)) {
      ov_input_activate(ov, ov->hover);
    }
    ov->drag.pressed = false;
    ov->drag.active = false;
    ov->drag.press = (struct target){0};
    ov_redraw(ov);
  } else if (button == BTN_MIDDLE && pressed && ov->hover.win) {
    ov_model_kill_window(ov->hover.win);
  } else if (button == BTN_RIGHT && pressed && !ov->drag.pressed) {
    ov_request_close(ov);
  }
}

/* ---- keyboard -------------------------------------------------------- */

static void select_target(struct ov *ov, struct target t) {
  if (!t.win && !t.ws)
    return;
  ov->selected = t;
  ov_redraw(ov);
}

static void move_selected(struct ov *ov, int delta) {
  struct window *w = ov->selected.win;
  if (!w)
    return;
  struct workspace *ws = ov_layout_step_workspace(ov, w->ws, delta);
  if (ws)
    ov_model_move_window(w, ws);
}

static void move_selected_workspace(struct ov *ov, int delta) {
  struct workspace *ws = ov->selected.ws;
  if (!ov_layout_reorderable(ws))
    return;
  struct workspace **list;
  int n = ov_layout_reorder_list(ws, &list);
  for (int i = 0; i < n; ++i)
    if (list[i] == ws) {
      move_workspace(ws, i + delta);
      break;
    }
  free(list);
}

static void jump_to_number(struct ov *ov, int num) {
  struct model *m = &ov->model;
  for (int i = 0; i < m->nworkspaces; ++i) {
    struct workspace *ws = m->workspaces[i];
    if (!ws->placeholder && ws->num == num) {
      ov_input_activate(ov, (struct target){NULL, ws});
      return;
    }
  }
}

static bool mod_active(struct ov *ov, const char *name) {
  return ov->wl->xkb_state &&
         xkb_state_mod_name_is_active(ov->wl->xkb_state, name,
                                      XKB_STATE_MODS_EFFECTIVE) > 0;
}

/* Returns true if the key should auto-repeat. */
static bool handle_key(struct ov *ov, xkb_keysym_t sym) {
  bool shift = mod_active(ov, XKB_MOD_NAME_SHIFT);
  bool ctrl = mod_active(ov, XKB_MOD_NAME_CTRL);
  switch (sym) {
  case XKB_KEY_Escape:
  case XKB_KEY_q:
    ov_request_close(ov);
    return false;
  case XKB_KEY_Return:
  case XKB_KEY_KP_Enter:
  case XKB_KEY_space:
    ov_input_activate(ov, ov->selected);
    return false;
  case XKB_KEY_Tab:
    select_target(ov, ov_layout_step(ov, ov->selected, shift ? -1 : 1));
    return true;
  case XKB_KEY_ISO_Left_Tab:
    select_target(ov, ov_layout_step(ov, ov->selected, -1));
    return true;
  case XKB_KEY_H:
    move_selected(ov, -1);
    return false;
  case XKB_KEY_L:
    move_selected(ov, 1);
    return false;
  case XKB_KEY_Left:
  case XKB_KEY_h:
    if (ctrl) {
      move_selected_workspace(ov, -1);
      return false;
    }
    if (shift) {
      move_selected(ov, -1);
      return false;
    }
    select_target(ov, ov_layout_navigate(ov, ov->selected, -1, 0));
    return true;
  case XKB_KEY_Right:
  case XKB_KEY_l:
    if (ctrl) {
      move_selected_workspace(ov, 1);
      return false;
    }
    if (shift) {
      move_selected(ov, 1);
      return false;
    }
    select_target(ov, ov_layout_navigate(ov, ov->selected, 1, 0));
    return true;
  case XKB_KEY_Up:
  case XKB_KEY_k:
    select_target(ov, ov_layout_navigate(ov, ov->selected, 0, -1));
    return true;
  case XKB_KEY_Down:
  case XKB_KEY_j:
    select_target(ov, ov_layout_navigate(ov, ov->selected, 0, 1));
    return true;
  default:
    if (sym >= XKB_KEY_1 && sym <= XKB_KEY_9)
      jump_to_number(ov, sym - XKB_KEY_0);
    else if (sym == XKB_KEY_0)
      jump_to_number(ov, 10);
    return false;
  }
}

static void stop_repeat(struct ov *ov) {
  ov->repeat_sym = XKB_KEY_NoSymbol;
  if (ov->repeat)
    loop_timer_disarm(ov->repeat);
}

void ov_input_repeat(struct loop_timer *t, void *data) {
  struct ov *ov = data;
  if (ov->repeat_sym != XKB_KEY_NoSymbol && ov->open && !ov->close_pending)
    handle_key(ov, ov->repeat_sym);
}

static void keyboard_enter(void *data, struct wl_surface *s) {}

static void keyboard_leave(void *data, struct wl_surface *s) {
  struct output *out = data;
  stop_repeat(out->ov);
}

static void key(void *data, xkb_keycode_t keycode, bool pressed) {
  struct output *out = data;
  struct ov *ov = out->ov;
  struct swell_wl *wl = ov->wl;
  if (!wl->xkb_state)
    return;
  xkb_keysym_t sym = xkb_state_key_get_one_sym(wl->xkb_state, keycode);

  if (!pressed) {
    if (sym == ov->repeat_sym)
      stop_repeat(ov);
    return;
  }
  if (ov->close_pending)
    return;

  bool repeats = handle_key(ov, sym);
  if (repeats && ov->repeat && wl->repeat_rate > 0 &&
      xkb_keymap_key_repeats(wl->keymap, keycode)) {
    ov->repeat_sym = sym;
    int interval = 1000 / wl->repeat_rate;
    loop_timer_arm(ov->repeat, wl->repeat_delay > 0 ? wl->repeat_delay : 1,
                   interval > 0 ? interval : 1);
  } else {
    stop_repeat(ov);
  }
}

const struct swell_input ov_input_handler = {
    .pointer_enter = pointer_enter,
    .pointer_leave = pointer_leave,
    .pointer_motion = pointer_motion,
    .pointer_button = pointer_button,
    .keyboard_enter = keyboard_enter,
    .keyboard_leave = keyboard_leave,
    .key = key,
};
