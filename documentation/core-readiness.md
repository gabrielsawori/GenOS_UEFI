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
  selesainya IRET. Leader tidak dibebaskan selama thread grupnya masih ada.
- `PREEMPT_GUARD` sekarang menahan preemption lokal **dan** mengambil lock
  mutasi kernel yang rekursif per CPU. Semua dispatcher syscall diserialisasi;
  lock dilepas sebelum exit/sleep/wait. Ini kebijakan awal SMP untuk menjaga
  allocator, VFS, crypto, dan state lain yang belum memiliki lock tersendiri.
  Kode user dan kernel worker di luar guard dapat berjalan serentak.
- IRQ timer mencoba mengambil lock tanpa menunggu. IRQ input tidak mengambil
  lock tersebut. Page fault yang menunggu lock dengan IF=0 tetap melayani
  mailbox TLB. Jangan menunggu task atau barrier sambil memegang guard.
- IPI TLB meminta invalidasi pada CPU pengguna CR3 terkait; perubahan mapping
  kernel menjangkau seluruh CPU online. Pelepasan tabel menggunakan flush penuh.
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
  iteratif. Tahap berikutnya menambahkan [memori userspace dinamis](user-memory.md):
  malloc/free/realloc, tracking hingga 65.536 private page, VA reuse/guard,
  serta SHM hingga 16 MiB per segmen. Alokasi user masih berbasis halaman.

## Driver dan ABI

| Komponen | Kemampuan paket ini | Batas |
|---|---|---|
| LAPIC/PIT | Kalibrasi PIT, timer AP, IPI TLB/wakeup, spurious vector | Target QEMU dengan frekuensi LAPIC seragam; belum x2APIC/HPET |
| PCI | Enumerasi bus/slot/function, class/subclass, raw BAR, IRQ | Tidak melakukan BAR sizing, mengaktifkan DMA, atau menjadi driver disk/NIC |
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
| 2 | Exception user dan context FPU/SSE; lanjut fairness, lifetime address space eksplisit, lock subsistem | Implementasi exception/context di [task-context.md](task-context.md); lifetime/locking dan audit lanjutan masih terbuka |
| 3 | Block layer dan driver disk QEMU (virtio-blk), filesystem persisten | Data bertahan setelah reboot; timeout/I/O error dan pengujian disk sementara |
| 4 | Driver NIC QEMU (virtio-net) dan stack jaringan bertahap | RX/TX, ARP/IPv4, buffer lifetime, timeout; tidak mengandalkan host networking secara implisit |
| 5 | IPC event/wakeup, API buffer grafis dan ownership perangkat | Tidak ada lost wakeup; aplikasi tidak saling menimpa layar; buffer direklamasi |
| 6 | Regresi gabungan dan soak test | Boot, input, disk, memori, dan task berulang stabil pada seluruh matriks |

DE mulai setelah syarat yang disepakati di atas dipenuhi. Paket ini adalah
fondasi SMP dan driver dasar; disk, jaringan, serta subsistem GUI belum tersedia.

Pembaruan 31 Oktober 2026: tahap [memori user dinamis](user-memory.md) dan
regresinya lulus **36 kasus QEMU**, termasuk 8 vCPU dan RAM 64/128 MiB.
Tahap berikutnya, 2 November 2026, menambahkan [exception user dan context
FPU/SSE](task-context.md). Prioritas tersisa adalah lifetime/locking address
space dan fairness sebelum driver disk/jaringan serta DE.

Referensi hardware: [Intel SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)
untuk APIC, swapgs dan invalidasi TLB; [QEMU PC](https://www.qemu.org/docs/master/system/i386/pc.html)
untuk perangkat target emulasi.
