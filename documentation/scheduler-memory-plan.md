Rencana peningkatan scheduler dan memori GenOS — 17 Oktober 2026

Status: rencana berdasarkan inspeksi kode lokal. Perbaikan boot SMP sebelumnya
sudah tersedia; peningkatan scheduler dan memori di dokumen ini belum
diimplementasikan. Temuan di bawah berasal dari jalur kode, belum seluruhnya
direproduksi dengan workload. Belum ada angka peningkatan performa yang terukur.

Target awal adalah kernel stabil saat task dibuat, melakukan fork/clone, memakai
SHM, dan keluar; kemudian latensi penjadwalan yang terukur dan eksekusi task pada
banyak CPU. Implementasi dibagi menjadi perubahan kecil dengan tes regresi.

Fondasi yang sudah ada: priority round-robin, quantum, sleep/wait, per-task
kernel stack, PML4 per proses, bitmap PMM, heap yang dapat tumbuh, CoW, SHM,
dan boot AP dengan GDT/TSS sendiri. Scheduler masih membatasi eksekusi task
pada BSP melalui pemeriksaan di `kernel/task.c::schedule()`.

| Prioritas | Temuan dari kode | Dampak yang perlu diselesaikan |
|---|---|---|
| P0 | PIT dan LAPIC sama-sama memanggil `timer_tick()` dan `schedule()` di `cpu/isr.c`; AP juga menambah counter global, meski scheduler menolaknya. Kalibrasi LAPIC memakai loop `pause`. | Waktu sleep dan quantum tidak punya hubungan konsisten dengan waktu nyata; counter juga memiliki penulis lintas CPU tanpa operasi atomik. |
| P0 | `mm/shm.c::shm_destroy()` hanya menghapus `in_use`; `shm_create()` memakai HHDM konstan. | Create lalu destroy tanpa attach membocorkan halaman; pemetaan langsung tidak mengikuti offset dari bootloader. |
| P0 | VMM dan `cpu/isr.c::cow_resolve()` memakai `entry & ~0xFFF` sebagai alamat fisik. `task_fork()` memberi CoW pada semua leaf yang ditemukan. | Bit NX ikut dianggap alamat fisik; pencarian refcount/copy dapat salah. Halaman read-only dan shared kehilangan semantik yang benar saat fork. |
| P0 | `vmm_map_page_in()` tidak mengecek kegagalan alokasi page table dan tidak mengembalikan status. | Kehabisan RAM dapat mengakses fisik nol; caller tidak bisa membatalkan mapping sebagian. |
| P0 | `fork()` dan `clone()` mengalokasikan empat halaman stack terpisah lalu memakai akhir halaman terakhir sebagai stack top. Reaper tidak membebaskan kernel stack. | Stack belum dijamin berurutan; alokasi sebagian dan stack task yang mati bocor. Fallback ke stack boot menyebabkan berbagi stack. |
| P0 | Syscall mengaktifkan interrupt, tetapi PMM, heap, SHM, dan mutasi task list belum memakai protokol sinkronisasi yang konsisten. | Task dapat berganti di tengah mutasi allocator/list bahkan sebelum task dijalankan di banyak CPU. |
| P0 | Reaper dapat menghancurkan PML4 leader ketika thread lain dari grup itu masih menjadi `current_task`. | Address space yang masih digunakan dapat dibebaskan. |
| P1 | Resolver CoW menambahkan halaman baru ke `user_pages[]` tanpa menghapus kepemilikan halaman lama; tracking dibatasi 256 entri. | Cleanup dapat melepaskan referensi lama lagi; halaman melewati kapasitas tidak terlacak. |
| P1 | Refcount PMM bertipe `uint8_t` dan berhenti bertambah pada 255 tanpa melaporkan kegagalan. | Referensi sebenarnya dapat melampaui hitungan, lalu halaman dibebaskan terlalu awal. |
| P1 | `schedule()` berulang kali menyusuri task list, membebaskan memori, dan menulis serial dalam interrupt. Reset ke base priority tetap memilih prioritas tertinggi. | Biaya tick tumbuh bersama jumlah task; task prioritas rendah masih bisa kelaparan. |
| P1 | `kmalloc()` tidak membulatkan ukuran/header ke alignment yang dijamin; `heap_grow()` tidak rollback saat gagal sebagian. | Alamat hasil/split header dapat tidak selaras; pertumbuhan yang gagal menyisakan halaman. |
| P1 | `current_task`, stack syscall, dan penyimpanan sementara RSP user masih global. | Mengaktifkan scheduler AP sekarang akan membuat CPU saling menimpa state. |

Urutan pelaksanaan yang disarankan:

