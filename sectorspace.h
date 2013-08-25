#include <stdint.h>

struct sectorspace;

struct sectorspace *init_sectorspace(uint64_t start, uint64_t len);
void sectorspace_mark(struct sectorspace *space, uint64_t start, uint64_t len);
uint64_t sectorspace_find(struct sectorspace *space, uint64_t len);
