#include "timer.h"
#include "ioport.h"
#include "irq.h"

#include "serial.h"

static volatile uint32_t tick_count = 0;

// timer fires = timer_handler()
void timer_handler(regs_t *r) {
    (void)r;                                    // pass CPU register state (future dev)
    tick_count++;
}

uint32_t timer_get_ticks(void) {
    return tick_count;
}

// interrupt at wanted frequency
void timer_init(uint32_t hz) {

    // divisor = reload value
    uint32_t divisor = PIT_BASE_FREQ / hz;                  // divisor = 1193182 / desired frequency
    if (divisor == 0) divisor = 1;                          // clamp range 1 - 65535
    if (divisor > 0xFFFF) divisor = 0xFFFF;                 // PIT quirk = (divisor = 0  -> interpreted as 65536)

    // PIT configuration
    outb(PIT_CMD, 0x36);                                    // | channel 0 | access = low byte -> high byte | square wave | binary counter |

    // send reload value
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));          // low byte value
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));   // high byte value

    irq_install_handler(0, timer_handler);                  // install IRQ handler (connect IRQ0 -> timer handler)

    serial_write("Timer: PIT initialized \n");

}
