#ifndef ERRORCODE_H
#define ERRORCODE_H

// Note this is not in namespace os.
enum error_code {
  ENOENT = 2,
  EIO = 5,
  ENOEXEC = 8,
  EAGAIN = 11,
  EISDIR = 21,
  EINVAL = 22,
};

#endif
