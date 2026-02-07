//GCC provided header files for basic types

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "idt.h"

//Basic checks to see if x86-elf cross compiler is in use correctly

#if defined (__linux__)                                                         //Y/N Linux?
    #error "Code must be compiled with cross-compiler"
#elif !defined(__i386__)                                                        //Y/N architecture = 32-bit x86?
    #error "Code must be compiled with x86-elf compiler"
#endif


// -------------------------- Port I/O Functions -------------------------- //

static inline void outb(uint16_t port, uint8_t val) {			//Function to write byte to I/O port
    asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}



// -------------------------- Serial Debugging and Output -------------------------- //

#define COM1_PORT 0x3F8               //COM1 port address

void serial_init() {                 //Function to initialize serial port for debugging

	outb(COM1_PORT + 1, 0x00);    // Disable all interrupts
	outb(COM1_PORT + 3, 0x80);    // Enable DLAB (set baud rate divisor)
	outb(COM1_PORT + 0, 0x01);    // Set divisor to 3 (low byte) 115200 baud
	outb(COM1_PORT + 1, 0x00);    //                  (high byte)
	outb(COM1_PORT + 3, 0x03);    // 8 bits, no parity, one stop bit
	outb(COM1_PORT + 2, 0xC7);    // Enable FIFO, clear them, with 14-byte threshold
	outb(COM1_PORT + 4, 0x0B);    // IRQs enabled, RTS/DSR set
}

static int serial_is_transmit_empty(void) {		 //Function to check if serial port is ready to transmit
    return inb(COM1_PORT + 5) & 0x20;
}

void serial_putchar(char c) {				 //Function to write character to serial port
    while (!serial_is_transmit_empty());
    outb(COM1_PORT, c);
}

void serial_write(const char *s) {						 //Function to write string to serial port
    while (*s) {
        if (*s == '\n')
            serial_putchar('\r');
        serial_putchar(*s++);
    }
}

void printk(const char *s) {						 //Kernel print function for debugging via serial port
    serial_write(s);
}

void IRQ_init() {						//Function to initialize IRQs (not fully implemented here)


	
	serial_write("IRQ: Initialized (stub)\n");
}





// -------------------------- Panic & Assertions -------------------------- //

__attribute__((noreturn))										//Function to handle kernel panic situations
void panic(const char *msg) {
    serial_write("\n====================\n");
    serial_write("KERNEL PANIC\n");
    serial_write(msg);
    serial_write("\nSystem halted.\n");
    serial_write("====================\n");

    asm volatile ("cli");   
    asm volatile ("hlt");   
    for (;;);               // Safety: never return
}

#define kassert(condition)                                  \
    do {                                                     \
        if (!(condition)) {                                 \
            panic("Assertion failed: " #condition);         \
        }                                                    \
    } while (0)


// -------------------------- VGA Text Mode Output -------------------------- //

enum vga_colour {                        //Define VGA text mode colours
	VGA_COLOUR_BLACK = 0,
	VGA_COLOUR_BLUE = 1,
	VGA_COLOUR_GREEN = 2,
	VGA_COLOUR_CYAN = 3,
	VGA_COLOUR_RED = 4,
	VGA_COLOUR_MAGENTA = 5,
	VGA_COLOUR_BROWN = 6,
	VGA_COLOUR_LIGHT_GREY = 7,
	VGA_COLOUR_DARK_GREY = 8,
	VGA_COLOUR_LIGHT_BLUE = 9,
	VGA_COLOUR_LIGHT_GREEN = 10,
	VGA_COLOUR_LIGHT_CYAN = 11,
	VGA_COLOUR_LIGHT_RED = 12,
	VGA_COLOUR_LIGHT_MAGENTA = 13,
	VGA_COLOUR_LIGHT_BROWN = 14,
	VGA_COLOUR_WHITE = 15,
};

static inline uint8_t vga_entry_colour(enum vga_colour fg, enum vga_colour bg)         //Function to create colour byte by combining foreground and background colours
{
	return fg | bg << 4;
}

static inline uint16_t vga_entry(unsigned char uc, uint8_t colour)                    //Function to create VGA entry by combining character and colour byte   
{
	return (uint16_t) uc | (uint16_t) colour << 8;
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
uint8_t terminal_colour;                                 //Current terminal colour
uint16_t* terminal_buffer = (uint16_t*)VGA_MEMORY;      //Pointer to VGA text mode buffer

void terminal_initialize(void)                          //Function to initialize terminal
{
	terminal_row = 0;
	terminal_column = 0;
	terminal_colour = vga_entry_colour(VGA_COLOUR_LIGHT_GREY, VGA_COLOUR_BLACK);
	
	for (size_t y = 0; y < VGA_HEIGHT; y++) {
		for (size_t x = 0; x < VGA_WIDTH; x++) {
			const size_t index = y * VGA_WIDTH + x;
			terminal_buffer[index] = vga_entry(' ', terminal_colour);
		}
	}
}

void terminal_setcolour(uint8_t colour)                   //Function to set terminal colour    
{
	terminal_colour = colour;
}

void terminal_putentryat(char c, uint8_t colour, size_t x, size_t y)     //Function to put character at specific position with specific colour
{
	const size_t index = y * VGA_WIDTH + x;
	terminal_buffer[index] = vga_entry(c, colour);
}

void terminal_putchar(char c)                                           //Function to put character at current position and advance cursor
{
	terminal_putentryat(c, terminal_colour, terminal_column, terminal_row);
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
	serial_init();

	terminal_writestring("Kernel: Online");
	serial_write("Serial I/O: Online\n");

	//kassert(1 == 0);  //Example assertion that fails
}
