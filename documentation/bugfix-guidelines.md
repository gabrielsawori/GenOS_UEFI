# Panduan Bugfix GenOS

Dokumen ini membantu pengembang menemukan, mereproduksi, dan memperbaiki bug di GenOS.

## Alur Bugfix

1. Temukan masalah.
2. Reproduksi menggunakan QEMU atau debugging serial.
3. Catat langkah reproduksi, output yang muncul, dan file yang terlibat.
4. Perbaiki kode secara lokal.
5. Uji perbaikan secara berulang.
6. Tambahkan dokumentasi atau komentar jika diperlukan.
7. Ajukan perubahan ke branch fitur/bugfix baru.

## Alat Debugging Utama

- `make run` di QEMU untuk menjalankan GenOS.
- Serial output COM1 untuk melihat log jika layar gagal.
- Tes inisialisasi boot untuk memeriksa GDT/IDT/paging.
- Driver framebuffer untuk verifikasi output visual.

## Langkah Reproduksi Bug

Gunakan format berikut untuk setiap bug:

- **Ringkasan:** deskripsi singkat.
- **Lingkungan:** QEMU, build host, versi sumber.
- **Langkah:** langkah 1..n untuk memicu bug.
- **Hasil aktual:** apa yang muncul.
- **Hasil yang diharapkan:** hasil yang benar.
- **File terkait:** file kode atau dokumen yang relevan.

## Strategi Debugging

### 1. Debugging Boot

- Periksa apakah Limine memuat kernel.
- Pastikan `linker.ld` dan entry point sesuai.
- Tinjau output serial untuk pesan awal.
- Cek apakah GDT/IDT diinisialisasi tanpa fault.

### 2. Debugging Interrupt/Exception

- Pastikan IDT memetakan semua exception standar.
- Gunakan handler ISR yang mencetak error.
- Periksa apakah PIC berhasil diremap.
- Cek apakah IRQ digabungkan ke handler yang benar.

### 3. Debugging Memori

- Periksa tabel paging dan isi PML4.
- Pastikan alokasi PMM mengembalikan frame yang valid.
- Uji `kmalloc` dan `kfree` untuk kebocoran sederhana.
- Cek apakah alamat virtual dipetakan dengan benar.

### 4. Debugging Driver

- Mulai dengan serial logging untuk melihat status driver.
- Pastikan framebuffer menulis karakter.
- Periksa scancode keyboard dan translasi.
- Uji timer dengan interrupt periodik.

### 5. Debugging User Mode & Syscall

- Pastikan `syscall_init()` **selalu dipanggil SEBELUM** `create_user_task()`.
  Jika tidak, aplikasi Ring-3 yang langsung memanggil syscall akan memicu #GP.
- Cek register STAR di MSR 0xC0000081:
  - `STAR[47:32]` = Kernel CS (harus 0x08)
  - `STAR[63:48]` = nilai yang akan di-OR 3 oleh CPU untuk user CS (harus 0x23)
- Cek instruksi `mov %rsp, label` vs `mov %rsp, label(%rip)` — dalam kode 64-bit
  PIC/PIE atau dengan model kernel, selalu gunakan RIP-relative addressing!
- Untuk memverifikasi user stack: RSP harus 16-byte aligned sebelum CALL pertama
  di user mode. Gunakan `(stack_top - 16) & ~0xF`.
- Jika scheduler hang setelah aplikasi selesai: pastikan loop `do-while` punya
  batas iterasi agar tidak infinite loop ketika semua task TASK_DEAD.

### 6. Debugging Syscall Assembly (`syscall_asm.S`)

- **Return value hilang?** Periksa apakah `pop %rax` menimpa return value dari
  `call syscall_handler`. RAX harus disimpan ke register lain (misal RDI) SEBELUM
  pop sequence, lalu dikembalikan ke RAX SETELAH user RSP di-restore.
- **Nomor syscall salah?** Jangan gunakan RAX untuk menyimpan data sementara antara
  entry dan argument shuffle! RAX = nomor syscall. Gunakan R10 untuk temp data.
- **Scheduler mengganggu return path?** Pastikan `in_syscall` flag di-set sebelum
  dan di-clear setelah syscall handler. Scheduler harus skip context-switch saat flag aktif.
- **Keyboard/IRQ tidak menyala selama syscall?** `FMASK=0x200` mematikan IF saat
  masuk syscall. Handler harus `sti` di awal dan `cli` sebelum return.

### 7. Debugging VMM & TLB

- **Page Fault saat re-mapping halaman?** Jika `vmm_map_page` menulis PTE baru untuk
  alamat yang sudah pernah di-map, CPU mungkin masih menggunakan mapping LAMA dari
  TLB cache. Selalu gunakan `invlpg` setelah menulis PTE.
