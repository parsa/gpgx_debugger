/* Real core timing probes, called only while the test host is suspended. */
#include "shared.h"
#define m68ki_cpu m68k
#define MUL 7
#define INLINE static inline
#include "m68k/m68ki_cycles.h"
#include "m68k/m68kconf.h"
#include "m68k/m68kcpu.h"

unsigned int test_exception_cost(unsigned int vector)
{
  return CYC_EXCEPTION[vector];
}

unsigned int test_bus_refresh(int due, unsigned int *next)
{
  cpu_hook_t saved = cpu_hook;
  cpu_hook = NULL;
  m68k_set_reg(M68K_REG_SR, 0x2700);
  m68k_set_reg(M68K_REG_PC, 0x300); /* synthetic NOP */
  CPU_STOPPED = 0;
  CPU_INT_LEVEL = 0;
  m68k.cycles = 0;
  m68k.refresh_cycles = due ? 0 : 128*7;
  m68k_run(1);
  *next = m68k.refresh_cycles;
  cpu_hook = saved;
  return m68k.cycles;
}

unsigned int test_reset_refresh(unsigned int *next)
{
  cpu_hook_t saved = cpu_hook;
  cpu_hook = NULL;
  m68k.cycles = 0;
  m68k.refresh_cycles = 1234;
  m68k_pulse_reset();
  *next = m68k.refresh_cycles;
  cpu_hook = saved;
  return m68k.cycles;
}

unsigned int test_irq_phase(unsigned int phase, int waiting, unsigned int *stopped)
{
  cpu_hook_t saved = cpu_hook;
  unsigned int before = phase*7;
  cpu_hook = NULL;
  m68k_set_reg(M68K_REG_SR, 0x2700);
  m68k_set_reg(M68K_REG_A7, 0xFFF000);
  CPU_STOPPED = waiting ? STOP_LEVEL_WAIT : 0;
  m68k.cycles = before;
  m68ki_exception_interrupt(2);
  *stopped = CPU_STOPPED;
  cpu_hook = saved;
  return m68k.cycles - before;
}
