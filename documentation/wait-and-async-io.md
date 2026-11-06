# Wait queue dan I/O asinkron

Tahap lanjutan 5 November 2026. API kernel ini menyiapkan wakeup dan lifetime
request sebelum filesystem persisten, stack IP dan compositor Ring 3.

## Event kernel

`kernel/wait.h` menyediakan queue berinisialisasi nol dengan sequence 64-bit.
Pola penggunaannya:

```c
for (;;) {
    uint64_t observed = wait_queue_snapshot(&event);
    if (predicate_is_ready()) break; /* lindungi state predicate sesuai owner */
    int result = wait_queue_wait(&event, observed, deadline);
    if (result == WAIT_TIMEOUT || result == WAIT_CLOSED) return;
}
```

Snapshot diambil **sebelum** memeriksa predicate. Signal mengubah sequence
dengan release; pembaca memakai acquire. Signal sebelum enrollment terdeteksi
oleh pemeriksaan sequence, dan signal setelah enrollment terdeteksi scheduler.
Sequence bukan penghitung item: consumer tetap harus memeriksa predicate lagi.

`wait_queue_signal()` tidak alokasi, mengambil lock atau berjalan melalui
task list, sehingga dapat dipanggil dari IRQ. Wakeup broadcast ditunda sampai
scan scheduler berikutnya yang berhasil mengambil guard mutasi. Belum ada
antrean runnable per CPU atau jaminan latency wakeup tertentu.

Task yang menunggu memakai `TASK_EVENT_WAIT`, terpisah dari wait PID/reaper.
Deadline berupa tick PIT absolut; `UINT64_MAX` berarti tanpa deadline.
Penantian membutuhkan IRQ aktif, task non-idle yang hidup, dan tidak boleh
memegang guard/lock. IRQ mati atau guard masih dipegang menghasilkan
`WAIT_INVALID`; deadline menghasilkan `WAIT_TIMEOUT`.

`wait_queue_close()` mencegah enrollment dan melepas subscription semua task
sebelum kembali. Task yang bangun tidak membaca queue lagi: hasil disimpan
dalam task. Owner boleh membebaskan queue setelah menutupnya **dan** menghentikan
producer/caller baru. Close tidak menggantikan reference ownership untuk
producer yang masih dapat mengakses pointer. Jangan reset sequence atau
memakai ulang storage selagi ada pemakai. Group termination dan reaper juga
melepas subscription, sehingga task mati tidak menahan queue.

## Submit, completion, collect

| API | Perilaku |
|---|---|
| `block_submit` | READ/WRITE/FLUSH; write disalin ke DMA kernel saat submit; destination read diberikan saat collect |
| `block_collect` | `DRIVER_PENDING`, hasil akhir, atau buffer terlalu kecil; buffer kecil mempertahankan hasil untuk retry |
| `net_submit`, `net_collect` | TX asinkron, frame disalin saat submit, status diambil kemudian |
| `block_cancel`, `net_cancel` | Lepas request; jika masih pending, reset perangkat sebelum buffer dapat dipakai ulang |
| `block_read/write/flush`, `net_send` | Adapter sinkron: menunggu event tanpa memegang guard; menolak pemanggilan saat guard/IRQ mati |
| `net_receive_wait` | Tidur sampai frame, deadline, atau perubahan epoch perangkat karena reset |

Setiap disk/NIC TX memiliki satu slot reservasi. Slot tetap dimiliki token
sampai collect/cancel, termasuk setelah completion. Submit kedua mengembalikan
`DRIVER_BUSY`. Adapter sinkron menunggu ketersediaan slot hingga 1.000 tick;
caller asinkron mengatur backpressure sendiri. Admission sinkron memakai FIFO
hingga 32 tiket per perangkat; submit async tidak mendahului penunggu.
Antrean penuh menunggu kapasitas sampai deadline. Hanya satu request DMA
in-flight; tidak ada antrean banyak descriptor request.

Token berisi ID request dan indeks perangkat. ID monotonik dibagi driver
block/net, tidak direset bersama perangkat; overflow menolak submit baru.
Token lama tidak dapat mengambil/membatalkan request baru. Shutdown membatalkan
token dan membangunkan waiter; event driver berada di storage permanen yang
tidak dihapus saat reinit. RX memakai epoch terpisah supaya waiter lama tidak
membaca dari instance perangkat baru.

