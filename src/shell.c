#include "shell.h"
#include <string.h>
#include <ctype.h>

/* ANSI Escape Codes */
#define ANSI_CLEAR_LINE_FROM_CURSOR "\033[K"
#define ANSI_MOVE_CURSOR_HOME       "\033[H"
#define ANSI_CLEAR_SCREEN           "\033[2J"
#define ANSI_MOVE_CURSOR_RIGHT(n)   "\033[" #n "C"
#define ANSI_MOVE_CURSOR_COL(n)     "\033[" #n "G"

/* ===========================
 * Internal key codes
 * =========================== */
typedef enum {
    SH_PARSE_NONE = 0,
    SH_PARSE_CONTINUE,
    SH_PARSE_COMPLETE,
} sh_parse_result_t;

/* ===========================
 * Small I/O helpers
 * =========================== */
static void sh_putc(shell_t *sh, char c) {
    sh->putc_f((unsigned char)c);
}

static void sh_puts(shell_t *sh, const char *s) {
    while (*s) sh_putc(sh, *s++);
}

/* Helper to print an unsigned integer without stdio */
static void sh_puts_uint(shell_t *sh, unsigned int n) {
    char buf[8]; // max 65535
    char *p = &buf[7];
    *p = '\0';
    if (n == 0) {
        *--p = '0';
    } else {
        while (n > 0) {
            *--p = (char)('0' + (n % 10));
            n /= 10;
        }
    }
    sh_puts(sh, p);
}

static void sh_prompt(shell_t *sh) {
    sh->prompt_len = 2; /* "> " */
    sh_puts(sh, "> ");
}

/* ===========================
 * Input queue (SPSC)
 * =========================== */
bool shell_feed_char(shell_t *sh, uint8_t ch)
{
    uint16_t head = sh->in_head;
    uint16_t next = (uint16_t)((head + 1) & (SHELL_INPUT_QUEUE_SIZE - 1));

    if (next == sh->in_tail) {
        return false;
    }
    sh->in_q[head] = ch;
    sh->in_head = next;
    return true;
}

static int shell_dequeue_char(shell_t *sh)
{
    if (sh->in_tail == sh->in_head) {
        return -1;
    }
    uint8_t ch = sh->in_q[sh->in_tail];
    sh->in_tail = (uint16_t)((sh->in_tail + 1) & (SHELL_INPUT_QUEUE_SIZE - 1));
    return ch;
}

/* ===========================
 * ART helpers
 * =========================== */
static int16_t art_new_node(shell_t *sh, uint8_t key)
{
    if (sh->art_free >= SHELL_ART_MAX_NODES) {
        sh->art_overflow = true;
        return -1;
    }
    int16_t idx = (int16_t)sh->art_free++;
    if (sh->art_free > sh->art_max_used)
        sh->art_max_used = sh->art_free;

    shell_art_node_t *n = &sh->art_nodes[idx];
    n->n_children = 0;
    n->key        = key;
    n->cmd_idx    = -1;
    for (int i = 0; i < SHELL_ART_MAX_CHILDREN; i++) {
        n->child_idx[i] = -1;
        n->child_key[i] = 0;
    }
    return idx;
}

static int16_t art_find_child(shell_t *sh, int16_t node_idx, uint8_t c)
{
    shell_art_node_t *n = &sh->art_nodes[node_idx];
    for (uint8_t i = 0; i < n->n_children; i++) {
        if (n->child_key[i] == c)
            return n->child_idx[i];
    }
    return -1;
}

static int16_t art_add_child(shell_t *sh, int16_t node_idx, uint8_t c)
{
    shell_art_node_t *n = &sh->art_nodes[node_idx];
    if (n->n_children >= SHELL_ART_MAX_CHILDREN)
        return -1;
    int16_t new_idx = art_new_node(sh, c);
    if (new_idx < 0)
        return -1;
    n->child_key[n->n_children] = c;
    n->child_idx[n->n_children] = new_idx;
    n->n_children++;
    return new_idx;
}

static bool art_insert(shell_t *sh, const char *name, int16_t cmd_idx)
{
    int16_t cur = sh->art_root;
    const unsigned char *p = (const unsigned char*)name;

    while (*p) {
        int16_t child = art_find_child(sh, cur, *p);
        if (child < 0) {
            child = art_add_child(sh, cur, *p);
            if (child < 0) return false;
        }
        cur = child;
        p++;
    }

    sh->art_nodes[cur].cmd_idx = cmd_idx;
    return true;
}

