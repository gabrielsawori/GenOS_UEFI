# Core sebelum DE — QEMU x86-64

Prioritas proyek per 23 Oktober 2026: stabilkan core, eksekusi multicore, dan
driver dahulu. DE belum dimulai. Target pengujian awal adalah QEMU PC x86-64
dengan xAPIC, PIC/PIT, PCI configuration mechanism #1, dan PS/2.

## Paket SMP dan driver dasar

- Task userspace hasil exec/fork/clone ditempatkan bergiliran pada CPU aktif.
  Task tetap pada CPU pilihannya selama berjalan. Kernel worker dapat dibuat
  dengan `create_task_on_cpu()`. CPU yang gagal memperoleh stack/idle tidak
  menjadi target. Batas saat ini 16 CPU; belum ada load balancing otomatis.
- Current task, idle, stack syscall, RSP user, kedalaman preemption, CR3 aktif,
  dan statistik scheduler berada dalam state per-CPU. SYSCALL dan interrupt
  memakai `swapgs`; offset assembly diperiksa dengan static assertion.
  C memperoleh identitas CPU tanpa bergantung pada GS di jendela transisi.
- GDT/TSS dan stack tetap terpisah. RSP0 BSP maupun AP diperbarui saat switch.
  Kernel menahan stack task yang sedang ditinggalkan sampai entry berikutnya
  benar-benar terjadi di stack lain, sehingga reaper CPU lain tidak mendahului
  selesainya IRET. Kini leader boleh direklamasi lebih dahulu: setiap task
  memegang reference ke [objek address space](address-space-lifetime.md).
- `PREEMPT_GUARD` sekarang menahan preemption lokal **dan** mengambil lock
  mutasi kernel yang rekursif per CPU. Semua dispatcher syscall diserialisasi;
  lock dilepas sebelum exit/sleep/wait. Ini kebijakan awal SMP untuk menjaga
  ownership memori user/SHM, VFS, crypto, dan state lain yang belum memiliki
  lock tersendiri. Operasi raw VMM memakai lock page table independen.
  Kode user dan kernel worker di luar guard dapat berjalan serentak.
- IRQ timer mencoba mengambil lock tanpa menunggu. IRQ input tidak mengambil
  lock tersebut. Page fault yang menunggu lock dengan IF=0 tetap melayani
  mailbox TLB. Jangan menunggu task atau barrier sambil memegang guard.
- IPI TLB kini meminta invalidasi pada seluruh CPU scheduler yang online;
  [lock VMM](vmm-locking.md) menserialisasi sender tanpa guard mutasi.
  Pelepasan tabel menggunakan flush penuh.
  Backing page/tabel baru boleh dipakai ulang setelah seluruh acknowledgment.
  CoW mengganti ownership dan menginvalidasi sebelum melepas frame lama.
- BSP mengalibrasi LAPIC dengan PIT channel 2 sebelum membuka gate AP. PIT
  channel 0 pada BSP tetap satu-satunya penambah waktu global; LAPIC menjadwalkan
  AP. Tidak ada perkalian waktu/quantum karena jumlah CPU. IPI reschedule
  membangunkan CPU idle; pemilihan task dilakukan tick lokal berikutnya.
- Pemilihan runnable masih menyusuri task list yang dilindungi lock. Aging
  memberi kesempatan task yang menunggu minimal 500 tick. Ini belum run queue
  per-CPU atau klaim latency/performance tertentu.
- Heap kernel memiliki alignment 16 byte, penolakan overflow, dan pertumbuhan
  transaksional dengan [lock metadata sendiri](heap-locking.md). Tersedia juga
  [memori userspace dinamis](user-memory.md):
  malloc/free/realloc, tracking hingga 65.536 private page, VA reuse/guard,
  serta SHM hingga 16 MiB per segmen. Alokasi user masih berbasis halaman.

## Driver dan ABI

