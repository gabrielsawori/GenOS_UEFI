# Regresi boot SMP

Perbaikan di `cpu/smp.c`, `cpu/smp_asm.S`, dan `cpu/percpu.c`:

- Pergantian RSP dipindahkan dari inline assembly di tengah fungsi C ke
  trampoline assembly. Trampoline memulai frame C baru dengan alignment SysV.
  Sebelumnya compiler dapat mengakses variabel lokal lewat stack yang sudah
  diganti, sehingga hasil bergantung pada alokasi register dan optimisasi.
- BSP selalu menempati indeks CPU 0, sesuai asumsi per-CPU GDT/TSS dan IST.
  Seluruh daftar Limine diperiksa untuk menemukan BSP, termasuk ketika
  posisinya melampaui batas `MAX_CPUS`. LAPIC ID BSP tidak diasumsikan nol.
- LAPIC dan metadata BSP diinisialisasi juga pada fallback tanpa respons SMP,
  serta sebelum AP diluncurkan. Sebelumnya fallback melewatkan inisialisasi
  sehingga akses timer memakai basis MMIO nol; beberapa AP juga dapat
  menginisialisasi basis LAPIC bersama secara bersamaan.
- Pembacaan ID CPU sebelum inisialisasi tidak mengakses MMIO. Status AP dibaca
  secara atomik dan log pelepasan timer menghitung AP yang berhasil boot.
- Buffer konversi angka menyediakan ruang terminator untuk alamat 64-bit.
  Dekode extended model CPUID menggunakan base family sebelum ditambah extended
  family; string vendor disalin tanpa cast pointer yang bergantung alignment.

## Tes host

Jalankan dari root proyek:

```sh
gcc -std=gnu11 -Wall -Wextra -Werror -O2 -fno-builtin \
  -ffunction-sections -fdata-sections -fstack-protector-all \
  tests/smp_test.c kernel/utils.c -Wl,--gc-sections \
  -o /tmp/genos-smp-test
/tmp/genos-smp-test
```

Ulangi dengan `-O0` untuk build tanpa optimisasi. Tes meliputi 210 susunan CPU,
respons SMP kosong/tidak lengkap, pemetaan ID LAPIC yang tidak berurutan,
pembacaan ID sebelum LAPIC siap, dan pencetakan alamat XSDT 64-bit penuh.
Kode yang mengakses hardware dibuang oleh linker pada tes host.

## Tes boot QEMU

Memerlukan tool build proyek, Python 3, QEMU, xorriso, dan file bootloader
di `limine/` seperti build ISO biasa:

```sh
python3 tests/run_smp_boot_tests.py
```

Runner membuat kernel instrumentasi dan ISO di direktori sementara. Enam kasus
yang diperiksa: satu CPU, empat CPU, fallback tanpa SMP, urutan CPU terbalik,
17 CPU dengan BSP di akhir daftar, dan kegagalan alokasi seluruh stack AP.
Wrapper linker memeriksa alignment/alamat stack AP, identitas dan status CPU,
inisialisasi LAPIC, timer setiap CPU aktif, serta tercapainya context switch
pertama. Kernel produksi tidak memuat wrapper ini.

Untuk menjalankan satu kasus atau memperpanjang waktu observasi:

```sh
python3 tests/run_smp_boot_tests.py --case fallback --timeout 30
```

Tes QEMU memverifikasi boot dan awal eksekusi shell; tes ini tidak menggantikan
pengujian hardware fisik atau beban kerja jangka panjang.
