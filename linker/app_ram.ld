/* Application: loaded into AXI SRAM by the loader, then jumped to. */
MEMORY
{
  AXI     (xrw) : ORIGIN = 0x24000000, LENGTH = 512K   /* the image (at most 384K, the loader's limit), then the TLS heap */
  RAM_D3  (rw)  : ORIGIN = 0x38000000, LENGTH = 64K
  ITCMRAM (xrw) : ORIGIN = 0x00000400, LENGTH = 63K   /* skip NULL; buffers (.itcm_bss) */
  DTCMRAM (xrw) : ORIGIN = 0x20000000, LENGTH = 128K
  RAM_D2  (xrw) : ORIGIN = 0x30000000, LENGTH = 288K
}
REGION_ALIAS("CODE", AXI);
/* Uncached, for Ethernet DMA; eth_mac.c sets the MPU to match. */
DMA_D2_SIZE = 40K;
INCLUDE sections.ld

/* Buffers that need neither the image's place nor a DMA: the web server's, the stored
 * certificate. ITCM holds no code here (.itcm is the loader's), so its 63 KB are free,
 * and every byte kept out of AXI SRAM is a byte of TLS heap. Not zeroed, not in the image. */
SECTIONS
{
  .itcm_bss (NOLOAD) :
  {
    . = ALIGN(32);
    *(.itcm_bss)
    . = ALIGN(32);
    _eitcmbss = .;
  } >ITCMRAM

  /* D3 SRAM: not zeroed. */
  .d3_bss (NOLOAD) :
  {
    . = ALIGN(32);
    *(.d3_bss)
    . = ALIGN(32);
    _ed3bss = .;
  } >RAM_D3
}
/* From the image's end to the end of AXI SRAM is the TLS heap (tls.c). */
_eaxi = ALIGN(_image_end, 32);
ASSERT(DMA_D2_SIZE == 40K, "eth_mac.c makes 40 KB uncached for the application")
ASSERT(_image_end <= ORIGIN(AXI) + 384K, "the image is larger than the loader can load")
ASSERT(SIZEOF(.itcm) == 0, "ITCM is for buffers in the application")