| Tahap | Fokus | Ketergantungan | Hasil utama |
|---|---|---|---|
| 0 | Baseline dan tes reproduksi | Boot SMP saat ini | Ukuran waktu, alokasi, dan kegagalan dapat dipercaya. |
| 1 | Timer, sinkronisasi, dan lifetime task | 0 | Satu sumber waktu; stack dan address space tidak dibebaskan saat aktif. |
| 2 | PMM/VMM/SHM/CoW dan penanganan OOM | 1 | Kepemilikan halaman benar dan operasi gagal tanpa meninggalkan resource. |
| 3 | Scheduler yang lebih ringan dan adil | 1–2 | Antrean runnable, wakeup terurut, dan batas tunggu task. |
| 4 | Pemakaian memori dan allocator | 2; dibandingkan dengan baseline | Tracking dinamis, reuse alamat virtual, dan fragmentasi terukur. |
| 5 | Eksekusi task multicore | 1–4 dan audit subsistem bersama | State per-CPU, antrean lokal, IPI, serta invalidasi TLB lintas CPU. |

Tahap 0: baseline dan tes reproduksi.

Tambahkan penghitung halaman berdasarkan pemilik, alokasi gagal, halaman page
table, stack task, halaman SHM, CoW fault/copy, serta waktu tunggu dan runtime
task. Ekspor melalui diagnostik kernel atau syscall statistik dan command shell
`meminfo`/`schedstat`; pembacaan harus berupa snapshot yang konsisten. Pisahkan
clock monotonic dari jumlah interrupt scheduler. Baseline awal memakai referensi
waktu independen dari counter tick yang sedang bermasalah.

Siapkan fault injection yang membuat alokasi ke-N gagal, workload create/exit,
fork/write/exit, clone/join, dan SHM create/attach/detach/destroy. Gunakan kembali
tes boot SMP yang sudah ada. Ukur perilaku sebelum memperbaiki setiap bug;
persentase peningkatan baru dihitung dari workload dan konfigurasi yang sama.

Selesai bila tes dapat menunjukkan kebocoran SHM, masalah stack terfragmentasi,
ketergantungan waktu pada jumlah CPU, dan kegagalan CoW/cleanup secara terpisah.

Tahap 1: stabilkan waktu, sinkronisasi, dan lifetime task.

Perubahan awal mempertahankan PIT BSP sebagai satu-satunya sumber waktu dan tick
scheduler BSP. LAPIC AP tidak menambah waktu global. Setelah kalibrasi LAPIC
memakai interval PIT yang benar, timer scheduler dapat dipindahkan ke LAPIC tanpa
menghitung dua sumber sebagai waktu yang sama. Validasi frekuensi, pembagian nol,
dan overflow perhitungan count. Quantum berkurang tepat sekali per tick sumber
yang dipilih; pisahkan statistik runtime dari jumlah pemanggilan `schedule()`.

Buat helper alokasi/bebas kernel stack yang menyimpan base, ukuran, dan status
kepemilikan. Gunakan alokasi contiguous untuk perbaikan awal fork/clone. Jika
gagal, batalkan pembuatan task; jangan memakai stack boot bersama. Inisialisasi
seluruh field task melalui constructor bersama, termasuk identitas grup,
kredensial, state, dan metadata memori. Fork mengambil register dari frame syscall
yang sedang aktif, bukan snapshot interrupt lama yang dipaksa menjadi Ring 3.

Pisahkan penghentian task, pelepasan dari run queue, dan penghancuran resource.
Gunakan objek address space dengan refcount pengguna; jangan menghancurkannya
sampai tidak ada task yang menggunakannya. Kebijakan keluar leader saat ini
(menghentikan grup) bisa dipertahankan, dengan menunggu semua thread berhenti.
Pindahkan cleanup berat ke worker/reaper di luar interrupt. Pastikan CPU beralih
ke PML4 kernel yang valid ketika idle sehingga CR3 tidak menunjuk PML4 yang
kemudian dihancurkan.

Tetapkan critical section untuk perubahan state/list dan metadata allocator.
Gunakan IRQ-save/preemption control yang menyimpan dan memulihkan state awal;
hindari spinlock yang dapat menunggu pemiliknya sendiri setelah preemption.
Dokumentasikan urutan lock. Jangan memanggil alloc/free, logging panjang, atau
menunggu IPI sambil memegang lock run queue. Bagian pemetaan/copy yang panjang
harus dipisahkan dari critical section metadata yang singkat.

Selesai bila sleep tidak makin cepat saat CPU ditambah, pembuatan task gagal
secara bersih ketika stack habis, kernel stack direklamasi, dan keluar leader
ketika thread aktif tidak merusak PML4 atau stack yang sedang digunakan.

Tahap 2: benahi kepemilikan dan kegagalan alokasi memori.

