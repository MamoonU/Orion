// sched.h - Round-robin priority scheduler

#ifndef SCHED_H
#define SCHED_H

#include <stdint.h>
#include "proc.h"

void sched_init(void);                                          // called once after proc_init() and tss_init()

void sched_add(pcb_t *p);                                       // add READY process to scheduler:  p->state must already be PROC_READY
void sched_remove(pcb_t *p);                                    // remove process from the ready queue without destroying it

pcb_t *sched_current(void);                                     // return currently running PCB

void sched_tick(void);                                          // signal a timer tick

void sched_yield(void);                                         // voluntarily give up the CPU
void sched_force_switch(void);                                  // set tick + zero timeslice -> next irq switches

void sched_dump(void);                                          // dump scheduler state to serial

uint32_t sched_switch_esp(uint32_t current_esp);                // switch_process : no switch ? switch 

void sched_start(void);                                         // launch scheduler

extern void sched_start_first(uint32_t esp_kernel);             // syscall.asm

#endif