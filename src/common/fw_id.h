/* fw_id.h — Identity block at a fixed offset in every image we build. */
#ifndef FW_ID_H
#define FW_ID_H
#include <stdint.h>
#define FW_ID_OFFSET        0x2A0u          /* just past the vector table */
#define FW_ID_MAGIC_LOADER  0x444C354Eu     /* "N5LD" */
#define FW_ID_MAGIC_APP     0x5041354Eu     /* "N5AP" */
typedef struct {
    uint32_t magic;
    uint32_t link_addr;
    char     version[24];
} fw_id_t;
extern const fw_id_t g_fw_id;
#endif
