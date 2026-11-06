# Lock metadata heap dan pertumbuhan transaksional

Tahap 6 November 2026, sesudah [lock disk/NIC](device-locking.md). `kmalloc`,
`kfree`, dan `krealloc` memakai lock metadata heap sendiri. Alokasi dari blok
yang sudah tersedia, free/coalescing, dan pemeriksaan kapasitas realloc tidak
mengambil guard mutasi global. Copy data realloc dilakukan tanpa menahan lock
metadata; pemanggil wajib memiliki buffer secara eksklusif selama free/resize.

## Dua jalur alokasi

`cpu/local_lock.h` menyediakan primitive yang sama bagi heap dan driver:
status IRQ caller dipertahankan dan pergantian task lokal ditahan. Dalam
task context biasa IRQ tetap aktif; page fault dapat masuk dengan IF=0.
Guard global boleh berada
di luar lock lokal; lock lokal tidak boleh mengambil guard global atau lock
lokal lain. `cpu/device_lock.h` tetap menyediakan macro khusus driver dengan
aturan yang sama. IRQ handler tidak boleh memanggil allocator heap.

Jika first-fit tidak menemukan blok, allocator melepas lock heap, mengambil
guard mutasi, lalu memeriksa ulang daftar blok. CPU lain mungkin sudah free
atau menambah kapasitas ketika caller menunggu. Hanya bila masih kekurangan,
allocator menambah mapping melalui VMM tanpa memegang lock heap. Setelah
semua halaman tersedia, allocator mengambil lock heap, mencari ulang tail
terbaru, memublikasikan blok dan memberikan alokasi kepada caller.

Urutan ini memungkinkan CPU lain memakai blok lama saat pertumbuhan belum
selesai. Tail kosong dapat direservasi untuk pertumbuhan; blok sebelumnya
tetap tersedia. Ia menghindari siklus CPU A memegang lock heap sambil menunggu guard
global, sementara CPU B memegang guard global sambil menunggu lock heap.

**Pertumbuhan masih memakai guard global** untuk serialisasi reservasi tail
dan publikasi heap. Pada tahap berikutnya [operasi raw VMM/TLB telah dipisahkan](vmm-locking.md),
namun guard pertumbuhan ini tetap dipertahankan. Metadata heap sudah independen;
ini belum kernel dengan seluruh alokasi/user syscall berjalan paralel.

## Mapping, failure dan ownership

- Initial heap 64 KiB; pertumbuhan dibulatkan ke 64 KiB dan melengkapi kapasitas
  tail kosong bila tersedia. Alignment payload tetap 16 byte.
- Seluruh pertumbuhan untuk satu permintaan menjadi satu transaksi. OOM
  atau mapping gagal membatalkan semua halaman baru sebelum metadata/end
  heap berubah, termasuk bila kegagalan terjadi setelah tambahan 64 KiB.
- Tail kosong yang diperlukan direservasi: first-fit melewatinya, dan free
  di sebelahnya menunda merge supaya header reservasi tetap hidup. Tail tetap
  dicari ulang saat commit. Rollback melepas reservasi dan menyelesaikan merge
  tertunda. Kapasitas tail dipakai kembali sehingga allocator tidak perlu
  meminta seluruh ukuran buffer sekali lagi dari PMM.
- Heap berada dalam satu slot PML4 upper-half, indeks 384, mulai
  `0xffffc00000000000`. Batas 512 GiB mencegah pertumbuhan masuk ke root baru
  yang belum dibagikan ke address space lama. Ini batas VA, bukan RAM yang
  dialokasikan saat boot. Mapping baru bersifat supervisor, writable dan NX.
- Kegagalan realloc mempertahankan pointer/data lama. Shrink dapat tetap
  memakai kapasitas blok lama. Free menggabungkan blok kosong bersebelahan;
  backing heap belum dikembalikan ke PMM setelah free biasa.