static const shell_ext_cmd_t *art_lookup(shell_t *sh, const char *name)
{
    if (!sh->cmd_table) return NULL;
    int16_t cur = sh->art_root;
    const unsigned char *p = (const unsigned char*)name;

    while (*p) {
        int16_t child = art_find_child(sh, cur, *p);
        if (child < 0)
            return NULL;
        cur = child;
        p++;
    }

    int16_t ci = sh->art_nodes[cur].cmd_idx;
    if (ci >= 0 && ci < sh->cmd_count)
        return &sh->cmd_table[ci];
    return NULL;
}

/* ===========================
 * Arg parsing (with quote support)
 * =========================== */
static int build_argv(char *line, char **argv, int max_args)
{
    int argc = 0;
    char *p = line;
    enum { STATE_WHITESPACE, STATE_TOKEN, STATE_QUOTE } state = STATE_WHITESPACE;

    while (*p && argc < max_args) {
        switch (state) {
        case STATE_WHITESPACE:
            while (*p && isspace((unsigned char)*p)) {
                p++;
            }
            if (!*p) {
                break; // End of line
            }
            if (*p == '"') {
                argv[argc++] = p + 1; // Start token *after* quote
                state = STATE_QUOTE;
                p++;
            } else {
                argv[argc++] = p;
                state = STATE_TOKEN;
            }
            break;

        case STATE_TOKEN:
            if (isspace((unsigned char)*p)) {
                *p = '\0';
                state = STATE_WHITESPACE;
            }
            p++;
            break;

        case STATE_QUOTE:
            if (*p == '"') {
                *p = '\0';
                state = STATE_WHITESPACE;
            }
            p++;
            break;
        }
    }

    argv[argc] = NULL;
    return argc;
}

/* ===========================
 * Escape parsing
 * =========================== */
static void esc_reset(shell_esc_t *e)
{
    e->state = 0;
    e->num_params = 0;
    for (int i = 0; i < 4; i++) e->params[i] = 0;
}

static shell_key_t esc_finish_csi(shell_esc_t *e, char final)
{
    switch (final) {
    case 'A': return SHELL_KEY_UP;
    case 'B': return SHELL_KEY_DOWN;
    case 'C': return SHELL_KEY_RIGHT;
    case 'D': return SHELL_KEY_LEFT;
    case 'H': return SHELL_KEY_HOME;
    case 'F': return SHELL_KEY_END;
    case 'Z': return SHELL_KEY_TAB; /* Shift+Tab, treat as Tab */
    case '~':
        if (e->num_params == 0) return SHELL_KEY_NONE;
        switch (e->params[0]) {
        case 1:  return SHELL_KEY_HOME;
        case 2:  return SHELL_KEY_INS;
        case 3:  return SHELL_KEY_DEL;
        case 4:  return SHELL_KEY_END;
        case 5:  return SHELL_KEY_PGUP;
        case 6:  return SHELL_KEY_PGDN;
        case 15: return SHELL_KEY_F5;
        case 17: return SHELL_KEY_F6;
        case 18: return SHELL_KEY_F7;
        case 19: return SHELL_KEY_F8;
        case 20: return SHELL_KEY_F9;
        case 21: return SHELL_KEY_F10;
        case 23: return SHELL_KEY_F11;
        case 24: return SHELL_KEY_F12;
        default: return SHELL_KEY_NONE;
        }
    default:
        return SHELL_KEY_NONE;
    }
}

static sh_parse_result_t esc_parse(shell_t *sh, int ch, shell_key_t *out_key)
{
    shell_esc_t *e = &sh->esc;
    *out_key = SHELL_KEY_NONE;

    if (e->state == 0) {
        if (ch == 0x1B) {
            e->state = 1;
            return SH_PARSE_CONTINUE;
        }
        return SH_PARSE_NONE;
    }

    if (e->state == 1) {
        if (ch == '[') {
            e->state = 2;
            e->num_params = 0;
            return SH_PARSE_CONTINUE;
        } else if (ch == 'O') {
            e->state = 3;
            return SH_PARSE_CONTINUE;
        } else {
            esc_reset(e);
            return SH_PARSE_NONE;
        }
    }

    if (e->state == 2) {
        if (ch >= '0' && ch <= '9') {
            if (e->num_params == 0) {
                e->num_params = 1;
                e->params[0] = 0;
            }
            e->params[e->num_params - 1] =
                (uint16_t)(e->params[e->num_params - 1] * 10 + (ch - '0'));
            return SH_PARSE_CONTINUE;
        } else if (ch == ';') {
            if (e->num_params < 4) {
                e->params[e->num_params++] = 0;
            }
            return SH_PARSE_CONTINUE;
        } else {
            *out_key = esc_finish_csi(e, (char)ch);
            esc_reset(e);
            return SH_PARSE_COMPLETE;
        }
    }

    if (e->state == 3) {
        switch (ch) {
        case 'P': *out_key = SHELL_KEY_F1; break;
        case 'Q': *out_key = SHELL_KEY_F2; break;
        case 'R': *out_key = SHELL_KEY_F3; break;
        case 'S': *out_key = SHELL_KEY_F4; break;
        case 'H': *out_key = SHELL_KEY_HOME; break;
        case 'F': *out_key = SHELL_KEY_END; break;
        default:  *out_key = SHELL_KEY_NONE; break;
        }
        esc_reset(e);
        return SH_PARSE_COMPLETE;
    }

    esc_reset(e);
    return SH_PARSE_NONE;
}

