# Context FP dan exception aplikasi — 2 November 2026

Tahap ini melanjutkan fondasi SMP dan memori user pada QEMU x86-64. Task
sekarang membawa context x87/MMX/SSE sendiri. Exception aplikasi yang tidak
bisa dipulihkan menghentikan grup address space, lalu reaper membebaskan
resource-nya. DE dan driver disk/jaringan belum menjadi bagian tahap ini.

## Context FPU/SSE

`cpu/fpu.c` mengaktifkan x87, FXSAVE dan SSE2 pada BSP serta setiap AP sebelum
scheduler mulai berjalan. CR0.EM/TS dibersihkan, MP/NE diaktifkan, dan CR4.OSFXSR
serta OSXMMEXCPT diaktifkan. CR4.OSXSAVE tetap mati: AVX/AVX-512/AMX belum masuk
kontrak context yang disimpan kernel.

Setiap task, termasuk idle dan kernel worker, memiliki area FXSAVE64 sebesar
512 byte dengan alignment 16 byte. Scheduler menyimpan context lama dan
memulihkan context baru setiap kali memilih task berbeda. Ini juga berlaku
saat task tidur di dalam syscall. Tidak ada lazy ownership atau handler #NM
untuk menunda restore. Kernel harus tetap dibangun tanpa floating point/SIMD;
penggunaan register FP di kernel hanya boleh melalui helper context eksplisit.

ELF baru mendapat x87 control word `0x037f`, MXCSR `0x1f80`, stack x87 kosong,
dan payload register nol. Mengandalkan FNINIT saja tidak cukup untuk
menghapus payload yang bisa dibaca task berikutnya lewat FXSAVE. Fork dan
clone mengambil snapshot **register CPU yang sedang hidup**, bukan salinan
yang mungkin berasal dari switch sebelumnya. Setelah itu state tiap task
berdiri sendiri. Clone mewarisi environment FP pembuatnya; task hasil exec
memulai environment baru.

Makefile memisahkan `USER_CFLAGS` dari flag kernel. Aplikasi dan libc kini
dapat menggunakan SSE2 dan ABI floating point x86-64; kernel C tetap integer.
Target ISA tetap x86-64 dasar, bukan `-march=native`.

Referensi format dan konfigurasi CPU:
[Intel SDM volume 2A](https://cdrdv2-public.intel.com/812383/253666-sdm-vol-2a.pdf)
dan [volume 3A](https://cdrdv2-public.intel.com/812386/253668-sdm-vol-3a.pdf).

## Exception dan lifetime

Dispatcher menerima fault aplikasi hanya bila saved CS menunjukkan CPL3 dan
current task memiliki address space user. Vector sinkron yang ditangani adalah
0, 1, 3–7, 10–14, 16, 17, 19, dan 21. Tidak ada ABI signal/debugger; breakpoint
mengakhiri aplikasi sebagaimana exception lain. Gate INT3 boleh dipanggil dari
Ring 3; gate IRQ tetap hanya untuk kernel.

Page fault tetap lebih dahulu mencoba resolusi CoW atau fixup copy user yang
sudah ditandai. Reserved-bit violation pada page table selalu fatal, termasuk
ketika akses berasal dari CPL3. NMI, double fault, machine check, vector reserved,
dan exception kernel tetap masuk jalur fatal. Ini belum panic rendezvous yang
menghentikan seluruh CPU secara terkoordinasi.

Fault yang tidak pulih mencatat CPU/PID/vector/RIP/error/CR2 pada serial tanpa
menggambar pesan aplikasi ke framebuffer. Semua task dengan PML4 yang sama
langsung ditandai DEAD di bawah lock mutasi kernel, termasuk ketika thread
non-leader yang fault. Waiter dibangunkan dan CPU lain berhenti menjalankan
anggota grup pada entry scheduler/syscall berikutnya. Stack yang masih aktif
atau sedang ditinggalkan tetap dipin sampai reaper aman membebaskannya.

Diagnostik #GP hanya membaca frame yang diketahui valid; tidak lagi menebak
bahwa ada frame IRET kedua atau membaca stack melewati frame tersebut.
Exit status/signal delivery, TLS, ownership address space dengan refcount
tersendiri, run queue per CPU, dan lock per subsistem masih pekerjaan berikutnya.

## Pengujian

```sh
make test-context
python3 tests/run_scheduler_boot_tests.py --suite context --log-dir .test-results/serial --timeout 45
python3 tests/run_scheduler_boot_tests.py --suite context --fatal-probe kernel-ud --case quad --timeout 8
python3 tests/run_scheduler_boot_tests.py --suite context --fatal-probe user-nmi --case quad --timeout 8
python3 tests/run_scheduler_boot_tests.py --suite context --fatal-probe user-mce --case quad --timeout 8
python3 tests/run_scheduler_boot_tests.py --suite context --fatal-probe user-rsvd --case quad --timeout 8
```

Suite context menjalankan dua thread user per CPU, masing-masing 64 putaran
preemption dan sleep dengan pola berbeda pada delapan register x87, seluruh
XMM0–15, control word, dan MXCSR. Worker kernel ikut menukar pola pada semua
CPU. Tes lain memeriksa aritmetika `double` yang dikompilasi memakai flag user,
snapshot fork/clone, ELF baru dengan register bersih, dan proses induk yang
terus berjalan ketika anak-anaknya fault.

Probe native meliputi divide error integer, UD2, CLI terlarang, alamat unmapped,
INT3, single-step, divide error x87, stack noncanonical, INT IRQ terlarang, dan
eksekusi halaman NX. Probe terakhir memicu UD2 pada thread dalam grup empat
task. Setelah reaper selesai, frame fisik kembali ke baseline dan hanya stack
checker serta reaper yang tersisa. Tes ini mencakup seluruh matriks
1/2/4/8 vCPU dengan RAM 512 MiB dan 4 vCPU dengan RAM 64/128 MiB.

Batas emulator yang benar-benar diamati pada QEMU 10.2.2 TCG:

- Pembagian nol SSE menandai ZE pada MXCSR tetapi tidak mengirim #XM. Tes
  memverifikasi ZE dan mask-nya, lalu memakai syscall **fixture saja** untuk
  mengirim saved user frame vector 19 ke dispatcher produksi. Handler teruji;
  pengiriman exception SIMD oleh hardware belum divalidasi. Masalah emulasi
  sejenis tercatat di [QEMU issue 215](https://gitlab.com/qemu-project/qemu/-/issues/215).
- PUSH dengan RSP noncanonical dilaporkan sebagai #GP. Fixture menerima #SS
  atau #GP untuk alamat stack tepat yang dipasang probe tersebut.
- Negative control UD kernel adalah instruksi nyata. NMI/machine check dari
  CPL3 dan reserved-bit page fault memakai frame injeksi, untuk memeriksa
  keputusan dispatcher. Ini bukan uji pengiriman NMI/MCE hardware.

Runner tidak lagi memakai `-no-shutdown` bersama `isa-debug-exit`: QEMU harus
keluar sendiri dengan status debug-exit sukses. Timeout selalu menggagalkan
suite normal. Negative control justru harus bertahan di handler fatal hingga
batas observasi, tanpa kembali ke fixture atau mematikan grup user.
