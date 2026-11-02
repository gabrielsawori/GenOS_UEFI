# Akses memori user — 21 Oktober 2026

Paket ini melanjutkan [koreksi memori](memory-correctness.md) pada tahap 2
[roadmap scheduler/memori](scheduler-memory-plan.md). Pointer buffer syscall
yang tidak terpetakan, menunjuk halaman supervisor, atau tidak memiliki izin
tulis sekarang menghasilkan error. Kegagalan alokasi saat kernel menulis
halaman CoW juga kembali sebagai error tanpa menghentikan kernel.

## Modul dan alur

| Modul | Tanggung jawab |
|---|---|
| `cpu/uaccess.c`, `cpu/uaccess.h` | Validasi rentang/mapping, copy buffer dan string, persiapan CoW, pemeriksaan recovery. |
| `cpu/uaccess_asm.S` | Instruksi copy dengan dua entri recovery khusus untuk load/store user. |
| `mm/vmm.c` | Pemeriksaan izin efektif USER/WRITE pada semua tingkat page table; menolak huge mapping yang belum didukung. |
| `cpu/syscall_io.c` | Buffer kernel perantara untuk read/write/readdir dan perlindungan posisi FD saat copy gagal. |
| `cpu/syscall_crypto.c` | Snapshot parameter AES/keystore, validasi pointer bersarang, buffer input/output kriptografi. |
| `cpu/syscall.c`, `cpu/isr.c` | Dispatcher tetap di tempatnya; memanggil API copy/adaptor dan recovery yang baru. |

`copy_from_user()` dan `copy_to_user()` memeriksa seluruh rentang sebelum
menyalin, termasuk PRESENT dan USER pada semua ancestor. Write membutuhkan
WRITE pada ancestor dan WRITE atau CoW pada leaf. RO biasa tidak dinaikkan
izinnya. Null ditolak untuk akses nonzero, begitu juga overflow, alamat kernel,
dan alamat noncanonical. Copy sepanjang nol berhasil tanpa dereference.

Guard preemption mencakup validasi hingga copy, sehingga task/thread lain tidak
dapat mengubah mapping di antaranya pada BSP. Timer tetap aktif. Ini belum
menyediakan sinkronisasi lintas CPU atau dukungan SMAP.

Ketika CPU mengirim page fault, ISR mencoba resolver CoW terlebih dahulu.
Jika gagal, recovery hanya diizinkan bila RIP cocok dengan instruksi copy yang
terdaftar, fault berasal dari kernel dalam guard, arah akses cocok, dan CR2
cocok dengan operand user. Reserved-bit, instruction-fetch, alamat kernel,
serta instruksi lain tidak mendapat recovery. ISR mengganti RIP ke jalur
return `-1`; tidak ada state recovery global yang dapat tertukar antar task.
Kernel fault di luar jalur ini berhenti setelah diagnostik. Sebelumnya jalur
fatal page fault dapat kembali dan mengulang instruksi yang sama.

## Kontrak syscall

String disalin satu kali ke kernel dan harus memiliki terminator dalam batas:
print/password/passphrase maksimal 4.096 byte termasuk NUL, filename 100 byte,
username dan nama key 32 byte. String tanpa terminator ditolak, tidak dipotong.
Scanner tidak membaca halaman berikutnya jika NUL sudah ditemukan di akhir
halaman saat ini.

VFS, framebuffer, credential, dan fungsi kriptografi menerima buffer kernel.
Struct AES/keystore disalin terlebih dahulu; key, IV, input, output, nama, dan
pointer panjang output diperiksa melalui API copy masing-masing. Hasil statistik,
informasi scheduler/layar, username, dan read_file juga memakai `copy_to_user()`.
Jalur read_file yang tidak menemukan file tetap melalui epilog syscall
(`cli` dan penghapusan flag tracing), tidak lagi return lebih awal.