Driver tidak menyimpan pointer buffer caller. Caller token asinkron wajib
collect/cancel, dan boleh memindahkan tanggung jawab itu ke kernel consumer
lain. Adapter sinkron mengikat request ke PID task di dalam lock perangkat
yang sama dengan submit. Worker membatalkan request bila pemilik mati; tidak mengakses
stack atau address space task tersebut. Request async eksplisit tidak terikat
otomatis ke PID dan belum diekspos lewat syscall.

Tiket admission menyimpan ID/deadline tanpa pointer task. Timeout menghapus
tiket, reset membatalkan semua tiket, dan tiket yang ditinggalkan task mati
kedaluwarsa pada deadline awalnya. Worker menghapus tiket kedaluwarsa tanpa
mengakses stack pemanggil. FIFO mengatur giliran yang sudah terdaftar;
antrean penuh dan penjadwalan task tetap dapat menghasilkan timeout.

Cancel/timeout tidak membatalkan efek write atau paket yang sudah diterima
perangkat. Status reset harus diakui sebelum DMA dibebaskan. Reset gagal
tetap mengarantina buffer. Perangkat yang direset karena cancel pending atau
timeout menjadi offline sampai shutdown/reinit. Filesystem nantinya perlu
menangani write dengan hasil yang tidak pasti.

## Worker dan locking

Kernel membuat satu worker completion pada BSP jika ada perangkat virtio
yang dikenali. Worker memeriksa completion block/TX dan perubahan queue RX,
lalu menunggu event submission/INTx atau watchdog 50 tick. Jika worker gagal dibuat saat boot, kernel
menghentikan driver. Tidak ada worker tambahan pada boot tanpa virtio.

Timeout request diperiksa setelah 500 tick PIT; penanganan menunggu worker
atau collect mendapat giliran. Tidak ada busy-wait 500 ms sambil menahan lock.
Signal/slot reservation menghubungkan completion dengan task waiter tanpa
pointer task yang bisa direklamasi.

Metadata dan operasi queue kini memakai [lock per perangkat](device-locking.md).
Worker melewati lock yang sedang sibuk. Pemeriksaan task pemilik dilakukan
di luar lock perangkat, lalu ID/PID divalidasi ulang sebelum cancellation.
Worker dan caller menunggu tanpa lock. Reset masih memiliki loop polling
terbatas. Jalur [IRQ completion](interrupt-completion.md) memiliki lock
registry/PIC terpisah; IRQ hanya mengakui status dan memberi event. Fairness
scheduler dan beberapa request DMA in-flight masih menjadi pekerjaan berikutnya.

## Tes

```sh
make test-wait
make test-drivers
python3 tests/run_production_boot_test.py --virtio
```

Suite wait memeriksa signal sebelum enrollment, signal IRQ PIT nyata, deadline,
context invalid, 1.000 handshake antarworker, broadcast pada setiap CPU,
close/free storage sebelum waiter melanjutkan, dan cleanup task yang dimatikan.

Suite driver tetap menguji disk sementara, persistensi dua boot, Ethernet
internal, OOM dan error. Tambahan mencakup perubahan buffer caller segera
setelah submit, completion oleh worker saat caller tidur, collect buffer kecil,
token lama/reset/cancel, RX yang benar-benar blocked lalu mendapat frame,
RX canceled saat shutdown, serta request dari task yang dimatikan.
Saat disk sengaja timeout, worker lain pada CPU yang sama harus tetap berjalan,
mengambil guard dan mengalokasikan/membebaskan heap sedikitnya sepuluh kali.

Hasil tahap ini: suite wait **6/6**, driver **12/12 boot**, dan enam suite
regresi core masing-masing **6/6**. Total **54 kasus QEMU**, ditambah dua
smoke test produksi standar/virtio. Log utama tersimpan di
`.test-results/wait-matrix.log`, `.test-results/async-drivers-matrix.log`,
`.test-results/async-*-matrix.log` dan `.test-results/core-matrix.log`.

Matriks kedua suite: 1/2/4/8 vCPU pada 512 MiB dan 4 vCPU pada 64/128 MiB.
Tes correctness QEMU TCG ini tidak membuktikan fairness di semua beban,
ketahanan hardware fisik atau latency desktop.
