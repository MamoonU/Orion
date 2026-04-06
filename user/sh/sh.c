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

// save username -> /etc/username
static void save_username(const char *uname) {
    int fd = open("/etc/username", O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return;
    write(fd, uname, (uint32_t)strlen(uname));
    close(fd);
}

// load username from /etc/username into buf
static void load_username(char *buf, uint32_t bufsz) {
    buf[0] = '\0';
    int fd = open("/etc/username", O_RDONLY);
    if (fd < 0) return;
    int n = read(fd, buf, bufsz - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = '\0';
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r' || buf[n-1] == ' '))
        buf[--n] = '\0';
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

    sh_write("\nOrion");
    sh_write(cwd);
    sh_write("/: $ ");                            // Orion/username:/ $ 
}

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
    sh_write("  write <path> <data...>    write string to file (creates if needed)\n");
    sh_write("  open <path>               open file; prints slot number for clone files\n");
    sh_write("  bind [-b|-a] <src> <dst>  bind src into namespace at dst\n");
    sh_write("                            -b = before (union prepend)\n");
    sh_write("                            -a = after  (union append)\n");
    sh_write("                            default = replace\n");
    sh_write("  unbind <dst>              remove all bindings at dst\n");
    sh_write("  nsdump                    dump this process namespace\n");
    sh_write("  mount [-b|-a] <ip!port> <path>  dial PULSAR server + mount\n");
    sh_write("                            e.g. mount 192.168.1.2!564 /remote\n");
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

    const char *target;
    static char home_path[128];                             // static: survives the function call

    if (argc >= 2) {
        target = argv[1];
    } else {
        char uname[64];
        load_username(uname, sizeof(uname));
        if (uname[0]) snprintf(home_path, sizeof(home_path), "/%s", uname);
        else           strcpy(home_path, "/");
        target = home_path;
    }

    if (chdir(target) < 0) {
        sh_write("cd: no such directory: ");
        sh_write(target);
        sh_putchar('\n');
    }

}

// ls [path] = list directory
static void builtin_ls(int argc, char **argv) {

    int long_fmt = 0;
    const char *target = 0;                             // resolve target: no args = list cwd
    char cwd[256];
    getcwd(cwd, sizeof(cwd));

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] == 'l') long_fmt = 1;
        else if (!target) target = argv[i];
    }
    if (!target) target = cwd;

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

    while (readdir(fd, index, name_buf, sizeof(name_buf)) == 0) {

        if (long_fmt) {

            // build full child path
            char child[256];
            uint32_t tlen = (uint32_t)strlen(target);
            strncpy(child, target, sizeof(child) - 1);
            child[sizeof(child) - 1] = '\0';
            if (tlen > 0 && child[tlen - 1] != '/') {
                child[tlen] = '/'; child[tlen + 1] = '\0';
            }
            strncat(child, name_buf, sizeof(child) - strlen(child) - 1);

            char type_c = '?';
            int  size   = 0;

            int cfd = open(child, O_RDONLY);
            if (cfd >= 0) {
                char tmp[32];
                if (readdir(cfd, 0, tmp, sizeof(tmp)) == 0) {
                    type_c = 'd';                                           // readdir succeeded = directory
                } else {
                    type_c = 'f';
                    int32_t sz = (int32_t)lseek(cfd, 0, 2);                 // SEEK_END = 2
                    if (sz >= 0) size = (int)sz;
                }
                close(cfd);
            }

            char size_s[16];
            snprintf(size_s, sizeof(size_s), "%d", size);
            uint32_t slen = (uint32_t)strlen(size_s);

            sh_putchar(type_c);
            sh_write("  ");
            for (uint32_t p = slen; p < 8; p++) sh_putchar(' ');            // right-align size
            sh_write(size_s);
            sh_write("  ");
            sh_write(name_buf);
            sh_putchar('\n');

        } else {
            sh_write("  ");
            sh_write(name_buf);
            sh_putchar('\n');
        }

        index++;
        count++;
    }

    if (count == 0) sh_write("  (empty)\n");
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
    sh_write("\033[2J\033[H");
}

