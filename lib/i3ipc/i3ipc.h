/* SPDX-License-Identifier:  CC0-1.0 */

/* i3ipc-simple, a simple C/C++ library to interact with i3's IPC interface
 * Written by Philipp Czerner in 2020 <i3ipc-simple@nicze.de>
 *
 * To the extent possible under law, the author(s) have dedicated all copyright
 * and related and neighboring rights to this software to the public domain
 * worldwide. This software is distributed without any warranty.
 *
 * You should have received a copy of the CC0 Public Domain Dedication along
 * with this software. If not, see
 *     http://creativecommons.org/publicdomain/zero/1.0/    */

/* *** How to include ***
 * This is a single-header library, so you need to #define I3IPC_IMPLEMENTATION
 * before including this file to create the implementation. This needs to happen
 * exactly once. For example:
 *
 *     // file1.c
 *     #include "i3ipc.h"
 *     ... use the functions ...
 *
 *     // file2.c
 *     #define I3IPC_IMPLEMENTATION
 *     #include "i3ipc.h"
 *     ... use the functions ...
 *
 * The library can be found at https://github.com/suyjuris/i3ipc-simple .
 * The README contains high-level documentation. */

#ifndef I3IPC_INCLUDE_I3IPC_H
#define I3IPC_INCLUDE_I3IPC_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* Forward declare structs, definitions are below */
typedef struct I3ipc_reply_command I3ipc_reply_command;
typedef struct I3ipc_reply_workspaces I3ipc_reply_workspaces;
typedef struct I3ipc_reply_subscribe I3ipc_reply_subscribe;
typedef struct I3ipc_reply_outputs I3ipc_reply_outputs;
typedef struct I3ipc_reply_tree I3ipc_reply_tree;
typedef struct I3ipc_reply_marks I3ipc_reply_marks;
typedef struct I3ipc_reply_bar_config_ids I3ipc_reply_bar_config_ids;
typedef struct I3ipc_reply_bar_config I3ipc_reply_bar_config;
typedef struct I3ipc_reply_version I3ipc_reply_version;
typedef struct I3ipc_reply_binding_modes I3ipc_reply_binding_modes;
typedef struct I3ipc_reply_config I3ipc_reply_config;
typedef struct I3ipc_reply_tick I3ipc_reply_tick;
typedef struct I3ipc_reply_sync I3ipc_reply_sync;
typedef union I3ipc_event I3ipc_event;

/* *** Core API *** */

/* Run a command. Failure will cause an error. */
void i3ipc_run_command_simple(char const *command);

/* Run a command and return results.
 * You have to free() the result, unless staticalloc is set. */
I3ipc_reply_command *i3ipc_run_command(char const *commands);

/* Subscribe to events of that type.
 * See I3ipc_event_type for possible values for event_type.
 * If you are interested in multiple types, i3ipc_subscribe is the better
 * option. */
void i3ipc_subscribe_single(int event_type);

/* Subscribe events of the specified types.
 * See I3ipc_event_type for possible values for event_type. */
void i3ipc_subscribe(int *event_type, int event_type_size);

/* Wait for the next event, and return it.
 * If timeout_ms milliseconds elapse before an event arrives, return NULL.
 * Negative timeout_ms causes this to wait forever, zero has it return
 * immediately. You have to free() the result, unless staticalloc is set. */
I3ipc_event *i3ipc_event_next(int timeout_ms);

/* Query only major, minor and patch numbers.
 * out_major, out_minor, out_patch are output parameters, they may be NULL
 * You can query more detailed information using i3ipc_get_version . */
void i3ipc_get_version_simple(int *out_major, int *out_minor, int *out_patch);

/* Query the specified information.
 * You have to free() the result, unless staticalloc is set. */
I3ipc_reply_workspaces *i3ipc_get_workspaces(void);
I3ipc_reply_outputs *i3ipc_get_outputs(void);
I3ipc_reply_tree *i3ipc_get_tree(void);
I3ipc_reply_marks *i3ipc_get_marks(void);
I3ipc_reply_bar_config_ids *i3ipc_get_bar_config_ids(void);
I3ipc_reply_version *i3ipc_get_version(void);
I3ipc_reply_binding_modes *i3ipc_get_binding_modes(void);
I3ipc_reply_config *i3ipc_get_config(void);

/* Query the bar configuration of that name.
 * A list of names can be queried by i3ipc_get_bar_config_ids.
 * You have to free() the result, unless staticalloc is set. */
