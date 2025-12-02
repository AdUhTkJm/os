// leak_tracker.h  (include in kernel build)
#pragma once
#include <stdint.h>
#include <stddef.h>

#if defined(LEAK_DETECT) || defined(IN_VSCODE)

// GCC will call them on function enter/exit when compiled with -finstrument-functions.
extern "C" [[gnu::no_instrument_function]] void __cyg_profile_func_enter(void *this_fn, void *call_site);
extern "C" [[gnu::no_instrument_function]] void __cyg_profile_func_exit(void *this_fn, void *call_site);

namespace os::leak {

[[gnu::no_instrument_function]] void init();
[[gnu::no_instrument_function]] void dump();
[[gnu::no_instrument_function]] void record_alloc(void *ptr, size_t size);
[[gnu::no_instrument_function]] void record_free(void *ptr);

}

#endif
