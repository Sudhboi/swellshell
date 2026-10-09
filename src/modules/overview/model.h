/* overview: the model of sway's tree, and sway commands. */
#ifndef SWELL_OVERVIEW_MODEL_H
#define SWELL_OVERVIEW_MODEL_H

#include "overview.h"

/* Rebuild *m from get_tree/get_workspaces. new_slot adds a placeholder
 * workspace per output for "move to new workspace"; spread_gap is the gap
 * between spread-out tabs (tree pixels). */
bool ov_model_load(struct model *m, bool new_slot, double spread_gap);
void ov_model_free(struct model *m);
struct window *ov_model_find_window(struct model *m, long con_id);
struct workspace *ov_model_find_workspace(struct model *m, long id);

void ov_model_focus_window(struct window *win);
void ov_model_focus_workspace(struct workspace *ws);
void ov_model_move_window(struct window *win, struct workspace *ws);
void ov_model_kill_window(struct window *win);

/* Pure: the sway command list that gives the n workspaces names[i] the
 * numbers nums[i], keeping any non-numeric suffix ("2:web" -> "4:web"),
 * through temporary names so no rename hits a taken name. new_names[i]
 * receives each new name (malloc'd). Returns NULL if nothing changes. */
char *ov_renumber_command(const char *const *names, const int *nums, int n,
                          char **new_names);

/* Pure: move the element at from to slot (shifting the ones between) in the
 * array of n pointers, and write the ascending sort of nums to sorted.
 * Returns false if the move is out of range or does nothing. */
bool ov_reorder_plan(void **list, const int *nums, int n, int from, int slot,
                     int *sorted);

/* Give the n workspaces in order the numbers nums (ascending) by renaming
 * them in one sway command list. Since sway sorts workspaces by number,
 * this reorders them. Updates the names in the model. */
void ov_model_renumber_workspaces(struct workspace **order, const int *nums,
                                  int n);

#endif