| Komponen | Kemampuan paket ini | Batas |
|---|---|---|
| LAPIC/PIT | Kalibrasi PIT, timer AP, IPI TLB/wakeup, spurious vector | Target QEMU dengan frekuensi LAPIC seragam; belum x2APIC/HPET |
| PCI | Enumerasi bus/slot/function, class/subclass, raw BAR, IRQ; update command untuk driver | Enumerasi tidak mengaktifkan DMA atau melakukan BAR sizing; driver mengaktifkan bus master setelah queue siap |
| virtio-blk | API kernel read/write/flush, read-only dan timeout/reset | PCI legacy transitional, dua disk; belum filesystem persisten |
| virtio-net | API kernel Ethernet RX/TX, MAC/link, buffer DMA, INTx | PCI legacy transitional, dua NIC; watchdog fallback, belum ARP/IP/socket |
| PS/2 | Controller init dengan timeout, ACK/RESEND mouse, dispatch berdasarkan status AUX | Mouse standar tiga byte; belum wheel/USB, Pause E1 diabaikan |
| Keyboard | Jalur karakter terminal dan event make/break termasuk E0 | Layout karakter terminal masih sederhana |
| Input queue | 255 event, producer IRQ BSP, reader syscall diserialisasi, counter overflow | Global, nonblocking; belum fokus jendela/subscription per proses |

Nomor syscall lama dipertahankan. Syscall mouse/framebuffer lama yang sudah
dihapus tetap tidak aktif; API event memakai nomor baru:

| Nomor | Operasi | Argumen/hasil |
|---|---|---|
| 50 | `get_cpu_id` | ID CPU logis, BSP = 0 |
| 51 | `read_input_event` | pointer `genos_input_event`; 1 event, 0 kosong, -1 error; copy gagal tidak mengonsumsi event |
| 52 | `get_system_info` | pointer `genos_system_info`; versi 1, CPU online, free RAM, PCI, mouse, counter scheduler |
| 53 | snapshot PCI | indeks di arg1, pointer `pci_device` di arg2; 0 sukses, -1 error |
| 54 | alokasi user | ukuran byte; alamat user atau 0 |
| 55 | free user | alamat awal alokasi; 0 sukses, -1 invalid |
| 56 | realloc user | alamat awal dan ukuran baru; alamat user atau 0; gagal mempertahankan blok lama |

ABI system/input berada di `include/genos/`. Shell menyediakan `hwinfo`.
`make run` memakai 4 vCPU dan RAM 512 MiB; konfigurasi dapat diubah, misalnya
`make run QEMU_CPUS=8 QEMU_MEMORY=128M`. `make run-headless` memakai konfigurasi
yang sama tanpa jendela. `make test-core` menjalankan matriks tes core.
`wait_pid()` kini menunggu sungguh-sungguh sebelum kembali ke Ring 3; `sleep()`
juga tidak sekadar menandai task lalu langsung melanjutkan kode user.

## Validasi

```sh
python3 tests/run_scheduler_boot_tests.py --suite core --timeout 45
python3 tests/run_scheduler_boot_tests.py
python3 tests/run_memory_boot_tests.py
python3 tests/run_uaccess_boot_tests.py
python3 tests/run_smp_boot_tests.py
gcc -std=gnu11 -Wall -Wextra -Werror -O2 -ffunction-sections -fdata-sections \
  tests/input_test.c drivers/input.c drivers/keyboard.c drivers/ps2.c \
  -Wl,--gc-sections -o /tmp/genos-input-test
/tmp/genos-input-test
```

Suite core memakai worker yang dipasang pada setiap CPU, barrier bersama,
128 penggantian mapping kernel yang telah dibaca semua CPU, 1.000 siklus
alokasi/ref/free, dan 32 create/return task per CPU. Program Ring 3 memverifikasi
CPU tempat thread berjalan, fork/CoW, clone pada semua CPU, sleep, wait, dan
pembersihan task. Setelah reaper selesai, jumlah halaman stack kernel harus
kembali ke jumlah worker pemeriksa dan reaper yang masih hidup.
QMP mengirim key press/release, klik, serta gerakan mouse; pemeriksaan dilakukan
setelah event melewati IRQ dan syscall, bukan hanya dengan memanggil parser.
Runner memerlukan izin socket UNIX lokal untuk QMP.

