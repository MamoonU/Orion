// chatfs.c - /chat synthetic filesystem
// planets      = chat rooms
// satellites   = connected users

#include "chatfs.h"
#include "vfs.h"
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














