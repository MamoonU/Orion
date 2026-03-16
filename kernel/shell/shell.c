// shell.c - Orion Shell

#include "shell.h"
#include "proc.h"
#include "sched.h"
#include "vfs.h"
#include "namespace.h"
#include "fd.h"
#include "kprintf.h"
#include "string.h"
#include "vga.h"

static file_t **sh_fds = 0;                                 // pointer to shell process's own fd table: set once at startup

// write string -> stdout (fd 1)
static void sh_write(const char *s) {
    if (!s || !sh_fds) return;
    fd_write(sh_fds, 1, s, (uint32_t)strlen(s));
}

// write character
static void sh_putchar(char c) {
    if (!sh_fds) return;
    fd_write(sh_fds, 1, &c, 1);
}

// read character -> stdin (fd 0)
static char sh_readchar(void) {
    char c = 0;
    if (sh_fds) fd_read(sh_fds, 0, &c, 1);
    return c;
}

// read input -> echo chars -> return full line
static uint32_t sh_readline(char *buf, uint32_t max) {

    uint32_t len = 0;

    while (1) {                                             // read until "enter"

        char c = sh_readchar();                             // read chars

        if (c == '\n' || c == '\r') {                       // handle enter
            sh_putchar('\n');
            break;
        }

        if (c == '\b') {                                    // handle backspace
            if (len > 0) {
                len--;
                sh_write("\b \b");
            }
            continue;
        }

        if (c < 0x20 || c > 0x7E) continue;                 // ignore non-printable characters

        if (len < max - 1) {
            buf[len++] = c;
            sh_putchar(c);                                  // local echo
        }
    }
    buf[len] = '\0';                                        // terminate string (characters stored in buf)
    return len;
}

// command tokeniser ("ls home/user" -> argv[1] = "ls", argv[2] = "/home/user")
static int sh_tokenise(char *line, char **argv, int argv_max) {

    int argc = 0;

    while (*line) {

        while (*line == ' ' || *line == '\t') line++;               // skip leading whitespace
        if (!*line) break;

        if (argc >= argv_max - 1) break;                            // leave room for NULL sentinel

        argv[argc++] = line;                                        // mark arg start

        while (*line && *line != ' ' && *line != '\t') line++;      // advance to end of token
        if (*line) *line++ = '\0';                                  // null-terminate token
    }
    argv[argc] = 0;
    return argc;
}

// prompt printing
static void sh_print_prompt(void) {

    pcb_t *p = sched_current();

    sh_write("\norion");
    sh_write(":");

    if (p) sh_write(p->cwd_path);
    else   sh_write("/");
    sh_write(" $ ");

}

// built-in: help (print command arg descriptions)
static void builtin_help(void) {

    sh_write("\nOrion Shell - built-in commands:\n");
    sh_write("  help               show this message\n");
    sh_write("  echo [args...]     print arguments\n");
    sh_write("  pwd                print working directory\n");
    sh_write("  cd <path>          change directory\n");
    sh_write("  ls [path]          list directory\n");
    sh_write("  cat <file>         print file contents\n");
    sh_write("  mkdir <path>       create directory\n");
    sh_write("  clear              clear screen\n");
    sh_write("  ps                 list processes (placeholder)\n");
    sh_write("  bind [-b|-a] <src> <dst>   bind src into namespace at dst\n");
    sh_write("                             -b = before (union prepend)\n");
    sh_write("                             -a = after  (union append)\n");
    sh_write("                             default = replace\n");
    sh_write("  unbind <dst>               remove all bindings at dst\n");
    sh_write("  nsdump                     dump this process's namespace\n");
    // TODO(9p)        : mount <srv> <dst> - attach a 9P server to the namespace
    // TODO(network)   : dial <addr>       - open a network connection via /net
}

// built-in: echo (print args)
static void builtin_echo(int argc, char **argv) {

    for (int i = 1; i < argc; i++) {
        if (i > 1) sh_putchar(' ');
        sh_write(argv[i]);
    }
    sh_putchar('\n');
}

// built-in: pwd (print working directory)
static void builtin_pwd(void) {

    pcb_t *p = sched_current();
    sh_write(p ? p->cwd_path : "/");
    sh_putchar('\n');

}

