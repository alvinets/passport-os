#pragma once
#include "passport_workbuddy.h"

#ifdef __cplusplus
extern "C" {
#endif

bool wb_get_str(const char *key, char *out, size_t cap);
bool wb_set_str(const char *key, const char *value);

#ifdef __cplusplus
}
#endif
