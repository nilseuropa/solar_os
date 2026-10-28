#pragma once
#include <stddef.h>

void doc_test_memory_budget(size_t bytes);
void doc_test_memory_fail_after(int allocations);
size_t doc_test_memory_used(void);
size_t doc_test_memory_peak(void);
