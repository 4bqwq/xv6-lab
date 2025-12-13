#pragma once
#include "../arch/mod.h"
#include "../lib/mod.h"
#include "type.h"

// plic.c
void plic_init(void);
void plic_inithart(void);
int  plic_claim(void);
void plic_complete(int irq);

// timer.c
void   timer_init(void);
void   timer_create(void);
void   timer_update(void);
uint64 timer_get_ticks(void);
void   timer_wait(uint64 ntick);

// trap_kernel.c
void trap_kernel_init(void);
void trap_kernel_inithart(void);
void trap_kernel_handler(void);
void external_interrupt_handler(void);
void timer_interrupt_handler(void);
