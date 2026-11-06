# Koreksi memori — 20 Oktober 2026

Dokumen ini mencatat paket 20 Oktober. Recovery user copy yang masih disebut
sebagai batasan di bawah sudah ditangani pada paket
[uaccess 21 Oktober](uaccess-correctness.md). Validasi panjang file/header/segmen
ELF sudah ditangani pada paket [loader ELF 22 Oktober](elf-correctness.md).
Eksekusi AP/TLB remote kini dijelaskan di [status core](core-readiness.md),
sedangkan batas tracking, heap user, SHM, dan operasi rentang terbaru ada di
[memori userspace dinamis](user-memory.md). Uraian BSP/256 halaman di bawah
merupakan kondisi historis paket ini.

Paket ini melanjutkan tahap 2 dari [roadmap](scheduler-memory-plan.md).
Fokusnya kepemilikan halaman dan kegagalan operasi memori; eksekusi task tetap
pada BSP. Dispatcher syscall dan struktur scheduler tetap mengikuti modul
yang ada. Logika CoW yang baru memiliki API tersendiri di `mm/cow.c`.

| Modul | Tanggung jawab |
|---|---|
| `mm/cow.c` | Menyiapkan mapping fork, melakukan commit proteksi parent, menangani write fault dan mengganti kepemilikan halaman. |
| `mm/vmm.c` | Validasi/mapping/query/unmap halaman 4 KiB, traversal page table, rollback dan pembebasan tabel kosong. |
| `mm/pmm.c` | Alokasi frame, reservasi metadata, perolehan/pelepasan referensi. |
| `mm/shm.c` | Backing segmen, attachment milik address space, pewarisan fork, cleanup. |
| `kernel/task.c` | Constructor, publikasi dan lifetime task; memanggil API CoW untuk fork. |
| `cpu/isr.c` | Dispatch fault; menyerahkan fault CoW ke modul memori. |

## Kontrak mapping

`vmm_map_page[_in]()` sekarang mengembalikan `1` saat sukses dan `0` saat gagal.
VA/PA harus selaras 4 KiB, VA harus canonical, flags harus valid, dan mapping
user tidak boleh berada di upper half. Mapping yang sudah ada tidak ditimpa.
Huge leaf ditolak pada operasi 4 KiB, bukan dibaca sebagai pointer page table.
Caller CoW memakai snapshot `vmm_read_pte_in()` dan conditional replacement
`vmm_replace_page_in()` di bawah guard ownership user. Pointer PTE tidak keluar
dari [lock VMM](vmm-locking.md); snapshot sendiri tidak memin backing frame.

Alamat PTE dipisahkan memakai `PTE_ADDR_MASK`; NX tidak ikut menjadi alamat
fisik. Query mengembalikan base fisik halaman dan flags, termasuk NX. Operasi
tidak mengambil atau melepas referensi backing page secara otomatis.

Jika alokasi tabel gagal, semua tabel baru pada pemanggilan itu dibatalkan.
Unmap mengembalikan PA dan memangkas tabel kosong. Root PDPT upper half tetap
dipertahankan karena entry-nya disalin ke setiap PML4 proses. API destroy hanya
membebaskan frame tabel lower half; pelepasan backing tetap tanggung jawab owner.

Caller ELF, stack user, SHM attach dan heap growth memeriksa hasil mapping.
Constructor gagal membuang seluruh resource yang sudah tercatat. SHM attach
tidak menerbitkan attachment atau memajukan VA sebelum semua halaman terpetakan.
Heap growth membatalkan halaman parsial sebelum mengubah metadata blok/end heap.

## Kepemilikan dan CoW

`pmm_alloc_page()` menghasilkan satu referensi. `pmm_ref_page()` mengembalikan
status; frame tidak valid/reserved atau overflow ditolak tanpa perubahan.
`pmm_free_page()` melepas satu referensi. Frame nol, metadata, dan frame yang
tidak sepenuhnya berada dalam memmap usable tidak menjadi hasil alokasi.
Metadata/refcount disejajarkan dan dibulatkan tepat; refcount kini 32 bit,
memakai empat byte per frame dibanding satu byte sebelumnya.

Setiap entri `address_space.user_pages[]` dipasangkan dengan `user_vaddrs[]`.
Metadata proses yang dibagi thread berada dalam [objek address space dengan
reference](address-space-lifetime.md), terpisah dari struktur task leader.
Pada CoW write, entri untuk VA itu diganti dengan PA baru dan
referensi lama dilepas sekali. Jumlah record tidak bertambah. Last owner cukup
mengembalikan WRITE tanpa copy. NX dan izin read-only tetap dipertahankan.

Fork menyiapkan seluruh mapping anak serta referensi terlebih dahulu. Parent
baru diproteksi setelah seluruh operasi yang dapat gagal selesai. Fork gagal
tidak meninggalkan parent setengah berubah. Hanya halaman private writable
atau yang sudah CoW diberi CoW; kode/data read-only tetap read-only. Mapping
yang tidak tercatat sebagai private atau SHM ditolak sampai kebijakan
pewarisannya tersedia. Tracking kini dinamis hingga 65.536 halaman private;
lihat [memori user](user-memory.md).

