# Fitur GenOS v3

Dokumen ini menjelaskan fitur yang saat ini ada di GenOS, serta fitur yang direncanakan untuk fase selanjutnya.

## Fitur Saat Ini

### CPU & Interrupt
- GDT untuk konfigurasi segment dan hak akses.
- IDT untuk memetakan interrupt ke penangan.
- ISR untuk menangani exception CPU.
- PIC remapping untuk interrupt perangkat keras.
- Implementasi dasar syscall (jika sudah ada dalam kode).

### Manajemen Memori
- Physical Memory Manager (`PMM`) untuk alokasi RAM fisik.
- Virtual Memory Manager (`VMM`) dengan paging 4-level.
- Heap allocator (`kmalloc` / `kfree`) untuk alokasi dinamis.

### Driver & I/O
- Framebuffer untuk menulis teks/grafis ke layar.
- Driver keyboard PS/2 untuk input pengguna.
- Driver timer PIT untuk tick sistem.
- Driver serial COM1 untuk debug.

### Shell & Aplikasi
- Shell interaktif Mandor yang berjalan di layar GenOS.
- Dukungan aplikasi dasar melalui `app/`.

### File System Awal
- Pembaca ELF untuk format executable.
- Pembaca TAR untuk asset atau package sederhana.

## Fitur Yang Direncanakan

### Stabilitas dan Debug
- Logging yang lebih baik untuk crash.
- Handler exception yang lebih rinci.
- Mode debug serial yang dapat dinyalakan.

### Manajemen Memori Lanjutan
- Sistem page allocator yang lebih optimal.
- Heap dengan fragmentasi rendah.
- Proteksi memori antar proses.

### Driver Tambahan
- Driver USB/USB legacy.
- Driver disk SATA/AHCI.
- Driver mouse sederhana.
- Driver audio dasar.

### Multitasking dan User Space
- Scheduler yang lebih canggih.
- Model proses dan thread.
- Mode user (Ring 3) untuk aplikasi.
- Shell dan program pengguna yang berjalan terisolasi.

### File System & Storage
- Virtual File System (VFS) dasar.
- Dukungan FAT32, ext2, atau filesystem sederhana lainnya.
- Dukungan load executable dari storage.

### Ekosistem dan Pengembangan
- Dokumentasi developer lengkap.
- Panduan instalasi lingkungan build.
- Contoh modul dan aplikasi.
- Fitur otomasi build/test.

## Tujuan Fitur Utama

- Menjadikan GenOS sebagai proyek pembelajaran OS yang jelas.
- Memudahkan kontributor baru memahami kode dan menambahkan fitur.
- Menyediakan base kernel yang dapat diperluas untuk riset dan eksperimen.
- Menyusun OS agar siap digunakan sebagai referensi oleh model AI.
