# tiny-shell

![Build Status](https://img.shields.io/badge/build-passing-brightgreen)
![C Standard](https://img.shields.io/badge/C-C99-blue)
![License](https://img.shields.io/badge/license-MIT-blue)

A lightweight, non-blocking shell library in pure C for embedded systems.

`tiny-shell` is a small, portable shell that runs on microcontrollers. It's built for reliability: it uses **zero heap** (no `malloc`) and fits easily into any `main()` loop or RTOS task.

You get a proper CLI with line editing, history, and tab completion, but without the complex dependencies.

---

## Why tiny-shell?

Most embedded shells are either too basic (no line editing) or too heavy (need `malloc` or an RTOS). `tiny-shell` is a middle ground: it's small and reliable but still gives you the features you'd expect from a real shell.

### Features

* **No Heap, No Problem:** 100% static allocation. You pass in the memory, so there are no `malloc` calls, memory leaks, or fragmentation.
* **Non-Blocking:** Built to run in a simple `while(1)` loop. Just feed it characters with `shell_feed_char()` and call `shell_run()` regularly.
* **Portable C99:** Runs on just about anything. All platform-specific I/O (like `putchar`) is passed in as function pointers.
* **Real Line Editing:**
    * `Ctrl+A` (Home), `Ctrl+E` (End), `Ctrl+B/F` (Left/Right)
    * `Ctrl+K` (Kill to end), `Ctrl+U` (Kill to start), `Ctrl+W` (Kill word)
    * Arrow key support (Up, Down, Left, Right)
    * Backspace and Delete
* **Command History:** Use the Up/Down arrow keys to browse previous commands.
* **Tab Completion:** Built-in command completion that can show multiple matches.
* **Quotes Handled:** The parser understands arguments in `"quotes"`.
* **Secure Login (Optional):** Includes an optional login check that uses a constant-time comparison to prevent timing attacks.
* **Clean ANSI Redraw:** Uses ANSI escape codes for flicker-free line redrawing.

---

## Getting Started

### Try the Example (macOS, Linux, Windows)

The easiest way to see `tiny-shell` in action is to build the host example. It uses CMake and runs right in your computer's terminal.

```sh
# 1. Clone the repository
git clone [https://github.com/Yanujz/tiny-shell.git](https://github.com/Yanujz/tiny-shell.git)
cd tiny-shell

# 2. Create a build directory
mkdir build
cd build

# 3. Configure the project
cmake ..

# 4. Build the code
cmake --build .

# 5. Run the example!
./example/example
```

### Add to Your Project
Using `tiny-shell` in your own firmware (like Arduino, ESP32, or STM32) is straightforward:

1. Add src/shell.c and src/shell.h to your build.
2. Include the header: #include "shell.h"
3. Implement your putchar function:
    ```C
    static int my_putchar(int ch) {
        my_uart_write((char)ch);
        return ch;
    }
    ```
4. Define your shell instance and command table:
    ```C
    static shell_t g_shell;
    static const shell_ext_cmd_t g_commands[] = {
        { "help", "Show help", cmd_help, NULL },
        /* ... your other commands ... */
    };
    ```

5. In your main() or setup():
    ```C
    // Pass in your putchar function. Pass NULL for getchar
    // since we're feeding characters manually.
    shell_init(&g_shell, my_putchar, NULL);

    // Load your command table
    shell_load_table(&g_shell, g_commands, CMD_COUNT);
    ```

6. In your main loop:
    ```C
    // Feed characters from UART/USB
    while (my_uart_has_data()) {
        shell_feed_char(&g_shell, my_uart_read_byte());
    }

    // Process the shell state machine
    shell_run(&g_shell);
    ```

## License
This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
