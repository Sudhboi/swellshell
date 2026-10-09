/* overview: placing workspaces and windows on each output, hit testing,
 * navigation and workspace reordering. */
#ifndef SWELL_OVERVIEW_LAYOUT_H
#define SWELL_OVERVIEW_LAYOUT_H

#include "overview.h"

/* Assign workspaces to outputs and compute every box. */
void ov_layout_all(struct ov *ov);

/* What is under (x, y) on out. Either field may be NULL. */
struct target ov_layout_hit(struct output *out, double x, double y);

/* The next target in direction (dx, dy), one of which is zero. */
struct target ov_layout_navigate(struct ov *ov, struct target cur, int dx,
                                 int dy);
/* The target delta steps away in reading order (wraps around). */
struct target ov_layout_step(struct ov *ov, struct target cur, int delta);
/* The workspace delta steps away from ws in reading order, or NULL. */
struct workspace *ov_layout_step_workspace(struct ov *ov, struct workspace *ws,
                                           int delta);
/* Default selection: the focused window, else the focused workspace. */
struct target ov_layout_initial(struct ov *ov);

bool ov_target_eq(struct target a, struct target b);

/* Whether ws can be moved within its output (it must be numbered). */
bool ov_layout_reorderable(const struct workspace *ws);
/* The reorderable workspaces on ws's output, in display order. Returns the
 * count; *list is to be freed. */
int ov_layout_reorder_list(struct workspace *ws, struct workspace ***list);
/* The index in that list ws would move to when dropped on target, or -1 if
 * the drop would do nothing. */
int ov_layout_drop_slot(struct workspace *ws, struct workspace *target);

#endif