/* ===========================
 * History management
 * =========================== */
void shell_add_history(shell_t *sh, const char *line)
{
    if (!line || !*line) return;

    /* Don't add duplicate of last command */
    if (sh->history_count > 0) {
        uint8_t last = (sh->history_head + SHELL_HISTORY_SIZE - 1) % SHELL_HISTORY_SIZE;
        if (strcmp(sh->history[last].line, line) == 0)
            return;
    }

    strncpy(sh->history[sh->history_head].line, line, SHELL_LINEBUF_SIZE - 1);
    sh->history[sh->history_head].line[SHELL_LINEBUF_SIZE - 1] = '\0';

    sh->history_head = (sh->history_head + 1) % SHELL_HISTORY_SIZE;
    if (sh->history_count < SHELL_HISTORY_SIZE)
        sh->history_count++;
}

static void history_prev(shell_t *sh)
{
    if (sh->history_count == 0) return;

    if (sh->history_pos == -1) {
        /* Save current line before browsing */
        strncpy(sh->history_saved, sh->linebuf, SHELL_LINEBUF_SIZE);
        sh->history_pos = (sh->history_head + SHELL_HISTORY_SIZE - 1) % SHELL_HISTORY_SIZE;
    } else {
        int next_pos = (sh->history_pos + SHELL_HISTORY_SIZE - 1) % SHELL_HISTORY_SIZE;
        int oldest = (sh->history_head + SHELL_HISTORY_SIZE - sh->history_count) % SHELL_HISTORY_SIZE;
        
        if (sh->history_count == SHELL_HISTORY_SIZE) {
            if (next_pos == oldest) return;
        } else {
            if (next_pos < 0 || next_pos < oldest) return;
        }
        sh->history_pos = next_pos;
    }

    strncpy(sh->linebuf, sh->history[sh->history_pos].line, SHELL_LINEBUF_SIZE);
    sh->line_len = (uint16_t)strlen(sh->linebuf);
    sh->cursor_pos = sh->line_len;
    shell_redraw_line(sh);
}

static void history_next(shell_t *sh)
{
    if (sh->history_pos == -1) return;

    int next_pos = (sh->history_pos + 1) % SHELL_HISTORY_SIZE;
    
    if (next_pos == sh->history_head) {
        /* Restore saved line */
        sh->history_pos = -1;
        strncpy(sh->linebuf, sh->history_saved, SHELL_LINEBUF_SIZE);
    } else {
        sh->history_pos = next_pos;
        strncpy(sh->linebuf, sh->history[sh->history_pos].line, SHELL_LINEBUF_SIZE);
    }

    sh->line_len = (uint16_t)strlen(sh->linebuf);
    sh->cursor_pos = sh->line_len;
    shell_redraw_line(sh);
}

/* ===========================
 * Login
 * =========================== */
static void login_reset(shell_t *sh)
{
    sh->login_state = 0;
    sh->login_idx   = 0;
    memset(sh->login_user, 0, sizeof sh->login_user);
    memset(sh->login_pass, 0, sizeof sh->login_pass);
}

static void login_prompt_user(shell_t *sh)
{
    sh_puts(sh, "login: ");
    sh->login_idx = 0;
}

static void login_prompt_pass(shell_t *sh)
{
    sh_puts(sh, "password: ");
    sh->login_idx = 0;
}

