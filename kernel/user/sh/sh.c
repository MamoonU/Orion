// user/sh/sh.c - Orion usermode shell
 
#include "orion.h"
 
#define SH_LINE_MAX  256        // command line max length
#define SH_ARGV_MAX  16         // max # of args

// low level I/O helpers

// string -> stdout
static void sh_write(const char *s) {
    if (s) write(STDOUT_FILENO, s, (uint32_t)strlen(s));
}

// character -> stdout
static void sh_putchar(char c) {
    write(STDOUT_FILENO, &c, 1);
}

// stdin -> read one byte
static char sh_readchar(void) {
    char c = 0;
    read(STDIN_FILENO, &c, 1);
    return c;
}



// line editor: read until newline, echoes characters, handle backspaces
static uint32_t sh_readline(char *buf, uint32_t max) {

    uint32_t len = 0;

    while (1) {

        char c = sh_readchar();                 // read char

        if (c == '\n' || c == '\r') {           // enter
            sh_putchar('\n');
            break;
        }

        if (c == '\b' || c == 127) {            // backspace / DEL
            if (len > 0) {
                len--;
                sh_write("\b \b");
            }
            continue;
        }

        if (c < 0x20 || c > 0x7E) continue;     // ignore non-printable

        if (len < max - 1) {                    // echo char -> screen
            buf[len++] = c;
            sh_putchar(c);
        }
    }
    buf[len] = '\0';                            // null terminate string
    return len;                                 // return length
}
 
// tokeniser: split 'line' in-place into 'argv_max-1' tokens
static int sh_tokenise(char *line, char **argv, int argv_max) {

    int argc = 0;

    while (*line) {

        while (*line == ' ' || *line == '\t') line++;       // skip whitespace
        if (!*line) break;                                  // stop if end of line

        if (argc >= argv_max - 1) break;                    // leave room for NULL (prevent overflow)

        argv[argc++] = line;                                // start of token

        while (*line && *line != ' ' && *line != '\t') {    // scan token
            line++;
        }

        if (*line) *line++ = '\0';                          // terminate token
    }
    argv[argc] = 0;                                         // null terminate argv (output array)
    return argc;                                            // return arg count
}



