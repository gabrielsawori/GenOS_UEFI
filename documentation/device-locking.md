# Lock disk/NIC per perangkat

Implementasi 6 November 2026 untuk QEMU x86-64, setelah
[completion INTx](interrupt-completion.md). Setiap disk dan NIC memiliki lock
sendiri. RX/TX satu NIC berbagi lock; disk/NIC lain tidak mengambil lock itu.
Submit, collect, cancel, receive dan snapshot informasi tidak lagi mengambil
guard mutasi global. Init/shutdown, scheduler/wait queue dan pemeriksaan PID
pemilik masih memakai guard global sesuai ownership masing-masing.

## Urutan lock dan preemption

| Lapisan | Aturan |
|---|---|
| Guard mutasi | Lifecycle driver dan task list; boleh mengambil satu lock perangkat |
| Lock perangkat | Queue, request, konfigurasi, buffer DMA, epoch RX; tidak boleh mengambil guard mutasi atau lock perangkat lain |
| Lock leaf IRQ | PMM, PCI CF8/CFC, registry IRQ atau PIC; tidak boleh mengambil lock lain |

`cpu/device_lock.h` menahan pergantian task **lokal**, tanpa mengambil lock
global atau mematikan interrupt selama critical section. Counter baru berada
di akhir `percpu_data`; offset GS yang dipakai assembly tidak berubah.
Scheduler menolak context switch jika counter lokal masih aktif. Pengambilan
guard mutasi di dalam critical section lokal dan nesting lock perangkat
menimbulkan trap agar pelanggaran urutan segera terlihat.

Lock perangkat hanya untuk task context/boot, bukan handler IRQ. Handler
virtio tetap hanya memakai leaf registry dan menerbitkan event atomik.
Spin yang menunggu lock melayani mailbox TLB; leaf mematikan IRQ lokal saat
mengakses state. PCI command read/modify/write kini memakai leaf khusus untuk
pasangan CF8/CFC, tetap menulis hanya 16 bit Command agar Status W1C tidak
terhapus. Inventory PCI dibuat saat boot dan tidak mendukung hotplug.

Wait queue dan adapter I/O blocking menolak pemanggilan dengan preemption
lokal ditahan. Heap, page-table mutation, sleep dan pencarian task tidak boleh
berjalan di dalam lock perangkat. Alokasi DMA memakai PMM yang sudah memiliki
leaf lock sendiri, melalui mapping HHDM yang tersedia.

## Lifetime dan worker

Lock, request dan event berada di storage permanen, terpisah dari state
perangkat yang dihapus saat reinit. Count perangkat dipublikasikan secara
atomik setelah konstruksi; pemanggil memeriksa count di bawah lock slot.
Lifecycle tetap diserialisasi guard global dan mengunci satu perangkat pada
satu waktu. Queue/DMA hanya dibebaskan setelah reset diakui, dengan lock yang
sama seperti submit/collect. Reset gagal tetap mengarantina buffer.

ID token global dibagikan melalui compare/exchange atomik antarperangkat.
ID tidak dipakai ulang; nilai maksimum menolak submit berikutnya. Adapter
sinkron memasang PID pemilik sebelum melepas lock submit. Worker mengambil
snapshot ID/PID di bawah lock, melepas lock untuk memeriksa task list, lalu
mengunci kembali dan memvalidasi **keduanya** sebelum membatalkan request.
Snapshot lama tidak boleh membatalkan request pengganti.

Worker memakai try-lock dan melewati perangkat yang sedang sibuk. Ia tidak
boleh menghentikan pelayanan semua perangkat hanya karena satu lock ditahan.
Event atau watchdog berikutnya mencoba kembali; timeout tetap bukan jaminan
latency real-time. RX mengambil snapshot event, epoch dan predicate di bawah
lock NIC, kemudian menunggu tanpa lock. Shutdown menaikkan epoch dan memberi
event sebelum reinit sehingga waiter lama mendapat cancellation.

## Giliran pemanggil sinkron

Pemisahan lock memperlihatkan starvation pada tes delapan CPU: pemanggil yang
baru selesai bisa merebut slot berulang kali sebelum task penunggu mendapat
giliran. Batas tunggu 1.000 tick tetap dipertahankan. Admission sinkron kini
memakai FIFO hingga 32 tiket per perangkat; hanya kepala antrean yang boleh
mengambil slot kosong. Submit asinkron mengembalikan `DRIVER_BUSY` bila masih
ada penunggu. Ini antrean pemanggil, bukan banyak request DMA in-flight.

