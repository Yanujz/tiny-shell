#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * ===========================
 * Configuration (override in your build)
 * ===========================
 */

/* Max length of one input line */
#ifndef SHELL_LINEBUF_SIZE
#define SHELL_LINEBUF_SIZE      128
#endif

/* Max argv entries */
#ifndef SHELL_MAX_ARGS
#define SHELL_MAX_ARGS          8
#endif

/* ART node pool size (increase if shell_load_table() reports overflow) */
#ifndef SHELL_ART_MAX_NODES
#define SHELL_ART_MAX_NODES     128
#endif

/* Fixed children per ART node */
#ifndef SHELL_ART_MAX_CHILDREN
#define SHELL_ART_MAX_CHILDREN  16
#endif

/* Input queue for ISR → shell. Must be power of two for fastest wrap. */
#ifndef SHELL_INPUT_QUEUE_SIZE
#define SHELL_INPUT_QUEUE_SIZE  64
#endif

/* History buffer size (number of commands) */
#ifndef SHELL_HISTORY_SIZE
#define SHELL_HISTORY_SIZE      8
#endif

/* Max custom key bindings */
#ifndef SHELL_MAX_KEYBINDS
#define SHELL_MAX_KEYBINDS      16
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ===========================
 * Public types
 * ===========================
 */

/* Key event types for shortcuts */
typedef enum {
    SHELL_KEY_NONE = 0,
    SHELL_KEY_CTRL_A,       /* Home (beginning of line) */
    SHELL_KEY_CTRL_B,       /* Left */
    SHELL_KEY_CTRL_C,       /* Cancel/Interrupt */
    SHELL_KEY_CTRL_D,       /* EOF/Delete char */
    SHELL_KEY_CTRL_E,       /* End (end of line) */
    SHELL_KEY_CTRL_F,       /* Right */
    SHELL_KEY_CTRL_K,       /* Kill line (from cursor to end) */
    SHELL_KEY_CTRL_L,       /* Clear screen */
    SHELL_KEY_CTRL_N,       /* Next history */
    SHELL_KEY_CTRL_P,       /* Previous history */
    SHELL_KEY_CTRL_U,       /* Kill line (from beginning to cursor) */
    SHELL_KEY_CTRL_W,       /* Kill word backwards */
    SHELL_KEY_CTRL_R,       /* Reverse search (reserved) */
    SHELL_KEY_CTRL_T,       /* Transpose chars */
    SHELL_KEY_TAB,          /* Tab completion */
    SHELL_KEY_UP,
    SHELL_KEY_DOWN,
    SHELL_KEY_RIGHT,
    SHELL_KEY_LEFT,
    SHELL_KEY_HOME,
    SHELL_KEY_END,
    SHELL_KEY_DEL,
    SHELL_KEY_INS,
    SHELL_KEY_PGUP,
    SHELL_KEY_PGDN,
    SHELL_KEY_F1,
    SHELL_KEY_F2,
    SHELL_KEY_F3,
    SHELL_KEY_F4,
    SHELL_KEY_F5,
    SHELL_KEY_F6,
    SHELL_KEY_F7,
    SHELL_KEY_F8,
    SHELL_KEY_F9,
    SHELL_KEY_F10,
    SHELL_KEY_F11,
    SHELL_KEY_F12,
    SHELL_KEY_BACKSPACE,
    SHELL_KEY_ENTER,
} shell_key_t;

/* Forward declaration */
struct shell;

/* Command function signature */
typedef void (*shell_cmd_fn)(int argc, char **argv, void *user_data);

/* Key event handler signature */
typedef bool (*shell_key_handler)(struct shell *sh, shell_key_t key, void *user_data);

/* Command descriptor */
typedef struct {
    const char   *name;
    const char   *desc;
    shell_cmd_fn  fn;
    void         *user_data;
} shell_ext_cmd_t;

/* Key binding descriptor */
typedef struct {
    shell_key_t        key;
    shell_key_handler  handler;
    void              *user_data;
} shell_keybind_t;

/* I/O callbacks */
typedef int  (*shell_putchar_func)(int ch);
typedef int  (*shell_getchar_func)(void); /* only used in "poll" mode, you can pass NULL */

/* Login callback */
typedef bool (*shell_login_cb)(const char *user, const char *pass);

/* Custom tab completion callback (overrides default behavior) */
typedef int (*shell_complete_cb)(struct shell *sh, const char *partial, 
                                 char **matches, int max_matches);

/* Return codes */
typedef enum {
    SHELL_OK = 0,
    SHELL_ERR_ARG,
    SHELL_ERR_NO_SPACE,
    SHELL_ERR_ART_OVERFLOW,
} shell_status_t;

/* Stats you can query at runtime */
typedef struct {
    uint16_t max_nodes_used;
    bool     art_overflow;
    uint16_t history_count;
    uint16_t cmd_count;
    uint8_t  keybind_count;
} shell_stats_t;

/* Escape state (internal) */
typedef struct {
    uint8_t  state;
    uint8_t  num_params;
    uint16_t params[4];
} shell_esc_t;

/* ART node (internal) */
typedef struct shell_art_node {
    uint8_t  n_children;
    uint8_t  key;
    int16_t  cmd_idx;
    int16_t  child_idx[SHELL_ART_MAX_CHILDREN];
    uint8_t  child_key[SHELL_ART_MAX_CHILDREN];
} shell_art_node_t;

