// Battery-SRAM file store for the workstation ROM (snes/header_sram.s
// declares 32KB cartridge SRAM). A fixed 16-entry file table plus one
// contiguous, compacted data area — just enough filesystem to keep Python
// sources across power-off (emulators persist it as the .srm file).
//
// Logical addresses 0x0000..0x7FFF map onto four 8KB HiROM SRAM windows
// (banks $30-$33 at $6000-$7FFF); all access goes through window-aware
// helpers because Calypsi far pointers do not carry across 64K banks.
#ifndef SNES_SRAM_FS_H
#define SNES_SRAM_FS_H

#include <stdint.h>

#define SFS_MAX_FILES 16
#define SFS_NAME_MAX 15 // + NUL
#define SFS_DATA_START 0x0200u
#define SFS_DATA_LIMIT 0x8000u

void sfs_mount(void);        // format (with a starter file) if magic bad
uint8_t sfs_formatted(void); // 1 if mount() had to format (fresh SRAM)
uint8_t sfs_count(void);
uint8_t sfs_used(uint8_t slot);
void sfs_name(uint8_t slot, char *out16); // copies name + NUL
uint16_t sfs_size(uint8_t slot);
// NB: int (16-bit), not int8_t — Calypsi tests 1-byte values with 16-bit
// reads in some shapes (bug ledger #21), which turned "slot 1" into a
// negative and silently skipped the replace-delete in sfs_write.
int sfs_find(const char *name); // slot index or -1
uint16_t sfs_read(uint8_t slot, void *dst, uint16_t max); // bytes copied
// replace-or-create; 0 ok, -1 no data space, -2 no free slot
int sfs_write(const char *name, const void *src, uint16_t len);
void sfs_delete(uint8_t slot); // compacts the data area
uint16_t sfs_free_bytes(void);

#endif
