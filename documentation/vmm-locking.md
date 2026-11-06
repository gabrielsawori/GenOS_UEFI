# Lock VMM dan snapshot page table

Tahap 6 November 2026, setelah [metadata heap](heap-locking.md). Operasi raw
VMM sekarang memakai **satu lock page table terpisah dari guard mutasi kernel**.
Create root, map, query, conditional replacement, unmap dan destroy root yang
sudah tidak aktif dapat berjalan ketika CPU lain memegang guard mutasi.
Semua root masih berbagi lock VMM ini; belum ada lock per-address-space.

## Urutan dan lifetime

Urutan tetap **guard mutasi opsional → satu lock lokal → leaf PMM**. VMM
menahan pergantian task lokal, mempertahankan status IRQ caller, dan memproses
mailbox TLB ketika menunggu lock. Jalur ini juga bekerja pada fault context
dengan IF=0. IRQ handler tidak boleh memanggil VMM. VMM tidak mengalokasikan
heap, mengambil guard mutasi, atau memanggil callback sambil memegang lock.

Lock page table tidak menggantikan ownership address space:

- Caller menjaga root dan backing frame tetap hidup selama operasi. Array
  physical page pada range mapping harus tetap stabil sampai fungsi kembali.
- Query mengembalikan snapshot, bukan pin frame. Lookup lalu akses/free tetap
  memerlukan perlindungan ownership di lapisan pemanggil.
- Operasi gabungan user-memory, CoW, fork, SHM dan reference address space
  tetap memakai guard mutasi. Caller raw VMM tidak boleh mengubah mapping
  milik subsistem tersebut tanpa mengikuti guard ownership-nya.
- Destroy hanya untuk root tidak aktif yang dimiliki eksklusif dan tidak
  dapat dipakai CPU lain lagi. Scheduler/reaper tetap menjaga lifetime CR3
  sampai task keluar dari stack dan address space lama.
- Pertumbuhan heap masih memakai guard mutasi untuk serialisasi reservasi
  tail dan publikasi heap. Lock metadata heap dilepas sebelum memanggil VMM.

## Snapshot dan penggantian mapping

Pointer PTE internal tidak lagi diekspos. `vmm_read_pte_in()` mengembalikan
nilai leaf 4 KiB atau nol. Pemeriksaan user-access, guard VA, CoW dan fixture
pengujian memakai snapshot atau API query yang dilindungi lock.

`vmm_replace_page_in()` hanya mengganti leaf bila physical address dan flag
masih sesuai snapshot yang diharapkan. Perubahan bit Accessed/Dirty oleh CPU
tidak dianggap konflik. Penggantian pada frame yang sama mempertahankan bit
hardware yang sudah terpasang, dan izin ancestor tetap diperiksa. Fungsi
tidak mengubah reference backing; caller baru boleh melepas frame lama
setelah replacement berhasil dan acknowledgment TLB selesai. Snapshot bukan
token generasi: caller harus mencegah reuse/ABA melalui ownership frame.

CoW memeriksa hasil penggantian sebelum mengganti ownership dan melepas frame
lama. Walk fork mengambil snapshot per leaf, lalu melepas lock VMM sebelum
callback memetakan child atau memeriksa SHM. Guard mutasi milik caller menjaga
tree user stabil selama seluruh traversal. Walk ini bukan snapshot atomik
seluruh tree untuk caller raw yang mengubahnya serentak.

## TLB dan retirement

Lock VMM juga menserialisasi mailbox shootdown. Sender memeriksa kepemilikan
lock VMM; memegang guard mutasi saja tidak cukup. Handler/poller tidak mengambil
kedua lock tersebut. Unmap dan rollback tetap menunggu acknowledgment sebelum
frame page table bisa dipakai ulang, termasuk retirement rentang besar.

Shootdown kini dikirim ke seluruh CPU scheduler yang online. Scheduler dapat
mengganti CR3 tanpa lock VMM, sehingga sender tidak lagi menyaring target dari
snapshot `active_cr3`. Target saat ini memakai reload CR3 tanpa PCID; dukungan
PCID dan optimisasi target CPU belum tersedia. Broadcast ini menambah biaya
IPI, terutama saat membangun root yang belum aktif. Belum ada klaim peningkatan
throughput atau latency.

## Pengujian

```sh
make test-vmm
python3 tests/run_scheduler_boot_tests.py --suite memory --timeout 45
python3 tests/run_scheduler_boot_tests.py --suite umem --timeout 45
```

Suite `vmm` memakai PMM/page table dan TLB native QEMU:

- Walk sparse melintasi batas PT, PD, PDPT sampai leaf terakhir lower half;
  callback memetakan root lain tanpa menahan lock VMM.
- Replacement menolak snapshot berbeda, alamat invalid, mapping kosong dan
  pelebaran izin ancestor, sambil menerima perbedaan Accessed/Dirty.
- Worker setiap CPU memetakan/menghapus root bersama dan root pribadi,
  membaca tabel tetangga yang sedang dibuat/dipangkas, serta berlomba
  mengganti satu leaf. Jumlah free frame kembali ke baseline setelah operasi.
- Enam puluh empat replacement remote diperiksa pada semua CPU, dengan
  pergantian CR3 di sisi pembaca.
- Pada multicore, AP menyelesaikan 128 siklus map/replace/unmap dengan IF=0
  sementara BSP menahan guard mutasi. Status IRQ dan kedalaman lock harus
  kembali sesuai caller. Case satu CPU tidak menjalankan probe AP.

Suite memory/user-memory/uaccess/context tetap menguji rollback, fault CoW,
pin address space, cleanup leader/thread, heap/SHM dan Ring 3. Pemisahan
reference address space/SHM serta fairness scheduler masih diperlukan sebelum
filesystem persisten, IP/socket dan API IPC/surface desktop.

Validasi akhir 6 November 2026: **74 kasus boot QEMU lulus**. Sembilan suite
(vmm, heap, memory, user-memory, scheduler, uaccess, context, wait, core)
masing-masing mencakup 1/2/4/8 vCPU serta RAM 64/128 MiB; driver INTx 12 boot,
fallback watchdog dua boot, dan startup/topologi SMP enam kasus. Dua smoke
test ISO produksi standar/virtio lulus pada 4 vCPU/512 MiB selama 15 detik
masing-masing. Build ulang ISO selesai tanpa warning compiler. Ini belum soak
test atau validasi hardware fisik.

Log tersedia di `.test-results/vmm-final-*-matrix.log`,
`.test-results/vmm-final-fallback.log`,
`.test-results/vmm-final-production-build.log`, dan
`.test-results/vmm-final-production*-smoke.log`. Fixture kernel dikompilasi
dengan `-Werror`; probe instrumentasi tidak masuk ISO produksi.
