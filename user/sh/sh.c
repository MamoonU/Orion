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

// prompt ( orion: $ )
static void sh_print_prompt(void) {

    char cwd[256];

    if (getcwd(cwd, sizeof(cwd)) < 0) {         // return current working directory
        strcpy(cwd, "/");
    }

    sh_write("\norion:");
    sh_write(cwd);
    sh_write(" $ ");                            // orion:/net/tcp $
}

// builtins

// list builtin commands with descriptions
static void builtin_help(void) {
    sh_write("\nOrion Shell [ring-3] built-in commands:\n");
    sh_write("  help               show this message\n");
    sh_write("  echo [args...]     print arguments\n");
    sh_write("  pwd                print working directory\n");
    sh_write("  cd <path>          change directory\n");
    sh_write("  ls [path]          list directory\n");
    sh_write("  cat <file>         print file contents\n");
    sh_write("  clear              clear screen\n");
    sh_write("  ps                 list processes (stub)\n");
    sh_write("  bind [-b|-a] <src> <dst>  bind src into namespace at dst\n");
    sh_write("                            -b = before (union prepend)\n");
    sh_write("                            -a = after  (union append)\n");
    sh_write("                            default = replace\n");
    sh_write("  unbind <dst>              remove all bindings at dst\n");
    sh_write("  nsdump                    dump this process namespace\n");
    sh_write("  exit [code]               exit shell\n");
}

// echo [args...] = print arguments
static void builtin_echo(int argc, char **argv) {

    for (int i = 1; i < argc; i++) {                    // skip argv[0] (command name)
        if (i > 1) sh_putchar(' ');                     // insert space between args
        sh_write(argv[i]);                              // write args
    }

    sh_putchar('\n');
}

// pwd = print working directory
static void builtin_pwd(void) {
    
    char cwd[256];
    if (getcwd(cwd, sizeof(cwd)) >= 0) {                // return current working directory
        sh_write(cwd);
        sh_putchar('\n');
    } else {
        sh_write("pwd: failed\n");                      // getcwd() failed
    }
}

// cd <path> = change directory
static void builtin_cd(int argc, char **argv) {

    const char *target = (argc >= 2) ? argv[1] : "/";   // no args = go to root "/"
    if (chdir(target) < 0) {
        sh_write("cd: no such directory: ");
        sh_write(target);
        sh_putchar('\n');
    }
}

// ls [path] = list directory
static void builtin_ls(int argc, char **argv) {

    char cwd[256];
    getcwd(cwd, sizeof(cwd));
    const char *target = (argc >= 2) ? argv[1] : cwd;   // resolve target: no args = list cwd

    int fd = open(target, O_RDONLY);                    // open the directory as a file: vfs_open on dir gives file_t that sys_readdir can iterate
    if (fd < 0) {
        sh_write("ls: cannot open: ");
        sh_write(target);
        sh_putchar('\n');
        return;
    }

    char     name_buf[128];                             // output name buffer
    uint32_t index = 0;                                 // entry number
    int      count = 0;

    while (readdir(fd, index, name_buf, (uint32_t)sizeof(name_buf)) == 0) {     // iterate entries
        sh_write("  ");
        sh_write(name_buf);
        sh_putchar('\n');
        index++;
        count++;
    }

    if (count == 0) sh_write("  (empty)\n");            // empty directory handling
    close(fd);
}

// cat <file> = print file contents
static void builtin_cat(int argc, char **argv) {

    if (argc < 2) {
        sh_write("usage: cat <file>\n");
        return;
    }

    int fd = open(argv[1], O_RDONLY);                   // open file
    if (fd < 0) {
        sh_write("cat: cannot open: ");
        sh_write(argv[1]);
        sh_putchar('\n');
        return;
    }

    char buf[256];
    int  n;

    while ((n = read(fd, buf, (uint32_t)(sizeof(buf) - 1))) > 0) {      // read chunks from file loop
        buf[n] = '\0';
        sh_write(buf);                                                  // print
    }

    sh_putchar('\n');
    close(fd);
}

// clear = print 25 new lines
static void builtin_clear(void) {
    for (int i = 0; i < 25; i++) sh_putchar('\n');
}

// ps = list processes: !!!!!!!no /proc filesystem yet!!!!!!!!
static void builtin_ps(void) {
    sh_write("ps: /proc not implemented.\n");
}

// bind [-b|-a] <src> <dst> = bind src into namespace at dst
static void builtin_bind(int argc, char **argv) {

    if (argc < 3) {
        sh_write("usage: bind [-b|-a] <src> <dst>\n");
        return;
    }

    uint8_t     flags   = NS_BIND_REPLACE;                      // NS_BIND_REPLACE = default
    const char *src_arg = argv[1];
    const char *dst_arg = argv[2];

    if (argc >= 4 && argv[1][0] == '-') {                       // detect flags

        if      (argv[1][1] == 'b') flags = NS_BIND_BEFORE;     // NS_BIND_BEFORE = (-b)
        else if (argv[1][1] == 'a') flags = NS_BIND_AFTER;      // NS_BIND_AFTER = (-a)
        else {
            sh_write("bind: unknown flag (use -b or -a)\n");
            return;
        }

        src_arg = argv[2];
        dst_arg = argv[3];
    }

    if (bind(src_arg, dst_arg, flags) < 0) {                    // syscall
        sh_write("bind: failed (namespace table full?)\n");
    }
}

