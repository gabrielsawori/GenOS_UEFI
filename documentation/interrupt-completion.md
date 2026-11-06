# Completion virtio melalui interrupt

Tahap lanjutan 5 November 2026, setelah [wait queue/I/O asinkron](wait-and-async-io.md).
Target tetap QEMU PC x86-64, PCI transitional legacy dan PIC/ELCR. Belum ada
MSI/MSI-X, IOAPIC routing, hotplug atau dukungan NIC/disk fisik.

## Jalur interrupt

`virtio_ready()` mendaftarkan I/O base dan IRQ perangkat sebelum mengaktifkan
PCI INTx. Queue meminta interrupt, lalu bus mastering dan DRIVER_OK diaktifkan.
Jika routing/registrasi IRQ gagal, INTx tetap dimask dan driver memakai watchdog
polling. Informasi mode dan counter tersedia melalui `block_info.irq` dan
`net_info.irq`.

PIC/ELCR dikonfigurasi untuk level interrupt PCI; jalur ISA yang reserved dan
PS/2 IRQ12 ditolak. Handler menyaring spurious IRQ7/15 sebelum dispatch.
Saat satu IRQ dipakai beberapa perangkat, router membaca status **semua**
perangkat pada jalur itu. Pembacaan ISR virtio mengakui interrupt; bit queue
dan bit configuration dicatat terpisah. Perilaku register ini mengikuti
[spesifikasi Virtio](https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html).

Top half hanya mengakses port, memperbarui counter/flag dan menerbitkan event
atomik. Ia tidak mengambil guard mutasi, mengalokasikan memori, mengikuti
pointer task, membaca virtqueue atau melepas DMA. EOI PIC diberikan setelah
sumber perangkat diakui. Referensi target emulasi:
[PIC QEMU](https://github.com/qemu/qemu/blob/master/hw/intc/i8259.c) dan
[transport virtio PCI QEMU](https://github.com/qemu/qemu/blob/master/hw/virtio/virtio-pci.c).

Worker mengambil completion di task context lalu membangunkan waiter.
Worker sekarang tidur pada event IRQ/submission, dengan watchdog **50 tick**
untuk notification yang hilang, perangkat tanpa IRQ dan cleanup pemilik mati.
Ini menggantikan polling setiap tick. Deadline request tetap 500 tick dan
penanganannya mengikuti giliran worker/collect; belum jaminan latency real-time.

## Lock dan reset

Registry IRQ memiliki empat slot berisi salinan nomor port/IRQ, tanpa pointer
device atau DMA. Registry dan konfigurasi PIC masing-masing memakai lock leaf
tersendiri dengan IRQ lokal dimatikan. Tidak ada nested leaf lock. Urutan task
context kini adalah guard mutasi (lifecycle) → satu lock perangkat → salah satu
leaf lock; IRQ tidak mengambil guard atau lock perangkat. Metadata queue/request
memakai [lock per perangkat](device-locking.md), dengan IRQ tetap aktif.

Urutan stop: tandai offline → mask INTx perangkat → unregister sambil menunggu
pembacaan ISR yang sedang berjalan selesai → reset → lepaskan DMA sesudah
acknowledgment. Reset gagal tetap menahan DMA. PIC line tidak dimask saat satu
perangkat dilepas karena perangkat lain dapat memakai line yang sama.

Hasil baca yang selesai tetapi belum di-collect juga harus dibatalkan sebelum
buffer DMA dilepas. Pembatalan ini mencegah collect membaca buffer yang sudah
dibebaskan. Token generasi lama tetap tidak berlaku sesudah shutdown/reinit.

## Perubahan konfigurasi

- IRQ link NIC membuat worker membaca status link dan memberi event ke waiter
  RX. Link-down mengakhiri receive yang blocked dengan `DRIVER_OFFLINE`;
  queued frame masih dapat diambil. Link-up memungkinkan TX/RX kembali.
- Perubahan kapasitas disk membuat disk offline. Request pending **dan hasil
  yang selesai tetapi belum diambil** gagal dengan `DRIVER_IO`. Kernel belum
  mempunyai filesystem yang dapat memvalidasi ulang perubahan geometri ini.
- Dalam mode fallback, worker membaca ISR/config pada watchdog. Ini tetap
  menangani perubahan konfigurasi meskipun IRQ tidak dapat dipakai.

## Pengujian

```sh
make test-drivers
python3 tests/run_driver_boot_tests.py --case quad --irq-fallback --timeout 60
python3 tests/run_production_boot_test.py --virtio
```

Runner memasang disk pada slot PCI 4/5 dan NIC pada 8/9 supaya setiap pasangan
berbagi IRQ. Guest memeriksa counter IRQ nyata pada keempat perangkat, IRQ
tetap terlayani ketika guard mutasi dipegang, peer tetap menerima IRQ setelah
disk pada line yang sama dilepas, spurious IRQ7/15 dan registry kosong setelah
shutdown. OOM/rollback serta reset berulang tetap diuji.

Fault injection mengakui IRQ tetapi membuang event software dan menunda
pengamatan completion. Worker wajib menyelesaikannya lewat watchdog tanpa
caller melakukan collect terlebih dahulu. Mode fallback memaksa registrasi
IRQ gagal; disk/Ethernet/config harus tetap berfungsi tanpa IRQ terdaftar.

QMP melalui stdin/stdout pipa mengubah link NIC down/up dan me-resize **disk
sementara milik runner** dua kali. Resize pertama mengenai hasil baca yang
sudah selesai tetapi belum diambil; resize kedua mengenai request pending.
Runner menunggu marker guest yang cocok persis untuk tiap perubahan.
Tidak ada socket listener, host networking, atau path disk pengguna.
Perintah kontrol mengikuti [skema QMP net QEMU](https://github.com/qemu/qemu/blob/master/qapi/net.json).

Matriks utama: 1/2/4/8 vCPU pada 512 MiB, 4 vCPU pada 64/128 MiB, dua boot
per konfigurasi. Probe fallback memakai dua boot pada 4 vCPU/512 MiB.
Tes ini belum soak test atau pembuktian correctness pada hardware fisik.

Hasil: driver INTx **12/12 boot**, fallback **2/2 boot**, serta scheduler,
memory, uaccess, user-memory, context, core dan wait masing-masing **6/6**.
Total **56 kasus QEMU**, di luar dua smoke test ISO produksi standar/virtio
4 vCPU/512 MiB selama 15 detik. Build tanpa warning compiler; tes host
virtqueue lulus 200.000 completion. Log utama: `.test-results/irq-*-matrix.log`,
`.test-results/irq-fallback.log`, `.test-results/core-matrix.log`, dan
`.test-results/irq-production-*-smoke.log`.