I3ipc_reply_bar_config *i3ipc_get_bar_config(char const *name);

/* Send a tick with the specified payload to subscribers of tick events. */
void i3ipc_send_tick(char const *payload);

/* Send a sync message. See the i3 documentation for details. */
void i3ipc_sync(int random_value, size_t window);

/* Return the file descriptor for the socket used for events.
 * You can use this if you want to wait on multiple sources, e.g. with poll().
 */
int i3ipc_event_fd(void);

/* Same as i3ipc_event_fd, but for messages.
 * This is not as useful. */
int i3ipc_message_fd(void);

/* Set the staticalloc flag, return the old value.
 * If this flag is set, you do not have to free results of the functions above,
 * but only the last one is valid. See the README for details.*/
bool i3ipc_set_staticalloc(bool value);

/* Set the loglevel, return the old value.
 * This controls how much information is printed to the terminal.
 * Values are -1 (silent), 0 (errors, default), 1 (debug messages) */
int i3ipc_set_loglevel(int value);

/* *** Data structures. ***
 * See the README for details.
 * Uninitialised members are NULL (for arrays, strings and pointers) or have a
 *  <name>_set member indicating whether they are set.
 * Arrays and strings have a <name>_size member indicating their length.
 * Strings which can take only a limited set of values have a <name>_enum
 * member, which you can check using integer comparison. The <type>_value enum
 * lists possible values. Invalid values are represented as -1. Names of members
 * correspond exactly to their JSON representation, except for 'class', which is
 * named 'window_class' to make C++ people happy. */

typedef struct I3ipc_rect {
  int x;
  int y;
  int width;
  int height;
} I3ipc_rect;

typedef struct I3ipc_string {
  char *str;
  int str_size;
} I3ipc_string;

enum I3ipc_node_values {
  I3IPC_NODE_TYPE_ROOT = 0,
  I3IPC_NODE_TYPE_OUTPUT,
  I3IPC_NODE_TYPE_CON,
  I3IPC_NODE_TYPE_FLOATING_CON,
  I3IPC_NODE_TYPE_WORKSPACE,
  I3IPC_NODE_TYPE_DOCKAREA,

  I3IPC_NODE_BORDER_NORMAL = 0,
  I3IPC_NODE_BORDER_NONE,
  I3IPC_NODE_BORDER_PIXEL,

  I3IPC_NODE_LAYOUT_SPLITH = 0,
  I3IPC_NODE_LAYOUT_SPLITV,
  I3IPC_NODE_LAYOUT_STACKED,
  I3IPC_NODE_LAYOUT_TABBED,
  I3IPC_NODE_LAYOUT_DOCKAREA,
  I3IPC_NODE_LAYOUT_OUTPUT,

  I3IPC_NODE_ORIENTATION_NONE = 0,
  I3IPC_NODE_ORIENTATION_HORIZONTAL,
  I3IPC_NODE_ORIENTATION_VERTICAL,

  I3IPC_NODE_WINDOW_TYPE_NULL = 0,
  I3IPC_NODE_WINDOW_TYPE_NORMAL,
  I3IPC_NODE_WINDOW_TYPE_DIALOG,
  I3IPC_NODE_WINDOW_TYPE_UTILITY,
  I3IPC_NODE_WINDOW_TYPE_TOOLBAR,
  I3IPC_NODE_WINDOW_TYPE_SPLASH,
  I3IPC_NODE_WINDOW_TYPE_MENU,
  I3IPC_NODE_WINDOW_TYPE_DROPDOWN_MENU,
  I3IPC_NODE_WINDOW_TYPE_POPUP_MENU,
  I3IPC_NODE_WINDOW_TYPE_TOOLTIP,
  I3IPC_NODE_WINDOW_TYPE_NOTIFICATION,
  I3IPC_NODE_WINDOW_TYPE_DOCK,
  I3IPC_NODE_WINDOW_TYPE_UNKNOWN
};

typedef struct I3ipc_node_window_properties {
  char *title;
  int title_size;
  char *instance;
  int instance_size;
  char *window_class;
  int window_class_size;
  char *window_role;
  int window_role_size;
  int transient_for;
  bool transient_for_set;
} I3ipc_node_window_properties;

