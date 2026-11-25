/* Application: loaded into AXI SRAM by the loader, then jumped to. */
MEMORY
{
  AXI     (xrw) : ORIGIN = 0x24000000, LENGTH = 384K   /* the image; what the loader loads */
  AXI_HI  (rw)  : ORIGIN = 0x24060000, LENGTH = 128K   /* the rest of it */
  ITCMRAM (xrw) : ORIGIN = 0x00000400, LENGTH = 63K   /* skip NULL */
  DTCMRAM (xrw) : ORIGIN = 0x20000000, LENGTH = 128K
  RAM_D2  (xrw) : ORIGIN = 0x30000000, LENGTH = 288K
}
REGION_ALIAS("CODE", AXI);
INCLUDE sections.ld

/* Buffers in what the image leaves of AXI SRAM (not zeroed, not in the image). */
SECTIONS
{
  .axi_bss (NOLOAD) :
  {
    . = ALIGN(32);
    *(.axi_bss)
    . = ALIGN(32);
    _eaxi = .;
  } >AXI

  .axi_hi (NOLOAD) :
  {
    . = ALIGN(32);
    *(.axi_hi)
    . = ALIGN(32);
    _eaxi_hi = .;
  } >AXI_HI
}
ASSERT(ADDR(.axi_bss) >= _image_end, "axi_bss overlaps the image")