// ps = list processes: !!!!!!!no /proc filesystem yet!!!!!!!!
static void builtin_ps(void) {

    sh_write("PID  PPID NAME             STATE\n");
    sh_write("---  ---- ---------------  -----\n");

    char name_buf[8];
    uint32_t index = 0;

    int fd = open("/proc", O_RDONLY);                                               // open /proc as a directory
    if (fd < 0) { sh_write("ps: cannot open /proc\n"); return; }

    while (readdir(fd, index, name_buf, sizeof(name_buf)) == 0) {                   // iterate PID directory entries

        index++;

        if (name_buf[0] < '0' || name_buf[0] > '9') continue;                       // skip "uptime"

        char status_path[32];
        snprintf(status_path, sizeof(status_path), "/proc/%s/status", name_buf);    // build /proc/<pid>/status

        int sfd = open(status_path, O_RDONLY);                                      // open status file
        if (sfd < 0) continue;

        char sbuf[256];
        int  n = read(sfd, sbuf, sizeof(sbuf) - 1);                                 // read status file
        close(sfd);
        if (n <= 0) continue;
        sbuf[n] = '\0';

        char pid_s[8]="?", ppid_s[8]="?", name_s[32]="?", state_s[16]="?";          // extract pid, ppid, name, state from status file
    
        char *line = sbuf;
        while (*line) {                                                             // line by line parsing
            char *end = line;                                                       // find end of line
            while (*end && *end != '\n') end++;
            if (*end) *end = '\0';

            // match fields
            if      (strncmp(line, "pid: ",    5) == 0) strncpy(pid_s,   line+5, sizeof(pid_s)-1);      // pid
            else if (strncmp(line, "ppid: ",   6) == 0) strncpy(ppid_s,  line+6, sizeof(ppid_s)-1);     // ppid
            else if (strncmp(line, "name: ",   6) == 0) strncpy(name_s,  line+6, sizeof(name_s)-1);     // name
            else if (strncmp(line, "state: ",  7) == 0) strncpy(state_s, line+7, sizeof(state_s)-1);    // state

            line = end + 1;                                                         // move to next line
        }

        sh_write(pid_s);  sh_write("  ");                                           // print row
        sh_write(ppid_s); sh_write("  ");
        sh_write(name_s); sh_write("  ");
        sh_write(state_s);
        sh_putchar('\n');
    }
    close(fd);                                                                      // close
}

// write <path> <data...> = write string to file, creating it if it does not exist
static void builtin_write(int argc, char **argv) {

    if (argc < 3) {
        sh_write("usage: write <path> <data...>\n");
        return;
    }

    // join argv[2..] into one buffer, space-separated
    char data[SH_LINE_MAX];
    uint32_t pos = 0;
    for (int i = 2; i < argc && pos < sizeof(data) - 1; i++) {
        if (i > 2 && pos < sizeof(data) - 1) data[pos++] = ' ';
        const char *s = argv[i];
        while (*s && pos < sizeof(data) - 1) data[pos++] = *s++;
    }
    data[pos] = '\0';

    // strip surrounding double-quotes: write /net/ipifc/0/ctl "add 192.168.1.1 ..."
    char *dp   = data;
    uint32_t dl = pos;
    if (dl >= 2 && dp[0] == '"' && dp[dl - 1] == '"') {
        dp[dl - 1] = '\0';
        dp++;
        dl -= 2;
    }

    // open existing device/file for writing; fall back to create for regular files
    int fd = open(argv[1], O_WRONLY);
    if (fd < 0) fd = open(argv[1], O_WRONLY | O_CREAT);
    if (fd < 0) {
        sh_write("write: cannot open: ");
        sh_write(argv[1]);
        sh_putchar('\n');
        return;
    }

    uint32_t wlen = (dl > 0) ? dl : (uint32_t)strlen(dp);
    int n = write(fd, dp, wlen);
    close(fd);

    if (n < 0) {
        sh_write("write: write failed\n");
    }
}

