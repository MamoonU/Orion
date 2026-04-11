// user/orbit/orbit.c — orbit: Orion IRC client

#include "orion.h"

#define ORBIT_BUF 256
#define ORBIT_PATH 128

static char g_planet[32];               // current chat-room/planet name
static char g_uname[64];                // current username
static int  g_log_fd = -1;              // /chat/planet/log file descriptor
static int  g_ctl_fd = -1;              // /chat/planet/ctl file descriptor

// helper function: write wrapper
static void ow(const char *s) {
    write(STDOUT_FILENO, s, (uint32_t)strlen(s));
}

// child process: message reciever (tails log, printing new bytes as they arrive)
static void tail_child(void) {
    char buf[ORBIT_BUF];
    while (1) {
        int n = read(g_log_fd, buf, sizeof(buf) - 1);
        if (n > 0) write(STDOUT_FILENO, buf, (uint32_t)n);
        else if (n < 0) _exit(0);
        else        sleep(10);      // don't busy-wait when log is empty
    }
}

// full orbit lifecycle
int main(int argc, char **argv) {

    if (argc < 2) {                                                                     // args validation
        ow("usage: orbit <planet>\n");
        return 1;
    }

    g_uname[0] = '\0';
    int ufd = open("/etc/username", O_RDONLY);                                          // load username

    if (ufd >= 0) {
        int n = read(ufd, g_uname, sizeof(g_uname) - 1);
        close(ufd);
        if (n > 0) {
            g_uname[n] = '\0';
            int l = n;
            while (l > 0 && (g_uname[l-1] == '\n' || g_uname[l-1] == '\r'))
                g_uname[--l] = '\0';
        }
    }

    if (!g_uname[0]) strncpy(g_uname, "anonymous", sizeof(g_uname) - 1); 

    strncpy(g_planet, argv[1], sizeof(g_planet) - 1);                                   // store planet name

    char ctl_path[ORBIT_PATH], log_path[ORBIT_PATH];                                    // build filesystem paths
    snprintf(ctl_path, sizeof(ctl_path), "/chat/%s/ctl", g_planet);
    snprintf(log_path, sizeof(log_path), "/chat/%s/log", g_planet);

    g_ctl_fd = open(ctl_path, O_WRONLY | O_CREAT);                                      // open ctl (create room if first access)
    if (g_ctl_fd < 0) g_ctl_fd = open(ctl_path, O_WRONLY | O_CREAT);
    if (g_ctl_fd < 0) { ow("orbit: cannot open ctl\n"); return 1; }

    g_log_fd = open(log_path, O_RDONLY);                                                // open log
    if (g_log_fd < 0) { ow("orbit: cannot open log\n"); close(g_ctl_fd); return 1; }

    char cmd[ORBIT_BUF];
    snprintf(cmd, sizeof(cmd), "join %s\n", g_uname);                                   // join room
    write(g_ctl_fd, cmd, (uint32_t)strlen(cmd));

    ow("\033[2J\033[H");                                                                // clear screen
    ow("orbit: /chat/"); ow(g_planet);
    ow(" - empty line to leave\n\n");

    int child = fork((uint32_t)tail_child);                                             // fork tail child process: prints incoming log entries
    if (child < 0) { ow("orbit: fork failed\n"); return 1; }

    while (1) {                                                                         // parent loop: read stdin, send messages

        ow(g_uname); ow(": ");                                                          // prompt = username: (message)

        char line[ORBIT_BUF];
        uint32_t len = 0;
        char c;

        while (len < sizeof(line) - 1) {
            int n = read(STDIN_FILENO, &c, 1);                                          // character by character read
            if (n <= 0) goto leave;
            if (c == '\n' || c == '\r') break;                                          // enter detection
            if ((c == '\b' || c == 127) && len > 0) { len--; continue; }                // backspace detection
            if (c >= 0x20 && c <= 0x7E) line[len++] = c;                                // printable only detection
        }

        line[len] = '\0';                                                               // end of line
        write(STDOUT_FILENO, "\n", 1);

        if (len == 0) break;                                                            // empty line = leave

        snprintf(cmd, sizeof(cmd), "msg %s:%s\n", g_uname, line);                       // send message
        write(g_ctl_fd, cmd, (uint32_t)strlen(cmd));
    }

leave:                                                      // exit logic

    kill(child, SIGKILL);                                   // kill child: stop log reader process

    snprintf(cmd, sizeof(cmd), "leave %s\n", g_uname);      // send leave command
    write(g_ctl_fd, cmd, (uint32_t)strlen(cmd));

    close(g_ctl_fd);                                        // cleanup
    close(g_log_fd);

    ow("orbit: left "); ow(g_planet); ow("\n");             // final message
    return 0;
}








