# Bug List — `isr.S`

Dokumen ini berisi bug dan risiko yang ditemukan saat review `isr.S`, beserta prioritas dan arah perbaikannya.

## Prioritas

- 🔴 **Critical** — harus diperbaiki sebelum mengandalkan scheduler / user-mode.
- 🟠 **Important** — perlu diperbaiki agar desain interrupt lebih aman dan konsisten.
- 🟡 **Future / Low priority** — bukan bug langsung untuk konfigurasi sekarang.

---

# 1. 🔴 `struct registers` harus persis cocok dengan stack layout `isr.S`

## Masalah

Setiap interrupt masuk ke `isr_common` dan assembly melakukan push register dengan urutan:

```text
RAX
RCX
RDX
RBX
RBP
RSI
RDI
R8
R9
R10
R11
R12
R13
R14
R15
vector
error_code
RIP
CS
RFLAGS
RSP
SS
```

`isr_common` kemudian melakukan:

```asm
mov %rsp, %rdi
call isr_handler
```

Artinya pointer yang diterima C menunjuk langsung ke layout tersebut.

`task.c` juga menggunakan `struct registers` untuk menyimpan dan mengembalikan context task.

Jika definisi `struct registers` berbeda satu field atau satu urutan saja, scheduler dapat menulis nilai ke posisi yang salah. Akibatnya `iretq` dapat mengambil data yang salah sebagai:

```text
RIP
CS
RFLAGS
RSP
SS
```

yang bisa menyebabkan `#GP`, `#SS`, `#PF`, `#DF`, sampai triple fault.

## Perbaikan

Pastikan `struct registers` memiliki urutan yang **100% identik** dengan stack frame assembly.

Target konseptual:

```c
struct registers {
    uint64_t rax;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rbx;
    uint64_t rbp;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;

    uint64_t vector;
    uint64_t error_code;

    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};
```

**Catatan:** contoh di atas harus diverifikasi terhadap header aktual. Jangan langsung mengganti header sebelum dibandingkan dengan assembly.

---

# 2. 🔴 Defensive CS/SS validation tidak dapat memulihkan seluruh interrupt frame

## Masalah

Kode melakukan:

```asm
movw $0x08, 16(%rsp)
movw $0x10, 40(%rsp)
```

atau:

```asm
movw $0x1B, 40(%rsp)
```

Ini hanya memperbaiki `CS` dan `SS`.

Jika frame sudah rusak dan misalnya:

```text
RIP  = garbage
RSP  = garbage
RFLAGS = garbage
```

maka `iretq` tetap dapat fault.

Alurnya dapat menjadi:

```text
corrupted frame
    ↓
iretq
    ↓
#GP / #SS
    ↓
#DF
    ↓
triple fault
```

## Perbaikan

Anggap CS/SS validation sebagai **diagnostic / sanity check**, bukan sebagai recovery lengkap dari corrupted frame.

Jangan menganggap mengganti CS dan SS saja membuat `iretq` aman.

Untuk benar-benar menangani corrupted frame, perlu desain recovery/fault path yang lebih kuat.

---

# 3. 🟠 `DS/ES` baru diubah setelah `isr_handler()`

## Masalah

Urutan sekarang:

```text
interrupt
  ↓
push register
  ↓
cld
  ↓
isr_handler()
  ↓
restore register
  ↓
ubah DS/ES
  ↓
iretq
```

Jadi C handler berjalan terlebih dahulu dengan nilai `DS/ES` yang diwariskan dari context sebelumnya.

Pada kernel sederhana ini kemungkinan besar tidak langsung bermasalah, tetapi desain tersebut bergantung pada segment state yang selalu valid.

## Perbaikan yang disarankan

Jika tujuanmu adalah memastikan C ISR selalu berjalan dengan segment kernel, pertimbangkan mengatur:

```asm
DS = 0x10
ES = 0x10
```

sebelum:

```asm
call isr_handler
```

Kemudian pastikan state yang diperlukan untuk context user dikembalikan dengan benar sebelum `iretq`.

Jangan melakukan perubahan ini secara membabi buta sebelum melihat keseluruhan syscall/interrupt design.

---

# 4. 🟠 Error-code exception harus memiliki layout yang konsisten

## Masalah

Exception tanpa error code menggunakan:

```asm
push $0
push $i
```