Matriks core: 1/2/4/8 vCPU dengan RAM 512 MiB, serta 4 vCPU dengan RAM 64/128 MiB.
TCG dipakai untuk correctness. Hasil ini tidak menyatakan speedup multicore,
ketahanan hardware fisik, atau kelulusan seluruh persyaratan DE.

Validasi paket SMP awal, 24 Oktober 2026, dengan GCC 16.2.1 dan QEMU 10.2.2
(sebelum tahap memori user dinamis; hasil tahap tersebut ada di dokumen terpisah):

- Core: **6/6 konfigurasi lulus**, termasuk injeksi input QMP, penggantian
  mapping antar-CPU, siklus allocator, create/return task, dan reklamasi stack.
- Scheduler, memory, dan uaccess: masing-masing **6/6 konfigurasi lulus**
  pada matriks CPU/RAM yang sama.
- Boot SMP: **6/6 kasus lulus** — single, quad, fallback, urutan CPU terbalik,
  BSP di luar batas 16 entri awal (17 vCPU), dan kegagalan alokasi stack AP.
  Total bersama suite di atas: **30 kasus boot QEMU lulus**.
- Tes host input, PMM, dan SMP (210 topologi): lulus.
- Validator ELF dengan UBSan: shell/aplikasi hasil build diterima; guard page,
  44 kasus penolakan, batas kapasitas, dan 20.000 mutasi lulus.
- `make -B -j4 all`: berhasil tanpa warning compiler. ISO produksi diamati
  selama 15 detik pada 4 vCPU/512 MiB: shell Ring 3 diluncurkan, PCI/PS2 dan
  scheduler aktif, tanpa diagnostik fault/panic. Ini smoke test, bukan soak test.

## Syarat sebelum mulai DE

| Urutan | Pekerjaan berikutnya | Bukti selesai |
|---|---|---|
| 1 | Memori userspace dinamis, allocator dengan free, tracking lebih dari 256 page, SHM lebih besar, allocator VA/guard stack | Implementasi dan batasannya di [memori user](user-memory.md); validasi mencakup alokasi beberapa MiB, reuse, OOM/rollback, fork/clone/unmap |
| 2 | Exception/context, lifetime AS; lanjut fairness serta lock subsistem | [Context](task-context.md), [lifetime/PMM lock](address-space-lifetime.md), [metadata heap](heap-locking.md) dan [VMM](vmm-locking.md) tersedia; pertumbuhan heap serta metadata proses/SHM masih memakai guard global |
| 3 | Block layer, virtio-blk, filesystem persisten | [Driver blok](virtio-drivers.md) tersedia; persistensi sektor dan error diuji; filesystem persisten masih terbuka |
| 4 | virtio-net dan stack jaringan | [Ethernet RX/TX](virtio-drivers.md) tersedia; ARP/IPv4/socket masih terbuka |
| 5 | IPC event/wakeup, API buffer grafis dan ownership perangkat | [Wait queue/I/O kernel](wait-and-async-io.md) tersedia; IPC Ring 3, buffer grafis dan hak scanout masih terbuka |
| 6 | Regresi gabungan dan soak test | Boot, input, disk, memori, dan task berulang stabil pada seluruh matriks |

DE mulai setelah syarat yang disepakati di atas dipenuhi. Fondasi SMP dan API
disk/Ethernet kernel tersedia; filesystem persisten, IP dan GUI masih terbuka.
Lihat [kebutuhan penghubung font/decoder](../GenOS_Desktop/Concept/kernel-api-status.md).

Pembaruan 31 Oktober 2026: tahap [memori user dinamis](user-memory.md) dan
regresinya lulus **36 kasus QEMU**, termasuk 8 vCPU dan RAM 64/128 MiB.
Tahap berikutnya, 2 November 2026, menambahkan [exception user dan context
FPU/SSE](task-context.md). Prioritas tersisa adalah lifetime/locking address
space dan fairness sebelum driver disk/jaringan serta DE.