static void handle_login(shell_t *sh, int ch)
{
    switch (sh->login_state) {
    case 0:
        if (ch == (unsigned char)sh->login_trigger) {
            sh->login_state = 1;
            login_prompt_user(sh);
        }
        break;
    case 1: /* username */
        if (ch == '\r' || ch == '\n') {
            sh_putc(sh, '\r'); sh_putc(sh, '\n');
            sh->login_user[sh->login_idx] = 0;
            sh->login_state = 2;
            login_prompt_pass(sh);
        } else if (ch == 0x7F || ch == '\b') {
            if (sh->login_idx) {
                sh->login_idx--;
                sh_putc(sh, '\b'); sh_putc(sh, ' '); sh_putc(sh, '\b');
            }
        } else if (sh->login_idx < sizeof sh->login_user - 1) {
            sh->login_user[sh->login_idx++] = (char)ch;
            sh_putc(sh, (char)ch);
        }
        break;
    case 2: /* password */
        if (ch == '\r' || ch == '\n') {
            sh_putc(sh, '\r'); sh_putc(sh, '\n');
            sh->login_pass[sh->login_idx] = 0;
            bool ok = false;
            if (sh->login_cb)
                ok = sh->login_cb(sh->login_user, sh->login_pass);
            if (ok) {
                sh->logged_in = true;
                login_reset(sh);
                sh_prompt(sh);
            } else {
                sh_puts(sh, "Login failed\r\n");
                login_reset(sh);
            }
        } else if (ch == 0x7F || ch == '\b') {
            if (sh->login_idx) {
                sh->login_idx--;
            }
        } else if (sh->login_idx < sizeof sh->login_pass - 1) {
            sh->login_pass[sh->login_idx++] = (char)ch;
        }
        break;
    }
}

/* ===========================
 * Line editor + exec
 * =========================== */
static void reset_line(shell_t *sh)
{
    sh->line_len   = 0;
    sh->cursor_pos = 0;
    sh->history_pos = -1;
    memset(sh->linebuf, 0, sizeof sh->linebuf);
}

static void exec_line(shell_t *sh)
{
    char *argv[SHELL_MAX_ARGS + 1];
    int argc;

    sh_putc(sh, '\r'); sh_putc(sh, '\n');

    if (sh->line_len == 0) {
        sh_prompt(sh);
        return;
    }

    /* Add to history */
    shell_add_history(sh, sh->linebuf);

    /* We need a mutable copy for build_argv */
    char line_copy[SHELL_LINEBUF_SIZE];
    strncpy(line_copy, sh->linebuf, SHELL_LINEBUF_SIZE);
    line_copy[SHELL_LINEBUF_SIZE - 1] = '\0';

    argc = build_argv(line_copy, argv, SHELL_MAX_ARGS);
    if (argc == 0) {
        sh_prompt(sh);
        return;
    }

    const shell_ext_cmd_t *cmd = art_lookup(sh, argv[0]);
    if (cmd && cmd->fn) {
        cmd->fn(argc, argv, cmd->user_data);
    } else {
        sh_puts(sh, "Command not found\r\n");
    }

    sh_prompt(sh);
}

void shell_clear_screen(shell_t *sh)
{
    sh_puts(sh, ANSI_CLEAR_SCREEN);
    sh_puts(sh, ANSI_MOVE_CURSOR_HOME);
    shell_redraw_line(sh);
}

/* Repositions cursor to the correct spot based on cursor_pos */
static void sh_reposition_cursor(shell_t *sh)
{
    sh_putc(sh, '\r');
    if (sh->cursor_pos + sh->prompt_len > 0) {
        sh_puts(sh, "\033[");
        sh_puts_uint(sh, sh->cursor_pos + sh->prompt_len);
        sh_putc(sh, 'C');
    }
}

void shell_insert_text(shell_t *sh, const char *text)
{
    size_t len = strlen(text);
    if (sh->line_len + len >= SHELL_LINEBUF_SIZE) {
        len = SHELL_LINEBUF_SIZE - 1 - sh->line_len;
    }
    if (len == 0) return;
    
    /* Make space */
    memmove(&sh->linebuf[sh->cursor_pos + len],
            &sh->linebuf[sh->cursor_pos],
            sh->line_len - sh->cursor_pos);
    
    /* Insert */
    memcpy(&sh->linebuf[sh->cursor_pos], text, len);
    
    sh->line_len += (uint16_t)len;
    sh->cursor_pos += (uint16_t)len;
    sh->linebuf[sh->line_len] = '\0';
    
    shell_redraw_line(sh);
}

void shell_redraw_line(shell_t *sh)
{
    sh_putc(sh, '\r'); // Go to start of line
    sh_puts(sh, ANSI_CLEAR_LINE_FROM_CURSOR); // Clear to end of line
    sh_prompt(sh); // Prints "> " and sets prompt_len
    
    for (uint16_t i = 0; i < sh->line_len; i++) {
        sh_putc(sh, sh->linebuf[i]);
    }
    
    // Now move cursor back to correct position
    sh_reposition_cursor(sh);
}

