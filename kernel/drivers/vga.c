// drivers/vga.c — VGA text mode (80x25) driver

#include "vga.h"
#include "string.h"

#define VGA_WIDTH   80
#define VGA_HEIGHT  25
#define VGA_MEMORY  0xB8000

static size_t    terminal_row;
static size_t    terminal_col;
static uint8_t   terminal_colour;
static uint16_t *terminal_buf = (uint16_t *)VGA_MEMORY;

static inline uint8_t  vga_colour(vga_colour_t fg, vga_colour_t bg) { return fg | bg << 4; }
static inline uint16_t vga_entry (unsigned char c, uint8_t colour)  { return (uint16_t)c | (uint16_t)colour << 8; }

void terminal_initialize(void) {
    terminal_row    = 0;
    terminal_col    = 0;
    terminal_colour = vga_colour(VGA_COLOUR_LIGHT_GREY, VGA_COLOUR_BLACK);
    for (size_t y = 0; y < VGA_HEIGHT; y++)
        for (size_t x = 0; x < VGA_WIDTH; x++)
            terminal_buf[y * VGA_WIDTH + x] = vga_entry(' ', terminal_colour);
}

void terminal_setcolour(uint8_t colour) {
    terminal_colour = colour;
}

static void terminal_putentryat(char c, uint8_t colour, size_t x, size_t y) {
    terminal_buf[y * VGA_WIDTH + x] = vga_entry(c, colour);
}

void terminal_putchar(char c) {
    terminal_putentryat(c, terminal_colour, terminal_col, terminal_row);
    if (++terminal_col == VGA_WIDTH) {
        terminal_col = 0;
        if (++terminal_row == VGA_HEIGHT)
            terminal_row = 0;
    }
}

void terminal_write(const char *data, size_t size) {
    for (size_t i = 0; i < size; i++)
        terminal_putchar(data[i]);
}

void terminal_writestring(const char *data) {
    terminal_write(data, strlen(data));
}
