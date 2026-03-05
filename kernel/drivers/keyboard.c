#include "keyboard.h"
#include "ioport.h"
#include "irq.h"

#include "serial.h"

// US QWERTY scancode (UNSHIFTED)
static const char scancode_normal[128] = {

    0,                                                                          // 0x00 — unused
    0,                                                                          // 0x01 — Escape
    '1','2','3','4','5','6','7','8','9','0','-','=',
    '\b',                                                                       // 0x0E — Backspace
    '\t',                                                                       // 0x0F — Tab
    'q','w','e','r','t','y','u','i','o','p','[',']',
    '\n',                                                                       // 0x1C — Enter
    0,                                                                          // 0x1D — Left Ctrl
    'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,                                                                          // 0x2A — Left Shift
    '\\',
    'z','x','c','v','b','n','m',',','.','/',
    0,                                                                          // 0x36 — Right Shift
    '*',                                                                        // 0x37 — Keypad *
    0,                                                                          // 0x38 — Left Alt
    ' ',                                                                        // 0x39 — Space
    0,                                                                          // 0x3A — Caps Lock
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,    // F1–F10, Num Lock, Scroll Lock, Home, Up, Page Up, Keypad -, Left,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0                                                 // Keypad 5, Right, Keypad +, End, Down, Page Down, Insert, Delete — all 0

};

// US QWERTY scancode (SHIFTED)
static const char scancode_shifted[128] = {

    0,
    0,                                                                          // 0x01 — Escape
    '!','@','#','$','%','^','&','*','(',')','_','+',
    '\b',                                                                       // 0x0E — Backspace
    '\t',                                                                       // 0x0F — Tab
    'Q','W','E','R','T','Y','U','I','O','P','{','}',
    '\n',                                                                       // 0x1C — Enter
    0,                                                                          // 0x1D — Left Ctrl
    'A','S','D','F','G','H','J','K','L',':','"','~',
    0,                                                                          // 0x2A — Left Shift
    '|',
    'Z','X','C','V','B','N','M','<','>','?',
    0,                                                                          // 0x36 — Right Shift
    '*',
    0,                                                                          // 0x38 — Left Alt
    ' ',                                                                        // 0x39 — Space
    0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0

};

static char     kb_buf[KB_BUFFER_SIZE];
static uint32_t kb_read  = 0;
static uint32_t kb_write = 0;

static inline void buf_push(char c) {

    uint32_t next = (kb_write + 1) & (KB_BUFFER_SIZE - 1);                      // power of 2 wrap ((index + 1) % SIZE)
    if (next != kb_read) {                                                      // drop character silently if buffer full
        kb_buf[kb_write] = c;
        kb_write = next;
    }

}

// return next character or 0 if empty
char keyboard_getchar(void) {

    if (kb_read == kb_write) return 0;                                          // empty = return 0
    char c = kb_buf[kb_read];
    kb_read = (kb_read + 1) & (KB_BUFFER_SIZE - 1);
    return c;

}

// return true if buffer != empty
int keyboard_has_char(void) {
    return kb_read != kb_write;
}

static int shift_held  = 0;                                                     // shift key
static int caps_lock   = 0;                                                     // caps lock toggle

void keyboard_handler(regs_t *r) {

    (void)r;

    uint8_t scancode = inb(KB_DATA_PORT);                                       // read scancode

    // bit 7 [ 0 = key pressed | 1 = key released]
    int released = scancode & 0x80;
    uint8_t key  = scancode & 0x7F;

    if (key == 0x2A || key == 0x36) {                                           // left / right shift
        shift_held = !released;
        return;
    }

    if (key == 0x3A && !released) {                                             // caps lock toggle on key-down only
        caps_lock = !caps_lock;
        return;
    }

    if (released) return;                                                       // only process key-down events from here on

    if (key >= 128) return;                                                     // ignore extended scancodes

    // translate to ascii
    char c = shift_held ? scancode_shifted[key] : scancode_normal[key];         // choose the correct map -> flip case for alpha keys if [ Caps Lock = ON ]

    if (caps_lock && c >= 'a' && c <= 'z') c -= 32;                             // caps lock flips for alphabetic characters only
    if (caps_lock && c >= 'A' && c <= 'Z') c += 32;

    if (c) {                                                                    // if printable
        buf_push(c);                                                            // push char to ring buffer
        serial_putchar(c);                                                      // send to serial for debugging
    }

}

void keyboard_init(void) {

    irq_install_handler(1, keyboard_handler);                                   // install IRQ handler (connect IRQ1 -> keyboard handler)
    serial_write("Keyboard: PS/2 driver initialized\n");

}