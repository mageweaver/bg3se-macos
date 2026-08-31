#ifndef PIPELINE_PROBE_H
#define PIPELINE_PROBE_H
#include <stdbool.h>
// Diagnostic tap on rf::metal::GPUDevice::CreatePipelineState.
// Only active with BG3SE_PIPELINE_PROBE=1.
bool pipeline_probe_init(void *binary_base);
#endif
