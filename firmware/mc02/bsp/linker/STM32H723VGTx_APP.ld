/* Linker script for STM32H723VGTx — Application
 * Flash layout:
 *   Sector 0 (0x08000000, 128 KB): Bootloader
 *   Sector 1 (0x08020000, 128 KB): Metadata
 *   Sector 2-7 (0x08040000, 768 KB): App  ← this binary
 */

ENTRY(Reset_Handler)

MEMORY
{
    DTCMRAM  (xrw): ORIGIN = 0x20000000, LENGTH = 128K - 64
    MAILBOX   (rw): ORIGIN = 0x2001FFC0, LENGTH = 64
    RAM      (xrw): ORIGIN = 0x24000000, LENGTH = 128K
    RAM_D2   (xrw): ORIGIN = 0x30000000, LENGTH = 32K
    RAM_D3   (xrw): ORIGIN = 0x38000000, LENGTH = 16K
    ITCMRAM  (xrw): ORIGIN = 0x00000000, LENGTH = 64K
    FLASH     (rx): ORIGIN = 0x08040000, LENGTH = 768K
}

/* Main/ISR stack lives in zero-wait DTCM (CPU-private, never a DMA target), so
 * every call frame, local and ISR context push is single-cycle and free of AXI
 * bus-contention jitter -- the lowest-latency place for the stack on this part.
 * Grows down from just below the boot mailbox at the top of DTCM. */
_estack = ORIGIN(DTCMRAM) + LENGTH(DTCMRAM);
_Min_Heap_Size  = 0x2000;
_Min_Stack_Size = 0x2000;