Validasi tahap context selesai 4 November 2026: **46 kasus QEMU lulus**, termasuk
empat negative control fatal, serta smoke test ISO produksi 4 vCPU/512 MiB.
Build user sekarang memakai SSE2 dan entry assembly yang menjaga alignment ABI
C. Wrapper font mempunyai scope FP kernel dan fallback bitmap; backend TrueType
belum diaktifkan. Rincian cakupan dan batas emulasi #XM ada di dokumen context.

Pembaruan 5 November 2026: [lifetime address space/lock PMM](address-space-lifetime.md)
dan [driver virtio-blk/net](virtio-drivers.md) tersedia. Regresi scheduler,
memory, uaccess, user-memory, context, dan core lulus **36/36 kasus QEMU**.
Driver lulus **12/12 boot** (dua boot pada enam konfigurasi), termasuk
persistensi raw sector dan Ethernet pada hub internal. Host PMM lulus
80.000 siklus dari delapan thread; virtqueue lulus 200.000 completion dengan
wrap indeks. Suite boot SMP lulus **6/6**, termasuk fallback, urutan CPU
terbalik, 17 CPU dan kegagalan alokasi stack AP. Total tahap ini **54 kasus
boot QEMU**, di luar dua smoke test produksi 4 vCPU/512 MiB selama 15 detik:
konfigurasi standar dan konfigurasi dengan disk/NIC virtio. Build ulang ISO
berhasil tanpa warning compiler. Ini belum soak test atau validasi hardware fisik.

Tahap lanjutan pada tanggal yang sama menambahkan [wait queue dan I/O
asinkron](wait-and-async-io.md). Submit/collect/cancel memakai token yang tidak
dipakai ulang, caller sinkron tidur tanpa guard, dan worker membatalkan request
milik task yang mati. RX mendukung wait/deadline dan pembatalan saat reset.
Metadata tetap memakai guard global untuk transaksi pendek; completion belum
memakai IRQ perangkat dan lock driver belum dipisah.

Regresi tahap asinkron: **54 kasus QEMU** — enam suite core sebelumnya
(scheduler, memory, uaccess, user-memory, context, core) masing-masing 6 kasus,
suite wait 6 kasus, serta driver 12 boot. Dua smoke test ISO produksi
4 vCPU/512 MiB selama 15 detik mencakup konfigurasi standar dan virtio.
Tes host virtqueue juga lulus 200.000 completion; build tanpa warning compiler.
Suite wait mencakup IRQ signal, 1.000 handshake, close/free dan kill waiter.
Suite driver membuktikan task lain pada CPU yang sama tetap dapat memakai
allocator saat disk timeout, serta cleanup request pemilik yang mati.

Tahap [completion interrupt](interrupt-completion.md) berikutnya pada 5 November
2026 menambahkan shared INTx, lock leaf registry/PIC, watchdog 50 tick,
spurious IRQ7/15, serta event perubahan link dan kapasitas. Perubahan geometri
membatalkan request pending dan hasil baca yang belum diambil sebelum DMA
dilepas. IRQ tidak mengambil guard mutasi; metadata queue/request masih global.

Validasi tahap IRQ: **56 kasus QEMU lulus** — tujuh suite core/wait masing-masing
6 kasus, driver INTx 12 boot, dan fallback 2 boot pada 4 vCPU/512 MiB. Dua smoke
test produksi standar/virtio lulus 15 detik, build tanpa warning compiler,
serta tes host virtqueue 200.000 completion lulus. Perubahan link/kapasitas
dikendalikan QMP melalui pipa, hanya terhadap perangkat/disk sementara.

Tahap [lock per perangkat](device-locking.md), 6 November 2026, memisahkan
transaksi disk/NIC dari guard global. Critical section menahan pergantian task
lokal dengan IRQ tetap aktif; worker melewati lock perangkat yang sedang sibuk.
PCI CF8/CFC mendapat leaf lock sendiri. Init/shutdown tetap global, dan
pemeriksaan task pemilik dilakukan tanpa memegang lock perangkat lalu
memvalidasi ulang token. Heap/VMM/SHM, fairness, filesystem persisten dan
stack jaringan masih menjadi pekerjaan berikutnya sebelum DE.