typedef struct I3ipc_node I3ipc_node;
struct I3ipc_node {
  size_t id;
  char *name;
  int name_size;
  char *type;
  int type_size;
  int type_enum;
  char *border;
  int border_size;
  int border_enum;
  int current_border_width;
  char *layout;
  int layout_size;
  int layout_enum;
  char *orientation;
  int orientation_size;
  int orientation_enum;
  float percent;
  bool percent_set;
  I3ipc_rect rect;
  I3ipc_rect window_rect;
  I3ipc_rect deco_rect;
  I3ipc_rect geometry;
  int window;
  bool window_set;
  I3ipc_node_window_properties *window_properties;
  char *window_type;
  int window_type_size;
  int window_type_enum;
  bool urgent;
  I3ipc_string *marks;
  int marks_size;
  bool focused;
  size_t *focus;
  int focus_size;
  int fullscreen_mode;
  I3ipc_node *nodes;
  int nodes_size;
  I3ipc_node *floating_nodes;
  int floating_nodes_size;
  /* sway extensions (swayverview) */
  char *app_id;
  int app_id_size;
  int pid;
  bool pid_set;
  bool visible;
  bool visible_set;
  int num;
  bool num_set;
  char *foreign_toplevel_identifier;
  int foreign_toplevel_identifier_size;
};

enum I3ipc_reply_bar_config_values {
  I3IPC_BAR_CONFIG_MODE_DOCK = 0,
  I3IPC_BAR_CONFIG_MODE_HIDE,

  I3IPC_BAR_CONFIG_POSITION_BOTTOM = 0,
  I3IPC_BAR_CONFIG_POSITION_TOP
};

typedef struct I3ipc_bar_config_colors {
  char *background;
  int background_size;
  char *statusline;
  int statusline_size;
  char *separator;
  int separator_size;
  char *focused_background;
  int focused_background_size;
  char *focused_statusline;
  int focused_statusline_size;
  char *focused_separator;
  int focused_separator_size;
  char *focused_workspace_text;
  int focused_workspace_text_size;
  char *focused_workspace_bg;
  int focused_workspace_bg_size;
  char *focused_workspace_border;
  int focused_workspace_border_size;
  char *active_workspace_text;
  int active_workspace_text_size;
  char *active_workspace_bg;
  int active_workspace_bg_size;
  char *active_workspace_border;
  int active_workspace_border_size;
  char *inactive_workspace_text;
  int inactive_workspace_text_size;
  char *inactive_workspace_bg;
  int inactive_workspace_bg_size;
  char *inactive_workspace_border;
  int inactive_workspace_border_size;
  char *urgent_workspace_text;
  int urgent_workspace_text_size;
  char *urgent_workspace_bg;
  int urgent_workspace_bg_size;
  char *urgent_workspace_border;
  int urgent_workspace_border_size;
  char *binding_mode_text;
  int binding_mode_text_size;
  char *binding_mode_bg;
  int binding_mode_bg_size;
  char *binding_mode_border;
  int binding_mode_border_size;
} I3ipc_bar_config_colors;

typedef struct I3ipc_bar_config {
  char *id;
  int id_size;
  char *mode;
  int mode_size;
  int mode_enum;
  char *position;
  int position_size;
  int position_enum;
  char *status_command;
  int status_command_size;
  char *font;
  int font_size;
  bool workspace_buttons;
  bool binding_mode_indicator;
  bool verbose;
  I3ipc_bar_config_colors colors;
} I3ipc_bar_config;

/* see https://i3wm.org/docs/ipc.html#_command_reply */
typedef struct I3ipc_reply_command_el {
  bool success;
  char *error;
  int error_size;
} I3ipc_reply_command_el;

struct I3ipc_reply_command {
  I3ipc_reply_command_el *commands;
  int commands_size;
};

/* see https://i3wm.org/docs/ipc.html#_workspaces_reply */
typedef struct I3ipc_reply_workspaces_el {
  size_t id;
  int num;
  char *name;
  int name_size;
  bool visible;
  bool focused;
  bool urgent;
  I3ipc_rect rect;
  char *output;
  int output_size;
} I3ipc_reply_workspaces_el;

struct I3ipc_reply_workspaces {
  I3ipc_reply_workspaces_el *workspaces;
  int workspaces_size;
};

/* see https://i3wm.org/docs/ipc.html#_subscribe_reply */
struct I3ipc_reply_subscribe {
  bool success;
};

