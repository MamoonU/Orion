// drivers/vga.c — VGA text mode (80x25) driver

#include "vga.h"
#include "string.h"
#include "kprintf.h"
#include "ioport.h"

#define VGA_WIDTH   80
#define VGA_HEIGHT  25
#define VGA_MEMORY  0xB8000

// VGA CRT controller ports
#define VGA_CRTC_ADDR   0x3D4
#define VGA_CRTC_DATA   0x3D5
#define VGA_CURSOR_HI   0x0E
#define VGA_CURSOR_LO   0x0F

static size_t    terminal_row;
static size_t    terminal_col;
static uint8_t   terminal_colour;
static uint16_t *terminal_buf = (uint16_t *)VGA_MEMORY;

static inline uint8_t  vga_colour(vga_colour_t fg, vga_colour_t bg) { return fg | bg << 4; }
static inline uint16_t vga_entry (unsigned char c, uint8_t colour)  { return (uint16_t)c | (uint16_t)colour << 8; }

static void cursor_sync(void) {
    uint16_t pos = (uint16_t)(terminal_row * VGA_WIDTH + terminal_col);
    outb(VGA_CRTC_ADDR, VGA_CURSOR_HI); outb(VGA_CRTC_DATA, (uint8_t)(pos >> 8));
    outb(VGA_CRTC_ADDR, VGA_CURSOR_LO); outb(VGA_CRTC_DATA, (uint8_t)(pos & 0xFF));
}

void terminal_init(void) {
    terminal_row    = 0;
    terminal_col    = 0;
    terminal_colour = vga_colour(VGA_COLOUR_LIGHT_GREY, VGA_COLOUR_BLACK);
    for (size_t y = 0; y < VGA_HEIGHT; y++)
        for (size_t x = 0; x < VGA_WIDTH; x++)
            terminal_buf[y * VGA_WIDTH + x] = vga_entry(' ', terminal_colour);
    cursor_sync();
    kprintf("Terminal: Initialised\n");
}

void terminal_setcolour(uint8_t colour) {
    terminal_colour = colour;
}

static void terminal_putentryat(char c, uint8_t colour, size_t x, size_t y) {
    terminal_buf[y * VGA_WIDTH + x] = vga_entry(c, colour);
}

static void terminal_scroll(void) {
    for (size_t y = 0; y < VGA_HEIGHT - 1; y++)
        for (size_t x = 0; x < VGA_WIDTH; x++)
            terminal_buf[y * VGA_WIDTH + x] = terminal_buf[(y + 1) * VGA_WIDTH + x];
 
    for (size_t x = 0; x < VGA_WIDTH; x++)
        terminal_buf[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = vga_entry(' ', terminal_colour);
}
 
static void terminal_newline(void) {
    terminal_col = 0;
    if (++terminal_row == VGA_HEIGHT) {
        terminal_scroll();
        terminal_row = VGA_HEIGHT - 1;
    }
}
 
void terminal_putchar(char c) {
    switch (c) {
        case '\n': terminal_newline();                         break;
        case '\r': terminal_col = 0;                          break;
        case '\t': terminal_col = (terminal_col + 8) & ~7u;
                   if (terminal_col >= VGA_WIDTH) terminal_newline();
                   break;
        case '\b': if (terminal_col > 0) terminal_col--;      break;
        default:
            terminal_putentryat(c, terminal_colour, terminal_col, terminal_row);
            if (++terminal_col == VGA_WIDTH)
                terminal_newline();
            break;
    }
    cursor_sync();
}

void terminal_write(const char *data, size_t size) {
    for (size_t i = 0; i < size; i++)
        terminal_putchar(data[i]);
}

void terminal_writestring(const char *data) {
    terminal_write(data, strlen(data));
}

void terminal_clear(void) {
    for (size_t y = 0; y < VGA_HEIGHT; y++)
        for (size_t x = 0; x < VGA_WIDTH; x++)
            terminal_buf[y * VGA_WIDTH + x] = vga_entry(' ', terminal_colour);
    terminal_row = 0;
    terminal_col = 0;
    cursor_sync();
}