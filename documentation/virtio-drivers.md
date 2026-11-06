# Driver disk dan Ethernet QEMU

Implementasi 5 November 2026: `drivers/virtio.c`, `block.c`, dan `net.c`.
Target awal PCI virtio transitional dengan transport I/O legacy: vendor
`1af4`, device `1001` (block) dan `1000` (net). Modern-only PCI, virtio MMIO,
AHCI/NVMe, USB dan NIC fisik belum ditangani.

## API kernel

| API | Kontrak |
|---|---|
| `block_count`, `block_get_info` | Maksimal dua disk; count termasuk perangkat dikenali yang gagal init, lihat `online` |
| `block_read`, `block_write` | Sektor 512 byte, 1–8 sektor per panggilan, validasi rentang tanpa overflow, buffer kernel |
| `block_flush` | FLUSH jika dinegosiasikan; filesystem mengatur urutan write/flush sendiri |
| `block_submit/collect/cancel` | Token request asinkron, satu slot per disk hingga collect/cancel |
| `net_count`, `net_get_info` | Maksimal dua NIC, MAC, link, online dan counter |
| `net_send` | Ethernet 14–1514 byte tanpa FCS/VLAN; pad nol sampai 60 byte; completion sinkron |
| `net_submit/collect/cancel` | TX asinkron dengan token dan copy frame saat submit |
| `net_receive` | Poll nonblocking: panjang positif, nol jika kosong, kode negatif bila error; buffer terlalu kecil mengonsumsi dan menghitung drop |
| `net_receive_wait` | RX menunggu event/deadline tanpa guard; reset membatalkan waiter lama |
| `block_shutdown`, `net_shutdown` | Reset perangkat dan lepaskan DMA; init ulang diizinkan setelah seluruh reset berhasil |

Kode return ada di `drivers/device.h`. API hanya menerima buffer kernel yang
valid sepanjang panggilan. I/O membutuhkan task context dan IRQ aktif.
Belum ada syscall raw disk/NIC, socket, ARP/IPv4/TCP/DHCP, filesystem persisten
atau mount disk. VFS masih ramdisk/tmpfs; persistensi sektor tidak berarti
file VFS sudah tersimpan ke disk.

## Queue, DMA, dan error

Split virtqueue menggunakan descriptor langsung, ukuran queue 4–1024 dari
perangkat, alignment legacy 4096 dan DMA contiguous PMM. Indeks menggunakan
acquire/release; completion tidak boleh melebihi descriptor outstanding.
Indeks 16-bit boleh wrap. [PCI INTx](interrupt-completion.md) membangunkan worker
completion; mode polling tersedia jika registrasi IRQ gagal. Belum ada MSI-X,
indirect descriptor, event-index, packed queue atau offload.

Block mempunyai satu request aktif dan bounce buffer 4 KiB. Header, status
dan data hidup hingga completion/reset. Status I/O error tidak menyalin buffer
baca ke caller; perangkat dapat menerima request berikutnya. Write read-only
ditolak sebelum submit. Panjang used legacy block diabaikan sesuai catatan
kompatibilitas spesifikasi; status tetap diperiksa.

NIC mempunyai queue RX/TX terpisah, hingga 32 buffer RX @2048 byte dan satu
buffer TX. Hanya MAC dan link status dinegosiasikan. Header legacy 10 byte,
panjang dan flags offload diperiksa sebelum copy. RX malformed/drop mendaur
ulang descriptor; ID completion rusak menghentikan perangkat. Buffer RX terbatas,
sehingga overload dapat menjatuhkan paket.

Transaksi queue diserialisasi [lock per perangkat](device-locking.md). Worker completion memeriksa
request dan timeout setelah 500 tick PIT. Caller sinkron tidur melalui wait
queue tanpa memegang guard; caller asinkron dapat melanjutkan pekerjaan lain.
Timeout/queue rusak membuat perangkat offline dan memulai reset. Buffer hanya dilepas
setelah status reset terbaca nol. Bila reset tidak diakui, DMA dikarantina
hingga reset berikutnya berhasil.

Kontrak token, cancellation, cleanup pemilik mati dan wait queue dijelaskan di
[wait dan I/O asinkron](wait-and-async-io.md). Worker kini menunggu event INTx
dan watchdog 50 tick. Init/shutdown dan pemeriksaan task pemilik masih memakai
guard global, di luar critical section data path. Belum ada klaim throughput, latency real-time, hotplug,
IOMMU, atau toleransi perangkat DMA berbahaya.

## Validasi tanpa disk/jaringan host

```sh
make test-drivers
python3 tests/run_driver_boot_tests.py --case quad
python3 tests/run_driver_boot_tests.py --case quad --irq-fallback --timeout 60
python3 tests/run_production_boot_test.py --virtio
gcc -std=gnu11 -O2 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
  tests/virtqueue_test.c drivers/virtio.c -Wl,--gc-sections -o /tmp/genos-virtqueue-test
/tmp/genos-virtqueue-test
```

Runner membuat dua raw disk 4 MiB **di temporary directory**, satu read-only.
Tidak menerima path disk pengguna. Dua NIC memakai `hubport` internal QEMU,
tanpa tap, bridge, forwarding, atau jaringan host. Perangkat dijalankan dengan
`disable-modern=on`.

Setiap konfigurasi boot dua kali dari disk sementara yang sama. Guest
memeriksa seed, read/write/flush, isi boot sebelumnya, batas sektor, read-only,
dan buffer caller. Host memeriksa byte persistensi dan bahwa seluruh disk
read-only tidak berubah. NIC bertukar 600 frame dua arah, minimum/maksimum,
padding nol dan reuse queue setelah drop. Worker semua CPU menguji I/O
serentak dan ownership buffer caller.

Fault injection link-time mencakup setiap alokasi DMA yang gagal, reset
ditolak (buffer wajib tertahan), notify ditahan untuk timeout nyata, status
I/O error, completion ID rusak, dan header/panjang RX rusak. Ini validasi jalur
driver, bukan kegagalan media fisik atau reset hardware yang benar-benar macet.
Tes ring host menjalankan 200.000 completion untuk wrap indeks 16-bit dan
menolak completion di luar outstanding.

Matriks: 1/2/4/8 vCPU pada 512 MiB dan 4 vCPU pada 64/128 MiB, masing-masing
dua boot. Log di `.test-results/serial/drivers-*.log`.

Referensi transport, queue, block dan network:
[OASIS Virtio 1.2](https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html),
bagian legacy/transitional PCI dan split virtqueue.
