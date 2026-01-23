//GCC provided header files for basic types

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

//Basic checks to see if x86-elf cross compiler is in use correctly

#if defined (__linux__)                                                         //Y/N Linux?
    #error "Code must be compiled with cross-compiler"
#elif !defined(__i386__)                                                        //Y/N architecture = 32-bit x86?
    #error "Code must be compiled with x86-elf compiler"
#endif

enum vga_color {                        //Define VGA text mode colors
	VGA_COLOR_BLACK = 0,
	VGA_COLOR_BLUE = 1,
	VGA_COLOR_GREEN = 2,
	VGA_COLOR_CYAN = 3,
	VGA_COLOR_RED = 4,
	VGA_COLOR_MAGENTA = 5,
	VGA_COLOR_BROWN = 6,
	VGA_COLOR_LIGHT_GREY = 7,
	VGA_COLOR_DARK_GREY = 8,
	VGA_COLOR_LIGHT_BLUE = 9,
	VGA_COLOR_LIGHT_GREEN = 10,
	VGA_COLOR_LIGHT_CYAN = 11,
	VGA_COLOR_LIGHT_RED = 12,
	VGA_COLOR_LIGHT_MAGENTA = 13,
	VGA_COLOR_LIGHT_BROWN = 14,
	VGA_COLOR_WHITE = 15,
};

static inline uint8_t vga_entry_color(enum vga_color fg, enum vga_color bg)         //Function to create color byte by combining foreground and background colors
{
	return fg | bg << 4;
}

static inline uint16_t vga_entry(unsigned char uc, uint8_t color)                    //Function to create VGA entry by combining character and color byte   
{
	return (uint16_t) uc | (uint16_t) color << 8;
}

size_t strlen(const char* str)      //Basic string length function because standard library is not available
{
	size_t len = 0;
	while (str[len])
		len++;
	return len;
}

#define VGA_WIDTH   80              //VGA text mode width
#define VGA_HEIGHT  25              //VGA text mode height
#define VGA_MEMORY  0xB8000         //VGA text mode memory address

size_t terminal_row;                                    //Current terminal row
size_t terminal_column;                                 //Current terminal column
uint8_t terminal_color;                                 //Current terminal color
uint16_t* terminal_buffer = (uint16_t*)VGA_MEMORY;      //Pointer to VGA text mode buffer

void terminal_initialize(void)                          //Function to initialize terminal
{
	terminal_row = 0;
	terminal_column = 0;
	terminal_color = vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
	
	for (size_t y = 0; y < VGA_HEIGHT; y++) {
		for (size_t x = 0; x < VGA_WIDTH; x++) {
			const size_t index = y * VGA_WIDTH + x;
			terminal_buffer[index] = vga_entry(' ', terminal_color);
		}
	}
}

void terminal_setcolor(uint8_t color)                   //Function to set terminal color    
{
	terminal_color = color;
}

void terminal_putentryat(char c, uint8_t color, size_t x, size_t y)     //Function to put character at specific position with specific color
{
	const size_t index = y * VGA_WIDTH + x;
	terminal_buffer[index] = vga_entry(c, color);
}

void terminal_putchar(char c)                                           //Function to put character at current position and advance cursor
{
	terminal_putentryat(c, terminal_color, terminal_column, terminal_row);
	if (++terminal_column == VGA_WIDTH) {
		terminal_column = 0;
		if (++terminal_row == VGA_HEIGHT)
			terminal_row = 0;
	}
}

void terminal_write(const char* data, size_t size)              //Function to write a string of given size to terminal
{
	for (size_t i = 0; i < size; i++)
		terminal_putchar(data[i]);
}

void terminal_writestring(const char* data)                     //Function to write null-terminated string to terminal
{
	terminal_write(data, strlen(data));
}

void kernel_main(void)                                          //Kernel main function
{
	terminal_initialize();

	terminal_writestring("kernel: hello world");
}