const char *shell_get_line(shell_t *sh)
{
    return sh->linebuf;
}

static bool handle_key_event(shell_t *sh, shell_key_t key)
{
    /* Check custom bindings first */
    for (uint8_t i = 0; i < sh->keybind_count; i++) {
        if (sh->keybinds[i].key == key) {
            if (sh->keybinds[i].handler) {
                if (sh->keybinds[i].handler(sh, key, sh->keybinds[i].user_data))
                    return true; /* Handled */
            }
        }
    }

    /* Default handlers */
    switch (key) {
    case SHELL_KEY_CTRL_A:
    case SHELL_KEY_HOME:
        sh->cursor_pos = 0;
        sh_reposition_cursor(sh);
        return true;

    case SHELL_KEY_CTRL_E:
    case SHELL_KEY_END:
        sh->cursor_pos = sh->line_len;
        sh_reposition_cursor(sh);
        return true;

    case SHELL_KEY_CTRL_B:
    case SHELL_KEY_LEFT:
        if (sh->cursor_pos > 0) {
            sh->cursor_pos--;
            sh_putc(sh, '\b');
        }
        return true;

    case SHELL_KEY_CTRL_F:
    case SHELL_KEY_RIGHT:
        if (sh->cursor_pos < sh->line_len) {
            sh_putc(sh, sh->linebuf[sh->cursor_pos]);
            sh->cursor_pos++;
        }
        return true;

    case SHELL_KEY_CTRL_D:
    case SHELL_KEY_DEL:
        if (sh->cursor_pos < sh->line_len) {
            memmove(&sh->linebuf[sh->cursor_pos],
                    &sh->linebuf[sh->cursor_pos + 1],
                    sh->line_len - sh->cursor_pos);
            sh->line_len--;
            sh->linebuf[sh->line_len] = 0;
            shell_redraw_line(sh);
        }
        return true;

    case SHELL_KEY_CTRL_K:
        if (sh->cursor_pos < sh->line_len) {
            /* Kill from cursor to end */
            uint16_t killed_len = sh->line_len - sh->cursor_pos;
            memcpy(sh->killed_text, &sh->linebuf[sh->cursor_pos], killed_len);
            sh->killed_text[killed_len] = '\0';
            
            sh->line_len = sh->cursor_pos;
            sh->linebuf[sh->line_len] = 0;
            shell_redraw_line(sh);
        }
        return true;

    case SHELL_KEY_CTRL_U:
        if (sh->cursor_pos > 0) {
            /* Kill from beginning to cursor */
            uint16_t killed_len = sh->cursor_pos;
            memcpy(sh->killed_text, sh->linebuf, killed_len);
            sh->killed_text[killed_len] = '\0';

            memmove(sh->linebuf, &sh->linebuf[sh->cursor_pos], sh->line_len - sh->cursor_pos);
            sh->line_len -= sh->cursor_pos;
            sh->cursor_pos = 0;
            sh->linebuf[sh->line_len] = 0;
            shell_redraw_line(sh);
        }
        return true;

    case SHELL_KEY_CTRL_W: {
        /* Kill word backwards */
        if (sh->cursor_pos == 0) return true;
        
        uint16_t start = sh->cursor_pos;
        while (start > 0 && isspace((unsigned char)sh->linebuf[start - 1]))
            start--;
        while (start > 0 && !isspace((unsigned char)sh->linebuf[start - 1]))
            start--;

        if (start < sh->cursor_pos) {
            uint16_t killed_len = sh->cursor_pos - start;
            memcpy(sh->killed_text, &sh->linebuf[start], killed_len);
            sh->killed_text[killed_len] = '\0';

            memmove(&sh->linebuf[start], &sh->linebuf[sh->cursor_pos], 
                    sh->line_len - sh->cursor_pos);
            sh->line_len -= killed_len;
            sh->cursor_pos = start;
            sh->linebuf[sh->line_len] = '\0';
            shell_redraw_line(sh);
        }
        return true;
    }

    case SHELL_KEY_CTRL_T:
        /* Transpose characters */
        if (sh->cursor_pos > 0 && sh->line_len > 1) {
            uint16_t pos = sh->cursor_pos;
            if (pos == sh->line_len) pos--;
            
            if (pos > 0) {
                char tmp = sh->linebuf[pos];
                sh->linebuf[pos] = sh->linebuf[pos - 1];
                sh->linebuf[pos - 1] = tmp;
                shell_redraw_line(sh);
            }
        }
        return true;

    case SHELL_KEY_CTRL_L:
        shell_clear_screen(sh);
        return true;

    case SHELL_KEY_CTRL_C:
        sh_putc(sh, '^'); sh_putc(sh, 'C');
        sh_putc(sh, '\r'); sh_putc(sh, '\n');
        reset_line(sh);
        sh_prompt(sh);
        return true;

    case SHELL_KEY_CTRL_P:
    case SHELL_KEY_UP:
        history_prev(sh);
        return true;

    case SHELL_KEY_CTRL_N:
    case SHELL_KEY_DOWN:
        history_next(sh);
        return true;

    case SHELL_KEY_TAB:
        if (sh->complete_cb) {
            /* User has a custom override callback */
            sh->complete_cb(sh, shell_get_line(sh), NULL, 0);
        } else {
            /* Default built-in command completion */
            const char *line = sh->linebuf;
            size_t len = sh->line_len;

            // Only complete at end of line
            if (sh->cursor_pos != sh->line_len) {
                sh_putc(sh, '\a'); // Beep
                return true;
            }

            // Check if we're completing the first word (command)
            bool in_first_word = true;
            for (size_t i = 0; i < len; i++) {
                if (line[i] == ' ') {
                    in_first_word = false;
                    break;
                }
            }
            
            if (!in_first_word) {
                sh_putc(sh, '\a'); // Beep
                return true; // Don't complete arguments
            }

            int match_count = 0;
            int last_match_idx = -1;
            char common_prefix[SHELL_LINEBUF_SIZE];
            common_prefix[0] = '\0';

            // Find matching commands
            for (uint16_t i = 0; i < sh->cmd_count; i++) {
                const char *cmd_name = sh->cmd_table[i].name;
                
                // Check if command starts with current input
                if (strncmp(cmd_name, line, len) == 0 && cmd_name[len] != '\0') {
                    match_count++;
                    last_match_idx = i;
                    
                    if (match_count == 1) {
                        strncpy(common_prefix, cmd_name, SHELL_LINEBUF_SIZE - 1);
                        common_prefix[SHELL_LINEBUF_SIZE - 1] = '\0';
                    } else {
                        // Update common prefix
                        size_t j = 0;
                        while (common_prefix[j] && cmd_name[j] && common_prefix[j] == cmd_name[j]) {
                            j++;
                        }
                        common_prefix[j] = '\0';
                    }
                }
            }

            if (match_count == 0) {
                // No matches - beep
                sh_putc(sh, '\a');
            } else if (match_count == 1) {
                // Single match - complete it with a space
                const char *cmd_name = sh->cmd_table[last_match_idx].name;
                
                // Overwrite current line buffer
                strncpy(sh->linebuf, cmd_name, SHELL_LINEBUF_SIZE - 2);
                strncat(sh->linebuf, " ", SHELL_LINEBUF_SIZE - 1);
                sh->line_len = (uint16_t)strlen(sh->linebuf);
                sh->cursor_pos = sh->line_len;
                shell_redraw_line(sh);
            } else {
                // Multiple matches
                size_t common_len = strlen(common_prefix);
                
                if (common_len > len) {
                    // Insert common prefix
                    shell_insert_text(sh, common_prefix + len);
                } else {
                    // Show all matches
                    sh_putc(sh, '\r'); sh_putc(sh, '\n');

                    const int cols = 80;
                    int max_width = 0;

                    // Find longest command name
                    for (uint16_t i = 0; i < sh->cmd_count; i++) {
                        const char *cmd_name = sh->cmd_table[i].name;
                        if (strncmp(cmd_name, line, len) == 0 && cmd_name[len] != '\0') {
                            int name_len = (int)strlen(cmd_name);
                            if (name_len > max_width) {
                                max_width = name_len;
                            }
                        }
                    }

                    int col_width = max_width + 2;
                    int num_cols = cols / col_width;
                    if (num_cols < 1) num_cols = 1;

                    // Display matches in columns
                    int col = 0;
                    for (uint16_t i = 0; i < sh->cmd_count; i++) {
                        const char *cmd_name = sh->cmd_table[i].name;
                        if (strncmp(cmd_name, line, len) == 0 && cmd_name[len] != '\0') {
                            sh_puts(sh, cmd_name);
                            
                            // Add padding
                            int pad = col_width - (int)strlen(cmd_name);
                            for (int p = 0; p < pad; p++) {
                                sh_putc(sh, ' ');
                            }

                            col++;
                            if (col >= num_cols) {
                                sh_putc(sh, '\r'); sh_putc(sh, '\n');
                                col = 0;
                            }
                        }
                    }
                    
                    if (col > 0) {
                        sh_putc(sh, '\r'); sh_putc(sh, '\n');
                    }

                    // Redraw prompt and line
                    shell_redraw_line(sh);
                }
            }
        }
        return true;

    default:
        return false;
    }
}

