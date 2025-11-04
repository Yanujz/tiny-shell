#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "shell.h"

// Platform-specific includes for terminal I/O
#ifdef _WIN32
    #include <conio.h>
#else
    #include <unistd.h>
    #include <termios.h>

static struct termios orig_termios;
static int            is_tty = 0;

void disableRawMode()
{
    if(is_tty)
    {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    }
}

void enableRawMode()
{
    if(!isatty(STDIN_FILENO))
    {
        is_tty = 0;
        return;
    }
    is_tty = 1;

    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disableRawMode);

    struct termios raw = orig_termios;
    raw.c_lflag    &= ~(ECHO | ICANON | ISIG);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

#endif

// === Shell I/O ===
static int my_putchar(int ch)
{
    return fputc(ch, stdout);
}

// === Shell Commands ===
static shell_t g_shell;

static void cmd_help(int argc, char** argv, void* user_data)
{
    printf("tiny-shell example. Available commands:\n");
    printf("  help     - Show this help\n");
    printf("  echo     - Echo arguments\n");
    printf("  clear    - Clear screen\n");
    printf("  stats    - Show shell stats\n");
    printf("  exit     - Exit the shell\n");
}

static void cmd_echo(int argc, char** argv, void* user_data)
{
    for(int i = 1; i < argc; i++)
    {
        printf("%s ", argv[i]);
    }
    printf("\n");
}

static void cmd_clear(int argc, char** argv, void* user_data)
{
    shell_clear_screen(&g_shell);
}

static void cmd_stats(int argc, char** argv, void* user_data)
{
    shell_stats_t stats;

    shell_get_stats(&g_shell, &stats);

    printf("Shell Statistics:\n");
    printf("  History: %u / %u\n", stats.history_count, SHELL_HISTORY_SIZE);
    printf("  Commands: %u\n", stats.cmd_count);
    printf("  Keybinds: %u / %u\n", stats.keybind_count, SHELL_MAX_KEYBINDS);
    printf("  ART Nodes: %u / %u\n", stats.max_nodes_used, SHELL_ART_MAX_NODES);
    printf("  ART Overflow: %s\n", stats.art_overflow ? "YES" : "no");
}

static void cmd_exit(int argc, char** argv, void* user_data)
{
    exit(0);
}

// === Command Table ===
static const shell_ext_cmd_t g_commands[] =
{
    { "help",  "Show available commands", cmd_help,  &g_shell },
    { "echo",  "Echo arguments",          cmd_echo,  NULL     },
    { "clear", "Clear the screen",        cmd_clear, &g_shell },
    { "stats", "Show shell statistics",   cmd_stats, &g_shell },
    { "exit",  "Exit the shell",          cmd_exit,  NULL     },
};
static const uint16_t CMD_COUNT = sizeof(g_commands) / sizeof(g_commands[0]);


// === Main ===
int main(void)
{
#ifndef _WIN32
    enableRawMode();
#endif

    shell_status_t status = shell_init(&g_shell, my_putchar, NULL);
    if(status != SHELL_OK)
    {
        fprintf(stderr, "shell_init failed: %d\n", status);
        return 1;
    }

    status = shell_load_table(&g_shell, g_commands, CMD_COUNT);
    if(status != SHELL_OK)
    {
        fprintf(stderr, "shell_load_table failed: %d\n", status);
        return 1;
    }

    printf("===========================================\n");
    printf("  tiny-shell Host Example (Raw Mode)\n");
    printf("===========================================\n");
    printf("Type 'help' for commands, Tab for completion.\n");
    printf("Type 'exit' or press Ctrl+D on empty line to quit.\n\n");

    shell_run(&g_shell);

    while(1)
    {
        int c;

#ifdef _WIN32
        c = _getch();
#else
        c = getchar();
#endif

        if(c == EOF)
        {
            break;
        }

        if(c == 4 && shell_get_line(&g_shell)[0] == '\0')
        {
            break;
        }

        shell_feed_char(&g_shell, (uint8_t)c);
        shell_run(&g_shell);
    }

    printf("\r\nGoodbye.\r\n");

    return 0;
}
