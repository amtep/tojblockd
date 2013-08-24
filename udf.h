#include <stdint.h>

void init_udf(const char *target_dir, uint64_t image_size, uint64_t free_space);;
void udf_fill(void *buf, uint64_t from, uint32_t len);
