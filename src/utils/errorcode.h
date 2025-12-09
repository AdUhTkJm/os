#ifndef ERRORCODE_H
#define ERRORCODE_H

// Note this is not in namespace os.
enum error_code {
  EPERM = 1,
  ENOENT = 2,
  ESRCH = 3,
  EINTR = 4,
  EIO = 5,
  ENXIO = 6,
  E2BIG = 7,
  ENOEXEC = 8,
  EBADF = 9,
  ECHILD = 10,
  EAGAIN = 11,
  ENOMEM = 12,
  EACCES = 13,
  EFAULT = 14,
  ENOTBLK = 15,
  EBUSY = 16,
  EEXIST = 17,
  EXDEV = 18,
  ENODEV = 19,
  ENOTDIR = 20,
  EISDIR = 21,
  EINVAL = 22,
  EROFS = 30,
  ERANGE = 34,
  ENOSYS = 38,
  ELOOP = 40,
  EDQUOT = 122,
};

#endif
