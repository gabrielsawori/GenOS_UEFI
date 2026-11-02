CC = gcc
CFLAGS = -Wall -Wextra -O2 -pipe -ffreestanding -fno-stack-protector -fno-stack-check -fno-lto -fno-pic -m64 -march=x86-64 -mno-80387 -mno-mmx -mno-sse -mno-sse2 -mno-red-zone -mcmodel=kernel

LD = ld
LDFLAGS = -nostdlib -static -z max-page-size=0x1000 -T linker.ld
QEMU_CPUS ?= 4
QEMU_MEMORY ?= 512M

SRC_DIRS = kernel drivers cpu mm fs crypto security
C_SRCS = $(foreach dir, $(SRC_DIRS), $(wildcard $(dir)/*.c))
ASM_SRCS = $(foreach dir, $(SRC_DIRS), $(wildcard $(dir)/*.S))
OBJS = $(C_SRCS:.c=.o) $(ASM_SRCS:.S=.o)

# Daftar file libc yang digunakan oleh semua aplikasi user-space
LIBC_SRCS = libc/stdio.c libc/stdlib.c libc/string.c libc/crypto.c
LIBC_OBJS = $(LIBC_SRCS:.c=.o)

CFLAGS += -DGENOS_KERNEL -MMD -MP
# Userspace may use the standard x86-64 SSE2 ABI; kernel code must remain
# integer-only because syscall/interrupt entry borrows the current FP state.
USER_CFLAGS = $(filter-out -mno-80387 -mno-mmx -mno-sse -mno-sse2 -mcmodel=kernel -DGENOS_KERNEL,$(CFLAGS)) -mcmodel=small -msse2 -mfpmath=sse
-include $(OBJS:.o=.d) $(LIBC_OBJS:.o=.d) shell/shell.d app/app.d
.DEFAULT_GOAL := all

.PHONY: all clean run run-headless test-core test-user-memory test-context iso ramdisk

all: GenOS.iso

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@

$(LIBC_OBJS) shell/shell.o app/app.o: %.o: %.c
	$(CC) $(USER_CFLAGS) -c $< -o $@

kernel.elf: $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $^

# --- KOMPILASI SHELL SEBAGAI ELF USER-SPACE (Ring 3) ---
# Di-link di 0x50000000 agar tidak konflik dengan app.elf (0x40000000)
shell.elf: shell/shell.o $(LIBC_OBJS)
	$(LD) -nostdlib -Ttext 0x50000000 $^ -o $@

# --- KOMPILASI APLIKASI USER SEBAGAI ELF ---
app.elf: app/app.o $(LIBC_OBJS)
	$(LD) -nostdlib -Ttext 0x40000000 $^ -o $@

# --- BUNGKUS SEMUA KE RAMDISK ---
ramdisk.tar: shell.elf app.elf
	@echo "HELLO MANDOR! This is a secret from the outside world." > pesan.txt
	@tar -cvf ramdisk.tar pesan.txt shell.elf app.elf

GenOS.iso: kernel.elf ramdisk.tar
	mkdir -p iso_root
	cp kernel.elf iso_root/
	cp limine.cfg iso_root/
	cp ramdisk.tar iso_root/
	cp limine/limine-bios.sys limine/limine-bios-cd.bin limine/limine-uefi-cd.bin iso_root/ || true
	mkdir -p iso_root/EFI/BOOT
	cp limine/BOOTX64.EFI iso_root/EFI/BOOT/ || true
	xorriso -as mkisofs -b limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table --efi-boot limine-uefi-cd.bin -efi-boot-part --efi-boot-image --protective-msdos-label iso_root -o GenOS.iso
	./limine/limine bios-install GenOS.iso || true

run: GenOS.iso
	qemu-system-x86_64 -m $(QEMU_MEMORY) -smp $(QEMU_CPUS) -cdrom GenOS.iso -serial stdio

run-headless: GenOS.iso
	qemu-system-x86_64 -m $(QEMU_MEMORY) -smp $(QEMU_CPUS) -cdrom GenOS.iso -display none -monitor none -serial stdio

test-core:
	python3 tests/run_scheduler_boot_tests.py --suite core --timeout 45

test-user-memory:
	python3 tests/run_scheduler_boot_tests.py --suite umem --timeout 45

test-context:
	python3 tests/run_scheduler_boot_tests.py --suite context --timeout 45

clean:
	rm -f $(OBJS:.o=.d) $(LIBC_OBJS:.o=.d) app/app.d shell/shell.d
	rm -f $(OBJS) $(LIBC_OBJS) kernel.elf GenOS.iso ramdisk.tar pesan.txt
	rm -f app.elf app/app.o shell.elf shell/shell.o
	rm -rf iso_root
