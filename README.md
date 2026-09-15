# GenOS v4 — Security Kernel (64-Bit x86-64 UEFI)

> Kernel minimalis yang didesain untuk **cybersecurity** dan **backend development**, dibangun dari nol di atas arsitektur x86-64 Long Mode dengan bootloader Limine (UEFI/BIOS).

---

## ✨ Highlights

| Fitur | Detail |
|-------|--------|
| 🔒 **SMEP** | Kernel tidak bisa execute kode user — blokir ret2usr |
| 🛡️ **NX / W^X** | Stack & heap non-executable — blokir shellcode injection |
| 🔑 **AES-256-CBC** | Enkripsi simetris bawaan kernel |
| 🔐 **SHA-256 / HMAC / PBKDF2** | Hashing & key derivation |
| 🎲 **CSPRNG** | Hardware RNG (RDRAND) + xoshiro256** |
| 📜 **Scroll Terminal** | 256-line history + Page Up/Down |
| 🧱 **Ring 3 Isolation** | Shell berjalan sepenuhnya di user-space |

---

## 🏗️ Arsitektur

```
┌─────────────────────────────────────────────────────┐
│                    RING 3 (User)                    │
│   shell.elf ── Security Terminal (scroll, crypto)   │
│   app.elf   ── Test Application                     │
├─────────────────────────────────────────────────────┤
│                 SYSCALL BOUNDARY                    │
│         49 syscalls + pointer validation            │
├─────────────────────────────────────────────────────┤
│                    RING 0 (Kernel)                  │
│                                                     │
│   CPU       GDT/TSS · IDT · PIC · LAPIC · SMP      │
│   Memory    PMM · VMM (4-level paging + NX) · Heap  │
│   Filesystem VFS · TAR ramdisk · tmpfs · cache      │
│   Crypto    AES-256 · SHA-256 · HMAC · PBKDF2 · RNG│
│   Security  Credentials · Keystore · SMEP · uaccess │
│   Drivers   Framebuffer · Keyboard · Timer · Serial │
│   Task      Preemptive scheduler · fork · clone     │
│   IPC       Shared memory (POSIX-like)              │
└─────────────────────────────────────────────────────┘
```

---

## 🔐 Security Features

### Hardware Security
- **SMEP** (CR4 bit 20) — CPU menolak eksekusi kode user dari Ring 0
- **NX bit** (EFER.NXE) — Page Table Entry bit 63 aktif pada stack, heap, data
- **W^X Enforcement** — Halaman yang writable TIDAK executable, dan sebaliknya

### Software Security
- **User Pointer Validation** (`cpu/uaccess.h`) — Semua 20+ syscall yang menerima pointer divalidasi terhadap batas canonical x86-64 (`0x800000000000`)
- **Kernel Heap Isolated** — Syscall `kmalloc/kfree/krealloc` dihapus dari Ring 3
- **PBKDF2 Authentication** — Login dengan password hashing
- **Encrypted Keystore** — Key-value store terenkripsi AES-256

### Cryptography
| Algoritma | Implementasi |
|-----------|-------------|
| AES-256-CBC | `crypto/aes.c` — Enkripsi/dekripsi simetris |
| SHA-256 | `crypto/sha256.c` — Hash 256-bit |
| HMAC-SHA256 | `crypto/hmac.c` — Message authentication |
| PBKDF2 | `security/cred.c` — Key derivation dari password |
| CSPRNG | `crypto/random.c` — RDRAND + xoshiro256** fallback |

---

## 💻 Terminal Commands

```
  System:
    help        Daftar semua command
    clear       Bersihkan terminal + history
    info        Informasi sistem
    run         Jalankan app.elf

  Filesystem:
    ls          Daftar file (ramdisk + tmpfs)
    cat <f>     Baca isi file
    read        Baca pesan.txt
    write <f> <text>   Tulis ke tmpfs
    rm <f>      Hapus file tmpfs

  Security:
    whoami      Info user aktif
    login <u>   Autentikasi (password masked)
    hash <t>    SHA-256 hash
    random      Generate 128-bit random
    encrypt <t> Demo AES-256-CBC

  IPC & Process:
    shm         Demo shared memory
    cache       Statistik buffer cache
    fork        Demo fork process

  Power:
    shutdown    Matikan sistem
    restart     Reboot
```