/* see https://i3wm.org/docs/ipc.html#_outputs_reply */
typedef struct I3ipc_reply_outputs_el {
  char *name;
  int name_size;
  bool active;
  bool primary;
  char *current_workspace;
  int current_workspace_size;
  I3ipc_rect rect;
} I3ipc_reply_outputs_el;

struct I3ipc_reply_outputs {
  I3ipc_reply_outputs_el *outputs;
  int outputs_size;
};

/* see https://i3wm.org/docs/ipc.html#_tree_reply */
struct I3ipc_reply_tree {
  I3ipc_node root;
};

/* see https://i3wm.org/docs/ipc.html#_marks_reply */
struct I3ipc_reply_marks {
  I3ipc_string *marks;
  int marks_size;
};

/* see https://i3wm.org/docs/ipc.html#_bar_config_reply */
struct I3ipc_reply_bar_config_ids {
  I3ipc_string *ids;
  int ids_size;
};

/* see https://i3wm.org/docs/ipc.html#_bar_config_reply */
struct I3ipc_reply_bar_config {
  I3ipc_bar_config cfg;
};

/* see https://i3wm.org/docs/ipc.html#_version_reply */
struct I3ipc_reply_version {
  int major;
  int minor;
  int patch;
  char *human_readable;
  int human_readable_size;
  char *loaded_config_file_name;
  int loaded_config_file_name_size;
};

/* see https://i3wm.org/docs/ipc.html#_binding_modes_reply */
struct I3ipc_reply_binding_modes {
  I3ipc_string *modes;
  int modes_size;
};

/* see https://i3wm.org/docs/ipc.html#_config_reply */
struct I3ipc_reply_config {
  char *config;
  int config_size;
};

/* see https://i3wm.org/docs/ipc.html#_tick_reply */
struct I3ipc_reply_tick {
  bool success;
};

/* see https://i3wm.org/docs/ipc.html#_sync_reply */
struct I3ipc_reply_sync {
  bool success;
};

enum I3ipc_event_workspace_values {
  I3IPC_WORKSPACE_CHANGE_FOCUS = 0,
  I3IPC_WORKSPACE_CHANGE_INIT,
  I3IPC_WORKSPACE_CHANGE_EMPTY,
  I3IPC_WORKSPACE_CHANGE_URGENT,
  I3IPC_WORKSPACE_CHANGE_RELOAD,
  I3IPC_WORKSPACE_CHANGE_RENAME,
  I3IPC_WORKSPACE_CHANGE_RESTORED,
  I3IPC_WORKSPACE_CHANGE_MOVE
};

/* see https://i3wm.org/docs/ipc.html#_workspace_event */
typedef struct I3ipc_event_workspace {
  int type; /* = I3IPC_EVENT_WORKSPACE */
  char *change;
  int change_size;
  int change_enum;
  I3ipc_node *current;
  I3ipc_node *old;
} I3ipc_event_workspace;

enum I3ipc_event_output_values { I3IPC_OUTPUT_CHANGE_UNSPECIFIED = 0 };

/* see https://i3wm.org/docs/ipc.html#_output_event */
typedef struct I3ipc_event_output {
  int type; /* = I3IPC_EVENT_OUTPUT */
  char *change;
  int change_size;
  int change_enum;
} I3ipc_event_output;

/* see https://i3wm.org/docs/ipc.html#_mode_event */
typedef struct I3ipc_event_mode {
  int type; /* = I3IPC_EVENT_MODE */
  char *change;
  int change_size;
  bool pango_markup;
} I3ipc_event_mode;

enum I3ipc_event_window_values {
  I3IPC_WINDOW_CHANGE_NEW = 0,
  I3IPC_WINDOW_CHANGE_CLOSE,
  I3IPC_WINDOW_CHANGE_FOCUS,
  I3IPC_WINDOW_CHANGE_TITLE,
  I3IPC_WINDOW_CHANGE_FULLSCREEN_MODE,
  I3IPC_WINDOW_CHANGE_MOVE,
  I3IPC_WINDOW_CHANGE_FLOATING,
  I3IPC_WINDOW_CHANGE_URGENT,
  I3IPC_WINDOW_CHANGE_MARK
};

/* see https://i3wm.org/docs/ipc.html#_window_event */
typedef struct I3ipc_event_window {
  int type; /* = I3IPC_EVENT_WINDOW */
  char *change;
  int change_size;
  int change_enum;
  I3ipc_node container;
} I3ipc_event_window;