Read/write memakai buffer perantara 1 KiB dan menolak count di atas `INT_MAX`,
sesuai tipe hasil VFS. Kegagalan setelah sebagian transfer dapat menghasilkan
short read/write. Read dan readdir menyiapkan halaman CoW output sebelum
mengubah posisi FD; jika copy masih gagal, posisi untuk bagian tersebut
dikembalikan. Buffer nama readdir harus berukuran minimal 100 byte. Read/write
dengan count nol tidak mengakses pointer user, tetapi tetap memeriksa FD.
Wrapper libc `readdir(int*)` kini menerima ukuran 64-bit ABI ke variabel
sementara, memeriksa `INT_MAX`, lalu menulis empat byte ke caller. Sebelumnya
cast alamat tetap membuat kernel menulis delapan byte ke objek `int`.

Random mempertahankan batas 4.096 byte. SHA memproses input per 1 KiB dan
mendukung input kosong. AES dibatasi 64 KiB plaintext (ciphertext maksimal
64 KiB + satu blok padding) agar alokasi sementara terbatas dan panjang tidak
overflow. Nomor syscall dan layout struct ABI tidak berubah. Buffer sensitif
sementara pada adaptor kriptografi dibersihkan sebelum dilepas.

Copy bukan transaksi: kegagalan CoW/OOM di halaman berikutnya dapat terjadi
setelah prefix tersalin. Pemanggil tidak boleh memakai hasil `copy_from_user`
yang gagal. Persiapan write yang gagal juga dapat meninggalkan sebagian halaman
CoW sudah menjadi private, tetapi setiap referensi tetap terlacak. Snapshot
per chunk bukan jaminan seluruh buffer panjang tidak berubah selama hashing.

## Validasi

```sh
python3 tests/run_uaccess_boot_tests.py
python3 tests/run_memory_boot_tests.py
python3 tests/run_scheduler_boot_tests.py
python3 tests/run_smp_boot_tests.py
```

Suite uaccess memakai harness QEMU yang sama dengan tes memori/scheduler:
1/2/4/8 vCPU pada 512 MB serta 4 vCPU pada 64/128 MB. Fixture tidak masuk kernel
produksi. Cakupannya:

- Null, alamat kernel/noncanonical, overflow panjang, rentang melintasi halaman
  RO/tidak terpetakan, serta USER/WRITE pada setiap tingkat tabel.
- String tanpa NUL dan NUL tepat di akhir halaman.
- Fault CPU nyata pada load dari halaman kosong dan store ke RO, serta penolakan
  recovery untuk RIP/arah/operand/konteks atau error code yang tidak sesuai.
- 1.000 percobaan copy ke CoW dengan alokasi gagal; PTE/refcount/PMM tetap sama,
  guard kembali nol, lalu copy berhasil ketika alokasi dipulihkan.
- Syscall nyata dari Ring 3: buffer invalid, attach/detach SHM, transfer file
  3.000 byte, posisi FD, SHA/AES, pointer bersarang keystore, fork dengan copy
  OOM lalu retry, isolasi parent/child, dan cleanup task oleh reaper.
- Canary tepat setelah objek `int` untuk memeriksa wrapper libc `readdir()`.

Hasil validasi pada 21 Oktober 2026:

| Suite | Hasil |
|---|---|
| Uaccess | 6/6 konfigurasi lulus, termasuk canary libc dan 1.000 retry CoW OOM per konfigurasi. |
| Memori | 6/6 konfigurasi regresi lulus. |
| Scheduler | 6/6 konfigurasi regresi lulus. |
| Boot SMP | 6/6 skenario lulus, termasuk 17 vCPU dan kegagalan alokasi stack AP. |

Total 24 skenario QEMU lulus. Kernel, shell, dan aplikasi berhasil dibangun
tanpa warning compiler; fixture dan program uji uaccess juga dikompilasi dengan
`-Werror`. Pemeriksaan whitespace pada perubahan serta kompilasi sintaks runner
Python lulus. Ini validasi correctness, bukan hasil benchmark performa.

## Pekerjaan yang masih terbuka

Scheduler tetap BSP-only dan guard operasi I/O/AES panjang belum dipecah untuk
mengecilkan latensi penjadwalan. Aktivasi AP membutuhkan lock address space,
state per-CPU, dan TLB shootdown. Kebutuhan panjang file dan validasi rentang
header/segmen ELF sudah ditangani pada paket
[loader ELF 22 Oktober](elf-correctness.md). `vm_space`, heap alignment, kewenangan SHM,
tracking dinamis, serta fairness scheduler tetap mengikuti roadmap.
