#ifndef PIPELINE_WAIT_GUARD_H
#define PIPELINE_WAIT_GUARD_H
#include <stdbool.h>
// Engine bugfix: AddPipelineState's wait on a cached pipeline entry never
// terminates when the producer's compile failed. See the .c for the trace.
bool pipeline_wait_guard_init(void *binary_base);
#endif