/* see https://i3wm.org/docs/ipc.html#_barconfig_update_event */
typedef struct I3ipc_event_barconfig_update {
  int type; /* = I3IPC_EVENT_BARCONFIG_UPDATE */
  I3ipc_bar_config cfg;
} I3ipc_event_barconfig_update;

enum I3ipc_event_binding_values {
  I3IPC_BINDING_CHANGE_RUN = 0,

  I3IPC_BINDING_INPUT_TYPE_KEYBOARD = 0,
  I3IPC_BINDING_INPUT_TYPE_MOUSE
};

/* see https://i3wm.org/docs/ipc.html#_binding_event */
typedef struct I3ipc_event_binding_binding {
  char *command;
  int command_size;
  I3ipc_string *event_state_mask;
  int event_state_mask_size;
  int input_code;
  char *symbol;
  int symbol_size;
  char *input_type;
  int input_type_size;
  int input_type_enum;
} I3ipc_event_binding_binding;

typedef struct I3ipc_event_binding {
  int type; /* = I3IPC_EVENT_BINDING */
  char *change;
  int change_size;
  int change_enum;
  I3ipc_event_binding_binding binding;
} I3ipc_event_binding;

enum I3ipc_event_shutdown_values {
  I3IPC_SHUTDOWN_CHANGE_RESTART = 0,
  I3IPC_SHUTDOWN_CHANGE_EXIT
};

/* see https://i3wm.org/docs/ipc.html#_shutdown_event */
typedef struct I3ipc_event_shutdown {
  int type; /* = I3IPC_EVENT_SHUTDOWN */
  char *change;
  int change_size;
  int change_enum;
} I3ipc_event_shutdown;

/* see https://i3wm.org/docs/ipc.html#_tick_event */
typedef struct I3ipc_event_tick {
  int type; /* = I3IPC_EVENT_TICK */
  bool first;
  char *payload;
  int payload_size;
} I3ipc_event_tick;

union I3ipc_event { /*
^~~~~ This is a union! Use type to determine which member is valid. */

  int type;
  I3ipc_event_workspace workspace;
  I3ipc_event_output output;
  I3ipc_event_mode mode;
  I3ipc_event_window window;
  I3ipc_event_barconfig_update barconfig_update;
  I3ipc_event_binding binding;
  I3ipc_event_shutdown shutdown;
  I3ipc_event_tick tick;
};

/* *** Error handling *** */

/* Set the nopanic flag, return the old value.
 * If this is set, errors will not cause the program to abort. See "Error
 * Handling" in the README. */
bool i3ipc_set_nopanic(bool value);

enum I3ipc_error_codes {
  I3IPC_ERROR_CLOSED = 256, /* Connection with i3 closed */
  I3IPC_ERROR_MALFORMED,    /* i3 sent invalid data */
  I3IPC_ERROR_IO,           /* General IO failure */
  I3IPC_ERROR_FAILED,       /* Operation failed */
  I3IPC_ERROR_BADSTATE =
      -1 /* Library in error state, operation not attempted */
};

/* Return the current error state.
 * Zero indicates no error, else the value is positive and listed in
 * I3ipc_error_codes. Note that I3IPC_ERROR_BADSTATE is never returned here. */
int i3ipc_error_code(void);

/* Print an informative message describing the current error state onto stderr.
 * Each outputted line is prefixed with prefix followed by ": ".
 * prefix may be NULL, which is equivalent to "Error".
 * If there is no error, this may panic. */
void i3ipc_error_print(char const *prefix);

/* Reset the error state and revert the library to an un-initialized state, if
 * necessary. The latter part is skipped if the error state is
 * I3IPC_ERROR_FAILED. You can override this behaviour by setting force_reinit.
 * If there is no error, this may panic. */
void i3ipc_error_reinitialize(bool force_reinit);

/* *** Low-level API ***
 * Functions ending with _try return 0 on success and a nonzero error code on
 * failure. Error codes are defined in I3ipc_error_codes. The code of the first
 * failure matches subsequent calls to i3ipc_error_code. While the error state
 * is set, *_try functions return I3IPC_ERROR_BADSTATE instead of performing any
 * work.  */

typedef struct __attribute__((__packed__)) I3ipc_message {
  char magic[6];
  int32_t message_length; /* number of payload bytes after this struct */
  int32_t message_type;   /* see I3ipc_message_type, I3ipc_reply_type and
                             I3ipc_event_type */
                          /* followed by message_length bytes of payload */
} I3ipc_message;

