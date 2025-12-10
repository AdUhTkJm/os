#include "tty.h"
#include "../../fs/devfs.h"
#include "../../proc/schedule.h"

namespace os::tty {

tty::tty(console_inode *console): flags(TTY_ECHO | TTY_ICANON), width(80), height(25), console(console) {
  
}

void tty::echo(char c) {
  console->write(0, &c, 1, 0);
}

void tty::echo(const char *s) {
  console->write(0, s, strlen(s), 0);
}

void tty::send(int sig) {
  for (auto [_, pcb] : *pidmap) {
    if (pcb->pgid == pgid)
      pcb->pending.add(sig);
  }
}

string tty::readline() {
  auto tcb = active();
  auto pcb = tcb->pcb;

  pos = 0;
  for (char c; pos < sizeof(buf); ) {
    console->read(0, &c, 1, 0);
    if (c == ctrl + 'C') {
      send(SIGINT);
      break;
    }
    
    if (c == ctrl + 'Z') {
      send(SIGTSTP);
      break;
    }

    if (!(flags & TTY_ICANON))
      continue;

    bool do_echo = flags & TTY_ECHO;
    switch (c) {
    case '\r':
    case '\n':
      // Line end. Return.
      echo("\r\n");
      goto ret;
    
    case '\b':
    case 0x7f: // Delete
      echo("\b \b");
      do_echo = false;
      break;
    }

    if (do_echo)
      echo(c);
  }
ret:
  return string(buf, pos);
}

void tty::write(const char *s, size_t len) {
  for (size_t i = 0; i < len; i++) {
    char c = s[i];
    switch (c) {
    case '\n':
      echo("\r\n");
      break;
    
    default:
      echo(c);
    }
  }
}

}
