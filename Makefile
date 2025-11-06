CROSS=riscv64-unknown-elf
CC=$(CROSS)-gcc
AS=$(CROSS)-as
LD=$(CROSS)-ld
OBJCOPY=$(CROSS)-objcopy
CFLAGS=-O0 -g -Wall -nostdlib -march=rv64imac -mabi=lp64 -mcmodel=medany

all: kernel.elf

kernel.elf: start.s main.c link.ld
	$(CC) $(CFLAGS) -T link.ld -o kernel.elf start.s main.c

run: kernel.elf
	qemu-system-riscv64 -nographic -machine virt -bios none -kernel kernel.elf

clean:
	rm -f kernel.elf