/* Initialise the connection to i3.
 * The connection is initialised automatically, you generally do not need to
 * call this. socketpath is the path to the i3 socket, it may be NULL. If
 * socketpath is NULL, a path is determined by calling 'i3 --get-socketpath'. */
int i3ipc_init_try(char *socketpath);

/* Send a message, receive an answer, parse the answer.
 * Same as calling i3ipc_message_try and i3ipc_parse_try in sequence. */
int i3ipc_message_and_parse_try(int message, int type, char const *payload,
                                int payload_size, char **out_data);

/* Send a message and receive an answer.
 * Same as calling i3ipc_message_send_try and i3ipc_message_receive try in
 * sequence. */
int i3ipc_message_try(int message_type, char const *payload, int payload_size,
                      I3ipc_message **out_reply);

/* Send a message to i3.
 * Events and SUBSCRIBE messages use a second socket. */
int i3ipc_message_send_try(int message_type, char const *payload,
                           int payload_size);

/* Receive the next message, its type must match message_type.
 * Events and SUBSCRIBE messages use a second socket.
 * out_reply is an output parameter, it may be NULL.
 * message_type may be I3IPC_EVENT_ANY, in which case any event or SUBSCRIBE
 * message matches. */
int i3ipc_message_receive_try(int message_type, I3ipc_message **out_reply);

/* Receive messages until one with type message_type arrives, then return that.
 * The other messages are placed into a queue and considered for subsequent
 * calls. If the first message does not match, this allocates memory. out_reply
 * is an output parameter, it may be NULL. message_type may be I3IPC_EVENT_ANY,
 * in which case any event or SUBSCRIBE message matches. */
int i3ipc_message_receive_reorder_try(int message_type,
                                      I3ipc_message **out_reply);

/* Parse the json payload of a message.
 * message_type is the expected type of the message, or -1.
 * type_id is the id of the type of the data, see I3ipc_type_values.
 * out_data is an output parameter, it may be NULL. */
int i3ipc_parse_try(I3ipc_message *msg, int message_type, int type_id,
                    char **out_data);

/* Print a json representation of a type to stream f.
 * type_id is the id of the type of the data, see I3ipc_type_values.
 * f may be NULL, in which case stdout will be used. */
void i3ipc_printjson(int type_id, void *obj, FILE *f);

enum I3ipc_message_type {
  I3IPC_RUN_COMMAND = 0,
  I3IPC_GET_WORKSPACES = 1,
  I3IPC_SUBSCRIBE = 2,
  I3IPC_GET_OUTPUTS = 3,
  I3IPC_GET_TREE = 4,
  I3IPC_GET_MARKS = 5,
  I3IPC_GET_BAR_CONFIG = 6,
  I3IPC_GET_VERSION = 7,
  I3IPC_GET_BINDING_MODES = 8,
  I3IPC_GET_CONFIG = 9,
  I3IPC_SEND_TICK = 10,
  I3IPC_SYNC = 11,
  I3IPC_MESSAGE_TYPE_COUNT
};

enum I3ipc_reply_type {
  I3IPC_REPLY_COMMAND = 0,
  I3IPC_REPLY_WORKSPACES = 1,
  I3IPC_REPLY_SUBSCRIBE = 2,
  I3IPC_REPLY_OUTPUTS = 3,
  I3IPC_REPLY_TREE = 4,
  I3IPC_REPLY_MARKS = 5,
  I3IPC_REPLY_BAR_CONFIG = 6,
  I3IPC_REPLY_VERSION = 7,
  I3IPC_REPLY_BINDING_MODES = 8,
  I3IPC_REPLY_CONFIG = 9,
  I3IPC_REPLY_TICK = 10,
  I3IPC_REPLY_SYNC = 11,
  I3IPC_REPLY_TYPE_COUNT
};

enum I3ipc_event_type {
  I3IPC_EVENT_WORKSPACE = -2147483648, /* 0 ^ 1<<31 */
  I3IPC_EVENT_OUTPUT,                  /* 1 ^ 1<<31 */
  I3IPC_EVENT_MODE,                    /* 2 ^ 1<<31 */
  I3IPC_EVENT_WINDOW,                  /* 3 ^ 1<<31 */
  I3IPC_EVENT_BARCONFIG_UPDATE,        /* 4 ^ 1<<31 */
  I3IPC_EVENT_BINDING,                 /* 5 ^ 1<<31 */
  I3IPC_EVENT_SHUTDOWN,                /* 6 ^ 1<<31 */
  I3IPC_EVENT_TICK,                    /* 7 ^ 1<<31 */
  I3IPC_EVENT_TYPE_END,
  I3IPC_EVENT_TYPE_BEGIN = I3IPC_EVENT_WORKSPACE,
  I3IPC_EVENT_ANY = -2 /* matches any event or SUBSCRIBE messages */
};

