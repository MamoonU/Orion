// chatfs.c - /chat synthetic filesystem
// planets      = chat rooms
// satellites   = connected users

#include "chatfs.h"
#include "vfs.h"
#include "kheap.h"
#include "ramfs.h"
#include "kprintf.h"
#include "string.h"

#define CHAT_MAX_ROOMS    8         // max simultaneous planets
#define CHAT_MAX_MEMBERS  16        // max satellites per planet
#define CHAT_LOG_SIZE     8192      // message history buffer per room (bytes)
#define CHAT_NAME_MAX     32        // max planet/username length

// chat_room_t structure: represents one planet
typedef struct {
    char     name[CHAT_NAME_MAX];               // planet name
    char     log[CHAT_LOG_SIZE];                // flat append-only message log
    uint32_t log_len;                           // bytes written into log so far
    char     members[CHAT_MAX_MEMBERS][64];     // active satellite usernames
    uint32_t nmembers;                          // current satellite count
    int      active;                            // 1 = planet exists
}chat_room_t;

static chat_room_t rooms[CHAT_MAX_ROOMS];       // global planet table (host kernel memory)

// append a string to a room's log (hard stops at CHAT_LOG_SIZE)
static void log_append(chat_room_t *r, const char *s) {

    while (*s && r->log_len < CHAT_LOG_SIZE - 1) {              // append raw text -> room log buffer
        r->log[r->log_len++] = *s++;
    }

}

// append two strings and a newline (avoids needing snprintf in kernel)
static void log_msg(chat_room_t *r, const char *a, const char *b, const char *c, const char *d) {

    if (a) log_append(r, a);                                    // build message
    if (b) log_append(r, b);                                    // kernel safe string concatenation
    if (c) log_append(r, c);
    if (d) log_append(r, d);
    log_append(r, "\n");

}

// find active room by name
static chat_room_t *room_find(const char *name) {

    for (int i = 0; i < CHAT_MAX_ROOMS; i++) {                          // loop room slots
        if (rooms[i].active && strcmp(rooms[i].name, name) == 0) {      // active + name match
            return &rooms[i];
        }
    }
    return 0;

}

// find/create room
static chat_room_t *room_get_or_create(const char *name) {

    chat_room_t *r = room_find(name);                           // attempt find
    if (r) return r;

    for (int i = 0; i < CHAT_MAX_ROOMS; i++) {                  // find free slot
        if (!rooms[i].active) {
            memset(&rooms[i], 0, sizeof(chat_room_t));          // clear

            strncpy(rooms[i].name, name, CHAT_NAME_MAX - 1);    // initialise room
            rooms[i].active = 1;

            kprintf("CHATFS: planet \"%s\" created\n", name);
            return &rooms[i];                                   // return room
        }
    }
    kprintf("CHATFS: planet table full\n");
    return 0;

}

// vnode tag ftypes
#define CHAT_FILE_LOG  0    // read-only message history
#define CHAT_FILE_CTL  1    // write: join/leave/msg commands

// file tag structure
typedef struct {
    char    room_name[CHAT_NAME_MAX];
    uint8_t ftype;
} chatfs_file_tag_t;

// directory tag structure 
typedef struct {
    char room_name[CHAT_NAME_MAX];
} chatfs_dir_tag_t;

// /chat/planet/log read: returns history from offset
static int chatfs_log_read(vnode_t *v, void *buf, uint32_t len, uint32_t off) {

    chatfs_file_tag_t *tag = (chatfs_file_tag_t *)v->data;              // get room
    chat_room_t *r = room_find(tag->room_name);

    if (!r || off >= r->log_len) return 0;                              // bounds check

    uint32_t avail = r->log_len - off;                                  // calculate bytes to read
    uint32_t n = (len < avail) ? len : avail;

    memcpy(buf, r->log + off, n);                                       // copy data
    return (int)n;                                                      // return bytes read

}