Tiket tidak berisi pointer task/stack dan memakai ID unik yang sama dengan
token. Setelah mendapat slot, ID tiket menjadi ID request. Pemanggil yang
timeout menghapus tiketnya; stop/reset membatalkan seluruh antrean. Tiket milik
task yang mati sebelum mendapat slot dibuang setelah deadline aslinya oleh
worker atau submit berikutnya. Ini dapat menunda antrean sampai deadline,
tetapi tidak meninggalkan pointer task atau menahan DMA. Antrean penuh tetap
menghasilkan backpressure dan deadline, tanpa alokasi tambahan.

## Pengujian

```sh
gcc -std=gnu11 -Wall -Wextra -Werror -O2 -pthread -ffunction-sections \
  -fdata-sections tests/device_lock_test.c cpu/preempt.c drivers/io_request.c \
  -Wl,--gc-sections -o /tmp/genos-device-lock-test
/tmp/genos-device-lock-test
make test-wait
make test-drivers
python3 tests/run_driver_boot_tests.py --case quad --irq-fallback --timeout 60
```

Tes host memakai delapan thread: 80.000 transaksi, exclusion pada lock yang
sama, progres lock berbeda, ID unik lintas empat slot dan trap untuk urutan
lock terbalik/nesting. Fixture host tidak mengemulasikan scheduler kernel;
preemption dan interrupt diperiksa melalui QEMU. Tes FIFO memeriksa urutan,
antrean penuh, larangan submit async mendahului penunggu, penghapusan tiket,
reset dan expiry tiket yang ditinggalkan.

Tambahan tes guest pada konfigurasi multicore:

- BSP memegang guard global sementara AP menyelesaikan baca dua disk serta
  TX/RX Ethernet melalui API asinkron.
- AP ditahan di dalam lock disk pertama; worker BSP harus menyelesaikan disk
  kedua sebelum collect, dan NIC tetap dapat mengirim/menerima frame.
- Task pembanding pada AP yang sama tidak boleh berjalan selama lock masih
  dipegang; interrupt dan scheduler CPU lain tetap berfungsi.
- Enam belas shutdown/reinit disk dan NIC bersaing dengan submit/collect/RX
  pada AP. Data yang berhasil diambil diverifikasi, token lama ditolak, dan
  operasi berikutnya tetap berhasil.

Suite wait menguji penolakan blocking dalam critical section lokal dan event
dari IRQ saat lock dipegang. Tes sebelumnya tetap mencakup pemilik request
mati, OOM/rollback, reset tanpa acknowledgment, cancellation, perubahan
link/kapasitas melalui QMP, lost interrupt, serta fallback tanpa INTx.

Hasil akhir 6 November 2026: **62 kasus boot QEMU lulus**. Scheduler, memory,
uaccess, user-memory, context, wait dan core masing-masing 6 kasus; driver
INTx 12 boot, fallback 2 boot dan topologi/startup SMP 6 kasus. Matriks utama
memakai 1/2/4/8 CPU pada 512 MiB serta 4 CPU pada 64/128 MiB. Suite SMP juga
menguji fallback, urutan CPU terbalik, 17 CPU dan kegagalan alokasi stack AP.

Dua smoke test ISO produksi pada 4 CPU/512 MiB lulus, masing-masing 15 detik,
dengan dan tanpa virtio. Build ulang berhasil tanpa warning compiler. Tes
host lock/FIFO lulus; PMM lulus 80.000 siklus dari delapan thread dan virtqueue
200.000 completion termasuk wrap indeks. Log akhir tersedia di
`.test-results/device-lock-final-drivers-matrix.log`,
`.test-results/device-lock-final-fallback.log`,
`.test-results/device-lock-*-matrix.log`, `.test-results/device-lock-wait.log`,
`.test-results/core-matrix.log`, dan
`.test-results/device-lock-production*-smoke.log`.

Tahap berikutnya memisahkan [metadata heap](heap-locking.md) dan
[operasi raw VMM/TLB](vmm-locking.md). Pertumbuhan heap tetap memakai guard
mutasi setelah lock metadata heap dilepas. Pemisahan metadata address space/SHM,
fairness scheduler/run queue per-CPU, banyak request in-flight, filesystem
persisten dan stack IP tetap menjadi pekerjaan berikutnya.
Belum ada klaim speedup, soak test atau dukungan hardware fisik. Fondasi
surface/IPC Ring 3 untuk font dan decoder tetap mengikuti
[status API desktop](../GenOS_Desktop/Concept/kernel-api-status.md).