Pusatkan pemisahan alamat fisik dan flags PTE pada helper VMM, dengan mask alamat
yang valid dan pelestarian NX. Sediakan query untuk PML4 tertentu. Page walker
mendeteksi huge-page leaf dan menolaknya secara eksplisit bila operasi belum
mendukungnya. Ubah API mapping agar mengembalikan status, memeriksa alignment,
batas alamat, dan collision, serta membatalkan alokasi page table sebagian.
Perbarui semua caller: loader ELF, heap, SHM, fork, dan stack task.

PMM memakai refcount lebih lebar dengan penanganan overflow yang eksplisit,
validasi frame/alignment, reservasi frame nol, dan pembulatan rentang memmap serta
metadata yang tepat. Tambahkan sinkronisasi bitmap/refcount. Definisikan kontrak
siapa memperoleh dan melepas setiap referensi; kepemilikan backing SHM, mapping
proses, dan page table tidak boleh dihitung atau dibebaskan ganda.

CoW hanya berlaku untuk mapping private yang writable atau sudah CoW. Kode
read-only tetap read-only; SHM tetap shared setelah fork dan attachment anak ikut
dicatat. Mapping perangkat/framebuffer membutuhkan kebijakan eksplisit, bukan
dianggap halaman RAM privat. Saat write fault, ganti kepemilikan halaman yang
sesuai VA dan lepaskan referensi lama sekali saja. Pertahankan NX. Sertakan
rollback fork gagal, fork berulang, dan copy ke buffer user CoW dari kernel.
Selama tracking masih dibatasi 256 entri, tolak operasi yang akan melampauinya
sebelum mapping dipublikasikan.

Untuk SHM, ambil HHDM dari response Limine, bebaskan halaman pada destroy,
satukan jalur pelepasan resource, dan rollback attach parsial. Attachment
dikelola pada address space yang dibagi thread. Pada tahap awal pertahankan
kontrak API: destroy menolak segmen yang masih attached dan detach terakhir
melepas segmen. Definisikan cleanup segmen yang dibuat tetapi belum attached
saat pemilik keluar; periksa kewenangan destroy berdasarkan pemilik. Perubahan
semantik menjadi deletion tertunda dapat dievaluasi terpisah.

Lengkapi akses pointer user dengan pemeriksaan mapping/izin dan pemulihan fault
yang terkontrol. Batas canonical saja tidak menjamin halaman tersedia; user copy
yang gagal harus mengembalikan error tanpa menjatuhkan kernel. Hindari menyalin
data user sambil memegang lock yang dibutuhkan penanganan page fault.

Selesai bila fault injection pada setiap langkah allocation/mapping tidak
meninggalkan resource, private fork tetap terisolasi, RO tetap RO, NX tetap NX,
SHM tetap dibagi, dan refcount sesuai jumlah referensi yang benar-benar hidup.

Tahap 3: ringankan scheduler dan perbaiki fairness.

Ganti pemindaian task list untuk pemilihan runnable dengan empat FIFO run queue
berdasarkan prioritas dan bitmap antrean yang tidak kosong. Task tidur ditempatkan
di antrean deadline terurut atau min-heap; wait/exit memakai wait queue yang
mencegah lost wakeup. Task list tetap untuk enumerasi, bukan jalur setiap tick.
Jangan reload CR3 jika address space tidak berubah.

Pertahankan priority round-robin sebagai dasar. Tambahkan aging berdasarkan lama
menunggu: task READY naik prioritas setelah batas waktu yang ditentukan, kemudian
kembali sesuai kebijakan setelah memperoleh CPU. Reset ke base priority saja
tidak cukup. Idle hanya dipilih ketika tidak ada task runnable. Pisahkan logging
dari jalur interrupt dan catat latensi dengan counter/ring buffer.

Selesai bila task prioritas rendah tetap memperoleh CPU ketika task prioritas
tinggi terus runnable, task dengan prioritas sama bergiliran, dan pengujian
state sleep/wait/exit tidak menghasilkan lost wakeup atau task masuk dua antrean.
Dokumentasikan batas tunggu sebagai fungsi jumlah task, quantum, dan interval
aging. Bandingkan p95/p99 wakeup latency dan biaya scheduler pada 1/8/32/64 task.

Tahap 4: tingkatkan kapasitas dan efisiensi allocator.

Perbaiki heap lebih dulu: alignment hasil/header minimal 16 byte, cek overflow
ukuran, pertumbuhan iteratif, rollback pertumbuhan parsial, dan penggabungan blok
yang benar. Terapkan NX pada mapping data kernel yang relevan dengan pemeriksaan
dukungan CPU. Pisahkan statistik ruang terpakai, ruang bebas yang ditahan heap,
dan halaman yang benar-benar dikembalikan ke PMM.

