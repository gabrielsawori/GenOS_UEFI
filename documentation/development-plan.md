# Rencana Kerja GenOS Selanjutnya

Dokumen ini memberikan fase kerja operasional yang lebih terperinci untuk pengembangan GenOS.

## Tujuan Prioritas Saat Ini

1. Stabilkan booting dan debugging dasar.
2. Pastikan subsistem CPU dan interrupt berfungsi.
3. Kembangkan dokumentasi agar tim dapat bekerja dengan satu arah.

## Deadline Mini-Milestones

- Minggu 1: Pastikan `make` dan `make run` bekerja tanpa error blok.
- Minggu 2: Verifikasi output serial dan framebuffer untuk debugging.
- Minggu 3: Konfirmasi GDT/IDT/ISR/PIC stabil dan tidak crash.
- Minggu 4: Pastikan manajemen memori dasar bekerja dan `kmalloc` valid.

## Tugas Teknis Utama

### Tugas 1: Diagnostik Boot
- Periksa entry point kernel di `linker.ld`.
- Pastikan `limine.cfg` dan `limine.h` terkonfigurasi.
- Validasi inisialisasi awal di kode kernel.

### Tugas 2: Observabilitas
- Perbaiki/tingkatkan serial COM1 logging.
- Tambahkan lebih banyak pesan status di framebuffer.
- Pastikan output laporan exception ditulis ke serial.

### Tugas 3: Stabilitas Interrupt
- Audit definisi GDT/IDT.
- Pastikan semua exception umum di-handle.
- Verifikasi PIC remapping dan IRQ masking.

### Tugas 4: Inspect Memori
- Lakukan pemeriksaan fungsi PMM dan VMM.
- Cek apakah tabel paging dibuat dengan benar.
- Tambahkan test alokasi memori dasar di kernel.

### Tugas 5: Dokumentasi dan Kontribusi
- Perbarui `documentation/` dengan setiap perubahan besar.
- Buat checklist PR untuk pengembang baru.
- Tambahkan panduan debugging dalam bugfix workflow.

## Task Flow Rekomendasi

1. Kerjakan satu subsistem terlebih dahulu (misal serial/debugging).
2. Uji dengan QEMU dan catat hasil.
3. Setelah stabil, lanjutkan ke subsistem berikutnya (CPU -> memori -> driver).
4. Jangan gabungkan lebih dari satu perubahan arsitektur besar dalam satu commit.

## Catatan untuk Kontributor

- Gunakan branch terpisah untuk setiap modul/fix.
- Tulis commit message ringkas dan jelas.
- Lampirkan screenshot atau output serial jika ada.
- Apabila menemui bug sulit, dokumentasikan percobaan debugging.
