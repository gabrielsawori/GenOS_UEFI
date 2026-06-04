# Arsitektur Kernel GenOS

Dokumen ini menjelaskan struktur internal GenOS v3, alur boot, dan subsistem utama yang membentuk kernel.

## Struktur Folder Utama

- `cpu/` - kode CPU, GDT, IDT, ISR, PIC, syscall.
- `drivers/` - driver hardware seperti framebuffer, keyboard, serial, timer.
- `kernel/` - logika inti kernel, tasking, utils.
- `mm/` - manajemen memori fisik dan virtual.
- `libc/` - implementasi fungsi standar C yang digunakan oleh kernel atau aplikasi.
- `fs/` - pembacaan ELF dan TAR, penanganan file awal.
- `app/` - contoh aplikasi atau entry point program.
- `limine/` - bootloader Limine dan konfigurasi terkait.

## Alur Booting

1. Limine memuat kernel dari ISO/boot media.
2. Kernel masuk ke titik awal (`kernel entry`) yang didefinisikan pada `linker.ld`.
3. Kernel memulai konfigurasi CPU: GDT, IDT, paging, dan PIC.
4. Kernel menginisialisasi manajemen memori dan driver dasar.
5. Kernel menjalankan fungsi utama untuk melihat status dan memulai shell atau aplikasi.

## Subsystem Utama

### CPU & Interrupt

- `GDT` (Global Descriptor Table)
  - Menentukan segment selector untuk mode kernel dan user.
  - Penting untuk keamanan dan transisi mode.

- `IDT` (Interrupt Descriptor Table)
  - Menyimpan pointer ke rutinitas penanganan interrupt dan exception.
  - Kunci ketika kernel harus merespon fault, keyboard, atau timer.

- `ISR` / `IRQ`
  - `ISR` menangani interrupt CPU seperti divide-by-zero, page fault, dan breakpoint.
  - `IRQ` menangani interrupt perangkat keras melalui PIC.

- `PIC`
  - Meng-remap interrupt agar tidak bentrok dengan exception CPU.
  - Memungkinkan pengendalian interrupt legacy.

- `syscall`
  - Menyediakan cara aplikasi memanggil layanan kernel.
  - Diimplementasikan menggunakan instruksi 64-bit yang sesuai.

### Manajemen Memori

- `PMM` (Physical Memory Manager)
  - Mengelola blok memori fisik.
  - Menyediakan alokasi dan pembebasan page/frame.

- `VMM` (Virtual Memory Manager)
  - Menyusun tabel page 4-level (PML4, PDPT, PD, PT).
  - Memetakan alamat virtual ke alamat fisik.

- Heap Allocator
  - Implementasi `kmalloc` dan `kfree` untuk alokasi dinamis.
  - Berguna untuk struktur data runtime dan driver.

### Driver I/O

- `framebuffer`
  - Menulis teks dan grafis ke layar.
  - Digunakan untuk debug output dan shell internal.

- `serial`
  - Mengirim output log lewat COM1.
  - Sangat berguna saat framebuffer tidak tersedia.

- `keyboard`
  - Membaca scancode dari PS/2 keyboard.
  - Menerjemahkan menjadi karakter ASCII.

- `timer`
  - Mengonfigurasi PIT untuk interrupt periodik.
  - Digunakan untuk penjadwalan dan delay.

### Kernel & Tasking

- `kernel/` berisi fungsi inti seperti loop utama, inisialisasi subsistem, dan tasking dasar.
- `task` dan `task.h` menangani struktur tugas, context switch, dan penjadwalan sederhana.
- Shell pada GenOS dijalankan dari kernel untuk interaksi pengguna awal.

### File System dan Aplikasi

- `fs/elf.c` dan `fs/tar.c` menyediakan dukungan awal untuk format ELF dan TAR.
- Aplikasi dapat dimuat dari struktur file yang disediakan oleh kernel.
- Ini adalah dasar untuk mekanisme loading program di masa depan.

## Interaksi Antar Modul

- `kernel` memanggil `cpu` untuk inisialisasi CPU dan interrupt.
- `kernel` memanggil `mm` untuk mengatur memori sebelum alokasi dinamis.
- `kernel` memanggil `drivers` untuk mengaktifkan input/output.
- `kernel` memanggil `libc` untuk utilitas string, IO, dan fungsi standar.
- `kernel` dapat membaca data dari `fs` untuk memuat program atau sumber data.

## Konsep Kunci

- Modularity: setiap subsistem dirancang sebagai modul terpisah sehingga lebih mudah dikembangkan.
- Observabilitas: dokumentasi, logging, dan debug output menjadi prioritas sejak awal.
- Keamanan Dasar: penggunaan GDT/IDT dan paging untuk mencegah kesalahan proses merusak kernel.
- Ekstensibilitas: arsitektur memungkinkan penambahan driver, sistem file, atau subsistem baru tanpa merombak seluruh kernel.

## Catatan untuk Pengembang

- Gunakan `linker.ld` untuk memastikan layout memori dan posisi entry benar.
- Perbarui dokumentasi bila struktur kode berubah.
- Simpan kode driver dan subsistem tetap sederhana dan dapat diuji.
