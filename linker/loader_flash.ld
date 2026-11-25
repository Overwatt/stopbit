/* Loader, in internal flash (one 128 KB erase sector). */
MEMORY
{
  FLASH   (rx)  : ORIGIN = 0x08000000, LENGTH = 128K
  ITCMRAM (xrw) : ORIGIN = 0x00000400, LENGTH = 63K   /* skip NULL */
  DTCMRAM (xrw) : ORIGIN = 0x20000000, LENGTH = 128K
  RAM_D2  (xrw) : ORIGIN = 0x30000000, LENGTH = 288K
}
REGION_ALIAS("CODE", FLASH);
INCLUDE sections.ld
