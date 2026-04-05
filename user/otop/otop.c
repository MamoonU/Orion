// user/top/0top.c - Orion process monitor

#include "orion.h"

#define TOP_BUF  512    // status read buffer size
#define REFRESH  100    // ticks between refreshes (100Hz = 1 second)

// write string -> stdout
static void tw(const char *s) {
    if (s) write(STDOUT_FILENO, s, (uint32_t)strlen(s));
}

// write one character
static void tc(char c) {
    write(STDOUT_FILENO, &c, 1);
}

// uint32 -> decimal string
static int u32s(uint32_t v, char *buf, int bufsz) {

    if (bufsz <= 0) return 0;
    if (v == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return 1;
    }

    char tmp[12];                                   // build string backwards
    int i = 11;
    tmp[11] = '\0';

    while (v && i > 0) {                            // copy to output buffer
        tmp[--i] = (char)('0' + v % 10); v /= 10;
    
    }

    int len = 11 - i;
    if (len >= bufsz) len = bufsz - 1;
    memcpy(buf, tmp + i, (uint32_t)len);
    buf[len] = '\0';
    return len;
}

// wrapper: convert -> print
static void tw_u32(uint32_t v) {
    char tmp[12];
    u32s(v, tmp, sizeof(tmp));
    tw(tmp);
}

// write string + padding
static void tw_col(const char *s, int width) {
    int len = (int)strlen(s);
    tw(s);
    for (int i = len; i < width; i++) tc(' ');
}

// parsing helper: string -> number
static uint32_t parse_u32(const char *s) {
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (uint32_t)(*s++ - '0');
    }
    return v;
}

// find "key: value\n" in buf -> copy value into out
static int find_field(const char *buf, const char *key, char *out, int outsz) {

    int klen = (int)strlen(key);
    const char *p = buf;

    while (*p) {                                                        // scan line by line

        if (strncmp(p, key, (uint32_t)klen) == 0 &&                     // match key

            p[klen] == ':' && p[klen + 1] == ' ') {                     // verify format
            const char *v = p + klen + 2;
            int i = 0;
            while (*v && *v != '\n' && i < outsz - 1) out[i++] = *v++;  // copy value
            out[i] = '\0';
            return 1;
        }
        while (*p && *p != '\n') p++;   // advance to next line
        if (*p) p++;
    }
    return 0;
}

// open + read file into buf
static int read_file(const char *path, char *buf, int bufsz) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int n = read(fd, buf, (uint32_t)(bufsz - 1));
    close(fd);
    if (n > 0) buf[n] = '\0';
    return n;
}

// parse /proc/uptime -> total timer ticks (100Hz)
static uint32_t get_uptime_ticks(void) {
    char buf[64];
    if (read_file("/proc/uptime", buf, sizeof(buf)) <= 0) return 1;
    uint32_t secs = parse_u32(buf);
    const char *dot = strchr(buf, '.');
    uint32_t frac  = dot ? parse_u32(dot + 1) : 0;  // centiseconds
    return secs * 100 + frac;
}

// per-process snapshot
typedef struct {
    uint32_t pid;           // process id
    char     name[32];      // executable name
    char     state[16];     // running/sleeping etc
    uint32_t priority;      // scheduler priority
    uint32_t ticks_cpu;     // p->ticks_total: CPU ticks consumed
    uint32_t heap_top;      // p->heap_top: top of user heap (memory usage indicator)
} proc_info_t;

// populate proc_info_t from /proc/<pid_str>/status and /proc/<pid_str>/mem
static int read_proc_info(const char *pid_str, proc_info_t *info) {
    char path[64];
    char buf[TOP_BUF];
    char tmp[32];

    snprintf(path, sizeof(path), "/proc/%s/status", pid_str);                               // read status
    if (read_file(path, buf, sizeof(buf)) <= 0) return -1;

    if (!find_field(buf, "pid", tmp, sizeof(tmp))) return -1;                               // extract fields
    info->pid = parse_u32(tmp);

    if (!find_field(buf, "name",  info->name,  sizeof(info->name)))  return -1;
    if (!find_field(buf, "state", info->state, sizeof(info->state))) return -1;

    if (find_field(buf, "priority", tmp, sizeof(tmp))) info->priority  = parse_u32(tmp);    // optional fields
    if (find_field(buf, "ticks",    tmp, sizeof(tmp))) info->ticks_cpu = parse_u32(tmp);

    snprintf(path, sizeof(path), "/proc/%s/mem", pid_str);                                  // read memory
    if (read_file(path, buf, sizeof(buf)) > 0) {
        if (find_field(buf, "heap_top", tmp, sizeof(tmp))) {                                // extract heap 
            info->heap_top = parse_u32(tmp);
        }
    }
    return 0;
}

int main(void) {

    while (1) {

        uint32_t uptime = get_uptime_ticks();                                           // get uptime

        tw("\033[2J\033[H");                                                            // clear screen + home cursor

        tw("Orion top  uptime: ");                                                      // header bar
        tw_u32(uptime / 100);
        tw("s\n\n");

        tw_col("PID",   5);                                                             // column headers
        tw_col("NAME",  17);
        tw_col("STATE", 10);
        tw_col("PRI",   5);
        tw_col("CPU%",  7);
        tw("HEAP\n");
        tw("---- ---------------- --------- ---- ------ ----------\n");

        int proc_fd = open("/proc", O_RDONLY);                                          // open /proc directory
        if (proc_fd < 0) {
            tw("top: cannot open /proc\n");
            sleep(REFRESH);
            continue;
        }

        char     entry[16];
        uint32_t idx = 0;

        while (readdir(proc_fd, idx, entry, sizeof(entry)) == 0) {

            idx++;
            if (entry[0] < '0' || entry[0] > '9') continue;                             // skip "uptime"

            proc_info_t info;
            memset(&info, 0, sizeof(info));

            if (read_proc_info(entry, &info) < 0) continue;                             // read infoS

            uint32_t cpu_pct = (uptime > 0) ? (info.ticks_cpu * 100) / uptime : 0;      // lifetime average CPU%: ticks_cpu / uptime * 100
            if (cpu_pct > 100) cpu_pct = 100;                                           // clamp

            char tmp[16];                                                               // print rows

            u32s(info.pid, tmp, sizeof(tmp));                                           // pid
            tw_col(tmp, 5);
            tw_col(info.name,  17);                                                     // name
            tw_col(info.state, 10);                                                     // state
            u32s(info.priority, tmp, sizeof(tmp));                                      // priority
            tw_col(tmp, 5);

            u32s(cpu_pct, tmp, sizeof(tmp));                                            // cpu%: number + '%' padded to 7
            tw(tmp); tc('%');
            int cpulen = (int)strlen(tmp) + 1;
            for (int i = cpulen; i < 7; i++) tc(' ');

            if (info.heap_top == 0) {                                                   // heap: show raw VA in hex for kernel procs (heap=0), KB for user procs
                tw("-\n");
            } else {
                u32s(info.heap_top / 1024, tmp, sizeof(tmp));
                tw(tmp); tw(" KB\n");
            }
        }

        close(proc_fd);
        sleep(REFRESH);
    }

    return 0;
}