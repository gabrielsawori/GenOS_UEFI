# Validasi loader ELF — 22 Oktober 2026

Catatan terbaru: [memori user dinamis](user-memory.md) mengganti batas tracking
256 halaman dengan array yang bertumbuh hingga 65.536 halaman, termasuk stack.
Angka 252/256 di bawah mencatat kebijakan saat paket loader ini dibuat.
Fixture penolakan kapasitas sekarang mengikuti budget caller; pemeriksaan
format dan rollback tetap berlaku.

Loader sekarang menerima panjang binary dan memeriksa seluruh image sebelum
mengalokasikan halaman. Sebelumnya `phoff`, jumlah program header, offset data,
dan entry point langsung dipercaya; ELF terpotong dapat membuat kernel membaca
melewati buffer. Logika validasi baru berada di `fs/elf_validate.c`, dengan
integrasi kecil pada constructor task dan caller boot/exec.

| Modul | Perubahan |
|---|---|
| `fs/elf_validate.c` | Validasi murni tanpa alokasi, mapping, atau perubahan input; output opsional hanya ditulis saat sukses. |
| `fs/elf.c` | Memvalidasi sebelum memuat segmen; menerima kapasitas array tracking dari caller. |
| `kernel/task.c` | Memvalidasi sebelum alokasi task/stack/PML4; menyisihkan empat entri tracking untuk stack user. |
| `kernel/kernel.c`, `cpu/syscall.c` | Meneruskan ukuran dari `tar_read_file()` sampai constructor dan loader. Nomor/ABI syscall tetap sama. |

Constructor dan loader masing-masing memvalidasi: constructor menolak image
rusak sebelum alokasi resource apa pun, sedangkan loader tetap memeriksa input
jika dipanggil langsung. Buffer harus berupa memori kernel yang dapat dibaca
sepanjang ukuran yang diberikan dan tidak berubah selama operasi. Constructor
mempertahankan guard preemption yang sudah ada.

## Format yang didukung

