/* Loader run from the top of AXI SRAM: SWD testing with no flash writes. */
MEMORY
{
  AXI     (xrw) : ORIGIN = 0x24060000, LENGTH = 128K
  ITCMRAM (xrw) : ORIGIN = 0x00000400, LENGTH = 63K   /* skip NULL */
  DTCMRAM (xrw) : ORIGIN = 0x20000000, LENGTH = 128K
  RAM_D2  (xrw) : ORIGIN = 0x30000000, LENGTH = 288K
}
REGION_ALIAS("CODE", AXI);
/* Uncached, for Ethernet DMA (eth_mac.c). */
DMA_D2_SIZE = 32K;
INCLUDE sections.ld
