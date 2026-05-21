CC = gcc
LD = ld

# Flag Kompilasi 64-bit (Standar Bare-metal OS)
CFLAGS = -Wall -Wextra -O2 -pipe -ffreestanding -fno-stack-protector -fno-stack-check -fno-lto -fno-pic -m64 -march=x86-64 -mno-80387 -mno-mmx -mno-sse -mno-sse2 -mno-red-zone -mcmodel=kernel

# Flag Linking memanggil linker.ld
LDFLAGS = -nostdlib -static -z max-page-size=0x1000 -T linker.ld

# Daftar folder tempat file source code kita berada
SRC_DIRS = kernel drivers fs cpu mm

# Deteksi file .c dan .S secara otomatis di semua folder di atas
CFILES = $(foreach dir, $(SRC_DIRS), $(wildcard $(dir)/*.c))
SFILES = $(foreach dir, $(SRC_DIRS), $(wildcard $(dir)/*.S))

# Mengubah ekstensi file sumber menjadi target objek (.o)
OBJFILES = $(CFILES:.c=.o) $(SFILES:.S=.o)

.PHONY: all clean run

all: GenOS.iso

# Proses Kompilasi untuk file C (.c)
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Proses Kompilasi untuk file Assembly (.S)
%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@

# Proses Linking menjadi kernel.elf
kernel.elf: $(OBJFILES)
	$(LD) $(LDFLAGS) -o $@ $^

# Proses Perakitan Bootable ISO
GenOS.iso: kernel.elf
	rm -rf iso_root
	mkdir -p iso_root/EFI/BOOT
	cp kernel.elf iso_root/
	cp limine.cfg limine/limine-bios.sys limine/limine-bios-cd.bin limine/limine-uefi-cd.bin iso_root/
	cp limine/BOOTX64.EFI iso_root/EFI/BOOT/
	cp limine/BOOTIA32.EFI iso_root/EFI/BOOT/
	xorriso -as mkisofs -R -J -b limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		--efi-boot limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		iso_root -o GenOS.iso
	./limine/limine bios-install GenOS.iso

# Membersihkan file sementara di semua folder
clean:
	rm -f $(OBJFILES) kernel.elf GenOS.iso
	rm -rf iso_root

# Menjalankan GenOS v3 di QEMU dengan Serial Debugging
run: GenOS.iso
	qemu-system-x86_64 -m 2G -cdrom GenOS.iso -serial stdio