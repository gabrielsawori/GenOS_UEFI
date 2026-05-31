CC = gcc
CFLAGS = -Wall -Wextra -O2 -pipe -ffreestanding -fno-stack-protector -fno-stack-check -fno-lto -fno-pic -m64 -march=x86-64 -mno-80387 -mno-mmx -mno-sse -mno-sse2 -mno-red-zone -mcmodel=kernel

LD = ld
LDFLAGS = -nostdlib -static -z max-page-size=0x1000 -T linker.ld

SRC_DIRS = kernel drivers cpu mm fs
C_SRCS = $(foreach dir, $(SRC_DIRS), $(wildcard $(dir)/*.c))
ASM_SRCS = $(foreach dir, $(SRC_DIRS), $(wildcard $(dir)/*.S))
OBJS = $(C_SRCS:.c=.o) $(ASM_SRCS:.S=.o)

.PHONY: all clean run iso ramdisk

all: GenOS.iso

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@

kernel.elf: $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $^

# --- KOMPILASI APLIKASI USER SEBAGAI ELF ---
app.elf:
	mkdir -p app libc
	$(CC) $(CFLAGS) -c libc/stdio.c -o libc/stdio.o
	$(CC) $(CFLAGS) -c libc/stdlib.c -o libc/stdlib.o
	$(CC) $(CFLAGS) -c app/app.c -o app/app.o
	$(LD) -nostdlib -Ttext 0x40000000 app/app.o libc/stdio.o libc/stdlib.o -o app.elf

# --- BUNGKUS APLIKASI KE RAMDISK ---
ramdisk.tar: app.elf
	@echo "HELLO MANDOR! This is a secret from the outside world." > pesan.txt
	@tar -cvf ramdisk.tar pesan.txt app.elf

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
	qemu-system-x86_64 -m 2G -cdrom GenOS.iso -serial stdio

clean:
	rm -f $(OBJS) kernel.elf GenOS.iso ramdisk.tar pesan.txt app.bin app.elf app/app.o libc/stdio.o
	rm -rf iso_root