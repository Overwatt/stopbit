/* image.c — Is this image one of ours, and bootable? No hardware; built for the host too. */
#include "system.h"
#include "fw_id.h"
#include <string.h>

const char *image_check_ram(const void *img, uint32_t addr, uint32_t size)
{
    uint32_t sp, pc;
    if (size < 0x400 || size > STAGE_SIZE) return "bad size";
    memcpy(&sp, img, 4);
    memcpy(&pc, (const uint8_t *)img + 4, 4);
    if (!((sp > 0x20000000u && sp <= 0x20020000u) ||
          (sp > 0x24000000u && sp <= 0x24080000u)) || (sp & 7)) return "bad initial SP";
    if (!(pc & 1) || pc < addr || pc >= addr + size) return "reset vector outside image";
    return NULL;
}

const char *image_check_app(const void *img, uint32_t size)
{
    fw_id_t id;
    const char *why = image_check_ram(img, STAGE_ADDR, size);
    if (why) return why;
    memcpy(&id, (const uint8_t *)img + FW_ID_OFFSET, sizeof id);
    if (id.magic != FW_ID_MAGIC_APP) return "not an app image";
    if (id.link_addr != STAGE_ADDR) return "app not linked for 0x24000000";
    return NULL;
}

const char *image_check_loader(const uint8_t *img, uint32_t size)
{
    uint32_t sp, pc;
    fw_id_t id;
    if (size < 0x1000 || size > 128u * 1024u) return "bad size";
    memcpy(&sp, img, 4);
    memcpy(&pc, img + 4, 4);
    memcpy(&id, img + FW_ID_OFFSET, sizeof id);
    if (id.magic != FW_ID_MAGIC_LOADER) return "not a loader image";
    if (id.link_addr != 0x08000000u) return "loader not linked for internal flash";
    if (sp != 0x20020000u) return "bad initial SP";
    if (!(pc & 1) || pc < 0x08000000u || pc >= 0x08000000u + size) return "bad reset vector";
    return NULL;
}