enum I3ipc_type_values {
  /* Primitive types */
  I3IPC_TYPE_BOOL,  /* bool */
  I3IPC_TYPE_CHAR,  /* char, only used for strings, as array */
  I3IPC_TYPE_INT,   /* int */
  I3IPC_TYPE_FLOAT, /* float */
  I3IPC_TYPE_SIZET, /* size_t */

  /* Basic types. These are used in multiple places. */
  I3IPC_TYPE_RECT,       /* I3ipc_rect */
  I3IPC_TYPE_STRING,     /* I3ipc_string */
  I3IPC_TYPE_NODE,       /* I3ipc_rect */
  I3IPC_TYPE_BAR_CONFIG, /* I3ipc_bar_config */

  /* Inner types. All types are named, and these are only used once and declared
     inline. */
  I3IPC_TYPE_NODE_WINDOW_PROPERTIES, /* struct I3ipc_node_window_properties */
  I3IPC_TYPE_BAR_CONFIG_COLORS,      /* struct I3ipc_bar_config_colors  */
  I3IPC_TYPE_REPLY_COMMAND_EL,       /* struct I3ipc_reply_command_el  */
  I3IPC_TYPE_REPLY_OUTPUTS_EL,       /* struct I3ipc_reply_outputs_el  */
  I3IPC_TYPE_REPLY_WORKSPACES_EL,    /* struct I3ipc_reply_workspaces_el */
  I3IPC_TYPE_EVENT_BINDING_BINDING,  /* struct I3ipc_event_binding_binding */

  /* Message reply types */
  I3IPC_TYPE_REPLY_COMMAND,        /* I3ipc_reply_command */
  I3IPC_TYPE_REPLY_WORKSPACES,     /* I3ipc_reply_workspaces */
  I3IPC_TYPE_REPLY_SUBSCRIBE,      /* I3ipc_reply_subscribe */
  I3IPC_TYPE_REPLY_OUTPUTS,        /* I3ipc_reply_outputs */
  I3IPC_TYPE_REPLY_TREE,           /* I3ipc_reply_tree */
  I3IPC_TYPE_REPLY_MARKS,          /* I3ipc_reply_marks */
  I3IPC_TYPE_REPLY_BAR_CONFIG_IDS, /* I3ipc_reply_bar_config_ids */
  I3IPC_TYPE_REPLY_BAR_CONFIG,     /* I3ipc_reply_bar_config */
  I3IPC_TYPE_REPLY_VERSION,        /* I3ipc_reply_version */
  I3IPC_TYPE_REPLY_BINDING_MODES,  /* I3ipc_reply_binding_modes */
  I3IPC_TYPE_REPLY_CONFIG,         /* I3ipc_reply_config */
  I3IPC_TYPE_REPLY_TICK,           /* I3ipc_reply_tick */
  I3IPC_TYPE_REPLY_SYNC,           /* I3ipc_reply_sync */

  /* Event types */
  I3IPC_TYPE_EVENT,                  /* I3ipc_event */
  I3IPC_TYPE_EVENT_WORKSPACE,        /* I3ipc_event_workspace */
  I3IPC_TYPE_EVENT_OUTPUT,           /* I3ipc_event_output */
  I3IPC_TYPE_EVENT_MODE,             /* I3ipc_event_mode */
  I3IPC_TYPE_EVENT_WINDOW,           /* I3ipc_event_window */
  I3IPC_TYPE_EVENT_BARCONFIG_UPDATE, /* I3ipc_event_barconfig_update */
  I3IPC_TYPE_EVENT_BINDING,          /* I3ipc_event_binding */
  I3IPC_TYPE_EVENT_SHUTDOWN,         /* I3ipc_event_shutdown */
  I3IPC_TYPE_EVENT_TICK,             /* I3ipc_event_tick */

  I3IPC_TYPE_COUNT,
  I3IPC_TYPE_PRIMITIVE_COUNT = I3IPC_TYPE_RECT
};

#endif /* I3IPC_INCLUDE_I3IPC_H */