**Navigasi:** Page Up / Page Down untuk scroll history (256 baris).

---

## 🚀 Build & Run

### Dependencies

```bash
# Ubuntu/Debian
sudo apt install build-essential xorriso qemu-system-x86 mtools

# Arch Linux
sudo pacman -S base-devel xorriso qemu-full mtools
```

Tools yang dibutuhkan: `gcc`, `ld`, `make`, `xorriso`, `qemu-system-x86_64`

### Compile & Boot

```bash
# Clone repository
git clone <repo-url>
cd GenOS_v3

# Siapkan Limine bootloader (jika belum ada)
git clone https://github.com/limine-bootloader/limine.git \
    --branch v7.x-branch-latest --depth=1
make -C limine

# Build GenOS
make

# Jalankan di QEMU
make run

# Bersihkan build artifacts
make clean
```

### Output Build

```
kernel.elf  — Kernel Ring 0 (linked at HHDM)
shell.elf   — Security Terminal Ring 3 (linked at 0x50000000)
app.elf     — Test Application Ring 3 (linked at 0x40000000)
ramdisk.tar — TAR archive (shell.elf + app.elf + pesan.txt)
GenOS.iso   — Bootable ISO (UEFI + BIOS)
```

---

## 📁 Struktur Kode

```
GenOS_v3/
├── kernel/           Kernel entry point, task scheduler, utils
│   ├── kernel.c      Boot flow + hardware init + SMEP/NXE
│   └── task.c        Preemptive scheduler, fork, clone
├── cpu/              CPU management
│   ├── gdt.c         GDT + TSS (Ring 0/3 segments)
│   ├── idt.c         IDT (256 interrupt gates)
│   ├── isr.c         ISR handlers (exceptions + IRQs)
│   ├── isr_asm.S     ISR stubs (push regs → call C handler)
│   ├── syscall.c     49 syscalls + uaccess validation
│   ├── uaccess.h     User pointer validation module
│   ├── pic.c         Legacy 8259 PIC
│   ├── lapic.c       Local APIC + SMP
│   └── smp.c         Multi-core CPU detection
├── mm/               Memory management
│   ├── pmm.c         Physical page allocator (bitmap)
│   ├── vmm.c         4-level paging + NX bit support
│   ├── heap.c        Kernel heap (kmalloc/kfree)
│   └── shm.c         POSIX-like shared memory IPC
├── fs/               Filesystem
│   ├── vfs.c         Virtual filesystem layer
│   ├── tar.c         TAR ramdisk parser
│   ├── tmpfs.c       In-memory temporary filesystem
│   ├── elf.c         ELF64 loader (W^X page mapping)
│   └── cache.c       Block cache with LRU eviction
├── crypto/           Cryptography
│   ├── aes.c         AES-256-CBC (S-box, MixColumns)
│   ├── sha256.c      SHA-256 (FIPS 180-4)
│   ├── hmac.c        HMAC-SHA256
│   └── random.c      CSPRNG (RDRAND + xoshiro256**)
├── security/         Security subsystem
│   ├── cred.c        User credentials + PBKDF2 auth
│   └── keystore.c    Encrypted key-value store
├── drivers/          Hardware drivers
│   ├── framebuffer.c Pixel/text rendering (8x8 bitmap font)
│   ├── keyboard.c    PS/2 keyboard (scancode → ASCII)
│   ├── timer.c       PIT timer (preemptive scheduling)
│   └── serial.c      COM1 serial debug output
├── shell/            User-space terminal
│   └── shell.c       Security shell (scroll buffer, commands)
├── libc/             Minimal C library (Ring 3)
│   ├── stdio.c       Syscall wrappers (print, file I/O)
│   ├── stdlib.c      malloc (bump allocator), exec, fork
│   ├── string.c      String operations
│   └── crypto.c      Crypto syscall wrappers
├── app/              Test application
│   └── app.c         Sample Ring 3 program
├── Makefile          Build system
├── linker.ld         Kernel linker script
└── limine.cfg        Bootloader configuration
```

---

## 📜 Lisensi

MIT License — bebas dipelajari, dimodifikasi, dan didistribusikan.

> ⚠️ **PERINGATAN:** Selalu gunakan QEMU/VirtualBox untuk eksperimen. Jangan install di hardware fisik yang berisi data penting.