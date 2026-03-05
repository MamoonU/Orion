#ifndef VGA_H
#define VGA_H

#include <stdint.h>
#include <stddef.h>

typedef enum {
    VGA_COLOUR_BLACK         = 0,
    VGA_COLOUR_BLUE          = 1,
    VGA_COLOUR_GREEN         = 2,
    VGA_COLOUR_CYAN          = 3,
    VGA_COLOUR_RED           = 4,
    VGA_COLOUR_MAGENTA       = 5,
    VGA_COLOUR_BROWN         = 6,
    VGA_COLOUR_LIGHT_GREY    = 7,
    VGA_COLOUR_DARK_GREY     = 8,
    VGA_COLOUR_LIGHT_BLUE    = 9,
    VGA_COLOUR_LIGHT_GREEN   = 10,
    VGA_COLOUR_LIGHT_CYAN    = 11,
    VGA_COLOUR_LIGHT_RED     = 12,
    VGA_COLOUR_LIGHT_MAGENTA = 13,
    VGA_COLOUR_LIGHT_BROWN   = 14,
    VGA_COLOUR_WHITE         = 15,
} vga_colour_t;

void terminal_initialize(void);
void terminal_setcolour(uint8_t colour);
void terminal_putchar(char c);
void terminal_write(const char *data, size_t size);
void terminal_writestring(const char *data);

#endif