SHM memakai satu referensi PMM per backing page, dimiliki segmen. Jumlah
attachment dihitung terpisah. Fork menyalin mapping shared tanpa CoW dan
mempertahankan metadata attachment anak. Thread memakai attachment address space bersama;
keluar satu thread tidak melepas mapping address space bersama. Segmen yang
dibuat tanpa pernah attached ikut dibersihkan pada reference address space terakhir.

VMM memastikan CR0.WP aktif agar kernel tidak melewati proteksi read-only saat
menulis buffer user. Resolver menerima write fault CoW dari user atau kernel,
tetapi menolak RO biasa, NX, reserved-bit fault, dan alamat kernel. Loader ELF
memakai PF_W/PF_X, menangani segmen yang mulai di tengah halaman, dan menolak
segmen writable sekaligus executable.

## Pengujian

```sh
gcc -std=gnu11 -Wall -Wextra -Werror -O2 \
  tests/pmm_test.c cpu/preempt.c -o /tmp/genos-pmm-test
/tmp/genos-pmm-test
python3 tests/run_memory_boot_tests.py
python3 tests/run_scheduler_boot_tests.py
python3 tests/run_smp_boot_tests.py
```

Tes host PMM juga dijalankan pada `-O0`. Cakupannya batas memmap yang tidak
aligned, frame nol/metadata, pembulatan metadata tepat dua halaman, referensi
melebihi 255, batas overflow 32-bit, invalid free dan double free.

Runner memori menggunakan harness build/QEMU yang sama dengan scheduler,
dengan fixture dan program Ring 3 terpisah. Wrapper/injeksi tidak dimasukkan
ke kernel produksi. Matriks memori dan scheduler masing-masing terdiri dari
1/2/4/8 vCPU pada 512 MB serta 4 vCPU pada 64/128 MB.

| Uji memori | Ekspektasi |
|---|---|
| Mapping | Gagal pada masing-masing alokasi tabel; alamat invalid/collision/huge leaf ditolak; PA bebas NX; unmap mengembalikan frame tabel. |
| Constructor ELF | Setiap alokasi page pada constructor dibuat gagal bergiliran; PID/stack/PMM kembali ke baseline. |
| Heap growth | Gagal pada setiap alokasi dari 16 halaman growth; blok lama dan RAM bebas tidak berubah. |
| Fork | Kegagalan page/ref/mapping dibatalkan; parent tetap sama; fork berulang dan write melalui thread mengganti record owner yang benar. |
| 1.000 lifecycle | Create, attach SHM, fork, private CoW write, shared write, dan reap; penggunaan PMM kembali tepat ke baseline setiap siklus. |
| SHM parsial | Gagal alokasi tabel dan mapping di tengah attach; attachment belum diterbitkan dan halaman/tabel tidak tertinggal. |
| Ring 3 | Parent/child melihat private data berbeda dan shared data sama; write user dan write syscall ke CoW berhasil; CPU menolak write RO dan eksekusi NX. |

Probe RO/NX yang disengaja ditangkap fixture setelah CPU mengirim page fault,
diverifikasi, lalu task uji diakhiri. Fault tak terduga tetap membuat tes gagal.
Tes runtime juga memeriksa semua backing/stack proses kembali ke baseline.

Hasil validasi paket ini pada 20 Oktober 2026:

| Suite | Hasil |
|---|---|
| PMM host | Lulus pada `-O0` dan `-O2`, dengan `-Wall -Wextra -Werror`. |
| Memori QEMU | 6/6 konfigurasi lulus; setiap konfigurasi menjalankan 1.000 siklus lifecycle serta probe Ring 3. |
| Scheduler QEMU | 6/6 konfigurasi lulus, termasuk RAM 64 MB dan 128 MB. |
| Boot SMP QEMU | 6/6 skenario lulus: single, quad, fallback, reversed, BSP di luar batas enumerasi (17 vCPU), dan kegagalan alokasi stack AP. |

Total 18 skenario QEMU lulus. Hasil ini memvalidasi perilaku dan regresi,
bukan mengukur peningkatan performa. Kehadiran banyak vCPU pada tes boot
belum berarti task sudah dijadwalkan pada AP.

## Batasan berikutnya

Ini belum menyelesaikan seluruh tahap 2. Pointer user yang tidak terpetakan,
write kernel ke halaman RO biasa, atau OOM pada kernel copy belum memiliki
recovery exception-table dan masih dapat memicu panic. Parsing ELF belum
menerima panjang file untuk validasi seluruh rentang header/data; segmen yang
berbagi halaman yang sama ditolak sebagai collision.

Objek `vm_space` dengan refcount, tracking dinamis, reuse VA, kewenangan destroy
SHM, alignment heap, fairness scheduler dan kalibrasi timer tetap pekerjaan
lanjutan. Struktur task bertambah karena pasangan VA/PA masih berupa array
tetap. Guard preemption dan invalidasi TLB tetap khusus BSP; scheduler AP
memerlukan lock ant CPU dan TLB shootdown sebelum diaktifkan.