static void handle_line_char(shell_t *sh, int ch)
{
    shell_key_t key = SHELL_KEY_NONE;

    /* Try escape sequence first */
    sh_parse_result_t res = esc_parse(sh, ch, &key);
    if (res == SH_PARSE_CONTINUE) {
        return;
    } else if (res == SH_PARSE_COMPLETE) {
        if (key != SHELL_KEY_NONE) {
            handle_key_event(sh, key);
        }
        return;
    }

    /* Handle control characters */
    if (ch >= 1 && ch <= 26) {
        switch (ch) {
        case 1:  key = SHELL_KEY_CTRL_A; break;
        case 2:  key = SHELL_KEY_CTRL_B; break;
        case 3:  key = SHELL_KEY_CTRL_C; break;
        case 4:  key = SHELL_KEY_CTRL_D; break;
        case 5:  key = SHELL_KEY_CTRL_E; break;
        case 6:  key = SHELL_KEY_CTRL_F; break;
        case 9:  key = SHELL_KEY_TAB; break;
        case 11: key = SHELL_KEY_CTRL_K; break;
        case 12: key = SHELL_KEY_CTRL_L; break;
        case 14: key = SHELL_KEY_CTRL_N; break;
        case 16: key = SHELL_KEY_CTRL_P; break;
        case 18: key = SHELL_KEY_CTRL_R; break;
        case 20: key = SHELL_KEY_CTRL_T; break;
        case 21: key = SHELL_KEY_CTRL_U; break;
        case 23: key = SHELL_KEY_CTRL_W; break;
        default: break;
        }
        if (key != SHELL_KEY_NONE) {
            handle_key_event(sh, key);
            return;
        }
    }

    /* Enter/Return */
    if (ch == '\r' || ch == '\n') {
        exec_line(sh);
        reset_line(sh);
        return;
    }

    /* Backspace */
    if (ch == 0x7F || ch == '\b') {
        if (sh->cursor_pos > 0) {
            memmove(&sh->linebuf[sh->cursor_pos - 1],
                    &sh->linebuf[sh->cursor_pos],
                    sh->line_len - sh->cursor_pos);
            sh->line_len--;
            sh->cursor_pos--;
            sh->linebuf[sh->line_len] = 0;
            shell_redraw_line(sh);
        }
        return;
    }

    /* Printable characters */
    if (ch >= 0x20 && ch < 0x7F) {
        if (sh->line_len < (SHELL_LINEBUF_SIZE - 1)) {
            memmove(&sh->linebuf[sh->cursor_pos + 1],
                    &sh->linebuf[sh->cursor_pos],
                    sh->line_len - sh->cursor_pos);
            sh->linebuf[sh->cursor_pos] = (char)ch;
            sh->line_len++;
            sh->cursor_pos++;
            sh->linebuf[sh->line_len] = '\0';
            
            if (sh->echo_enabled) {
                shell_redraw_line(sh);
            }
        }
    }
}

