/* Application: loaded into AXI SRAM by the loader, then jumped to. */
MEMORY
{
  AXI     (xrw) : ORIGIN = 0x24000000, LENGTH = 384K
  ITCMRAM (xrw) : ORIGIN = 0x00000400, LENGTH = 63K   /* skip NULL */
  DTCMRAM (xrw) : ORIGIN = 0x20000000, LENGTH = 128K
  RAM_D2  (xrw) : ORIGIN = 0x30000000, LENGTH = 288K
}
REGION_ALIAS("CODE", AXI);
INCLUDE sections.ld
