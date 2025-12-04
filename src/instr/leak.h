#ifndef LEAK_H
#define LEAK_H

#include <stdint.h>
#include <stddef.h>
#include "stack.h"

#ifdef FUNC_INSTRUMENT

namespace os::leak {

[[gnu::no_instrument_function]] void init();
[[gnu::no_instrument_function]] void dump();
[[gnu::no_instrument_function]] void record_alloc(void *ptr, size_t size);
[[gnu::no_instrument_function]] void record_free(void *ptr);

}

#endif

#endif
