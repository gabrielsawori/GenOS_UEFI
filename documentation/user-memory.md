# Memori userspace dinamis

Tahap lanjutan core untuk QEMU x86-64, diperbarui 31 Oktober 2026. Implementasi ada di
`mm/user_memory.c`, `mm/shm.c`, `mm/cow.c`, dan lifecycle `kernel/task.c`.

## Kontrak alokasi

`malloc`, `free`, dan `realloc` di libc memakai ABI baru:

| Syscall | Argumen | Hasil |
|---|---|---|
| 54 — alokasi | ukuran byte | alamat user, atau 0 jika gagal/ukuran 0 |
| 55 — pelepasan | alamat awal alokasi | 0 berhasil/NULL, -1 untuk alamat yang tidak dimiliki |
| 56 — resize | alamat awal, ukuran baru | alamat user, atau 0 jika gagal; ukuran 0 membebaskan blok |

Nomor 47–49 tetap diblokir. Tidak ada alamat heap kernel yang diberikan kepada
aplikasi. Metadata alokasi berada di kernel dan dicari berdasarkan alamat awal
persis; pointer interior, pointer kernel, ELF, stack awal, dan SHM tidak dapat
memakai syscall pelepasan heap. `free` libc bertipe void dan mengabaikan status
error untuk pointer tidak valid. Pointer yang sudah dibebaskan tidak boleh
lagi digunakan oleh aplikasi, termasuk bila alamat tersebut kemudian dipakai ulang.

- Setiap alokasi memetakan halaman private user RW/NX, dinolkan sebelum terlihat
  oleh user. Alamat selalu page-aligned, sehingga juga memenuhi alignment 16 byte.
- Rentang heap `0x60000000–0x68000000` menggunakan first fit per address space.
  Lubang hasil free dapat dipakai ulang. Mapping ELF yang sudah ada dilewati.
- Satu halaman setelah setiap alokasi dibiarkan tidak terpetakan dan dicatat
  sebagai bagian reservasi VA. Ini mendeteksi akses melewati batas halaman;
  padding pada halaman terakhir tetap dapat diakses, bukan red zone per byte.
- `realloc(NULL, n)` melakukan alokasi. Resize ke ukuran berbeda menyiapkan blok
  baru, menyalin `min(ukuran_lama, ukuran_baru)` lewat mapping fisik kernel, lalu
  melepas blok lama. Kegagalan menjaga alamat, data, dan ownership blok lama.
  Metode ini memerlukan memori/VA untuk blok lama dan baru selama operasi.
- Pelepasan menghapus PTE dan menunggu acknowledgment TLB remote sebelum
  mengembalikan frame ke PMM. Mapping parsial yang gagal di-rollback.
- Pemetaan heap dan SHM memakai operasi rentang: setelah seluruh PTE dibuat,
  satu invalidasi penuh menyelesaikan publikasinya. Unmap menghapus rentang,
  lalu melepas tabel kosong dalam kelompok maksimal 16 tabel setelah invalidasi
  diakui. Frame backing baru dilepas setelah operasi rentang selesai. Ini
  menghindari pengiriman IPI untuk setiap halaman pada attachment besar.

Allocator ini masih berbasis halaman: alokasi 1 byte memakai satu halaman fisik
4 KiB serta satu halaman VA guard. Belum ada suballocator ukuran kecil, arena
libc, demand paging, swapping, atau jaminan latency. Syscall mutasi masih
menggunakan lock global rekursif yang sama dengan paket SMP.

## Ownership, fork, dan thread

Tracking private page menggunakan 32 slot awal di task, lalu array dinamis yang
bertumbuh hingga batas kebijakan **65.536 halaman (256 MiB)** per address space.
Batas itu mencakup ELF, stack awal, dan heap; SHM mempunyai batas terpisah. ELF
besar tidak lagi ditolak hanya karena melebihi 256 halaman. Validasi menghitung
kebutuhan tracking sebelum pemetaan, termasuk empat halaman stack.

Stack awal setiap proses berada di `0x80001000–0x80005000`, dengan satu guard
page di masing-masing sisi. Proses berbeda memakai VA yang sama dalam PML4
masing-masing. ELF yang bertabrakan dengan stack atau guard ditolak. Stack
thread tetap diberikan oleh pemanggil clone; memakai malloc memberi guard di
ujung alokasi, tetapi kernel belum menyediakan API stack thread khusus.

Thread memakai metadata alokasi dan SHM milik leader. Operasi allocator antar
thread diserialisasi oleh kernel, tanpa lock userspace yang dapat tertinggal
terkunci setelah fork. Sinkronisasi akses data aplikasi tetap tanggung jawab
aplikasi; membebaskan buffer yang masih dipakai thread lain tetap tidak valid.

