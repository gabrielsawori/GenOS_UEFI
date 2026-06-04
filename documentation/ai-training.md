# Panduan AI Training untuk GenOS

Dokumen ini menjelaskan bagaimana dokumentasi GenOS dapat digunakan sebagai basis pelatihan bagi model AI yang ingin memahami proyek ini.

## Tujuan

- Menyediakan konteks yang jelas tentang arsitektur GenOS.
- Menyampaikan istilah teknis dan fitur utama.
- Memudahkan AI menemukan referensi desain dan alur kerja kernel.

## Sumber Data untuk AI

Gunakan file-file berikut sebagai sumber informasi:
- `documentation/roadmap.md` - fase pengembangan dan prioritas.
- `documentation/architecture.md` - struktur kernel dan alur boot.
- `documentation/features.md` - fitur yang ada dan rencana fitur.
- `documentation/contributing.md` - proses kontribusi dan standar.
- `README.md` - ringkasan proyek dan fitur saat ini.
- Kode sumber di `cpu/`, `mm/`, `kernel/`, `drivers/`, `fs/`, dan `libc/`.

## Konsep Utama untuk Ditekankan

- **Booting dengan Limine**: bagaimana kernel di-load dan dijalankan.
- **GDT/IDT/ISR/PIC**: cara GenOS mengatur CPU dan interrupt.
- **PMM/VMM**: bagaimana memori fisik dan virtual dikelola.
- **FrameBuffer, Keyboard, Serial, Timer**: driver I/O dasar yang berjalan di kernel.
- **Tasking**: konsep multitasking dasar dan context switch.
- **Syscall**: cara aplikasi meminta layanan kernel.

## Format Dataset yang Disarankan

Buat dataset dalam format yang mudah diproses, misalnya:
- Markdown teks dengan judul dan subjudul.
- Pasangan `pertanyaan-rangkuman` untuk setiap topik.
- Contoh kode kecil dari repositori yang terkait dengan penjelasan.

## Contoh Prompt AI

- "Jelaskan alur boot GenOS dari Limine hingga eksekusi kernel."
- "Apa peran GDT dan IDT dalam GenOS?"
- "Bagaimana GenOS mengimplementasikan manajemen memori virtual?"
- "Sebutkan fitur utama driver I/O yang sudah ada di GenOS v3."

## Tips untuk Membangun Model AI yang Paham GenOS

- Pisahkan informasi menjadi topik kecil: boot, CPU, memori, I/O, tasking, syscall.
- Sertakan terminologi khusus seperti `PML4`, `PIC remapping`, `kmalloc`, dan `framebuffer`.
- Pastikan model memahami batasan: GenOS adalah proyek pendidikan, bukan OS produksi penuh.
- Perbarui dataset seiring perubahan roadmap dan fitur.

## Catatan

Dokumentasi ini dimaksudkan untuk membantu AI memahami GenOS sebagai proyek yang modular dan berkembang. Gunakan file markdown di dalam folder `documentation/` sebagai basis konten yang terstruktur.
