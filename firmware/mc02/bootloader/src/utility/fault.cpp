#include <main.h>

// Fault recovery hook for the four fault handlers in the generated
// stm32h7xx_it.c, which is compiled into both the application and the
// bootloader. The application resets into DFU so a faulting image cannot brick
// the board (see app/src/utility/assert.cpp); the bootloader deliberately does
// not.
//
// Resetting here would be a downgrade. The bootloader is the last line of
// recovery, and a fault in it is almost certainly deterministic -- a reset would
// re-enter the same code and fault again, so the device would reboot in a loop
// and never stay enumerated long enough for a host to flash anything. Returning
// leaves the fault handler's own while(1) in place: the board stops with the
// faulting frame intact for a debugger, which is the only tool that can help at
// that point anyway.
extern "C" void librmcs_fault_recover(void) {}