/* ===========================
 * Public API
 * =========================== */
shell_status_t shell_init(shell_t              *sh,
                          shell_putchar_func     putc_f,
                          shell_getchar_func     getc_f)
{
    if (!sh || !putc_f) return SHELL_ERR_ARG;

    memset(sh, 0, sizeof(*sh));
    sh->putc_f = putc_f;
    sh->getc_f = getc_f;

    /* Init ART root */
    for (int i = 0; i < SHELL_ART_MAX_NODES; i++) {
        sh->art_nodes[i].n_children = 0;
        sh->art_nodes[i].key = 0;
        sh->art_nodes[i].cmd_idx = -1;
        for (int j = 0; j < SHELL_ART_MAX_CHILDREN; j++) {
            sh->art_nodes[i].child_idx[j] = -1;
            sh->art_nodes[i].child_key[j] = 0;
        }
    }
    sh->art_root = 0;
    sh->art_free = 1;
    sh->art_max_used = 1;
    sh->art_overflow = false;
    esc_reset(&sh->esc);

    /* History */
    sh->history_pos = -1;
    sh->history_head = 0;
    sh->history_count = 0;

    /* Flags */
    sh->echo_enabled = true;
    sh->initial_prompt_shown = false;
    sh->prompt_len = 2; /* Default */

    return SHELL_OK;
}