Admission I/O sinkron kini FIFO hingga 32 penunggu per perangkat. Ini mengatasi
pemanggil yang berulang kali mendahului penunggu pada tes delapan CPU tanpa
menaikkan timeout 1.000 tick. Tiket tanpa pointer task dibatalkan saat reset
atau kedaluwarsa; fairness scheduler dan banyak request DMA in-flight belum
ditambahkan.

Validasi akhir tahap lock/FIFO: **62 kasus boot QEMU lulus** — tujuh suite
core/wait masing-masing 6 kasus, driver INTx 12 boot, fallback 2 boot, serta
startup/topologi SMP 6 kasus. Dua smoke test ISO produksi standar/virtio
lulus 15 detik pada 4 CPU/512 MiB. Build tanpa warning compiler; tes host
lock/FIFO (80.000 token unik), PMM (80.000 siklus delapan thread), dan virtqueue
(200.000 completion) lulus. Rincian ada di [dokumen locking](device-locking.md).

Tahap [metadata heap](heap-locking.md), 6 November 2026, membuat alokasi dari
blok yang tersedia, free/coalescing dan pemeriksaan resize independen dari
guard global. Pertumbuhan melepas lock heap sebelum VMM dan memublikasikan
blok hanya setelah seluruh mapping berhasil. Mapping heap supervisor/NX dan
dibatasi pada satu root upper-half yang sudah dibagikan. Pertumbuhan VMM,
SHM dan metadata address space masih memakai guard global.

Regresi tahap heap lulus **68 kasus boot QEMU**: delapan suite
(heap, memory, user-memory, scheduler, uaccess, context, wait, core)
masing-masing enam kasus, driver INTx 12 boot, fallback dua boot, dan
startup/topologi SMP enam kasus. Tes host heap biasa dan UBSan mode trap
masing-masing lulus 80.000 siklus dari delapan thread, termasuk rollback
pertumbuhan dan free di sebelah tail yang sedang direservasi. ASan belum
dijalankan karena runtime tidak tersedia di environment pengujian.
Build ulang ISO tanpa warning compiler dan dua smoke test produksi
standar/virtio lulus, masing-masing 15 detik pada 4 vCPU/512 MiB. Rincian
pengujian dan batas allocator tersedia di [dokumen heap](heap-locking.md).

Tahap [lock VMM](vmm-locking.md), 6 November 2026, memisahkan operasi raw page
table dan sender TLB dari guard mutasi. Pembacaan PTE memakai snapshot;
penggantian CoW memvalidasi entry sebelum commit dan menunggu acknowledgment
sebelum frame lama dilepas. Callback traversal berjalan setelah lock VMM
dilepas. Shootdown menjangkau seluruh CPU online karena context switch kini
berjalan dengan lock berbeda dari VMM. Semua root masih memakai satu lock
VMM; reference address space, metadata user/SHM serta pertumbuhan heap tetap
memakai guard mutasi. Ini belum locking per-address-space atau tahap DE.

Regresi tahap VMM lulus **74 kasus boot QEMU**: sembilan suite
(vmm, heap, memory, user-memory, scheduler, uaccess, context, wait, core)
masing-masing enam kasus, driver INTx 12 boot, fallback dua boot, dan
startup/topologi SMP enam kasus. Dua smoke test produksi standar/virtio lulus
15 detik pada 4 vCPU/512 MiB; build ulang ISO tanpa warning compiler. Probe
multicore memastikan AP menyelesaikan map/replace/unmap dengan IF=0 saat BSP
memegang guard mutasi. Rincian ada di [dokumen VMM](vmm-locking.md).

Referensi hardware: [Intel SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)
untuk APIC, swapgs dan invalidasi TLB; [QEMU PC](https://www.qemu.org/docs/master/system/i386/pc.html)
untuk perangkat target emulasi.