/* History entry */
typedef struct {
    char line[SHELL_LINEBUF_SIZE];
} shell_history_entry_t;

/* Main shell struct – you allocate this (on stack/BSS) */
typedef struct shell {
    /* I/O */
    shell_putchar_func putc_f;
    shell_getchar_func getc_f;

    /* Login */
    shell_login_cb login_cb;
    char           login_trigger;
    bool           logged_in;
    uint8_t        login_state;
    char           login_user[SHELL_LINEBUF_SIZE];
    char           login_pass[SHELL_LINEBUF_SIZE];
    uint16_t       login_idx;

    /* Line editing */
    char           linebuf[SHELL_LINEBUF_SIZE];
    uint16_t       line_len;
    uint16_t       cursor_pos;
    char           killed_text[SHELL_LINEBUF_SIZE];  /* For yank/kill operations */
    uint8_t        prompt_len;

    /* External command table */
    const shell_ext_cmd_t *cmd_table;
    uint16_t               cmd_count;

    /* ART/trie */
    shell_art_node_t art_nodes[SHELL_ART_MAX_NODES];
    int16_t          art_root;
    uint16_t         art_free;

    /* Stats */
    uint16_t         art_max_used;
    bool             art_overflow;

    /* Escape parsing */
    shell_esc_t      esc;

    /* History */
    shell_history_entry_t history[SHELL_HISTORY_SIZE];
    uint8_t               history_head;    /* Next slot to write */
    uint8_t               history_count;   /* Total entries (max SHELL_HISTORY_SIZE) */
    int8_t                history_pos;     /* Current position in history (-1 = not browsing) */
    char                  history_saved[SHELL_LINEBUF_SIZE]; /* Saved current line when browsing */

    /* Key bindings */
    shell_keybind_t  keybinds[SHELL_MAX_KEYBINDS];
    uint8_t          keybind_count;

    /* Tab completion */
    shell_complete_cb complete_cb;

    /* Single-producer/single-consumer input queue */
    uint8_t          in_q[SHELL_INPUT_QUEUE_SIZE];
    volatile uint16_t in_head;   /* producer (ISR) writes head */
    volatile uint16_t in_tail;   /* consumer (main) writes tail */

    /* Flags */
    bool             echo_enabled;
    bool             initial_prompt_shown;

} shell_t;

/*
 * ===========================
 * API
 * ===========================
 */

/**
 * Initialize shell into user-provided memory.
 * - You must provide putchar
 * - You may provide getchar (can be NULL if using shell_feed_char)
 */
shell_status_t shell_init(shell_t              *sh,
                          shell_putchar_func     putc_f,
                          shell_getchar_func     getc_f);

/**
 * Load an external, static command table and build the trie.
 * Returns:
 * - SHELL_OK on success
 * - SHELL_ERR_ART_OVERFLOW if there weren't enough nodes
 */
shell_status_t shell_load_table(shell_t *sh,
                                const shell_ext_cmd_t *table,
                                uint16_t count);

/** Enable login; user must type the trigger char first, e.g. '#' */
void shell_set_login(shell_t *sh,
                     shell_login_cb cb,
                     char trigger_char);

/** Force logout; next input will require login again */
void shell_logout(shell_t *sh);

/**
 * Feed one character into the shell (ISR-safe single producer).
 * Returns true if enqueued, false if queue full.
 */
bool shell_feed_char(shell_t *sh, uint8_t ch);

/**
 * Process pending characters and run commands.
 * Call this often from your main loop / task.
 */
void shell_run(shell_t *sh);

/** Get runtime stats (ART usage, overflow, history) */
void shell_get_stats(shell_t *sh, shell_stats_t *out);

/**
 * Register a custom key binding.
 * Handler returns true if key was handled (prevents default behavior).
 */
bool shell_bind_key(shell_t *sh, shell_key_t key, 
                    shell_key_handler handler, void *user_data);

/**
 * Unregister a key binding.
 */
void shell_unbind_key(shell_t *sh, shell_key_t key);

/**
 * Set custom tab completion callback.
 * This overrides the default command completion behavior.
 */
void shell_set_complete(shell_t *sh, shell_complete_cb cb);

/**
 * Enable/disable echo (useful for password entry or special modes).
 */
void shell_set_echo(shell_t *sh, bool enabled);

/**
 * Get echo enabled status
 */
bool shell_get_echo(shell_t *sh);

/**
 * Manually insert text into the current line (useful for completion).
 */
void shell_insert_text(shell_t *sh, const char *text);

/**
 * Redraw the current line (useful for custom key handlers)
 */
void shell_redraw_line(shell_t *sh);

/**
 * Get current line buffer (useful for completion handlers).
 */
const char *shell_get_line(shell_t *sh);

/**
 * Clear the screen and redraw prompt.
 */
void shell_clear_screen(shell_t *sh);

/**
 * Add command to history manually (useful for remote commands).
 */
void shell_add_history(shell_t *sh, const char *line);

/**
 * Get a specific history entry.
 * @param sh Shell instance
 * @param index 0 is the oldest entry, (N-1) is the most recent.
 * @return Read-only pointer to history line, or NULL if index is out of bounds.
 */
const char *shell_get_history_entry(shell_t *sh, uint16_t index);


#ifdef __cplusplus
}
#endif