- **Dua user task saling menimpa stack?** Pastikan setiap task punya alamat stack
  virtual UNIK. Gunakan static counter dengan gap (misal 64KB) antar stack.

### 8. Debugging Bootloader & Ramdisk

- **`shell.elf not found in ramdisk` padahal sudah masuk tar?** Periksa `limine.cfg`:
  setiap modul yang dibutuhkan kernel harus didaftarkan dengan baris
  `MODULE_PATH=boot:///<file>`. Tanpa itu, `module_request.response->module_count`
  bernilai 0 dan `tar_init()` tidak pernah dipanggil → semua `tar_read_file()`
  mengembalikan NULL.
- Verifikasi isi tar dengan `tar -tvf ramdisk.tar` sebelum mencurigai kode kernel.

### 9. Debugging Rendering Terminal (Tumpang Tindih Teks)

- **Teks baru tumpang tindih dengan teks lama?** Font 8x8 dengan `scale=2`
  menghasilkan cell 16x16 piksel, tetapi shell memakai tinggi baris 24 px →
  ada **8 px gap** vertikal yang TIDAK pernah ditimpa oleh `fb_draw_char`.
  Selain itu, glyph yang sempit (mis. `i`, `l`) hanya menyala beberapa kolom
  sehingga residu karakter sebelumnya tetap terlihat di kolom yang gelap.
- **Perbaikan baku:** Sebelum menulis baris baru, isi area baris penuh
  (`x,y,w=line_width,h=line_height`) dengan warna background lewat
  `fill_rect()` (syscall 10) atau `fb_fill_rect()` di kernel. Lakukan hal
  yang sama untuk satu cell saat backspace atau saat menulis ulang karakter
  di kolom yang sama.
- **Jangan mengandalkan `bg_color` `fb_draw_char` saja** — bg hanya menutupi
  area 8x8 (atau 16x16 bila scale=2) per glyph, bukan jarak antar baris/cell.

### 10. Debugging exec / wait Antar Task User

- **Shell tidak menampilkan prompt baru setelah child task selesai?** Pada
  desain awal, syscall `exec` membuat task baru lalu langsung return.
  Karena child task menulis di koordinat absolut sementara shell mencetak
  prompt di posisi cursornya, output saling menimpa & input user menumpuk
  di atas tulisan child.
- **Jangan blocking di kernel** untuk menunggu child! `kernel_stack_top`
  di-share oleh semua syscall. Jika shell menunggu di kernel handler
  (mis. `while(child->state != TASK_DEAD) hlt`), saat child melakukan
  syscall ia akan **mereset** RSP ke top kernel_stack dan menimpa frame
  kernel shell yang masih hidup → korupsi state.
- **Pola yang aman:** `exec` mengembalikan PID; tambahkan syscall
  non-blocking `wait_pid` yang hanya cek state task. Shell polling di
  Ring 3 dengan `user_sleep(50)` di antara cek. Saat shell tidur,
  ia berada di stack-nya sendiri di Ring 3 — kernel_stack hanya dipakai
  sebentar per syscall, jadi child bebas memakainya juga.
- **Setelah child selesai**, shell tidak tahu seberapa luas child menggambar.
  Cara paling rapi: panggil `clear_screen()` & reset cursor sebelum
  menampilkan prompt baru.

## Perbaikan dan Validasi

- Gunakan `make clean && make` setelah perubahan besar.
- Jalankan `make run` dan validasi output serial serta layar.
- Pastikan perbaikan tidak menghentikan fitur lain.
- Tambahkan komentar di dalam kode untuk bagian yang rentan.

## Dokumentasi Bugfix

Setiap perbaikan bug besar harus memperbarui:
- `documentation/roadmap.md` jika perubahan memengaruhi prioritas.
- `documentation/architecture.md` jika ada modifikasi arsitektur.
- `documentation/features.md` jika bug terkait fitur yang telah disebutkan.
- `documentation/contributing.md` jika alur kontribusi baru diperlukan.

## Contoh Laporan Bug

- **Judul:** Kernel crash ketika menginisialisasi PIC.
- **Lingkungan:** QEMU 7.0, build terbaru.
- **Langkah:** `make clean && make && make run`.
- **Hasil aktual:** tampilan hang di layar hitam, tidak ada shell.
- **Hasil yang diharapkan:** shell muncul dengan prompt.
- **Debug:** output serial menunjukkan `IRQ handler invalid`.
- **Perbaikan:** betulkan vektor PIC dan handler IRQ di `cpu/pic.c`.