// open <path> = open file and print any immediate response
static void builtin_open(int argc, char **argv) {

    if (argc < 2) {
        sh_write("usage: open <path>\n");
        return;
    }

    // try read-write first (clone files need to return data); fall back read-only
    int fd = open(argv[1], O_RDWR);
    if (fd < 0) fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        sh_write("open: cannot open: ");
        sh_write(argv[1]);
        sh_putchar('\n');
        return;
    }

    // read response: clone files return the allocated slot number as a decimal string
    char buf[32];
    int n = read(fd, buf, (uint32_t)(sizeof(buf) - 1));
    close(fd);

    if (n > 0) {
        buf[n] = '\0';
        sh_write("slot: ");
        sh_write(buf);
        if (buf[n - 1] != '\n') sh_putchar('\n');
    }   // n == 0 is fine: ctl/data files have nothing to say on open
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

// dial a remote PULSAR server over TCP and bind it into the namespace
static void builtin_mount(int argc, char **argv) {
 
    if (argc < 3) {
        sh_write("usage: mount [-b|-a] <ip!port> <path>\n");
        sh_write("       -b  bind before (union prepend)\n");
        sh_write("       -a  bind after  (union append)\n");
        sh_write("       default: replace\n");
        sh_write("example: mount 192.168.1.2!564 /remote\n");
        return;
    }
 
    uint8_t     flags    = NS_BIND_REPLACE;
    const char *addr_arg = argv[1];
    const char *path_arg = argv[2];
 
    if (argv[1][0] == '-') {                                                // flag parsing
        if      (argv[1][1] == 'b') flags = NS_BIND_BEFORE;
        else if (argv[1][1] == 'a') flags = NS_BIND_AFTER;
        else {
            sh_write("mount: unknown flag (use -b or -a)\n");
            return;
        }
        if (argc < 4) { sh_write("mount: missing arguments\n"); return; }
        addr_arg = argv[2];
        path_arg = argv[3];
    }
 
    sh_write("mount: dialling ");
    sh_write(addr_arg);
    sh_write(" ...\n");
 
    int rc = dial(addr_arg, path_arg, flags);
    if (rc < 0) {
        switch (rc) {
            case -2: sh_write("mount: network error (TCP connect failed)\n");   break;
            case -3: sh_write("mount: protocol error (PULSAR handshake failed)\n"); break;
            case -4: sh_write("mount: namespace full (unbind something first)\n");  break;
            default: sh_write("mount: failed\n"); break;
        }
    } else {
        sh_write("mount: ");
        sh_write(addr_arg);
        sh_write(" -> ");
        sh_write(path_arg);
        sh_putchar('\n');
    }
}

static void builtin_serve(void) {
    sh_write("serve: PULSAR server is a kernel process (port 564)\n");
    sh_write("serve: use 'cat /net/tcp/0/status' to check state\n");
}

static const char *g_exec_path = 0;             // shared variable between parent & child: set by sh_exec before fork