// /chat/planet/ctl read: returns active member list
static int chatfs_ctl_read(vnode_t *v, void *buf, uint32_t len, uint32_t off) {

    chatfs_file_tag_t *tag = (chatfs_file_tag_t *)v->data;                  // get room
    chat_room_t *r = room_find(tag->room_name);
    if (!r) return 0;

    char out[1024];                                                         // build output string
    uint32_t pos = 0;

    for (uint32_t i = 0; i < r->nmembers && pos < sizeof(out) - 1; i++) {   // append each member
        const char *m = r->members[i];
        while (*m && pos < sizeof(out) - 1) {                               // for each username
            out[pos++] = *m++;
        }
        out[pos++] = '\n';
    }
    out[pos] = '\0';

    if (off >= pos) return 0;                                               // offset-aware read
    uint32_t avail = pos - off;
    uint32_t n = (len < avail) ? len : avail;
    memcpy(buf, out + off, n);                                              // copy slice
    return (int)n;

}

// /chat/planet/ctl write — commands:
//   "join username"        -> add satellite, log join message
//   "leave username"       -> remove satellite, log leave message
//   "msg username:message" -> log formatted message
static int chatfs_ctl_write(vnode_t *v, const void *buf, uint32_t len, uint32_t off) {

    (void)off;
    chatfs_file_tag_t *tag = (chatfs_file_tag_t *)v->data;

    char cmd[256];
    uint32_t clen = (len < sizeof(cmd) - 1) ? len : sizeof(cmd) - 1;
    memcpy(cmd, buf, clen);                                                         // copy user input
    cmd[clen] = '\0';
    int cl = (int)strlen(cmd);
    while (cl > 0 && (cmd[cl-1] == '\n' || cmd[cl-1] == '\r')) cmd[--cl] = '\0';    // trim new line

    chat_room_t *r = room_get_or_create(tag->room_name);                            // ensure room exists
    if (!r) return -1;

    if (strncmp(cmd, "join ", 5) == 0) {                                            // CASE 1: join

        const char *uname = cmd + 5;
        int found = 0;

        for (uint32_t i = 0; i < r->nmembers; i++) {                                // check duplicate
            if (strcmp(r->members[i], uname) == 0) { found = 1; break; }
        }

        if (!found && r->nmembers < CHAT_MAX_MEMBERS) {                             // add user
            strncpy(r->members[r->nmembers++], uname, 63);
        }

        log_msg(r, "Satellite ", uname, " deployed to ", r->name);                  // append join msg
        log_append(r, "'s orbit\n");
        kprintf("CHATFS: satellite \"%s\" joined \"%s\"\n", uname, r->name);

    } else if (strncmp(cmd, "leave ", 6) == 0) {                                    // CASE 2: leave

        const char *uname = cmd + 6;

        for (uint32_t i = 0; i < r->nmembers; i++) {                                // remove logic
            if (strcmp(r->members[i], uname) == 0) {
                for (uint32_t j = i; j + 1 < r->nmembers; j++) {
                    memcpy(r->members[j], r->members[j+1], 64);
                }
                r->nmembers--;
                break;
            }
        }

        log_msg(r, "Satellite ", uname, " recalled to Base-Station: Orbit terminated", 0);  // log message
        kprintf("CHATFS: satellite \"%s\" left \"%s\"\n", uname, r->name);

    } else if (strncmp(cmd, "msg ", 4) == 0) {                                      // CASE 3: message

        const char *rest = cmd + 4;
        const char *p = rest;
        while (*p && *p != ':') p++;                                                // find separator

        if (*p == ':') {
            char uname[64];
            uint32_t ulen = (uint32_t)(p - rest);
            if (ulen >= sizeof(uname)) ulen = sizeof(uname) - 1;
            memcpy(uname, rest, ulen);                                              // extract username
            uname[ulen] = '\0';
            log_msg(r, uname, ": ", p + 1, 0);                                      // "username: message"
        }

    } else {

        kprintf("CHATFS: unknown ctl command: %s\n", cmd);

    }
    return (int)len;
}

// /chat log and ctl operations
static vfs_ops_t chatfs_log_ops = { .read = chatfs_log_read };
static vfs_ops_t chatfs_ctl_ops = { .read = chatfs_ctl_read, .write = chatfs_ctl_write };

