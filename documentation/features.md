# Fitur GenOS v3

Dokumen ini menjelaskan fitur yang saat ini ada di GenOS, serta fitur yang direncanakan untuk fase selanjutnya.

## Fitur Saat Ini

### CPU & Interrupt
- GDT v2 dengan 7 slot: Null, Kernel Code/Data, User Code/Data, TSS (64-bit two-slot).
- IDT untuk memetakan 32 exception CPU + 16 hardware IRQ ke penangan.
- ISR untuk menangani exception CPU (divide-by-zero, page fault, GPF, dll).
- **Page Fault Handler Diagnostik:** Membaca CR2 (faulting address), mendekode error code (Present/Write/User/Reserved/InstructionFetch), menampilkan RIP dan CS ke serial + layar.
- **GPF Handler Diagnostik:** Menampilkan error code (biasanya selector yang bermasalah), RIP, CS, RSP, SS untuk mempercepat debugging segment selector.
- PIC remapping untuk hardware interrupt (keyboard, timer).
- **Syscall Gateway (MSR SYSCALL/SYSRET):** Gate Ring-3 → Ring-0 via `syscall`/`sysretq`. 11 syscall tersedia:
  - `1 (print)`: cetak teks ke framebuffer + serial
  - `2 (exit)`: akhiri task dengan aman
  - `3 (read_key)`: baca karakter dari keyboard buffer (non-blocking)
  - `4 (sleep)`: tahan eksekusi selama N milidetik
  - `5 (screen_clear)`: bersihkan seluruh layar
  - `6 (print_at)`: cetak teks pada posisi (x,y) dengan warna
  - `7 (draw_char)`: gambar 1 karakter pada posisi (x,y)
  - `8 (read_file)`: baca file dari TAR ramdisk ke buffer user
  - `9 (exec)`: jalankan program ELF dari ramdisk; return PID child (>0) atau -1
  - `10 (fill_rect)`: isi blok persegi (x,y,w,h) dengan satu warna — dipakai shell untuk membersihkan baris sebelum menulis ulang teks (mencegah tumpang tindih karakter)
  - `11 (wait_pid)`: cek apakah task dengan PID tertentu sudah selesai (non-blocking; 0=alive, 1=dead). Dipakai shell untuk polling sampai child task selesai sebelum menampilkan prompt baru
- TSS (Task State Segment) untuk stack kernel darurat saat interrupt dari Ring-3.
- **Shell di Ring 3:** Terminal sistem berjalan sepenuhnya di user-space (`shell.elf` @ `0x50000000`), terisolasi dari kernel. Semua akses hardware melalui syscall.

### Manajemen Memori
- Physical Memory Manager (`PMM`) berbasis bitmap untuk alokasi RAM fisik.
- Virtual Memory Manager (`VMM`) dengan paging 4-level (PML4→PDPT→PD→PT).
- Heap allocator (`kmalloc` / `kfree`) dengan strategi First-Fit dan merge blok bebas.

### Driver & I/O
- Framebuffer untuk menulis teks/grafis ke layar dengan font 8x8.
- Driver keyboard PS/2 untuk input pengguna.
- Driver timer PIT (1000 Hz) untuk tick sistem dan context switch.
- Driver serial COM1 untuk debug output.

### Shell & Aplikasi
- Shell interaktif `Mandor@GenOS:~$` yang berjalan di layar GenOS.
- Perintah: `help`, `clear`, `info`, `read`, `run`.
- Perintah `run`: memuat dan menjalankan `app.elf` dari ramdisk dalam Ring-3.

### File System Awal
- Pembaca ELF (PT_LOAD segments) untuk memuat executable user-space.
- Pembaca TAR (POSIX ustar) untuk membaca file dari ramdisk.

### Multitasking & User Space
- **Scheduler Round-Robin** berbasis timer interrupt (IRQ 0) yang melakukan context switch.
- **User Mode (Ring 3):** Aplikasi ELF dapat dijalankan terisolasi dari kernel.
- `create_user_task()`: memuat ELF, memetakan stack user, dan mendaftarkan task ke scheduler.
- `exit_current_task()`: mengakhiri task aktif secara aman (state = TASK_DEAD).

## Log Perbaikan Bug (Bugfix Log)

### [2026-06-04] Bugfix Post-Update: Kernel Panic saat menjalankan User Mode

Setelah pembaruan fitur user mode & syscall, kernel mengalami panic. Berikut daftar bug yang ditemukan dan diperbaiki:

| # | File | Bug | Dampak | Perbaikan |
|---|------|-----|--------|-----------|
| 1 | `cpu/syscall_asm.S` | `push user_rsp_tmp` dan `mov %rsp, user_rsp_tmp` tanpa `(%rip)` — addressing mode bisa salah di `-mcmodel=kernel` | RSP user tidak tersimpan/terpulihkan dengan benar | Gunakan RIP-relative: `mov %rsp, user_rsp_tmp(%rip)` dan load ke register dulu |
| 2 | `kernel/task.c` | Scheduler `do-while` infinite loop jika semua task `TASK_DEAD` | CPU hang setelah semua task selesai | Tambahkan counter batas iterasi = jumlah total task |
| 3 | `kernel/kernel.c` | `syscall_init()` dipanggil **setelah** `create_user_task()` | App Ring-3 syscall sebelum gate terdaftar → #UD/#GP | Pindahkan `syscall_init()` sebelum `create_user_task()` |
| 4 | `mm/heap.c` | `pmm_alloc_page()` return NULL tidak dicek | Heap di-map ke fisik 0x0 → korupsi memori | Tambahkan null-check + FATAL halt |
| 5 | `kernel/task.c` | User stack RSP = `base + 4096` (di luar page) dan tidak 16-byte aligned | Page fault saat context switch ke user task | Ganti ke `(base + 4096 - 16) & ~0xF` |
| 6 | `cpu/syscall.c` | **STAR[63:48] = 0x23** — `sysretq` menghitung CS=(0x23+16)\|3=**0x33** dan SS=(0x23+8)\|3=**0x2B**, keduanya menunjuk ke slot TSS! | **#GP Kernel Panic** setiap kali `sysretq` dieksekusi | Ganti ke **0x10** → CS=(0x10+16)\|3=0x23 ✓, SS=(0x10+8)\|3=0x1B ✓ |
| 7 | `kernel/task.c` | User task `SS = 0x2B` = GDT[5] = **TSS descriptor**, bukan User Data! | **#GP Kernel Panic** saat `iretq` context switch ke user task | Ganti ke **0x1B** = GDT[3] = User Data |
| 8 | `cpu/syscall_asm.S` | Argument shuffle kanan-ke-kiri (`rdx→rcx, rsi→rdx, rdi→rsi, rax→rdi`) diperlukan karena konvensi syscall ≠ konvensi C | Argumen handler C diterima salah → pointer string invalid | Kembalikan shuffle 4-baris yang benar |

### [2026-06-05] Bugfix & Fitur: Shell Ring 3 Migration + Critical Assembly Bugs

**Fitur baru:** Shell dipindahkan dari kernel (Ring 0) ke user-space (Ring 3) sebagai ELF terpisah (`shell.elf`). 5 syscall baru ditambahkan (screen_clear, print_at, draw_char, read_file, exec). Total syscall: 9.

| # | File | Bug | Dampak | Perbaikan |
|---|------|-----|--------|-----------|
| 9 | `cpu/syscall_asm.S` | `mov user_rsp_tmp(%rip), %rax` menimpa RAX yang masih berisi **nomor syscall** | Semua syscall terdeteksi "tidak dikenal" (nomor = alamat user RSP) | Ganti ke `%r10` yang aman |
| 10 | `mm/vmm.c` | `vmm_map_page` tidak flush TLB setelah mengubah page table entry | Run app.elf kedua kali → `memcpy` menulis ke physical page LAMA → data korup → Page Fault | Tambah `invlpg` instruction setelah setiap PTE write |
| 11 | `kernel/task.c` | Semua user task pakai stack virtual `0x80000000` tetap | Task kedua menimpa mapping stack task pertama | Static counter `next_user_stack` dengan 64KB gap antar stack |
| 12 | `cpu/syscall_asm.S` | `pop %rax` di return path **menimpa return value** syscall handler dengan saved user RSP | `read_key()` selalu return alamat RSP (0x80000FF0), bukan 0. Shell banjir karakter sampah 0xF0, keyboard asli tak bisa masuk | Simpan return value ke `%rdi` sebelum pop, kembalikan ke `%rax` setelah RSP di-restore |
| 13 | `cpu/syscall.c` + `kernel/task.c` | Scheduler bisa context-switch **di dalam syscall handler** yang pakai stack terpisah (`kernel_stack_top`) | Return path `sysretq` rusak karena scheduler mengganggu kernel stack | Flag `in_syscall`: scheduler skip switch saat flag aktif |
| 14 | `cpu/syscall.c` | `FMASK=0x200` mematikan interrupt (IF=0) saat masuk syscall, keyboard IRQ tak bisa menyala | `read_key()` selalu return 0 karena buffer tak pernah terisi selama handler berjalan | `sti` di awal handler, `cli` sebelum return |