Pemeriksaan header mengikuti field ELF64 pada
[System V ABI: ELF Header](https://gabi.xinuos.com/elf/02-eheader.html).
Pemeriksaan ukuran segmen, alignment, urutan `PT_LOAD`, dan zero-fill mengikuti
[System V ABI: Program Header](https://gabi.xinuos.com/elf/07-pheader.html).
Loader GenOS memakai subset yang lebih terbatas:

- ELF64 little-endian, x86-64, versi 1, executable statis `ET_EXEC`, OSABI
  System V atau GNU/Linux dengan ABI version nol. Penerimaan OSABI Linux
  tidak berarti syscall Linux didukung.
- Header berukuran 64 byte, entri program header 56 byte, dan 1–128 program
  header. Seluruh tabel dan data segmen harus berada di dalam panjang file;
  pengecekan memakai pengurangan agar penjumlahan offset/ukuran tidak overflow.
- Tabel section boleh dihilangkan. Jika ada, ukuran tabel dan indeks string
  table diperiksa. Isi section tidak dibaca atau dipakai untuk loading;
  extended counts/indices belum didukung.
- `PT_LOAD` nonkosong harus readable, terurut menurut VA, berada di lower
  canonical user space, tidak menyentuh page nol, dan memiliki `filesz <= memsz`.
  VA/offset harus kongruen modulo 4 KiB. Alignment 0/1 diterima; nilai lebih
  besar harus pangkat dua dan VA/offset kongruen terhadap alignment itu.
- Kebijakan GenOS menolak W+X, flag LOAD yang tidak dikenali, serta segmen
  yang berbagi halaman meskipun rentang byte tidak beririsan. Entry point
  wajib berada dalam byte file pada segmen executable, bukan BSS atau padding.
- Selain LOAD, yang diterima hanya `PT_NULL`, `PT_NOTE`, `PT_PHDR`,
  `PT_GNU_EH_FRAME`, `PT_GNU_PROPERTY`, serta `PT_GNU_STACK` tanpa executable
  flag. Metadata tersebut tidak diinterpretasi; rentang filenya diperiksa,
  kecuali `PT_NULL` yang field lainnya diabaikan sesuai ABI.
- Interpreter, dynamic linking, PIE/`ET_DYN`, TLS, `PT_GNU_RELRO`, dan jenis
  segmen lain ditolak. Kernel belum menyediakan runtime untuk fitur tersebut.

Dengan tracking task saat ini, image paling banyak memakai 252 halaman;
empat dari 256 entri dicadangkan untuk stack user. Perhitungan mencakup
pembulatan awal/akhir segmen yang tidak selaras halaman dan seluruh BSS.

## Kegagalan dan cleanup

ELF rusak ditolak sebelum task, stack, page table, atau page image dialokasikan.
ELF yang valid masih bisa gagal karena OOM, kegagalan mapping, atau benturan
dengan stack user. Loader mencatat kepemilikan setiap page sebelum mapping;
constructor membuang seluruh task yang belum dipublikasikan melalui jalur
cleanup yang sudah ada. PID baru hanya diberikan setelah konstruksi sukses.

`elf_load()` mengembalikan entry point atau nol. Caller langsung harus
membuang address space yang belum dipublikasikan beserta seluruh page yang
tercatat bila loading gagal. Loader tidak melakukan rollback sendiri.
Prefix/suffix alignment dan BSS dinolkan; data disalin hanya pada irisan
page dengan byte file segmen. Perhitungan selisih VA dilakukan sebelum
menambah pointer sumber agar pointer perantara tetap berada dalam buffer.

## Pengujian

Tes host menggunakan fixture bersama di `tests/elf_fixture.h` dan validator
produksi, tanpa PMM/VMM tiruan:

```sh
gcc -std=gnu11 -Wall -Wextra -Werror -O0 tests/elf_validate_test.c fs/elf_validate.c -o /tmp/genos-elf-o0
/tmp/genos-elf-o0 shell.elf app.elf
gcc -std=gnu11 -Wall -Wextra -Werror -O2 tests/elf_validate_test.c fs/elf_validate.c -o /tmp/genos-elf-o2
/tmp/genos-elf-o2 shell.elf app.elf
gcc -std=gnu11 -Wall -Wextra -Werror -O2 -fsanitize=undefined -fsanitize-undefined-trap-on-error tests/elf_validate_test.c fs/elf_validate.c -o /tmp/genos-elf-ubsan
/tmp/genos-elf-ubsan shell.elf app.elf
python3 tests/run_memory_boot_tests.py
python3 tests/run_scheduler_boot_tests.py
python3 tests/run_uaccess_boot_tests.py
python3 tests/run_smp_boot_tests.py
```

`shell.elf` dan `app.elf` dihasilkan oleh build proyek. Tes host menempatkan
akhir buffer tepat sebelum guard page, memeriksa 44 kasus penolakan, batas
kapasitas, header/tabel tidak selaras, section table opsional, serta 20.000
mutasi/truncation deterministik. Input harus tetap utuh; output validator
tidak boleh berubah ketika gagal.

Tes host lulus pada `-O0`, `-O2`, dan UBSan mode trap. Mode trap tidak
memerlukan runtime sanitizer. Build gabungan ASan/UBSan biasa gagal link
karena `libasan.so.8.0.0` dan `libubsan.so.1.0.0` tidak tersedia di lingkungan
ini; ASan belum dijalankan. Guard page menguji batas buffer yang dinyatakan,
tetapi tidak menggantikan seluruh pemeriksaan ASan.

Suite memori QEMU memeriksa penolakan 44 kasus sebelum alokasi, segmen rusak
terakhir sebelum mapping pertama, permission code/data, byte segmen tidak
selaras, BSS lintas page, dan segmen BSS tanpa data file. Fault injection
menggagalkan setiap alokasi page dan setiap mapping pada konstruksi image
uji. Benturan ELF dengan stack user juga harus rollback tanpa PID atau
page tertinggal. Program Ring 3 menjalankan syscall exec terhadap
`truncated.elf` sepanjang 64 byte dan harus menerima `-1`.

Hasil regresi pada build terakhir, 22 Oktober 2026:

| Suite | Hasil |
|---|---|
| Memori QEMU | 6/6; mencakup validasi/layout ELF, OOM/mapping rollback, exec terpotong, dan regresi CoW/SHM/RO/NX. |
| Scheduler QEMU | 6/6; fixture OOM constructor kini memakai ELF valid agar tetap menguji kegagalan allocator. |
| Uaccess QEMU | 6/6; wrapper constructor meneruskan panjang file. |
| Boot SMP QEMU | 6/6; termasuk shell proyek, fallback, urutan terbalik, 17 vCPU, dan kegagalan stack AP. |

Total 24 skenario QEMU lulus. Suite memori/scheduler/uaccess memakai 1/2/4/8
vCPU pada 512 MB serta 4 vCPU pada 64/128 MB. Build kernel dan fixture tidak
menghasilkan warning compiler; sintaks runner Python dan whitespace perubahan
juga diperiksa. Pengujian QEMU memakai TCG dan bukan benchmark performa.

## Batasan

Validasi ini mempercayai bahwa caller memberikan buffer yang benar-benar
tersedia sepanjang `binary_size`. Parser TAR masih membutuhkan audit ukuran
arsip/header sendiri; patch ini tidak menjamin arsip TAR rusak aman dibaca.
ELF dengan segment type yang belum didukung ditolak secara sengaja.

Tracking tetap 256 page, stack masih memakai allocator VA yang lama, dan
penjadwalan task tetap hanya pada BSP. Reuse VA, heap alignment, fairness,
serta aktivasi scheduler AP tetap mengikuti
[roadmap scheduler/memori](scheduler-memory-plan.md). Paket ini memperbaiki
correctness, tanpa klaim peningkatan performa.