Ganti `user_pages[256]` dengan metadata region/mapping milik address space.
Kelola rentang virtual stack, heap, SHM, dan perangkat pada satu allocator VA
per address space: cek overlap, gunakan kembali rentang setelah unmap, dan
sediakan guard page stack. Thread berbagi objek address space tersebut.

Pertahankan bitmap PMM dan first-fit heap sebagai baseline yang sudah dibenahi.
Jika profil menunjukkan scan PMM atau kebutuhan contiguous dominan, evaluasi
buddy allocator; jika alokasi objek kecil dominan, evaluasi size classes/slab.
Pilih dari hasil pengukuran, bukan mengganti kedua allocator sekaligus.
Demand paging, swap, huge pages baru, NUMA, dan tickless menjadi pekerjaan
lanjutan setelah jalur alokasi serta fault yang ada stabil.

Selesai bila workload lebih dari 256 halaman dapat dibuat/dihancurkan tanpa
kehilangan tracking, semua hasil alokasi memenuhi alignment, penggunaan VA tidak
tumbuh tanpa batas pada attach/detach berulang, dan perubahan allocator memiliki
perbandingan biaya alokasi, fragmentasi, serta overhead metadata.

Tahap 5: aktifkan eksekusi task multicore secara bertahap.

Pindahkan current task, stack syscall, RSP user sementara, idle task, counter
preemption, dan statistik ke state per-CPU. Rancang akses GS/swapgs beserta jalur
interrupt, exception, dan syscall yang konsisten; audit alignment serta konteks
register yang harus disimpan. Terapkan lebih dulu pada BSP untuk mengisolasi
perubahan dari aktivasi AP.

Bangun IPI reschedule dan TLB shootdown dengan acknowledgment serta daftar CPU
pengguna setiap address space. Halaman/page table baru boleh dipakai ulang
setelah CPU terkait selesai menginvalidasi mapping lama. Audit pemetaan kernel
bersama, bukan hanya PML4 user. Prasyarat ini juga berlaku untuk dua thread yang
berjalan di CPU berbeda dengan PML4 yang sama.

Mulai dengan task dipasang tetap pada CPU dan run queue lokal; tambahkan migrasi,
affinity, dan work stealing sesudah invariannya lulus. AP yang gagal boot atau
belum memiliki semua resource wajib tidak boleh menjadi target task. Audit juga
state bersama di VFS, cache, kredensial, framebuffer, serta serial sebelum
syscall dijalankan serentak di AP. Menghapus guard BSP bukan langkah pertama.

Selesai bila task benar-benar berjalan pada CPU berbeda, satu task tidak pernah
RUNNING pada dua CPU, dan fork/clone/SHM/unmap tetap benar ketika ada migrasi dan
tekanan memori. QEMU TCG dipakai untuk correctness; klaim speedup multicore harus
berasal dari KVM atau hardware dengan konfigurasi dan workload yang dicatat.

Matriks validasi untuk setiap tahap:

| Uji | Konfigurasi/ekspektasi |
|---|---|
| Boot regresi | Pertahankan enam kasus `tests/run_smp_boot_tests.py`; tambah 2 dan 8 CPU pada tes scheduler. |
| Memori terbatas | QEMU 64/128/512 MB; jika konfigurasi tidak cukup untuk boot, gagal secara terdiagnosis. OOM setelah boot harus terkontrol. |
| Lifetime | Minimal 1.000 siklus create/exit, SHM, dan fork/write/exit; halaman milik testcase dilepas dan penggunaan menetap setelah warmup/cache yang terdokumentasi. |
| Fault injection | Gagal pada setiap alokasi ke-N dalam create, map, fork, clone, attach, dan heap growth; tidak ada mapping setengah jadi atau double-free. |
| CoW dan proteksi | Parent/child menulis VA sama dengan hasil terisolasi; SHM tetap terlihat bersama; tulis RO dan eksekusi NX ditolak. |
| Waktu dan fairness | Referensi waktu independen, variasi jumlah CPU, sleeper + CPU-bound + prioritas berbeda; catat p50/p95/p99 latency dan waktu tunggu maksimum. |
| SMP stress | 1/2/4/8 CPU, invalidasi TLB dan unmap saat thread berjalan; tidak ada akses halaman yang telah didaur ulang. |

Paket implementasi pertama yang direkomendasikan: tes reproduksi dan penghitung
resource, pembetulan sumber tick, helper kernel stack beserta cleanup yang aman,
kemudian perbaikan VMM/SHM/CoW sesuai urutan ketergantungan di atas. Target
persentase speedup dan estimasi waktu kalender ditetapkan setelah baseline;
kelulusan tes correctness menjadi syarat melanjutkan ke scheduler multicore.
