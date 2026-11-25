/*
 * sections.ld — Section layout shared by every image. Include it after a
 * MEMORY block and REGION_ALIAS("CODE", ...) saying where code lives.
 * The vector table must be first; fw_id sits at a fixed offset behind it.
 */
ENTRY(Reset_Handler)

_estack = ORIGIN(DTCMRAM) + LENGTH(DTCMRAM);   /* top of stack */
_Min_Heap_Size  = 0x1000;   /* 4 KB  */
_Min_Stack_Size = 0x2000;   /* 8 KB  */

SECTIONS
{
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    . = 0x2A0;                 /* FW_ID_OFFSET */
    KEEP(*(.fw_id))
    . = ALIGN(4);
  } >CODE

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
  } >CODE

  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)
    *(.rodata*)
    . = ALIGN(4);
  } >CODE

  .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } >CODE
  .ARM : {
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
  } >CODE

  .preinit_array :
  {
    PROVIDE_HIDDEN(__preinit_array_start = .);
    KEEP(*(.preinit_array*))
    PROVIDE_HIDDEN(__preinit_array_end = .);
  } >CODE
  .init_array :
  {
    PROVIDE_HIDDEN(__init_array_start = .);
    KEEP(*(SORT(.init_array.*)))
    KEEP(*(.init_array*))
    PROVIDE_HIDDEN(__init_array_end = .);
  } >CODE
  .fini_array :
  {
    PROVIDE_HIDDEN(__fini_array_start = .);
    KEEP(*(SORT(.fini_array.*)))
    KEEP(*(.fini_array*))
    PROVIDE_HIDDEN(__fini_array_end = .);
  } >CODE

  _sidata = LOADADDR(.data);

  /* Initialised data: lives in DTCMRAM, loaded from FLASH. */
  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data)
    *(.data*)
    . = ALIGN(4);
    _edata = .;
  } >DTCMRAM AT> CODE

  /* Code that must not execute from flash; copied by self_flash(). */
  .itcm :
  {
    . = ALIGN(4);
    _sitcm = .;
    *(.itcm)
    . = ALIGN(4);
    _eitcm = .;
  } >ITCMRAM AT> CODE
  _siitcm = LOADADDR(.itcm);
  _image_end = LOADADDR(.itcm) + SIZEOF(.itcm);

  /* Ethernet descriptors + buffers (not zeroed at startup). */
  .dma_d2 (NOLOAD) :
  {
    . = ALIGN(32);
    *(.dma_d2)
  } >RAM_D2

  /* Large buffers in the rest of D2 SRAM, past the MPU region (not zeroed). */
  ASSERT(ADDR(.dma_d2) + SIZEOF(.dma_d2) <= ORIGIN(RAM_D2) + 32K, "dma_d2 exceeds its 32 KB MPU region")
  .d2_bss ORIGIN(RAM_D2) + 32K (NOLOAD) :
  {
    . = ALIGN(8);
    *(.d2_bss)
    . = ALIGN(8);
    _ed2bss = .;
  } >RAM_D2

  /* Zero-initialised data. */
  .bss :
  {
    . = ALIGN(4);
    _sbss = .;
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
    __bss_end__ = _ebss;
  } >DTCMRAM

  /* Heap + stack guard. */
  ._user_heap_stack :
  {
    . = ALIGN(8);
    PROVIDE(end = .);
    PROVIDE(_end = .);
    . = . + _Min_Heap_Size;
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } >DTCMRAM

  /DISCARD/ : { libc.a(*) libm.a(*) libgcc.a(*) }

  .ARM.attributes 0 : { *(.ARM.attributes) }
}
