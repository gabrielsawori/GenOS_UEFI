# Panduan Kontribusi GenOS

Dokumen ini menjelaskan cara berkontribusi pada proyek GenOS, baik untuk pengembang, peninjau, maupun pemelihara.

## Cara Memulai

1. Clone repositori ke mesin lokal.
2. Pastikan lingkungan build Linux memiliki `gcc`, `ld`, `make`, `xorriso`, dan `qemu-system-x86_64`.
3. Jalankan `make` untuk memastikan proyek masih dapat dibangun.
4. Gunakan `make run` untuk menjalankan GenOS di QEMU.

## Alur Kontribusi

1. Buat branch baru berdasarkan `main` atau branch stabil.
   - Contoh: `feature/keyboard-improvement` atau `bugfix/paging-crash`
2. Kerjakan perubahan dengan fokus pada satu masalah atau satu fitur.
3. Tambahkan dokumentasi bila ada perubahan arsitektur atau fitur.
4. Uji perubahan dengan QEMU dan pastikan tidak merusak fitur existing.
5. Buat commit yang jelas dan ringkas.
6. Ajukan pull request dengan deskripsi perubahan dan hasil pengujian.

## Melaporkan Bug

Gunakan format ringkas berikut:
- **Judul:** deskripsi singkat dan jelas.
- **Langkah reproduksi:** bagaimana cara memunculkan bug.
- **Hasil saat ini:** apa yang terjadi.
- **Hasil yang diharapkan:** bagaimana seharusnya.
- **Informasi tambahan:** log serial, screenshot QEMU, perubahan terakhir.

## Standar Penulisan Kode

- Gunakan gaya C yang konsisten.
- Tambahkan komentar singkat pada bagian logika penting.
- Hindari perubahan besar di banyak modul dalam satu commit.
- Pastikan setiap fungsi memiliki tujuan jelas.

## Testing & Validasi

Langkah testing dasar:
- `make clean`
- `make`
- `make run`

Perhatikan output serial dan layar QEMU.

## Dokumentasi Perubahan

Setiap fitur baru atau perubahan logika penting harus dicatat di:
- `documentation/roadmap.md` jika mengubah prioritas fitur.
- `documentation/architecture.md` jika ada perubahan arsitektur.
- `documentation/features.md` jika ada fitur baru.

## Etika Kontribusi

- Hormati keputusan maintainer.
- Ajukan diskusi bila perubahan besar diperlukan.
- Jangan menghapus sejarah file penting tanpa persetujuan.

## Catatan Tambahan

Bekerja di proyek kernel berarti perubahan kecil dapat berdampak besar. Periksa setiap bug secara hati-hati, dan gunakan QEMU untuk eksperimen sebelum mencoba di hardware nyata.
