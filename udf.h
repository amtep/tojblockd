#include <stdint.h>

#define SECTOR_SIZE 512
#define ALIGN(x, sz) (((x) + (sz) - 1) & ~((typeof(x))(sz) - 1))

void init_udf(const char *target_dir, uint64_t image_size, uint64_t free_space);;
int udf_fill(void *buf, uint64_t from, uint32_t len);
uint16_t udf_crc(void *buf, uint32_t len);