- Pointer yang tidak cocok dengan awal blok aktif memicu trap sebelum menulis
  metadata, termasuk interior pointer dan double free yang diuji. Pointer lama
  yang alamatnya sudah dipakai ulang tetap memerlukan disiplin ownership dari
  caller. Ini bukan validasi pointer userspace; caller tidak boleh free/resize
  buffer yang sama secara serentak.
- `heap_get_stats()` memberi snapshot mapped/allocated/free/reserved bytes dan jumlah
  blok/alokasi di bawah lock heap; API ini belum menjadi syscall.

## Validasi

```sh
gcc -std=gnu11 -Wall -Wextra -Werror -O2 -pthread \
  tests/heap_test.c -o /tmp/genos-heap-test
/tmp/genos-heap-test
make test-heap
make test-core
make test-drivers
```

Tes host menggunakan kode heap/lock produksi, arena VA terbatas dan stub
PMM/VMM dengan guard global sungguhan melalui mutex. Delapan thread memeriksa
80.000 siklus alokasi/realloc/free dan isi payload. Failure disuntikkan pada
setiap halaman dari transaksi pertumbuhan 192 KiB, di allocator fisik maupun
mapping. Snapshot dan jumlah frame harus kembali ke baseline. Tes juga
menahan transaksi mapping sebelum publikasi sambil thread lain memakai heap,
memegang guard global sambil thread lain melakukan fast-path, memeriksa
coalescing, overflow, serta trap invalid/double free. Free tepat sebelum tail
yang direservasi diuji saat commit **dan** rollback. Budget frame terbatas
memastikan pertumbuhan benar-benar memakai kembali kapasitas tail.

Fixture host juga dijalankan dengan UBSan mode trap
(`-fsanitize=undefined -fsanitize-undefined-trap-on-error`). ASan tidak dapat
dijalankan di environment ini karena runtime `libasan` tidak tersedia;
pengujian sanitizer tersebut belum diklaim lulus.

Suite QEMU `heap` menjalankan PMM/page table nyata: partial OOM/map failure,
realloc failure, NX/supervisor pada mapping multi-MiB, dan keterlihatan dari
root yang dibuat sebelum pertumbuhan. Worker per CPU menahan buffer besar
bersamaan, memeriksa data, menjalankan 1.000 siklus resize lalu direklamasi.
Pada multicore, AP juga wajib menyelesaikan 1.000 siklus alokasi/resize/free
ketika BSP memegang guard global. Case satu CPU tidak menjalankan probe AP.

Validasi akhir 6 November 2026: **68 kasus boot QEMU lulus**. Suite heap,
memory, user-memory, scheduler, uaccess, context, wait dan core masing-masing
menjalankan enam konfigurasi (1/2/4/8 vCPU serta RAM 64/128 MiB). Driver INTx
lulus 12 boot, fallback watchdog dua boot, dan startup/topologi SMP enam kasus.
Log matriks disimpan di `.test-results/heap-final-*-matrix.log`,
`.test-results/core-matrix.log`, dan `.test-results/heap-final-fallback.log`.
Tes host biasa dan UBSan mode trap masing-masing lulus 80.000 siklus delapan
thread; log berada di `.test-results/heap-host.log` dan
`.test-results/heap-host-ubsan.log`.

Build ulang ISO produksi selesai tanpa warning compiler. Dua smoke test
produksi, standar dan dengan disk/NIC virtio, lulus pada 4 vCPU/512 MiB
selama 15 detik masing-masing. Log hasil ada di
`.test-results/heap-final-production-smoke.log` dan
`.test-results/heap-final-production-virtio-smoke.log`. Ini belum soak test
atau pengujian perangkat fisik.

Operasi raw VMM kini memakai [lock sendiri](vmm-locking.md). SHM, reference
address space, heap userspace dan scheduler masih memakai guard global.
Tahap berikutnya perlu mempertahankan transaksi mapping dan aturan retirement/TLB
saat metadata ownership tersebut dipisahkan. Filesystem persisten, stack
jaringan dan API IPC/surface Ring 3 tetap diperlukan sebelum DE.