Fork menyalin metadata alokasi, mempertahankan referensi halaman private dengan
CoW, dan mempertahankan attachment SHM sebagai mapping shared. Free/resize pada
child tidak melepas halaman milik parent. Semua pekerjaan yang dapat gagal
selesai sebelum proteksi CoW parent diubah. Reaper melepas backing page, array
tracking, serta metadata alokasi setelah semua thread berhenti memakai address
space tersebut. Pencarian ownership masih linear; belum ada indeks VA untuk
mengoptimalkan fork/CoW berukuran sangat besar.

Array tracking dan heap kernel mempertahankan kapasitas yang sudah diperoleh
untuk penggunaan ulang. Free user mengembalikan backing halaman user dan tabel
kosong ke PMM, tetapi tidak selalu mengurangi kapasitas metadata kernel saat itu.
Array tracking dilepas saat proses dibersihkan; backing heap kernel mengikuti
kebijakan high-water allocator kernel yang sudah ada.

## Shared memory

- Maksimum **32 segmen global**, **16 MiB per segmen**, dan **16 attachment per
  address space**. Daftar frame segmen dialokasikan sesuai ukuran permintaan.
- Attachment memakai first fit di `0x90000000–0xd0000000`, dengan satu guard
  page setelah setiap mapping. VA dapat digunakan ulang setelah detach.
- Alias, fork, dan thread tetap menunjuk backing yang sama. SHM tidak menjadi CoW.
- Detach terakhir membebaskan segmen, sesuai kontrak sebelumnya. Destroy hanya
  berhasil untuk segmen yang tidak sedang terpasang; creator yang keluar juga
  membersihkan segmen miliknya yang belum terpasang.
- Alokasi frame, metadata, dan pemetaan parsial gagal tanpa menerbitkan attachment
  atau kehilangan referensi segmen. ID tidak di-wrap ke angka negatif/ID lama.

API SHM masih memakai ID global dan belum memiliki ACL atau capability per segmen.
Ini bukan model isolasi IPC final untuk aplikasi yang saling tidak dipercaya.

## Pengujian

```sh
make test-user-memory
python3 tests/run_memory_boot_tests.py
python3 tests/run_scheduler_boot_tests.py
python3 tests/run_uaccess_boot_tests.py
python3 tests/run_smp_boot_tests.py
make test-core
```

Runner scheduler/memory/uaccess/core/umem menerima
`--log-dir .test-results/serial` untuk menyimpan log guest melewati akhir sesi.

Suite `umem` menjalankan matriks 1/2/4/8 vCPU dengan RAM 512 MiB serta 4 vCPU
pada RAM 64/128 MiB. Fixture kernel menguji ELF 300 halaman, alokasi lebih dari
3 MiB, zeroing, guard, reuse VA, batas input, metadata OOM, setiap titik kegagalan
alokasi/pemetaan kecil, resize OOM, SHM lebih dari 2 MiB, batas segmen/attachment,
fork/CoW, dan jumlah frame setelah cleanup.
Regresi VMM juga mencakup collision di tengah rentang, lubang mapping, batas VA,
dan pelepasan lebih dari 16 tabel halaman. Kegagalan alokasi PT setelah 512
halaman terpetakan harus mengembalikan seluruh mapping dan frame yang baru.

Program Ring 3 memakai libc produksi untuk malloc/free/realloc, fork blok lebih
dari 2 MiB, alias SHM besar, serta 64 siklus alokasi/resize/free/attach/detach pada
thread di setiap CPU. Setelah seluruh proses/thread direap, pemeriksa memverifikasi
jumlah frame dan stack kembali ke baseline. Ini pengujian correctness, bukan
benchmark allocator atau soak test jangka panjang.

Validasi 31 Oktober 2026:

| Suite | Hasil |
|---|---|
| Memori user dinamis (`umem`) | 6/6 konfigurasi lulus; batas pemeriksaan guest tetap 30.000 tick. |
| Regresi memori | 6/6 konfigurasi lulus, termasuk rollback rentang dan pembebasan lebih dari 16 tabel. |
| Regresi scheduler | 6/6 konfigurasi lulus. |
| Akses buffer userspace | 6/6 konfigurasi lulus. |
| Boot SMP | 6/6 kasus lulus, termasuk fallback, topologi 17 vCPU, dan kegagalan stack AP. |
| Core dan input QMP | 6/6 konfigurasi lulus. |
| Host PMM, SMP, input | Lulus; SMP mencakup 210 topologi. |
| Validator ELF dengan UBSan | 44 kasus penolakan, guard page, batas kapasitas, dan 20.000 mutasi lulus. |

Total **36 kasus boot QEMU lulus** dengan GCC 16.2.1 dan QEMU 10.2.2.
`make -B -j4 all` berhasil tanpa warning compiler, dan validator menerima
`shell.elf` serta `app.elf` hasil build tersebut. Log berada di `.test-results/`
(diabaikan oleh Git); perintah di atas dapat digunakan untuk mengulang validasi.
