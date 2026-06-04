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
