/* 2> /dev/null #*/ R"(
cd /mnt/$libc/ltp/testcases/bin
export PATH="$(pwd):$PATH"

started=0
ended=0

echo "#### OS COMP TEST GROUP START ltp-$libc ####"
ls | grep -E '^[a-z0-9_-]+[0-9][0-9]$' | while read -r f; do
  if [ "$started" -eq 0 ]; then
    [ "$f" = "$startfrom" ] && started=1 || continue
  fi
  # We don't `break` here - breaking fails strangely.
  if [ "$ended" -eq 1 ]; then continue; fi

  case "$f" in
    crash*) continue;;
    copy_file_range*) continue;;      # Doesn't end.
    cve*) continue;;
    epoll*) continue;;                # NOSYS
    exec*) continue;;
    fallocate05) continue;;           # No tmpfs size limit now. This will exhaust all memory.
    fallocate06) continue;;           # Exhausts all memory.
    fanotify*) continue;;             # NOSYS
    fcntl15*) continue;;
    fork14) continue;;                # Allocates 16TB mmap; not supported yet.
    ftruncate03*) continue;;
    ftest*) continue;;                # Doesn't end.
    futex_cmp_requeue*) continue;;
    getrusage03*) continue;;
    *xattr*) continue;;
    huge*) continue;;                 # Needs huge pages
    icmp*) continue;;                 # Needs locale info?
    kill02|kill10) continue;;         # Seems to fail but no idea why.
    memctl*|memcontrol*) continue;;
    mmap-corruption*) continue;;
    mmap001|mmap03|mprotect02) continue;; # munmap() needs to clear in-memory pages. TODO
    # h*) ended=1; continue;;
  esac
  echo "RUN LTP CASE $f"
  "./$f"
  echo "FAIL LTP CASE $f : $?"
done

echo "#### OS COMP TEST GROUP END ltp-$libc ####"
# )"