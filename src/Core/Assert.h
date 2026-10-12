#pragma once

#include <cassert>

#ifdef _DEBUG
#define ENGINE_ASSERT(condition) assert(condition)
#else
#define ENGINE_ASSERT(condition) ((void)0)
#endif