// built-in: cd (change working directory)
static void builtin_cd(int argc, char **argv) {

    const char *target = (argc >= 2) ? argv[1] : "/";

    pcb_t *p = sched_current();
    if (!p) return;

    char resolved[VFS_PATH_MAX];
    vfs_path_resolve(p->cwd_path, target, resolved);                                // resolve path using the VFS helper

    vnode_t *v = ns_resolve(p->namespace, resolved);                               // locate vnode
    if (!v) {
        sh_write("cd: no such directory: ");
        sh_write(target);
        sh_putchar('\n');
        return;
    }
    if (v->type != VNODE_DIR) {                                                     // validate directory
        sh_write("cd: not a directory: ");
        sh_write(target);
        sh_putchar('\n');
        return;
    }
    strncpy(p->cwd_path, resolved, VFS_PATH_MAX - 1);                               // update process cwd
    p->cwd_path[VFS_PATH_MAX - 1] = '\0';
}

// built-in: ls (list directory contents)
static void builtin_ls(int argc, char **argv) {

    pcb_t *p = sched_current();
    if (!p) return;

    char resolved[VFS_PATH_MAX];
    const char *target = (argc >= 2) ? argv[1] : p->cwd_path;
    vfs_path_resolve(p->cwd_path, target, resolved);                                // resolve path 

    vnode_t *v = ns_resolve(p->namespace, resolved);                                             // resolve vnode
    if (!v) {
        sh_write("ls: no such path: ");
        sh_write(resolved);
        sh_putchar('\n');
        return;
    }
    if (v->type != VNODE_DIR) {
        const char *name = resolved;
        for (const char *q = resolved; *q; q++)
            if (*q == '/') name = q + 1;
        sh_write(name);
        sh_putchar('\n');
        return;
    }

    char     name_buf[VFS_NAME_MAX];
    vnode_t *child = 0;
    uint32_t index = 0;

    while (ns_readdir(p->namespace, resolved, index,
                      name_buf, VFS_NAME_MAX, &child) == 0) {
        sh_write("  ");
        sh_write(name_buf);
        if (child && child->type == VNODE_DIR) sh_putchar('/');
        sh_putchar('\n');
        index++;
    }
    if (index == 0) sh_write("  (empty)\n");
}

// built-in: cat (read file parameters in sequence -> write to stdout)
static void builtin_cat(int argc, char **argv) {

    if (argc < 2) {                                                                 // validate args
        sh_write("usage: cat <file>\n");
        return;
    }

    pcb_t *p = sched_current();                                                     // return current process
    if (!p) return;

    char resolved[VFS_PATH_MAX];
    vfs_path_resolve(p->cwd_path, argv[1], resolved);                               // resolve path

    vnode_t *v = ns_resolve(p->namespace, resolved);
    if (!v) {
        sh_write("cat: cannot open: ");
        sh_write(argv[1]);
        sh_putchar('\n');
        return;
    }
    file_t *f = vfs_open_vnode(v, O_RDONLY);
    if (!f) {                                                                       // handle failure
        sh_write("cat: cannot open: ");
        sh_write(argv[1]);
        sh_putchar('\n');
        return;
    }

    char buf[128];                                                                  // create read buffer
    int  n;
    while ((n = vfs_read(f, buf, sizeof(buf) - 1)) > 0) {                           // read file loop
        buf[n] = '\0';                                                              // null terminate
        sh_write(buf);                                                              // print -> shell stdout
    }
    sh_putchar('\n');
    vfs_close(f);
}

// built-in: mkdir (create directory)
static void builtin_mkdir(int argc, char **argv) {

    if (argc < 2) {                                                                 // validate args
        sh_write("usage: mkdir <path>\n");
        return;
    }

    pcb_t *p = sched_current();                                                     // return current process
    if (!p) return;

    char resolved[VFS_PATH_MAX];
    vfs_path_resolve(p->cwd_path, argv[1], resolved);                               // resolve path

    if (vfs_mkdir(resolved) < 0) {                                                  // call vfs
        sh_write("mkdir: failed: ");
        sh_write(argv[1]);
        sh_putchar('\n');
    }
}

