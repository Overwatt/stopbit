/* The firmware's identity, writable here: it becomes whatever the simulator boots. */
#include <stdint.h>
#include <string.h>
struct { uint32_t magic, link_addr; char version[24]; } g_fw_id = { 0x5041354Eu, 0x24000000u, "" };
void sim_version(const char *v) { strncpy(g_fw_id.version, v, sizeof g_fw_id.version - 1); }
