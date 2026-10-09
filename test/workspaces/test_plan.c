#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plan.h"

int main(void) {
  int want[8];
  const char *ok[] = {"1", "2", "3"};
  assert(ws_plan_renumber(ok, 3, want) == 0);
  assert(want[0] == 0 && want[1] == 0 && want[2] == 0);

  const char *gap[] = {"1", "3", "4", "mail"};
  assert(ws_plan_renumber(gap, 4, want) == 3);
  assert(want[0] == 0 && want[1] == 2 && want[2] == 3 && want[3] == 4);

  const char *named[] = {"2:web", "5"};
  assert(ws_plan_renumber(named, 2, want) == 2);
  assert(want[0] == 1 && want[1] == 2);
  /* commands */
  assert(ws_plan_commands(ok, 3) == NULL);
  char *c = ws_plan_commands(gap, 4);
  assert(strcmp(c, "rename workspace \"3\" to \"2\"; rename workspace \"4\" "
                   "to \"3\"; rename workspace \"mail\" to \"4\"") == 0);
  free(c);
  /* "1:mail" must become 2 while "2" still exists: temporary names */
  const char *mail[] = {"1", "1:mail", "2"};
  c = ws_plan_commands(mail, 3);
  assert(strstr(c, "swell-renumber-") != NULL); /* "2" is taken by index 2 */
  free(c);
  const char *quoted[] = {"a\"b"};
  c = ws_plan_commands(quoted, 1);
  assert(strcmp(c, "rename workspace \"a\\\"b\" to \"1\"") == 0);
  free(c);

  assert(ws_parse_number("3") == 3 && ws_parse_number("10") == 10);
  assert(ws_parse_number("0") == 0 && ws_parse_number("") == 0);
  assert(ws_parse_number("1\"; exec x") == 0);
  assert(ws_parse_number("99999999999") == 0);
  puts("plan ok");
  return 0;
}