### [2026-06-05] Bugfix: Ramdisk Tidak Termuat & Teks Terminal Tumpang Tindih

| # | File | Bug | Dampak | Perbaikan |
|---|------|-----|--------|-----------|
| 15 | `limine.cfg` | Tidak ada entry `MODULE_PATH=` untuk `ramdisk.tar` | `module_request.response->module_count` = 0 → `tar_init()` tak dipanggil → `tar_read_file("shell.elf", ...)` selalu NULL → kernel halt dengan pesan `[FATAL] shell.elf not found in ramdisk!` | Tambahkan baris `MODULE_PATH=boot:///ramdisk.tar` di entry boot |
| 16 | `shell/shell.c`, `drivers/framebuffer.c`, `cpu/syscall.c`, `libc/stdio.c` | `terminal_print()` menggambar teks baru langsung di atas teks lama. Font 8x8 (scale 2 = cell 16x16) tidak mengisi seluruh tinggi baris terminal (24 px) → 8 px gap antar baris tidak pernah ditimpa, dan glyph sempit menyisakan piksel karakter sebelumnya | Saat perintah seperti `info` lalu `help` dijalankan, sisa karakter lama terlihat di sela-sela karakter baru (efek tumpang tindih / "ghosting") | Tambah primitif `fb_fill_rect()` di driver, syscall baru `10 = fill_rect`, libc `fill_rect()`. Shell sekarang selalu memanggil `terminal_clear_line()` sebelum mencetak baris, dan membersihkan cell tujuan sebelum `draw_char()` saat mengetik / backspace |
| 17 | `shell/shell.c`, `cpu/syscall.c`, `kernel/task.{c,h}`, `libc/stdlib.{c,h}` | Setelah `run` (exec), shell langsung mencetak prompt baru tanpa menunggu `app.elf` selesai. App menulis di koordinat absolut (Y=300..504) sementara shell mencetak prompt di Y rendah → ketika user mengetik, baris terminal turun & menumpuk di atas output app, dan respons app tetap "stuck" di layar | Prompt baru tidak muncul di bawah output app; karakter yang diketik menimpa tulisan app | `create_user_task()` sekarang mengembalikan `struct task*`. Syscall `9 (exec)` mengembalikan PID child. Tambahkan syscall `11 (wait_pid)` non-blocking. Shell di Ring 3 polling `wait_pid()` + `user_sleep(50)` sampai child DEAD, lalu `clear_screen()` & reset cursor sebelum menampilkan prompt baru. Polling dilakukan di Ring 3 (bukan blocking di kernel) karena `kernel_stack_top` di-share antar syscall — child yang melakukan syscall akan menimpa frame kernel shell yang sedang block |

## Fitur Yang Direncanakan

### Stabilitas dan Debug
- Logging yang lebih baik untuk crash dengan info register (RIP, CR2, CS).
- Handler page fault yang menampilkan alamat fault dari CR2.
- Mode debug serial yang dapat dinyalakan.

### Manajemen Memori Lanjutan
- Heap yang dapat berkembang dinamis (heap expansion).
- Heap dengan fragmentasi rendah (buddy allocator atau slab allocator).
- Proteksi memori antar proses (per-process page table).

### Driver Tambahan
- Driver disk SATA/AHCI.
- Driver mouse sederhana.

### File System & Storage
- Virtual File System (VFS) dasar.
- Dukungan FAT32 atau ext2.
- Load executable dari disk (bukan hanya ramdisk).

### Ekosistem dan Pengembangan
- Dokumentasi developer lengkap.
- Contoh modul dan aplikasi.
- Fitur otomasi build/test.

## Tujuan Fitur Utama

- Menjadikan GenOS sebagai proyek pembelajaran OS yang jelas.
- Memudahkan kontributor baru memahami kode dan menambahkan fitur.
- Menyediakan base kernel yang dapat diperluas untuk riset dan eksperimen.
- Menyusun OS agar siap digunakan sebagai referensi oleh model AI.