// built-in: ps (print process table)
static void builtin_ps(void) {

    // TODO(userland): when kprintf is redirected through -> fd table
    // builtin_ps = write -> shell(stdout)

    sh_write("--- process table ---\n");
    proc_dump_all();
}

// built-in: clear (clear terminal display)
static void builtin_clear(void) {
    terminal_clear();
}

// built-in: bind
static void builtin_bind(int argc, char **argv) {

    if (argc < 3) {
        sh_write("usage: bind [-b|-a] <src> <dst>\n");
        return;
    }

    pcb_t *p = sched_current();
    if (!p) return;

    uint8_t     flags   = NS_BIND_REPLACE;
    const char *src_arg = argv[1];
    const char *dst_arg = argv[2];

    if (argc >= 4 && argv[1][0] == '-') {
        if      (argv[1][1] == 'b') flags = NS_BIND_BEFORE;
        else if (argv[1][1] == 'a') flags = NS_BIND_AFTER;
        else {
            sh_write("bind: unknown flag (use -b or -a): ");
            sh_write(argv[1]);
            sh_putchar('\n');
            return;
        }
        src_arg = argv[2];
        dst_arg = argv[3];
    }

    char src_resolved[VFS_PATH_MAX];
    char dst_resolved[VFS_PATH_MAX];
    vfs_path_resolve(p->cwd_path, src_arg, src_resolved);
    vfs_path_resolve(p->cwd_path, dst_arg, dst_resolved);

    vnode_t *v = ns_resolve(p->namespace, src_resolved);
    if (!v) {
        sh_write("bind: no such path: ");
        sh_write(src_arg);
        sh_putchar('\n');
        return;
    }

    if (ns_bind(&p->namespace, v, dst_resolved, flags) < 0)
        sh_write("bind: failed (namespace table full?)\n");
}

// built-in: unbind
static void builtin_unbind(int argc, char **argv) {

    if (argc < 2) {
        sh_write("usage: unbind <dst>\n");
        return;
    }

    pcb_t *p = sched_current();
    if (!p) return;

    char resolved[VFS_PATH_MAX];
    vfs_path_resolve(p->cwd_path, argv[1], resolved);

    if (ns_unbind(&p->namespace, resolved) < 0) {
        sh_write("unbind: no binding at: ");
        sh_write(argv[1]);
        sh_putchar('\n');
    }
}

// built-in: namespace dump
static void builtin_nsdump(void) {
    pcb_t *p = sched_current();
    if (!p) return;
    ns_dump(p->namespace);
}

static void sh_exec_external(int argc, char **argv) {

    (void)argc;
    sh_write("exec: '");
    sh_write(argv[0]);
    sh_write("' not found\n");

    // TODO(9p/userland): search PATH (once /bin is a 9P-backed directory),
    // open binary, fork, exec.

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
    else if (strcmp(argv[0], "mkdir"  ) == 0) builtin_mkdir(argc, argv);
    else if (strcmp(argv[0], "ps"     ) == 0) builtin_ps();
    else if (strcmp(argv[0], "clear"  ) == 0) builtin_clear();
    else if (strcmp(argv[0], "bind"   ) == 0) builtin_bind(argc, argv);
    else if (strcmp(argv[0], "unbind" ) == 0) builtin_unbind(argc, argv);
    else if (strcmp(argv[0], "nsdump" ) == 0) builtin_nsdump();
    else                                      sh_exec_external(argc, argv);
}

// shell entry point
void shell_run(void) {

    pcb_t *self = sched_current();                                  // grab pointer to our own fd table once: everything else uses sh_fds
    if (self) sh_fds = self->fd_table;

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
    "Orion Shell - type 'help' for commands\n"
    );

    char  line[SHELL_LINE_MAX];                                     // allocate buffers
    char *argv[SHELL_ARGV_MAX];

    while (1) {                                                     // main loop
        sh_print_prompt();                                          // prompt
        sh_readline(line, SHELL_LINE_MAX);                          // read command line

        int argc = sh_tokenise(line, argv, SHELL_ARGV_MAX);         // tokenise
        if (argc == 0) continue;

        sh_dispatch(argc, argv);                                    // excecute
    }
}