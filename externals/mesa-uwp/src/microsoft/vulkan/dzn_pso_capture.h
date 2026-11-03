#ifndef DZN_PSO_CAPTURE_H
#define DZN_PSO_CAPTURE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void dzn_pso_capture_prepare(const void *desc, const uint8_t *hash);
void dzn_pso_capture_finish(void);
long dzn_pso_legacy_create(void *device, const void *desc, void **out);
int dzn_pso_use_legacy(void);
#ifdef __cplusplus
}
#endif
#endif