SECTIONS
{
    .isr_vector :
    {
        . = ALIGN(4);
        KEEP(*(.isr_vector))
        . = ALIGN(4);
    } >FLASH

    /* Latency-critical CAN forwarding code, executed from zero-wait ITCM so it
     * never depends on the I-cache or incurs FLASH-XIP fetch jitter. Loaded from
     * FLASH and copied to ITCM at boot by app.cpp (App constructor).
     *
     * Placed ahead of .text deliberately: the per-function input sections listed
     * below are named .text.<mangled>, so the *(.text*) wildcard would swallow
     * them -- the linker assigns each input section to the first output section
     * that matches it in script order.
     *
     * WARNING: nothing selected here may execute before that copy loop. The boot
     * path (Reset_Handler, SystemInit, main, utility::Lazy<App>::init) runs from
     * FLASH while ITCM still holds garbage, so a pattern matching e.g.
     * utility::Lazy would fault at startup. Keep the patterns narrow. */
    _siitcm = LOADADDR(.itcm);
    .itcm :
    {
        . = ALIGN(4);
        _sitcm = .;
        *(.itcm)
        *(.itcm*)

        /* Leaf calls of the CAN RX ISR. Left in FLASH they cost, per frame, a
         * long-branch veneer (ITCM at 0x0 is far outside the BL reach of FLASH
         * at 0x08040000) plus an I-cache-dependent fetch. Matched on the mangled
         * class prefix, so member signatures may change freely; a class rename
         * would silently stop matching, and the only consequence is that the
         * veneers come back -- slower, still correct. */
        *(.text._ZN7librmcs4core8protocol10Serializer*)         /* write_can + size helpers  */
        *(.text._ZN7librmcs4core7utility8Bitfield*)             /* protocol header bit packing */
        *(.text._ZN7librmcs8firmware3usb19InterruptSafeBuffer*) /* uplink batch allocation   */
        *(.text._ZN7librmcs8firmware7utility10RingBuffer*)      /* CAN transmit ring         */
        *(.text._ZZN7librmcs8firmware3can3Can*)                 /* out-of-line Can lambdas   */
        *(.text._ZN7librmcs8firmware3usb14get_serializerEv)

        /* Lazy accessors reached once per ISR entry. Full mangled names, since a
         * utility::Lazy wildcard would also catch Lazy<App>::init(), which main()
         * calls before the ITCM image exists. Template arguments are part of the
         * name, so these are best-effort: if a constructor signature changes the
         * match is lost and the call reverts to a veneer. */
        *(.text._ZN7librmcs8firmware7utility4LazyINS0_3can3CanEJP19FDCAN_HandleTypeDefmEE3getEv)
        *(.text._ZN7librmcs8firmware7utility4LazyINS0_3led3LedEJEEptEv)
        *(.text._ZN7librmcs8firmware7utility4LazyINS0_3usb6VendorEJEEptEv)

        . = ALIGN(4);
        _eitcm = .;
    } >ITCMRAM AT> FLASH

    .text :
    {
        . = ALIGN(4);
        *(.text)
        *(.text*)
        *(.glue_7)
        *(.glue_7t)
        *(.eh_frame)
        KEEP(*(.init))
        KEEP(*(.fini))
        . = ALIGN(4);
        _etext = .;
    } >FLASH

    .rodata :
    {
        . = ALIGN(4);
        *(.rodata)
        *(.rodata*)
        . = ALIGN(4);
    } >FLASH

    .ARM.extab (READONLY) : { *(.ARM.extab* .gnu.linkonce.armextab.*) } >FLASH
    .ARM (READONLY) :
    {
        __exidx_start = .;
        *(.ARM.exidx*)
        __exidx_end = .;
    } >FLASH

    .preinit_array (READONLY) :
    {
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array*))
        PROVIDE_HIDDEN(__preinit_array_end = .);
    } >FLASH
    .init_array (READONLY) :
    {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array*))
        PROVIDE_HIDDEN(__init_array_end = .);
    } >FLASH
    .fini_array (READONLY) :
    {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array*))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } >FLASH

    _sidata = LOADADDR(.data);
    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data)
        *(.data*)
        *(.RamFunc)
        *(.RamFunc*)
        . = ALIGN(4);
        _edata = .;
    } >RAM AT> FLASH

    /* Hot forwarding data (serializer + USB batch buffers + CAN objects) in
     * zero-wait DTCM, so the uplink ISR's data accesses never reach the AXI bus
     * and never contend with DMA -- completing the "CPU off the bus" hot path.
     * Loaded from FLASH and copied to DTCM at boot by app.cpp. These are CPU-only
     * structures; UART DMA buffers deliberately stay in non-cacheable AXI. */
    _sidtcm = LOADADDR(.dtcm);
    .dtcm :
    {
        . = ALIGN(32);
        _sdtcm = .;
        *(.dtcm)
        *(.dtcm*)
        . = ALIGN(32);
        _edtcm = .;
    } >DTCMRAM AT> FLASH

    .tbss (NOLOAD) : ALIGN(4)
    {
        _sbss = .;
        __bss_start__ = _sbss;
        *(.tbss .tbss.*)
        . = ALIGN(4);
    } >RAM

    .bss (NOLOAD) : ALIGN(4)
    {
        *(.bss)
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
        __bss_end__ = _ebss;
    } >RAM

    .boot_mailbox (NOLOAD) :
    {
        . = ALIGN(4);
        KEEP(*(.boot_mailbox))
        KEEP(*(.boot_mailbox.*))
        . = ALIGN(4);
    } >MAILBOX

    ._user_heap_stack (NOLOAD) :
    {
        . = ALIGN(8);
        PROVIDE(end = .);
        PROVIDE(_end = .);
        . = . + _Min_Heap_Size;
        . = . + _Min_Stack_Size;
        . = ALIGN(8);
    } >RAM

    /* WS2812 LED frame buffer for SPI6 TX. SPI6 is a D3-domain peripheral whose
     * only DMA path is BDMA, which can reach D3 SRAM (0x38000000) but never the
     * AXI/D2 SRAMs -- so this buffer must live here. NOLOAD: the CPU fills and
     * cache-cleans it before each transfer (see led.hpp). */
    .d3_sram (NOLOAD) :
    {
        . = ALIGN(32);
        *(.d3_sram)
        *(.d3_sram*)
        . = ALIGN(32);
    } >RAM_D3

    /DISCARD/ :
    {
        libc.a:*(*)
        libm.a:*(*)
        libgcc.a:*(*)
    }
}
