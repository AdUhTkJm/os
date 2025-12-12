#include "tty.h"
#include "../../fs/devfs.h"
#include "../../proc/schedule.h"

namespace os::tty {

tty::tty(console_inode *console): flags({
  .c_iflag = ICRNL,
  .c_oflag = ONLCR | OPOST,
  .c_cflag = CREAD | CS8,
  .c_lflag = TTY_ICANON | TTY_ECHO,
  .c_line = 0,
  .c_cc = {}
}), width(80), height(25), console(console) {
  
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
      pcb->send_signal(sig);
  }
}

string tty::readline() {
  auto tcb = active();
  auto pcb = tcb->pcb;

  pos = 0;
  for (char c; pos < sizeof(buf);) {
    for (int len = 0; len != 1;)
      len = console->read(0, &c, 1, 0);
    if (c == ctrl + 'C') {
      send(SIGINT);
      break;
    }
    
    if (c == ctrl + 'Z') {
      send(SIGTSTP);
      break;
    }

    if (!(flags.c_lflag & TTY_ICANON))
      continue;

    bool do_echo = flags.c_lflag & TTY_ECHO;
    switch (c) {
    case '\r':
      if (!(flags.c_iflag & ICRNL))
        break;
      [[fallthrough]];
    case '\n':
      // Line end. Return.
      echo("\r\n");
      buf[pos++] = '\n';
      return string(buf, pos);
    
    case '\b':
    case 0x7f: // Delete
      if (pos > 0) {
        pos--;
        echo("\b \b");
        continue;
      }
      break;
    }

    buf[pos++] = c;
    if (do_echo)
      echo(c);
  }
  return string(buf, pos);
}

void tty::write(const char *s, size_t len) {
  for (size_t i = 0; i < len; i++) {
    char c = s[i];
    switch (c) {
    case '\n':
      if (flags.c_oflag & ONLCR)
        echo("\r\n");
      else
        echo("\n");
      break;
    
    default:
      echo(c);
    }
  }
}

}
