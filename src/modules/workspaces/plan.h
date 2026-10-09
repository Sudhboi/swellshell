/* Pure renumbering logic (unit-tested without sway). */
#ifndef SWELL_WORKSPACES_PLAN_H
#define SWELL_WORKSPACES_PLAN_H

/* Workspaces should be named 1..n in sway's order. For each of the n names
 * that is not already its target, writes the target number into want[i];
 * otherwise want[i] = 0. Returns the number of renames needed. */
int ws_plan_renumber(const char *const *names, int n, int *want);

/* The sway command list that renames the workspaces to 1..n, or NULL if
 * they already are. When a target name is still taken by another workspace
 * (e.g. "1:mail" and "1"), everything goes through temporary names first.
 * One command list, so sway applies it at once. Caller frees. */
char *ws_plan_commands(const char *const *names, int n);

/* Is s a workspace number we accept for `goto` (1..999999999)? */
int ws_parse_number(const char *s);

#endif