// runs in child process
static void exec_child(void) {
    signal(SIGINT, SIG_DFL);
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

// scan line for > operator, split line in place, return path to redirect target
static int sh_parse_redirect(char *line, char **redir_out) {

    *redir_out = 0;

    for (char *p = line; *p; p++) {
        if (*p == '>') {
            *p = '\0';                          // terminate command at >
            p++;
            while (*p == ' ') p++;              // skip spaces after >
            if (!*p) return 0;                  // nothing after >, ignore
            *redir_out = p;
            char *end = p + strlen(p) - 1;
            while (end > p && *end == ' ') *end-- = '\0'; // strip trailing spaces
            return 1;
        }
    }
    return 0;
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
    else if (strcmp(argv[0], "write"  ) == 0) builtin_write(argc, argv);
    else if (strcmp(argv[0], "open"   ) == 0) builtin_open(argc, argv);
    else if (strcmp(argv[0], "bind"   ) == 0) builtin_bind(argc, argv);
    else if (strcmp(argv[0], "unbind" ) == 0) builtin_unbind(argc, argv);
    else if (strcmp(argv[0], "nsdump" ) == 0) nsdump();
    else if (strcmp(argv[0], "mount"  ) == 0) builtin_mount(argc, argv);
    else if (strcmp(argv[0], "serve"  ) == 0) builtin_serve();
    else if (strcmp(argv[0], "exit"   ) == 0) {
        int code = (argc >= 2) ? atoi(argv[1]) : 0;
        _exit(code);                                            // terminate shell
    }
    else sh_exec_external(argc, argv);                          // fallback: unknown = /bin/<cmd>
}

static void wizard_host(void) {

    sh_write("\n  Enter a Host Username: ");
    char uname[64];
    sh_readline(uname, sizeof(uname));
    if (uname[0]) save_username(uname);

    sh_write("\n  -- Host Setup --\n");
    sh_write("  Enter IP address   (e.g. 10.0.0.1): ");
    char ip[64];
    sh_readline(ip, sizeof(ip));

    sh_write("  Enter subnet mask  (e.g. 255.255.255.0): ");
    char mask[64];
    sh_readline(mask, sizeof(mask));

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "add %s %s", ip, mask);

    int fd = open("/net/ipifc/0/ctl", O_WRONLY);
    if (fd < 0) { sh_write("  error: cannot open /net/ipifc/0/ctl\n"); return; }
    write(fd, cmd, (uint32_t)strlen(cmd));
    close(fd);

    sh_write("  IP configured.\n");
    sh_write("  PULSAR server running on port 564.\n");
    sh_write("  Clients should dial: ");
    sh_write(ip);
    sh_write("!564\n");
}

static void wizard_client(void) {

    sh_write("\n  Enter a Client Username: ");
    char uname[64];
    sh_readline(uname, sizeof(uname));
    if (uname[0]) save_username(uname);

    sh_write("\n  -- Client Setup --\n");
    sh_write("  Enter your IP address  (e.g. 10.0.0.2): ");
    char ip[64];
    sh_readline(ip, sizeof(ip));

    sh_write("  Enter subnet mask      (e.g. 255.255.255.0): ");
    char mask[64];
    sh_readline(mask, sizeof(mask));

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "add %s %s", ip, mask);

    int fd = open("/net/ipifc/0/ctl", O_WRONLY);
    if (fd < 0) { sh_write("  error: cannot open /net/ipifc/0/ctl\n"); return; }
    write(fd, cmd, (uint32_t)strlen(cmd));
    close(fd);

    sh_write("  IP configured.\n");

    while (1) {
        sh_write("  Host to mount (e.g. 10.0.0.1!564) or blank to finish: ");
        char addr[64];
        sh_readline(addr, sizeof(addr));
        if (addr[0] == '\0') break;

        sh_write("  Mount point (e.g. /remote): ");
        char mpath[64];
        sh_readline(mpath, sizeof(mpath));
        if (mpath[0] == '\0') { sh_write("  skipped.\n"); continue; }

        sh_write("  Mounting ");
        sh_write(addr);
        sh_write(" at ");
        sh_write(mpath);
        sh_write(" ...\n");

        int rc = dial(addr, mpath, NS_BIND_REPLACE);
        if (rc < 0) sh_write("  mount failed.\n");
        else        sh_write("  mounted ok.\n");
    }
}

