#include "sig.h"

namespace os {

void sigset::add(int signum) {
  sig |= (1 << signum);
}

void sigset::remove(int signum) {
  sig &= ~(1 << signum);
}

bool sigset::operator[](int signum) const {
  return sig & (1 << signum);
}

int sigset::next(const sigset &ignored) const {
  for (unsigned i = 1; i < sizeof(sig) * 8; i++) {
    if ((*this)[i] && !ignored[i])
      return i;
  }
  return SIGNONE;
}

}