shell_status_t shell_load_table(shell_t *sh,
                                const shell_ext_cmd_t *table,
                                uint16_t count)
{
    if (!sh || !table) return SHELL_ERR_ARG;

    sh->cmd_table = table;
    sh->cmd_count = count;

    /* Reset ART */
    for (int i = 0; i < SHELL_ART_MAX_NODES; i++) {
        sh->art_nodes[i].n_children = 0;
        sh->art_nodes[i].key = 0;
        sh->art_nodes[i].cmd_idx = -1;
        for (int j = 0; j < SHELL_ART_MAX_CHILDREN; j++) {
            sh->art_nodes[i].child_idx[j] = -1;
            sh->art_nodes[i].child_key[j] = 0;
        }
    }
    sh->art_root = 0;
    sh->art_free = 1;
    sh->art_max_used = 1;
    sh->art_overflow = false;

    for (uint16_t i = 0; i < count; i++) {
        const char *name = table[i].name;
        if (!name) continue;
        if (!art_insert(sh, name, (int16_t)i)) {
            sh->art_overflow = true;
            return SHELL_ERR_ART_OVERFLOW;
        }
    }

    return SHELL_OK;
}

void shell_set_login(shell_t *sh,
                     shell_login_cb cb,
                     char trigger_char)
{
    if (!sh) return;
    sh->login_cb      = cb;
    sh->login_trigger = trigger_char;
}

void shell_logout(shell_t *sh)
{
    if (!sh) return;
    sh->logged_in  = false;
    login_reset(sh);
}

bool shell_bind_key(shell_t *sh, shell_key_t key, 
                    shell_key_handler handler, void *user_data)
{
    if (!sh || sh->keybind_count >= SHELL_MAX_KEYBINDS)
        return false;

    /* Check if already bound, replace if so */
    for (uint8_t i = 0; i < sh->keybind_count; i++) {
        if (sh->keybinds[i].key == key) {
            sh->keybinds[i].handler = handler;
            sh->keybinds[i].user_data = user_data;
            return true;
        }
    }

    /* Add new binding */
    sh->keybinds[sh->keybind_count].key = key;
    sh->keybinds[sh->keybind_count].handler = handler;
    sh->keybinds[sh->keybind_count].user_data = user_data;
    sh->keybind_count++;
    return true;
}

void shell_unbind_key(shell_t *sh, shell_key_t key)
{
    if (!sh) return;

    for (uint8_t i = 0; i < sh->keybind_count; i++) {
        if (sh->keybinds[i].key == key) {
            /* Shift remaining bindings down */
            for (uint8_t j = i; j < sh->keybind_count - 1; j++) {
                sh->keybinds[j] = sh->keybinds[j + 1];
            }
            sh->keybind_count--;
            return;
        }
    }
}

void shell_set_complete(shell_t *sh, shell_complete_cb cb)
{
    if (!sh) return;
    sh->complete_cb = cb;
}

void shell_set_echo(shell_t *sh, bool enabled)
{
    if (!sh) return;
    sh->echo_enabled = enabled;
}

bool shell_get_echo(shell_t *sh)
{
    if (!sh) return false;
    return sh->echo_enabled;
}

void shell_run(shell_t *sh)
{
    if (!sh) return;

    /* Drain ISR queue first */
    int ch = shell_dequeue_char(sh);

    /* If queue empty, optionally poll user getchar (if provided) */
    if (ch < 0 && sh->getc_f)
        ch = sh->getc_f();

    if (ch < 0)
        return;

    /* First prompt: only if no login and not yet shown */
    if (!sh->initial_prompt_shown && !sh->login_cb) {
        sh->logged_in = true;
        sh->initial_prompt_shown = true;
        sh_prompt(sh);
    }

    if (sh->login_cb && !sh->logged_in) {
        handle_login(sh, ch);
        return;
    }

    handle_line_char(sh, ch);
}

void shell_get_stats(shell_t *sh, shell_stats_t *out)
{
    if (!sh || !out) return;
    out->max_nodes_used = sh->art_max_used;
    out->art_overflow   = sh->art_overflow;
    out->history_count  = sh->history_count;
    out->cmd_count      = sh->cmd_count;
    out->keybind_count  = sh->keybind_count;
}

const char *shell_get_history_entry(shell_t *sh, uint16_t index)
{
    if (!sh || index >= sh->history_count) {
        return NULL;
    }
    int start = (sh->history_head + SHELL_HISTORY_SIZE - sh->history_count) % SHELL_HISTORY_SIZE;
    int idx = (start + index) % SHELL_HISTORY_SIZE;
    return sh->history[idx].line;
}