// room directory lookup: maps log and ctl
static int chatdir_lookup(vnode_t *dir, const char *name, vnode_t **out) {

    chatfs_dir_tag_t *dt = (chatfs_dir_tag_t *)dir->data;

    vfs_ops_t *ops;
    uint8_t    ftype;

    if      (strcmp(name, "log") == 0) { ftype = CHAT_FILE_LOG; ops = &chatfs_log_ops; }            // identify file type
    else if (strcmp(name, "ctl") == 0) { ftype = CHAT_FILE_CTL; ops = &chatfs_ctl_ops; }
    else return -1;

    room_get_or_create(dt->room_name);                                                              // ensure room exists on first access

    chatfs_file_tag_t *tag = (chatfs_file_tag_t *)kmalloc(sizeof(chatfs_file_tag_t));               // create tag
    if (!tag) return -1;
    strncpy(tag->room_name, dt->room_name, CHAT_NAME_MAX - 1);
    tag->ftype = ftype;

    vnode_t *fv = vnode_alloc(VNODE_DEV, ops, tag);                                                 // create vnode
    if (!fv) { kfree(tag); return -1; }
    *out = fv;
    return 0;

}

// chat directory files
static const char *chatdir_files[] = { "log", "ctl" };
#define CHATDIR_FILE_COUNT 2

// read directories: list log and ctl
static int chatdir_readdir(vnode_t *dir, uint32_t index, char *name_out, vnode_t **node_out) {

    if (index >= CHATDIR_FILE_COUNT) return -1;
    if (name_out) strncpy(name_out, chatdir_files[index], VFS_NAME_MAX - 1);
    if (node_out) chatdir_lookup(dir, chatdir_files[index], node_out);
    return 0;

}

// chat directory operations
static vfs_ops_t chatdir_ops = { .lookup = chatdir_lookup, .readdir = chatdir_readdir };

// make room directory: create vnode for room directory
static vnode_t *make_room_dir(const char *room_name) {

    chatfs_dir_tag_t *tag = (chatfs_dir_tag_t *)kmalloc(sizeof(chatfs_dir_tag_t));      // allocate tag
    if (!tag) return 0;

    strncpy(tag->room_name, room_name, CHAT_NAME_MAX - 1);                              // store room name

    vnode_t *v = vnode_alloc(VNODE_DIR, &chatdir_ops, tag);                             // create vnode
    if (!v) { kfree(tag); return 0; }
    return v;

}

// /chat root lookup: any name creates/finds a planet
static int chatroot_lookup(vnode_t *dir, const char *name, vnode_t **out) {

    (void)dir;
    vnode_t *v = make_room_dir(name);
    if (!v) return -1;
    *out = v;
    return 0;

}

// /chat root readdir: lists all active planets
static int chatroot_readdir(vnode_t *dir, uint32_t index, char *name_out, vnode_t **node_out) {

    (void)dir;
    uint32_t found = 0;

    for (int i = 0; i < CHAT_MAX_ROOMS; i++) {
        if (!rooms[i].active) continue;                                         // skip inactive
        if (found == index) {                                                   // return nth active
            if (name_out) strncpy(name_out, rooms[i].name, VFS_NAME_MAX - 1);
            if (node_out) *node_out = make_room_dir(rooms[i].name);
            return 0;
        }
        found++;
    }
    return -1;

}

// chat root operations
static vfs_ops_t chatroot_ops = { .lookup = chatroot_lookup, .readdir = chatroot_readdir };

// initialise chat filesystem
void chatfs_init(void) {

    kprintf("CHATFS: Initialising\n");
    memset(rooms, 0, sizeof(rooms));                                        // clear state

    vnode_t *root = vnode_alloc(VNODE_DIR, &chatroot_ops, 0);               // create root vnode
    if (!root) {
        kprintf("CHATFS: FATAL - could not allocate root vnode\n");
        return;
    }

    if (vfs_mkdir("/chat") < 0) {                                           // create /chat
        kprintf("CHATFS: FATAL - could not create /chat\n");
        return;
    }

    if (ramfs_register_dev("/chat", root) < 0) {                            // mount /chat
        kprintf("CHATFS: FATAL - could not register /chat\n");
        return;
    }

    kprintf("CHATFS: /chat ready (%d planets max, %d satellites each)\n", CHAT_MAX_ROOMS, CHAT_MAX_MEMBERS);

}









