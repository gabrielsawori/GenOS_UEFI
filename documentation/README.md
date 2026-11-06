# Dokumentasi GenOS v4

**Prioritas 23 Oktober 2026:** core, multicore, dan driver pada QEMU x86-64
sebelum DE. [Status core terbaru](core-readiness.md) menjelaskan eksekusi task
pada AP, TLB shootdown, dan driver PCI/PS/2. Tabel migrasi di bawah adalah
catatan historis v3 → v4; mouse kini tersedia lagi melalui API event baru.

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
- [scheduler-memory-progress.md](scheduler-memory-progress.md) — Implementasi awal, tes regresi, dan batasan yang masih terbuka
- [memory-correctness.md](memory-correctness.md) — Kontrak mapping, kepemilikan CoW/SHM, rollback, dan tes memori
- [uaccess-correctness.md](uaccess-correctness.md) — Copy buffer user, recovery fault/OOM, adaptor syscall, dan tes Ring 3
- [elf-correctness.md](elf-correctness.md) — Validasi ukuran/format ELF, batas segmen dan entry point, serta tes rollback loader
- [user-memory.md](user-memory.md) — Heap user dinamis, free/realloc, tracking halaman, VA guard/reuse, dan SHM besar
- [task-context.md](task-context.md) — Context x87/SSE per task, isolasi exception aplikasi, pengujian multicore dan batas QEMU TCG
- [address-space-lifetime.md](address-space-lifetime.md) — Reference address space, retirement leader, pin dan urutan lock PMM
- [virtio-drivers.md](virtio-drivers.md) — API disk/Ethernet kernel, lifetime DMA, timeout dan tes QEMU
- [wait-and-async-io.md](wait-and-async-io.md) — Event, deadline, token request, pembatalan dan cleanup pemilik mati
- [interrupt-completion.md](interrupt-completion.md) — Shared INTx, lock IRQ/PIC, watchdog dan perubahan konfigurasi disk/NIC
- [device-locking.md](device-locking.md) — Lock disk/NIC per perangkat, preemption lokal, urutan lock dan reset serentak
- [heap-locking.md](heap-locking.md) — Lock metadata heap, fast-path antar-CPU, pertumbuhan transaksional dan tes OOM
- [vmm-locking.md](vmm-locking.md) — Lock page table, snapshot PTE, conditional replacement dan shootdown tanpa guard mutasi
- [Kebutuhan API desktop](../GenOS_Desktop/Concept/kernel-api-status.md) — Penghubung compositor, font dan decoder Ring 3
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
