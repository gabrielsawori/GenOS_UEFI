# Lifetime address space dan locking

Implementasi 5 November 2026. Target validasi QEMU x86-64; lihat juga
[memori user](user-memory.md) dan [context task](task-context.md).

## Kepemilikan

`mm/address_space.h` memisahkan resource proses dari struktur task leader.
Objek memiliki PML4, tracking private page, metadata alokasi user, attachment
SHM, dan identitas grup. `task.space` memiliki satu reference; `task.pml4`
hanya alias yang dipinjam untuk jalur context switch.

- `address_space_create()` mengembalikan satu reference, tanpa task terpasang.
- Exec mengikat reference itu ke task; fork membuat objek baru dengan CoW
  dan attachment SHM. Clone menambah reference ke objek yang sama. Metadata
  memori tidak lagi diambil melalui pencarian task leader.
- `task_refs` menghitung task yang belum dilepas reaper, termasuk task mati
  yang masih aktif atau sedang ditinggalkan CPU. Scheduler melindungi stack
  dan CR3 sampai CPU benar-benar masuk pada stack task berikutnya.
- Reaper boleh membebaskan struktur leader lebih dahulu. Task lain tetap
  memegang reference sendiri; metadata memori tidak berada di leader.
- Exit leader dan fault fatal user menandai address space `dying`, mematikan
  task pemakainya, dan membangunkan waiter. Clone/fork baru dan pin current
  baru ditolak. Exit thread biasa tidak mematikan saudara lainnya.
- Pelepasan terakhir membersihkan SHM, private page/metadata, tabel halaman,
  dan objek. Reference eksternal dapat bertahan setelah semua task selesai.
  Reference task hanya dilepas setelah pointer task dikosongkan dan
  `task_refs` dikurangi.

`address_space_get()` membutuhkan reference yang sudah dimiliki atau pointer
yang masih dilindungi guard mutasi. `task_get_address_space()` dan hasil
lookup task adalah pointer pinjaman, bukan pegangan untuk kerja asinkron.
`task_pin_current_address_space()` mengembalikan reference milik pemanggil;
pasangkan dengan `address_space_put()`. Overflow reference ditolak.

**Pin address space tidak memin halaman tertentu.** Thread lain masih dapat
free/unmap VA tersebut. Driver sekarang memakai buffer DMA milik kernel,
tidak menjadikan pin address space sebagai izin DMA ke pointer user.
DMA langsung ke user memerlukan mekanisme page pin terpisah.

## Urutan lock

| Lapisan | Perlindungan saat ini |
|---|---|
| Task, reference AS, heap user, CoW, SHM | `PREEMPT_GUARD`, lock mutasi global rekursif per CPU |
| Page table dan sender TLB | Satu lock VMM sendiri; metadata ownership user tetap memakai guard mutasi di luar VMM |
| Metadata heap kernel | Lock lokal sendiri; pertumbuhan mapping masih memakai guard mutasi, tanpa menahan lock heap |
| Syscall/subsistem yang belum dipisah | Guard mutasi global |
| PMM bitmap, refcount dan free-page counter | Lock PMM sendiri, IRQ lokal dimatikan selama critical section |
| PCI configuration | Leaf lock khusus CF8/CFC |
| Metadata queue/request virtio | Lock lokal per perangkat; lifecycle tetap memakai guard mutasi |
| PIC dan registry INTx virtio | Lock leaf IRQ-save terpisah; top half hanya port/status/event, tanpa guard mutasi |

Urutan yang sah: **guard mutasi → satu lock lokal → lock leaf**; lock lokal
tidak boleh mengambil guard mutasi atau lock lokal lain. Heap melepas lock
metadata sebelum masuk jalur pertumbuhan VMM. PMM juga dapat dipanggil tanpa
guard mutasi. Lock PMM adalah leaf: jangan alokasi heap, sleep, mengambil lock
lain, atau masuk guard mutasi sambil memegangnya. CPU yang menunggu leaf lock
tetap memproses mailbox TLB; IRQ timer tidak menunggu lock mutasi global.
Primitive spinlock memakai operasi atomik 32-bit yang konsisten.

Ini belum lock per-address-space atau kernel dengan semua subsistem berjalan
paralel. [Metadata heap](heap-locking.md) dan [operasi raw VMM](vmm-locking.md)
sudah dipisahkan; pertumbuhan heap, reference address space/SHM dan fairness
scheduler masih pekerjaan selanjutnya. Snapshot PTE bukan pin backing frame;
destroy tetap membutuhkan root tidak aktif dengan ownership eksklusif.
[I/O asinkron dan wait queue](wait-and-async-io.md) kini melepas guard selama
menunggu perangkat. Metadata driver memakai [lock per perangkat](device-locking.md);
belum ada jaminan latency desktop.

## Pengujian

Suite `umem` menjalankan 128 lifecycle dengan leader dilepas lebih dahulu,
saudara masih aktif, pin eksternal melewati task terakhir, private heap,
SHM attached dan created-only, overflow reference, OOM, dan cleanup tepat
pada reference terakhir. Free RAM kembali ke baseline; jumlah objek hidup
kembali nol. Suite core memegang/melepas pin bersama dari setiap CPU.

```sh
python3 tests/run_scheduler_boot_tests.py --suite umem --timeout 45
python3 tests/run_scheduler_boot_tests.py --suite core --timeout 45
gcc -std=gnu11 -O2 -Wall -Wextra -Werror -pthread \
  tests/pmm_concurrency_test.c -o /tmp/genos-pmm-concurrency
/tmp/genos-pmm-concurrency
```

Tes host PMM tidak melink guard mutasi global: delapan thread menjalankan
80.000 siklus contiguous allocation/ref/free dan memeriksa ownership data.
Tes ini melengkapi pengujian SMP native QEMU, bukan menggantikannya.
