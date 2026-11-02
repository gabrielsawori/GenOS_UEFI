# Progres scheduler dan memori — 18 Oktober 2026

Dokumen ini mencatat paket 18 Oktober. Perbaikan lanjutan pada 20 Oktober,
termasuk CoW write, NX, rollback mapping, dan refcount, ada di
[memory-correctness.md](memory-correctness.md). Batasan di akhir dokumen ini
adalah kondisi saat paket pertama selesai.

Paket awal dari [rencana peningkatan](scheduler-memory-plan.md) menstabilkan
waktu, stack, dan penghancuran task. Eksekusi task tetap pada BSP; AP masih idle.
Ini belum menyelesaikan seluruh tahap 0–1 atau memperbaiki seluruh jalur CoW.

## Perubahan

- IRQ PIT (vector 32) menjadi satu-satunya penambah tick global dan pemanggil
  scheduler. IRQ LAPIC (48) hanya mengirim EOI. Pembacaan tick memakai atomic;
  frekuensi PIT tidak valid ditolak sebelum pembagian atau pemrograman port.
  Boot memakai 1.000 Hz. Quantum berkurang sekali dan habis tepat pada tick
  terakhir; `cpu_ticks` tidak lagi dihitung dua kali saat penjadwalan ulang.
- Constructor bersama menginisialisasi seluruh field task. Kernel task,
  proses ELF, fork, dan clone memiliki stack kernel empat halaman contiguous
  dengan base dan jumlah halaman yang tercatat. Kegagalan stack membatalkan
  constructor, tanpa fallback ke stack bersama. Cleanup membebaskan halaman
  stack milik task; stack idle yang dipinjam tidak dibebaskan.
- Reaper kernel membebaskan task di luar handler interrupt. Leader yang mati
  dipertahankan sampai semua thread dengan PML4 sama berhenti dan dihapus.
  Thread yang sedang berjalan serta stack aktif tidak boleh dibebaskan.
  Pemilihan idle/reaper mengembalikan CR3 ke PML4 kernel. Reaper memakai PID 1;
  shell pertama sekarang PID 2. Kernel task yang return otomatis keluar.
- `PREEMPT_GUARD` menahan pergantian task saat mutasi task list, PMM, heap,
  VMM, dan SHM, termasuk jalur gagal serta pemanggilan bersarang. Interrupt
  tetap aktif. Pembacaan daftar task dan pembacaan PID hasil exec juga dilindungi.
  Pointer task lain hanya valid selama caller memegang guard; polling memakai PID.
- Entry syscall menyimpan frame user lengkap sesuai `struct registers` dan
  menjaga alignment sebelum memanggil C. Fork mengambil RIP/RSP/register dari
  frame syscall aktif. Stack kernel, identitas grup, dan batas heap anak diisi.
- Loader ELF dan fork menolak penambahan halaman di luar tracking 256 entri.
  Constructor ELF membersihkan resource yang sudah tercatat jika gagal; boot
  melaporkan kegagalan pembuatan shell. Ini belum mencakup kegagalan alokasi
  page table di dalam API mapping yang masih mengembalikan `void`.
- SHM menggunakan offset HHDM dari Limine. Destroy segmen tanpa attachment
  membebaskan seluruh backing page; destroy berulang ditolak.

`task_get_resource_stats()` memberi snapshot jumlah halaman stack kernel,
kegagalan alokasi stack, dan task yang sudah dibersihkan. Stack AP/IST/boot dan
halaman data user tidak termasuk penghitung ini.

## Validasi yang dapat dijalankan

```sh
python3 tests/run_scheduler_boot_tests.py
python3 tests/run_smp_boot_tests.py
```

Hasil 18 Oktober 2026: seluruh enam kasus scheduler/memori dan enam kasus boot
SMP **PASS** pada QEMU TCG 10.2.2 dengan GCC 16.2.1. Build kernel `-O2` selesai
tanpa warning compiler. Tes host SMP juga **PASS** pada `-O0` dan `-O2`, masing-masing
memeriksa 210 susunan CPU serta fallback, ID CPU, dan pencetakan alamat ACPI.

