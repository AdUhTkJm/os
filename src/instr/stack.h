#ifndef STACK_H
#define STACK_H

#ifdef FUNC_INSTRUMENT

// GCC will call them on function enter/exit when compiled with -finstrument-functions.
extern "C" [[gnu::no_instrument_function]] void __cyg_profile_func_enter(void *this_fn, void *call_site);
extern "C" [[gnu::no_instrument_function]] void __cyg_profile_func_exit(void *this_fn, void *call_site);

namespace os::stack {

constexpr int SHADOW_DEPTH = 16;

struct shadow_stack {
  void *frames[SHADOW_DEPTH];
  int top;
};

[[gnu::no_instrument_function]] void dump();
[[gnu::no_instrument_function]] void dump(const shadow_stack &stack);
[[gnu::no_instrument_function]] void reset();
[[gnu::no_instrument_function]] void copy(shadow_stack *stack);
[[gnu::no_instrument_function]] const char *lookup_symbol(unsigned long pc);

};

#endif

#endif