Sedangkan exception dengan error code menggunakan:

```asm
push $i
```

Karena CPU sudah menyediakan error code untuk exception tersebut.

Konsep ini benar, tetapi hasil akhirnya harus selalu memiliki layout:

```text
registers
vector
error_code
iret_frame
```

Dengan kata lain, `isr_handler()` dan `struct registers` tidak boleh memperlakukan posisi error code secara berbeda antara exception yang memiliki dan tidak memiliki error code.

## Perbaikan

Verifikasi seluruh daftar exception berikut:

```text
#DE  0  no error
#DB  1  no error
NMI  2  no error
#BP  3  no error
#OF  4  no error
#BR  5  no error
#UD  6  no error
#NM  7  no error
#DF  8  error code
#TS 10  error code
#NP 11  error code
#SS 12  error code
#GP 13  error code
#PF 14  error code
#AC 17  error code
#CP 21  error code
#SX 30  error code
```

Kemudian cocokkan dengan macro `ISR_ERRCODE` / `ISR_NOERRCODE`.

---

# 5. 🟡 `irq_stub_table` hanya menangani IRQ 0–15

## Kondisi sekarang

Tabel:

```asm
irq_stub_table:
    .quad irq0, irq1, irq2, irq3, irq4, irq5, irq6, irq7
    .quad irq8, irq9, irq10, irq11, irq12, irq13, irq14, irq15
```

sesuai dengan 16 IRQ legacy PIC:

```text
IRQ 0–15
```

dan timer LAPIC memiliki stub terpisah:

```asm
lapic_timer_stub:
```

## Status

**Bukan bug langsung** jika sistem sekarang memang menggunakan PIC untuk IRQ legacy dan LAPIC timer untuk timer.

Tetapi ketika nanti menggunakan IOAPIC/MSI/MSI-X, desain interrupt vector perlu diperluas.

Tidak perlu menjadi prioritas sekarang.

---

# 6. 🟢 `cld` pada entry ISR adalah keputusan yang baik

Ini bukan bug.

Kode:

```asm
cld
```

berguna karena C handler dapat menggunakan operasi memory/string yang bergantung pada Direction Flag.

Yang juga bagus: `iretq` nantinya mengambil kembali `RFLAGS` dari interrupt frame, sehingga DF context yang diinterupsi tetap dapat dipulihkan.

**Jangan menghapus `cld` hanya karena terlihat tidak perlu di QEMU.**

---

# Urutan perbaikan yang disarankan

Kerjakan dengan urutan:

```text
1. 🔴 Verifikasi struct registers
       ↓
2. 🔴 Verifikasi layout interrupt frame
       ↓
3. 🔴 Verifikasi iretq dengan frame Ring 0 dan Ring 3
       ↓
4. 🟠 Verifikasi error-code handling
       ↓
5. 🟠 Review DS/ES handling
       ↓
6. 🟡 IOAPIC/MSI future work
```

## Target akhir

Untuk interrupt dari Ring 3, frame harus konsisten:

```text
                 higher address
┌──────────────────────────────┐
│ SS                           │
│ RSP                          │
│ RFLAGS                       │
│ CS                           │
│ RIP                          │
│ error_code                  │
│ vector                      │
│ R15                          │
│ R14                          │
│ R13                          │
│ R12                          │
│ R11                          │
│ R10                          │
│ R9                           │
│ R8                           │
│ RDI                          │
│ RSI                          │
│ RBP                          │
│ RBX                          │
│ RDX                          │
│ RCX                          │
│ RAX                          │ ← %rsp saat isr_handler()
└──────────────────────────────┘
                 lower address
```

`struct registers` harus merepresentasikan bagian frame ini mulai dari `RAX`.

---

# File berikutnya untuk audit

Setelah memperbaiki/verifikasi bagian di atas, file yang paling penting untuk dikirim berikutnya adalah:

```text
isr.h
```

atau file apa pun yang mendefinisikan:

```text
struct registers
isr_handler()
```

Setelah itu urutannya:

```text
isr.h / registers
        ↓
syscall_asm.S
        ↓
pmm.c
        ↓
vmm.c
```

**Catatan:** daftar di atas hanya mencakup temuan dari review `isr.S`. Belum termasuk bug yang mungkin ada di `struct registers`, karena file tersebut belum diperiksa.
