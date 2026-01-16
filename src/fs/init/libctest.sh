/* 2> /dev/null #*/ R"SHELL(
cd /mnt/$libc
echo "#### OS COMP TEST GROUP START libctest-glibc ####"
grep -v "setvbuf_unget" ./run-static.sh | sh
grep -v "setvbuf_unget" ./run-dynamic.sh | sh
echo "#### OS COMP TEST GROUP END libctest-glibc ####"
# )SHELL"
