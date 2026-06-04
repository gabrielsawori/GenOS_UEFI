# Roadmap Pengembangan GenOS

GenOS v3 adalah proyek kernel 64-bit yang dirancang untuk pembelajaran, eksperimen, dan pembangunan sistem operasi modular. Roadmap ini membagi pengembangan menjadi fase-fase yang jelas agar tim dapat fokus pada fondasi sebelum menambahkan fitur lanjutan.

## Visi GenOS

GenOS bertujuan menjadi kernel pendidikan modern yang:
- dapat dibangun dan dijalankan di lingkungan Linux/QEMU,
- mendukung debugging awal melalui serial dan framebuffer,
- menyediakan manajemen memori 64-bit yang kuat,
- menjalankan multitasking dasar,
- membangun antarmuka syscall untuk aplikasi,
- mudah dipahami dan diperluas oleh kontributor.

## Fase 0: Stabilitas & Penyusunan Dasar

Tujuan utama:
- Memastikan proyek dapat dibangun dengan `make` di Linux.
- Memastikan booting bekerja dengan Limine.
- Menetapkan struktur folder dan modul.
- Membuat dokumentasi awal.

Keluaran:
- Sistem build yang bersih.
- Dokumen `README.md`, `plan.txt`, dan folder `documentation/`.
- Ikhtisar arsitektur dan daftar fitur.

## Fase 1: Observabilitas & Debugging

Tujuan utama:
- Menyediakan mekanisme logging dan output.
- Memudahkan pengembang melacak bug saat booting atau crash.

Fitur utama:
- Framebuffer text printing dan graphic debugging.
- Serial debug output melalui COM1.
- Konsol awal untuk menampilkan pesan error.
- Penanganan interrupt dasar untuk menangkap exception.

## Fase 2: Arsitektur CPU & Interrupt

Tujuan utama:
- Menyusun landasan CPU 64-bit yang benar.
- Menangani exception dan interupsi perangkat keras.

Fitur utama:
- GDT dan IDT yang lengkap.
- Penanganan ISR, IRQ, dan PIC remapping.
- Dasar syscall 64-bit.

## Fase 3: Manajemen Memori

Tujuan utama:
- Implementasi manajemen memori 64-bit yang aman.

Fitur utama:
- Alokasi memori fisik (PMM).
- Paging 4-level (VMM) untuk memori virtual.
- Heap allocator (kmalloc/kfree).
- Struktur memori yang membatasi akses antar proses.

## Fase 4: Driver & I/O Dasar

Tujuan utama:
- Menghubungkan kernel dengan perangkat input/output.

Fitur utama:
- Driver keyboard PS/2.
- Driver timer PIT.
- Driver framebuffer dan kemungkinan grafis sederhana.
- Driver serial untuk debug dan komunikasi.

## Fase 5: Multitasking & Model Proses

Tujuan utama:
- Menjalankan lebih dari satu tugas secara bersamaan.

Fitur utama:
- Struktur task/process dasar.
- Penjadwalan sederhana (round-robin atau prioritas ringan).
- Context switch antar tugas.
- Dukungan mode user jika memungkinkan.

## Fase 6: Syscall & API Kernel

Tujuan utama:
- Menyediakan antarmuka bagi aplikasi untuk memanggil layanan kernel.

Fitur utama:
- Mekanisme syscall yang aman.
- API dasar: `read`, `write`, `exit`, `sleep`, `gettime`.
- Layer ring 3 jika tersedia.

## Fase 7: Storage, Filesystem, dan Device Enumeration

Tujuan utama:
- Mengelola penyimpanan dan file.

Fitur utama:
- Enumerasi perangkat PCI/SATA.
- Driver media penyimpanan sederhana.
- VFS dasar dan dukungan filesystem (misal tar, FAT32, ext2).
- Booting program dari storage.

## Fase 8: Polishing, Stabilitas, dan Dokumentasi

Tujuan utama:
- Menyelesaikan bug, menguatkan dokumentasi, dan menyiapkan rilis.

Fitur utama:
- Tes boot dan regression.
- Perbaikan arsitektur berdasarkan penggunaan nyata.
- Dokumentasi kode, tutorial pengembang, dan panduan kontribusi.
- Dokumentasi untuk AI dan dataset.

## Jalur Tambahan: Bugfix dan Rilis Berkala

Setiap fase harus mencakup:
- audit kode untuk bug kritis,
- review penggunaan memori dan stabilitas,
- pembuatan milestone rilis internal,
- update dokumentasi untuk perubahan fitur.

## Prioritas Awal Saat Ini

1. Perbaiki bug booting dan logika awal kernel.
2. Pastikan semua mekanisme debugging (serial, framebuffer) bekerja.
3. Lengkapi dokumentasi agar tim pengembang dapat bekerja dengan rapi.
4. Kembangkan modul berikutnya secara bertahap: CPU -> Memori -> Driver -> Tasking.

## Cara Menggunakan Roadmap Ini

- Baca setiap fase dan bandingkan dengan implementasi saat ini.
- Tandai fase yang sudah selesai dan tambahkan catatan bug di setiap milestone.
- Gunakan dokumentasi ini untuk merencanakan pekerjaan mingguan.
- Perbarui `documentation/roadmap.md` setiap ada perubahan prioritas besar.