// entry point
int main(int argc, char **argv) {

    (void)argc;
    (void)argv;

    signal(SIGINT, SIG_IGN);                                    // shell ignores SIGINT: only foreground children die on Ctrl+C
    signal(SIGCHLD, SIG_IGN);                                   // shell ignores SIGCHLD: child exit does not kill the shell

    // ── CONFIGURATION WIZARD ──────────────────────────────────
    sh_write("                                OrionOS Setup\n");
    sh_write("\n");
    sh_write("----------------------------------------\n");
    sh_write("|  [h] Host   - serve your filesystem  |\n");
    sh_write("|  [c] Client - mount a host           |\n");
    sh_write("|  [s] Skip   - manual shell           |\n");
    sh_write("----------------------------------------\n");
    sh_write("\n");
    sh_write("  choice: ");

    char wizard_choice[4];
    sh_readline(wizard_choice, sizeof(wizard_choice));

    if (wizard_choice[0] == 'h' || wizard_choice[0] == 'H') {
        wizard_host();
    } else if (wizard_choice[0] == 'c' || wizard_choice[0] == 'C') {
        wizard_client();
    } else {
        sh_write("  Enter a Username: ");
        char uname[64];
        sh_readline(uname, sizeof(uname));
        if (uname[0]) save_username(uname);
    }

    char home_uname[64];
    load_username(home_uname, sizeof(home_uname));
    if (home_uname[0]) {
        char home[128];
        snprintf(home, sizeof(home), "/%s", home_uname);
        mkdir(home);                                            // create /(username) - ignored if already exists
        chdir(home);                                            // enter home directory
    }
    sh_write("\033[2J\033[H");

    sh_write(
    // "\n"
    // "_______/\\\\\\\\\\___________________________________________________________\n"
    // " _____/\\\\\\///\\\\\\_________________________________________________________\n"
    // "  ___/\\\\\\/__\\///\\\\\\_______________________________________________________\n"
    // "   __/\\\\\\______\\//\\\\\\______________________________________________________\n"
    // "    _\\/\\\\\\_______\\/\\\\\\______________________________________________________\n"
    // "     _\\//\\\\\\______/\\\\\\_______________________________________________________\n"
    // "      __\\///\\\\\\__/\\\\\\_________________________________________________________\n"
    // "       ____\\///\\\\\\\\\\/__________________________________________________________\n"
    // "        ______\\/////____________________________________________________________\n"
    // "\n"

    // "\n"
    // "_______/\\\\\\\\\\___________________________________________________________\n"
    // " _____/\\\\\\///\\\\\\_________________________________________________________\n"
    // "  ___/\\\\\\/__\\///\\\\\\_______________________________________________________\n"
    // "   __/\\\\\\______\\//\\\\\\__/\\\\/\\\\\\\\\\\\\\_________________________________________\n"
    // "    _\\/\\\\\\_______\\/\\\\\\_\\/\\\\\\/////\\\\\\________________________________________\n"
    // "     _\\//\\\\\\______/\\\\\\__\\/\\\\\\___\\///_________________________________________\n"
    // "      __\\///\\\\\\__/\\\\\\____\\/\\\\\\________________________________________________\n"
    // "       ____\\///\\\\\\\\\\/_____\\/\\\\\\________________________________________________\n"
    // "        ______\\/////_______\\///_________________________________________________\n"
    // "\n"

    // "\n"
    // "_______/\\\\\\\\\\___________________________________________________________\n"
    // " _____/\\\\\\///\\\\\\_________________________________________________________\n"
    // "  ___/\\\\\\/__\\///\\\\\\_________________/\\\\\\__________________________________\n"
    // "   __/\\\\\\______\\//\\\\\\__/\\\\/\\\\\\\\\\\\\\__\\///___________________________________\n"
    // "    _\\/\\\\\\_______\\/\\\\\\_\\/\\\\\\/////\\\\\\__/\\\\\\__________________________________\n"
    // "     _\\//\\\\\\______/\\\\\\__\\/\\\\\\___\\///__\\/\\\\\\__________________________________\n"
    // "      __\\///\\\\\\__/\\\\\\____\\/\\\\\\_________\\/\\\\\\__________________________________\n"
    // "       ____\\///\\\\\\\\\\/_____\\/\\\\\\_________\\/\\\\\\__________________________________\n"
    // "        ______\\/////_______\\///__________\\///___________________________________\n"
    // "\n"
    
    // "\n"
    // "_______/\\\\\\\\\\___________________________________________________________\n"
    // " _____/\\\\\\///\\\\\\_________________________________________________________\n"
    // "  ___/\\\\\\/__\\///\\\\\\_________________/\\\\\\__________________________________\n"
    // "   __/\\\\\\______\\//\\\\\\__/\\\\/\\\\\\\\\\\\\\__\\///______/\\\\\\\\\\_______________________\n"
    // "    _\\/\\\\\\_______\\/\\\\\\_\\/\\\\\\/////\\\\\\__/\\\\\\___/\\\\\\///\\\\\\_____________________\n"
    // "     _\\//\\\\\\______/\\\\\\__\\/\\\\\\___\\///__\\/\\\\\\__/\\\\\\__\\//\\\\\\____________________\n"
    // "      __\\///\\\\\\__/\\\\\\____\\/\\\\\\_________\\/\\\\\\_\\//\\\\\\__/\\\\\\_____________________\n"
    // "       ____\\///\\\\\\\\\\/_____\\/\\\\\\_________\\/\\\\\\__\\///\\\\\\\\\\/______________________\n"
    // "        ______\\/////_______\\///__________\\///_____\\/////________________________\n"
    // "\n"

    "\n"
    "_______/\\\\\\\\\\___________________________________________________________\n"
    " _____/\\\\\\///\\\\\\_________________________________________________________\n"
    "  ___/\\\\\\/__\\///\\\\\\___________________/\\\\\\________________________________\n"
    "   __/\\\\\\______\\//\\\\\\___/\\\\/\\\\\\\\\\\\\\___\\///_______/\\\\\\\\\\______/\\\\/\\\\\\\\\\\\____\n"
    "    _\\/\\\\\\_______\\/\\\\\\__\\/\\\\\\/////\\\\\\___/\\\\\\____/\\\\\\///\\\\\\___\\/\\\\\\////\\\\\\___\n"
    "     _\\//\\\\\\______/\\\\\\___\\/\\\\\\___\\///___\\/\\\\\\___/\\\\\\__\\//\\\\\\__\\/\\\\\\__\\//\\\\\\__\n"
    "      __\\///\\\\\\__/\\\\\\_____\\/\\\\\\__________\\/\\\\\\__\\//\\\\\\__/\\\\\\___\\/\\\\\\___\\/\\\\\\__\n"
    "       ____\\///\\\\\\\\\\/______\\/\\\\\\__________\\/\\\\\\___\\///\\\\\\\\\\/____\\/\\\\\\___\\/\\\\\\__\n"
    "        ______\\/////________\\///___________\\///______\\/////______\\///____\\///___\n"
    "\n"
    );

    sh_write("\nOrion Shell [ring-3] - type 'help' for commands\n");

    char  line[SH_LINE_MAX];
    char *argv_buf[SH_ARGV_MAX];

    while (1) {
        sh_print_prompt();
        sh_readline(line, SH_LINE_MAX);

        char *redir_path = 0;
        sh_parse_redirect(line, &redir_path);       // split off > target before tokenising

        int ac = sh_tokenise(line, argv_buf, SH_ARGV_MAX);
        if (ac == 0) continue;

        int saved_stdout = -1;
        if (redir_path) {
            int rfd = open(redir_path, O_WRONLY | O_CREAT | O_TRUNC);
            if (rfd < 0) {
                sh_write("sh: cannot open redirect: ");
                sh_write(redir_path);
                sh_putchar('\n');
                continue;
            }
            saved_stdout = dup2(1, 15);             // save stdout to slot 15
            dup2(rfd, 1);                           // point stdout at file
            close(rfd);
        }

        sh_dispatch(ac, argv_buf);

        if (saved_stdout >= 0) {
            dup2(saved_stdout, 1);                  // restore stdout
            close(saved_stdout);
            saved_stdout = -1;
        }
    }
    return 0;
}

 