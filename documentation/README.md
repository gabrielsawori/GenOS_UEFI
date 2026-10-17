# Dokumentasi GenOS v4

Folder ini berisi dokumentasi utama untuk **GenOS v4 Security Kernel**. GenOS telah berevolusi dari OS desktop menjadi kernel yang berfokus pada **cybersecurity** dan **backend development**.

## Perubahan dari v3 → v4

| Aspek | v3 | v4 |
|-------|----|----|
| Fokus | Desktop OS umum | Security Kernel |
| UI | Desktop + Terminal | Terminal-only (scroll buffer) |
| Mouse | PS/2 driver aktif | Dihapus |
| Security | Minimal | SMEP, NX/W^X, uaccess, PBKDF2 |
| Syscall | 49 (termasuk kmalloc) | 46 aktif (kmalloc dihapus) |
| Terminal | Statis, tidak bisa scroll | 256-line history + Page Up/Down |

## Isi Folder

- `roadmap.md` — Fase pengembangan dan peta jalan GenOS v4
- `architecture.md` — Arsitektur kernel, subsistem, dan alur boot
- `features.md` — Daftar fitur saat ini dan yang direncanakan
- `contributing.md` — Panduan kontribusi dan pelaporan bug
- `bugfix-guidelines.md` — Alur bugfix, debugging, dan validasi
- `development-plan.md` — Rencana kerja dan prioritas
- [scheduler-memory-plan.md](scheduler-memory-plan.md) — Audit, prioritas perbaikan, dan tahapan peningkatan scheduler/memori
- `ai-training.md` — Panduan untuk pelatihan AI dengan kode GenOS

## Security Checklist (v4)

- [x] SMEP enabled (CR4 bit 20)
- [x] NX/W^X enforcement (EFER.NXE + PTE bit 63)
- [x] User pointer validation di semua syscall
- [x] Kernel heap tidak accessible dari Ring 3
- [x] AES-256-CBC, SHA-256, HMAC, PBKDF2, CSPRNG
- [x] Encrypted keystore
- [x] PBKDF2 credential authentication
- [ ] SMAP (Supervisor Mode Access Prevention) — planned
- [ ] Syscall filtering (mini-seccomp) — planned
- [ ] Audit logging — planned
- [ ] Network stack — planned