Runner scheduler selalu membangun ulang kernel karena Makefile belum memiliki
dependency header. Fixture menggantikan `task.o` pada kernel uji saja, memakai
allocator dan page table asli. ISO, program Ring 3, dan ramdisk uji dibuat di
direktori sementara; binary aplikasi produksi tidak diganti.

| Uji | Pemeriksaan |
|---|---|
| 1.000 create/reap | Setiap stack contiguous, alignment entry kernel benar, penghitung stack dan RAM PMM kembali ke baseline setiap siklus. |
| Constructor gagal | Injeksi gagal stack pada kernel/ELF/fork/clone; gagal PML4 pada ELF/fork; ELF invalid atau melebihi kapasitas tracking, termasuk ruang untuk stack user. Tidak ada task terpublikasi atau halaman tertinggal. |
| Fork/clone dan leader exit | Frame fork sesuai input aktif; stack thread terpisah; PML4 owner tetap hidup ketika thread menjadi current; seluruh grup akhirnya direklamasi. |
| Quantum | Dua tick untuk task normal; satu pencatatan CPU per tick; scheduler tidak berganti task di dalam guard. Worker runtime juga menunggu delapan tick dengan guard aktif untuk memastikan IRQ tetap berjalan. |
| 1.000 create/destroy SHM | RAM kembali ke baseline; double destroy ditolak; injeksi gagal di masing-masing dari empat alokasi backing di-rollback. |
| Runtime Ring 0/Ring 3 | Dua kernel task return; program user syscall/fork/exit; parent dan child melaporkan hasil berbeda dengan register terjaga; reaper membebaskan resource. |
| Sumber waktu | Selama 1.000 PIT IRQ, tick global tepat sama dengan jumlah IRQ PIT BSP. IRQ LAPIC diamati pada setiap CPU tanpa menambah tick. |

Matriks scheduler: 1/2/4/8 vCPU dengan RAM 512 MB, serta 4 vCPU dengan 64/128 MB.
Regresi SMP mempertahankan enam topologi/injeksi kegagalan yang sudah ada.
Tes host SMP dan perintahnya tersedia di [smp_bugfix.md](smp_bugfix.md).

Tes 1.000 lifecycle menggunakan constructor dan helper reaper saat boot;
uji eksekusi task yang benar-benar dijadwalkan dilakukan terpisah. Probe fork
Ring 3 tidak menulis private page setelah fork, sehingga hasilnya tidak boleh
diartikan sebagai kelulusan pengujian CoW write.

## Batasan dan pekerjaan berikutnya

Guard ini hanya menahan preemption task BSP, **bukan lock ant CPU**. AP belum
boleh menjalankan allocator atau task umum. IRQ/NMI tidak boleh memutasi
struktur yang dilindungi; fault sinkron memerlukan aturan tersendiri. Critical
section fork dan cleanup masih panjang. `cpu_ticks` menghitung tick scheduler
yang diproses, bukan runtime presisi ketika preemption ditahan.

Lifetime PML4 masih memakai aturan leader/grup, belum objek `vm_space` dengan
refcount. Tracking heap/SHM antar-thread, SHM yang dibuat tanpa attach saat exit,
shared mapping saat fork, dan kepemilikan halaman setelah CoW write masih harus
dibenahi. Mask alamat PTE/NX, rollback alokasi page table, dan overflow refcount
PMM adalah prioritas paket berikutnya. Kapasitas tracking tetap 256 halaman.

Fairness antar-prioritas, run queue, alignment/rollback heap, kalibrasi LAPIC,
latensi dan perbandingan waktu independen masih mengikuti roadmap. Kesamaan
tick dengan IRQ PIT memeriksa sumber waktu, bukan kalibrasi jam atau klaim
speedup. Scheduler multicore memerlukan state per-CPU dan TLB shootdown dahulu.
