/* Application: loaded into AXI SRAM by the loader, then jumped to. */
MEMORY
{
  AXI     (xrw) : ORIGIN = 0x24000000, LENGTH = 512K   /* the image (at most 384K, the loader's limit), then buffers */
  RAM_D3  (rw)  : ORIGIN = 0x38000000, LENGTH = 64K
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

  /* D3 SRAM: not zeroed. */
  .d3_bss (NOLOAD) :
  {
    . = ALIGN(32);
    *(.d3_bss)
    . = ALIGN(32);
    _ed3bss = .;
  } >RAM_D3
}
ASSERT(ADDR(.axi_bss) >= _image_end, "axi_bss overlaps the image")
ASSERT(_image_end <= ORIGIN(AXI) + 384K, "the image is larger than the loader can load")
/* From _eaxi to the end of AXI SRAM is the TLS heap (tls.c). */
