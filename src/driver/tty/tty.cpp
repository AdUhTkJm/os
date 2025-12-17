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
  if (!(flags.c_lflag & TTY_ECHO))
    return;
  console->write(0, &c, 1, 0);
}

void tty::echo(const char *s) {
  if (!(flags.c_lflag & TTY_ECHO))
    return;
  console->write(0, s, strlen(s), 0);
}

void tty::echo(const char *s, size_t len) {
  if (!(flags.c_lflag & TTY_ECHO))
    return;
  console->write(0, s, len, 0);
}

void tty::send(int sig) {
  for (auto [_, pcb] : *pidmap) {
    if (pcb->pgid == pgid)
      pcb->send_signal(sig);
  }
}

void tty::backspace() {
  if (cursor > 0) {
    cursor--;
    line.erase(line.begin() + cursor);
    
    // Move everything one character backwards.
    echo('\b');
    echo(line.data() + cursor, line.size() - cursor);
    // Erase the last character.
    echo(' ');
    // Get to the correct cursor in terminal.
    for (size_t i = cursor; i <= line.size(); i++)
      echo('\b');
  }
}

string tty::readline() {
  cursor = 0;
  line.clear();
  enum {
    escape0, // Normal
    escape1, // Received esc
    escape2, // Received esc + [
    escape3, // In a multi-byte escape sequence that we don't understand
  } state = escape0;
  for (char c;;) {
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

    if (state == escape3) {
      // We don't understand it. Just output as-is.
      // Note the terminating character also doesn't belong to input.
      echo(c);
      if (c < 0x20 || c > 0x3f)
        state = escape0;
      continue;
    }

    if (state == escape2) {
      // Handle escape characters.
      state = escape0;
      switch (c) {
      case 'D':
        if (cursor > 0) {
          cursor--;
          echo("\b");
        }
        break;

      case 'C':
        if (cursor < line.size())
          echo(line[cursor++]);
        break;

      default:
        // We don't know the sequence; just print them as-is.
        echo("\x1b[");
        echo(c);
        // We're still inside this escape sequence.
        if (c >= 0x20 && c <= 0x3f)
          state = escape3;
      }
      continue;
    }

    if (state == escape1) {
      if (c == '[') {
        state = escape2;
        continue;
      } else state = escape0;
    }

    switch (c) {
    case '\r':
      if (!(flags.c_iflag & ICRNL))
        break;
      [[fallthrough]];
    case '\n':
      // Line end. Return. (This ignores cursor position.)
      echo("\r\n");
      line.push_back('\n');
      return string(line.data(), line.size());
    
    case '\b':
    case 0x7f:
      backspace();
      break;

    // This behaviour is currently buggy. Don't know why.
    // case 0x1b: // ESC
    //   state = escape1;
    //   break;

    default:
      if (state == escape0) {
        line.insert(line.begin() + cursor, c);
        cursor++;
        echo(c);
      }
    }
  }
  return string(line.data(), line.size());
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