// unbind <dst>
static void builtin_unbind(int argc, char **argv) {

    if (argc < 2) {
        sh_write("usage: unbind <dst>\n");
        return;
    }

    if (unbind(argv[1]) < 0) {                      // syscall: remove all bindings at path(dst)
        sh_write("unbind: no binding at: ");
        sh_write(argv[1]);
        sh_putchar('\n');
    }
}


static const char *g_exec_path = 0;             // shared variable between parent & child: set by sh_exec before fork

// runs in child process
static void exec_child(void) {
    execve(g_exec_path);                        // replace child image with ELF from /bin/<cmd>
    sh_write("exec: execve failed\n");
    _exit(127);
}

// execute external = main launcher
static void sh_exec_external(int argc, char **argv) {

    (void)argc;

    char path[256];
    snprintf(path, sizeof(path), "/bin/%s", argv[0]);   // build /bin/<cmd> path

    int test_fd = open(path, O_RDONLY);                 // check file existence: avoid useless fork
    if (test_fd < 0) {
        sh_write("sh: '");
        sh_write(argv[0]);
        sh_write("': not found (looked in /bin/)\n");
        return;
    }
    close(test_fd);

    g_exec_path = path;                             // set shared path before fork

    int child_pid = fork((uint32_t)exec_child);     // fork: child runs exec_child()
    if (child_pid < 0) {
        sh_write("sh: fork failed\n");
        g_exec_path = 0;
        return;
    }

    int exit_code = 0;
    wait(child_pid, &exit_code);                    // block until child finishes

    g_exec_path = 0;
}

// command dispatcher
static void sh_dispatch(int argc, char **argv) {

    if (argc == 0) return;

    if      (strcmp(argv[0], "help"   ) == 0) builtin_help();
    else if (strcmp(argv[0], "echo"   ) == 0) builtin_echo(argc, argv);
    else if (strcmp(argv[0], "pwd"    ) == 0) builtin_pwd();
    else if (strcmp(argv[0], "cd"     ) == 0) builtin_cd(argc, argv);
    else if (strcmp(argv[0], "ls"     ) == 0) builtin_ls(argc, argv);
    else if (strcmp(argv[0], "cat"    ) == 0) builtin_cat(argc, argv);
    else if (strcmp(argv[0], "clear"  ) == 0) builtin_clear();
    else if (strcmp(argv[0], "ps"     ) == 0) builtin_ps();
    else if (strcmp(argv[0], "bind"   ) == 0) builtin_bind(argc, argv);
    else if (strcmp(argv[0], "unbind" ) == 0) builtin_unbind(argc, argv);
    else if (strcmp(argv[0], "nsdump" ) == 0) nsdump();
    else if (strcmp(argv[0], "exit"   ) == 0) {
        int code = (argc >= 2) ? atoi(argv[1]) : 0;
        _exit(code);                                            // terminate shell
    }
    else sh_exec_external(argc, argv);                          // fallback: unknown = /bin/<cmd>
}

// entry point
int main(int argc, char **argv) {

    (void)argc;
    (void)argv;

    signal(SIGINT, SIG_IGN);                                    // shell ignores SIGINT: only foreground children die on Ctrl+C

    sh_write(
    "\n"
    "_______/\\\\\\\\\\______________________________________________________        \n"
    " _____/\\\\\\///\\\\\\____________________________________________________       \n"
    "  ___/\\\\\\/__\\///\\\\\\_________________/\\\\\\_____________________________      \n"
    "   __/\\\\\\______\\//\\\\\\__/\\\\/\\\\\\\\\\\\\\__\\///______/\\\\\\\\\\_____/\\\\/\\\\\\\\\\\\___     \n"
    "    _\\/\\\\\\_______\\/\\\\\\_\\/\\\\\\/////\\\\\\__/\\\\\\___/\\\\\\///\\\\\\__\\/\\\\\\////\\\\\\__    \n"
    "     _\\//\\\\\\______/\\\\\\__\\/\\\\\\___\\///__\\/\\\\\\__/\\\\\\__\\//\\\\\\_\\/\\\\\\__\\//\\\\\\_   \n"
    "      __\\///\\\\\\__/\\\\\\____\\/\\\\\\_________\\/\\\\\\_\\//\\\\\\__/\\\\\\__\\/\\\\\\___\\/\\\\\\_  \n"
    "       ____\\///\\\\\\\\\\/_____\\/\\\\\\_________\\/\\\\\\__\\///\\\\\\\\\\/___\\/\\\\\\___\\/\\\\\\_ \n"
    "        ______\\/////_______\\///__________\\///_____\\/////_____\\///____\\///__\n"
    "\n"
    "Orion Shell [ring-3] - type 'help' for commands\n"
    );

    char  line[SH_LINE_MAX];                                    // line input buffer max = 256
    char *argv_buf[SH_ARGV_MAX];                                // args max              = 16

    while (1) {
        sh_print_prompt();                                      // print prompt
        sh_readline(line, SH_LINE_MAX);                         // read input
        int ac = sh_tokenise(line, argv_buf, SH_ARGV_MAX);      // tokenise input
        if (ac == 0) continue;                                  // skip empty
        sh_dispatch(ac, argv_buf);                              // execute
    }
    return 0;                                                   // unreachable: _exit() called on "exit" command
}
 