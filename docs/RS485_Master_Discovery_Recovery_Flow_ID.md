# RS485 Master Discovery & Recovery Flow

Dokumen ini menjelaskan flow RS485 master berdasarkan kode aktif:

- `src/rs485_manager.cpp`
- `src/rs485_manager.h`
- `src/data.cpp`

Bahasa gampangnya: master punya 2 mode besar.

1. **Normal / auto recovery known slave**
   Master menjaga slave yang sudah pernah dipair dan MAC-nya sudah tersimpan.

2. **Discover / pairing unknown slave**
   Master mencari slave baru yang MAC-nya belum ada di registry. Ini harus diklik manual.

## Kesimpulan Paling Penting

Kalau master sudah hidup beberapa menit lalu slave dicolok dan otomatis muncul tanpa klik Discover, itu **bukan berarti master selalu Discover slave baru**.

Yang terjadi kemungkinan besar:

```text
Slave itu MAC-nya sudah pernah tersimpan di master
  |
  v
Master anggap dia known slave
  |
  v
Master auto recovery ke address 247 tiap 10 detik
  |
  v
Slave yang baru dicolok hidup di 247
  |
  v
MAC cocok
  |
  v
Slave dipulihkan ke address lama, misal 0x03
  |
  v
Master apply ulang assignment/profile tersimpan
  |
  v
UI muncul online
```

Jadi ada 2 definisi:

| Istilah | Arti | Bisa muncul otomatis? |
|---|---|---|
| **Known slave** | MAC sudah pernah tersimpan di master | Ya, lewat auto recovery |
| **Unknown/new slave** | MAC belum pernah tersimpan di master | Tidak, harus klik Discover |

Untuk known slave, master juga mengingat profile dan sensor assignment terakhir. Contoh: kalau slave dulu disimpan sebagai `TEMP_NODE`, saat dia auto recovery lagi master langsung menulis ulang assignment `TEMP_NODE` ke slave. Slave tidak perlu dipilih ulang kecuali user memang mau ganti profile.

## Address Yang Dipakai

| Address | Fungsi |
|---:|---|
| `0x01` | Master secara konsep |
| `0x02..0xF6` / `2..246` | Address normal slave setelah dipair |
| `0xF7` / `247` | Default slave setelah boot, pairing, dan recovery |
| `0x00` | Broadcast, tidak dipakai untuk read normal |

## Timing Aktual Dari Kode

| Timing | Nilai | Dipakai untuk |
|---|---:|---|
| RS485 task loop | `10 ms` | Memproses request, pairing, command, dan scheduler polling |
| Modbus timeout per attempt | `100 ms` | Tunggu respons tiap request |
| Retry | `1` | Total request = 2 attempt |
| Normal polling tick | `1000 ms` | Tiap 1 detik master proses 1 slave registry |
| Discover scan interval | `700 ms` | Saat Discover aktif |
| Known scan window saat Discover | `4000 ms` | 4 detik awal Discover juga cek address lama |
| Identity refresh | `10000 ms` | Refresh identity known slave |
| Capability refresh | `5000 ms` | Refresh capability/assignment known slave |
| Offline timeout | `5000 ms` | Kalau last_seen lewat ini, slave dianggap offline |
| Degraded threshold | `3 failed attempt` | Menandai komunikasi mulai bermasalah |
| Offline fail threshold | `5 failed attempt` | Menandai slave offline |
| Discover timeout | `30000 ms` | Discover berhenti sendiri |
| Auto recovery interval | `10000 ms` | Recovery known slave diulang tiap 10 detik per slave |

Catatan cara membaca timing:

- Tick `1000 ms` memproses **satu entry registry**, bukan semua slave sekaligus.
  Jika ada `N` entry, slave yang sama normalnya mendapat giliran lagi sekitar
  `N x 1 detik`.
- Threshold degraded/offline menghitung **failed attempt**, bukan jumlah tick.
  Karena satu transaksi memiliki satu retry, transaksi yang gagal penuh dapat
  menambah dua failed attempt.
- Satu transaksi yang tidak mendapat respons dapat memakan sekitar `200 ms`
  dari dua attempt dengan timeout `100 ms`, belum termasuk overhead bus.

## Tiga Skenario Lapangan

### 1. Master dan Slave Pertama Kali Hidup

```text
Master belum punya saved registry
Slave baru boot dan menunggu di address 247
  |
  v
Master tidak auto-discover dan tetap idle
  |
  v
User klik Discover
  |
  v
Master scan tiap 700 ms, maksimal 30 detik
  |
  v
Master baca MAC/capability, user pilih profile
  |
  v
Master tulis assignment dan address 2..246
  |
  v
Master simpan registry lalu polling normal
```

Discovery wajib dipicu user karena master tidak boleh otomatis mendaftarkan
unknown slave.

### 2. Master dan Known Slave Pernah Terhubung, Lalu Keduanya Mati

```text
Master boot dan load MAC/address/profile dari NVS
Slave reboot dan kembali ke address 247 karena RAM-only
  |
  v
Pada giliran registry pertama, master kirim recovery MAC + saved address ke 247
  |
  v
Slave dengan MAC cocok pindah ke saved address
  |
  v
Master confirm identity pada saved address
  |
  v
Master restore assignment/profile lalu poll sensor
```

Dengan satu slave, recovery pertama biasanya dimulai sekitar slot polling
pertama, yaitu sekitar `1 detik` setelah task RS485 berjalan. Dengan beberapa
entry, recovery dilakukan bergiliran satu entry per detik. Jika gagal, recovery
slave yang sama dibatasi minimal setiap `10 detik`.

### 3. Kabel RS485 Dicabut dan Dipasang Lagi, Power Master/Slave Tetap Hidup

Slave tidak reboot, sehingga slave **tetap memakai saved assigned address** dan
tidak kembali ke `247`.

Aturan alamatnya sederhana:

Untuk kasus kabel-only, urutan setiap giliran slave selalu dimulai dari assigned
address:

1. Master mencoba polling assigned address `0x03`.
2. Jika `0x03` berhasil, giliran selesai. Master tidak mencoba `247`.
3. Jika `0x03` gagal, master mengecek apakah recovery `10 detik` sudah boleh
   dicoba.
4. Jika belum 10 detik, giliran selesai setelah kegagalan `0x03`.
5. Jika sudah boleh, master menulis recovery ke `247`, lalu mencoba konfirmasi
   kembali ke `0x03`.

Jadi urutan request dalam satu giliran yang menjalankan recovery adalah:

```text
poll 0x03 gagal
  -> recovery write ke 247 gagal/tidak cocok
  -> recovery confirm ke 0x03 gagal
  -> giliran selesai
```

Saat kabel sudah dipasang kembali, urutannya menjadi:

```text
poll 0x03 berhasil
  -> slave langsung online
  -> tidak lanjut recovery ke 247
```

Jadi master **tidak menemukan slave melalui Discover** pada skenario ini.
Master sudah mengetahui alamat `0x03` dari registry dan terus mencoba alamat
tersebut melalui polling normal. Slave baru terbaca kembali ketika request
polling dan alamat yang didengarkan slave sama-sama `0x03`.

```text
Kabel dicabut
  |
  v
GILIRAN SLAVE DIMULAI
  |
  v
Master poll 0x03 + satu retry
  |
  +--> BERHASIL
  |      Slave online, giliran selesai
  |
  +--> GAGAL
         Failed attempt naik
         Slave dapat menjadi degraded/offline
         |
         +--> Recovery 10 detik belum due
         |      Giliran selesai
         |
         +--> Recovery 10 detik sudah due
                Write recovery ke 247
                Confirm kembali ke 0x03
                Tetap gagal selama kabel terputus

Kabel dipasang kembali
  |
  v
GILIRAN SLAVE BERIKUTNYA
  |
  v
Master poll 0x03
  |
  v
Slave menjawab, master clear consecutive_fail dan slave online
  |
  v
Giliran selesai tanpa mencoba 247
```

Kasus ini tidak membutuhkan Discover atau pairing ulang. Waktu reconnect setelah
kabel dipasang kembali secara nominal sekitar `1 detik` untuk satu entry, atau
hingga sekitar `jumlah entry x 1 detik` sebelum slave mendapat giliran lagi.

### Jangan Campur Tiga Istilah Ini

| Istilah | Dipicu oleh | Tujuan | Bisa membuat kabel-reconnect berhasil? |
|---|---|---|---|
| **Polling normal** | Otomatis setiap giliran registry | Membaca assigned address seperti `0x03` | **Ya**, karena powered slave masih di `0x03` |
| **Auto-recovery** | Known slave terlihat lost/offline | Memulihkan known slave yang reboot dan kembali ke `247` | Tidak pada kasus kabel-only, karena slave masih di `0x03` |
| **Discover/pairing** | User menekan Discover atau command debug pairing | Mencari dan mendaftarkan unknown slave di `247` | Tidak digunakan pada kasus kabel-only |

Auto-recovery request ke `247` boleh tetap terlihat di log ketika kabel putus.
Itu hanya percobaan recovery defensif dan **bukan bukti bahwa master menemukan
slave**. Pada kasus kabel-only, request tersebut gagal. Reconnect yang benar
terjadi lewat polling assigned address berikutnya.

Pengecualian urutan hanya terjadi saat boot master dengan saved slave yang
`last_seen == 0` dan offline. Pada kondisi boot tersebut, master memang mencoba
recovery `247` lebih dulu karena slave RAM-only diasumsikan baru reboot dan
kembali ke `247`. Pengecualian ini tidak berlaku setelah kabel-only terputus
dalam sesi master yang masih berjalan.

## Flow Normal Boot

Skenario ideal PoE:

```text
Master mati
Slave ikut mati
Power balik
Slave boot di address 247
Master boot dan load registry MAC -> address lama
```

Untuk kondisi ini, flow master wajib seperti ini:

```text
MASTER BOOT
  |
  v
Load registry dari storage
  - MAC saved
  - address saved, misal 0x03
  - profile
  - sensor slot assignment
  |
  v
Loop polling mulai tiap 1000 ms
  |
  v
Ambil slave dari registry
  |
  v
Apakah slave punya MAC saved, last_seen == 0, dan online == false?
  |
  +--> YA
  |     |
  |     v
  |   BOOT RECOVERY FIRST
  |   Master kirim recovery ke 247
  |     - dst = 247
  |     - reg = 0x00F4
  |     - qty = 4 register
  |     - isi = MAC saved + address saved
  |     |
  |     v
  |   Master confirm ke address saved
  |     - dst = 0x03, misalnya
  |     - read identity 0x0000..0x0004
  |     |
  |     +--> MAC cocok: online
  |     |     apply ulang assignment/profile tersimpan
  |     +--> gagal: retry recovery 10 detik lagi
  |
  +--> TIDAK
        |
        v
      Poll normal ke address saved
```

Jadi untuk boot normal PoE, **master tidak boleh mengawali known slave dengan polling address lama dulu**. Dia harus recovery ke `247` dulu, baru confirm ke address lama.

Di kode sekarang ini dijaga oleh kondisi:

```text
mac != 0
last_seen == 0
online == false
```

Kalau kondisi itu benar, master masuk `boot_recovery_first` dan kirim ke `247`.

## Kenapa Kadang Tetap Kirim Ke Address Lama?

Ada beberapa kondisi yang bikin master kirim ke address lama seperti `0x03`.

### 1. Setelah Recovery Berhasil

Ini normal dan wajib.

```text
Kirim recovery ke 247
  |
  v
Slave pindah ke 0x03
  |
  v
Master confirm / poll ke 0x03
```

Jadi log `dst=0x03` setelah recovery bukan salah. Itu tahap confirm/polling.

### 2. Master Tidak Baru Boot, Slave Lama Masih Hidup

Ini skenario debugging pakai adaptor/USB beda:

```text
Master direstart
Slave tidak ikut restart
Slave masih address 0x03
```

Secara ideal PoE ini tidak terjadi, tapi saat debugging bisa terjadi.

Karena kode sekarang mengutamakan recovery via `247` saat boot, master akan coba `247` dulu. Kalau slave memang masih hidup di `0x03`, recovery ke `247` bisa gagal. Untuk kasus debugging seperti ini, Discover punya jalur tambahan untuk cek address lama di 4 detik awal.

### 3. Slave Sudah Online Di Sesi Sekarang

Kalau slave sudah berhasil recovery/online, master polling normal ke address saved.

```text
0x03 identity sync
0x03 capability sync
0x03 sensor block poll
```

### 4. Data Legacy / MAC Kosong

Kalau registry punya address tapi MAC kosong, master tidak bisa recovery berbasis MAC. Ini fallback data lama, bukan flow ideal v2.1.

## Apakah Master Selalu Scan `247` Di Loop?

Jawaban pendek:

```text
Tidak selalu scan unknown device.
Tapi ya, untuk known slave yang offline/lost, master bisa mencoba recovery ke 247 terus berkala.
```

Detailnya:

### Untuk Known Slave

Kalau slave sudah ada di registry dan punya MAC saved:

```text
Master akan mencoba recovery ke 247
jika slave terlihat lost/offline/degraded.
```

Retry-nya:

```text
Paling cepat tiap 10 detik per slave.
```

Jadi untuk known slave, master memang **tidak nyerah permanen**. Dia akan coba lagi berkala.

Ini kenapa slave yang baru dicolok setelah master lama hidup bisa muncul otomatis:

```text
Slave itu sebenarnya known.
Master sudah punya MAC-nya.
Saat slave dicolok, dia hidup di 247.
Pada recovery attempt berikutnya, master restore dia.
```

### Untuk Unknown Slave

Kalau slave belum pernah dipair dan MAC belum ada di registry:

```text
Master tidak auto-register.
Master tidak auto-assign address.
Master tidak auto-munculkan sebagai device resmi.
Harus klik Discover.
```

Kalau unknown slave muncul tanpa klik Discover, cek salah satu ini:

- MAC slave itu ternyata sudah pernah tersimpan di master.
- User pernah menekan Discover dan window 30 detiknya masih aktif.
- Serial command debug `rs485 pair` atau `rs485 pairscan` pernah dikirim.
- Ada bug lain di luar flow utama ini.

## Flow Saat Master Diam Tidak Diapa-apain

```text
MASTER LOOP NORMAL
  |
  v
Setiap 1000 ms proses 1 slave registry
  |
  +--> Tidak ada slave registry valid
  |       |
  |       v
  |     Tidak scan 247 untuk unknown device
  |
  +--> Ada known slave offline/lost
  |       |
  |       v
  |     Master tetap mendapat giliran polling ke address saved
  |     Jika sudah lewat 10 detik dari recovery terakhir:
  |       master juga mencoba recovery ke 247
  |
  +--> Ada known slave online
          |
          v
        Poll address saved
```

Jadi master yang "diam" tetap kerja untuk known slave, tapi bukan auto-discover device asing.

Untuk known slave offline, polling saved address dan percobaan recovery `247`
berjalan sebagai dua jalur berbeda:

- Jika slave tetap hidup di saved address, polling normal yang akan berhasil.
- Jika slave sempat reboot dan kembali ke `247`, auto-recovery yang akan
  memindahkannya ke saved address, lalu polling normal dilanjutkan.

## Flow Discover

Discover hanya aktif kalau:

- user klik tombol `DISCOVER`, atau
- command debug `rs485 pair ...` dipakai.

Saat Discover dimulai:

```text
pairing_active = true
pairing_started_ms = millis()
pairing_timeout_ms = 30000
poll_enabled = false
pairing_known_scan_index = 0
```

Lalu tiap `700 ms`:

```text
DISCOVER LOOP
  |
  v
Apakah masih dalam 4 detik awal?
  |
  +--> YA
  |     |
  |     v
  |   Cek 1 known address lama dari registry
  |     - read identity ke address saved
  |     - label log: PAIR_KNOWN_IDENTITY
  |     |
  |     +--> MAC cocok
  |     |     mark known alive
  |     |     sync capability jika due
  |     |     scan 247 pada interval itu diskip
  |     |
  |     +--> gagal / tidak ada known hit
  |           lanjut scan 247
  |
  +--> TIDAK
        |
        v
      Scan 247
```

Scan `247`:

```text
Read identity 247:0x0000..0x0004
  |
  +--> timeout
  |     tunggu 700 ms berikutnya
  |
  +--> identity OK
        |
        v
      Cek MAC identity
        |
        +--> MAC sudah ada di registry
        |     recovery known slave
        |     lalu Discover selesai
        |
        +--> MAC belum ada di registry
              read capability 247:0x0010..0x0017
              pairing_candidate_ready = true
              UI tampilkan UNPAIRED DEVICE
              tunggu user assign
```

Kalau `30 detik` habis:

```text
pairing_active = false
poll_enabled balik ke nilai sebelum Discover
candidate dibersihkan
status = RS485 pairing timeout
```

## Flow Pair Unknown Device

```text
Unknown candidate muncul di 247
  |
  v
User tekan PAIR DEVICE / ASSIGN AUTO
  |
  v
Master pilih address kosong 2..246
  |
  v
Master write capability assignment ke 247:0x0010..0x0017
  |
  v
Master write NODE_ADDRESS ke 247:0x0000
  |
  v
Slave pindah ke address baru
  |
  v
Master simpan MAC/address/profile/slot ke registry
  |
  v
Discover selesai, polling normal lanjut
```

## Flow Jika Master Reboot Tapi Slave Tidak Reboot

Ini bukan skenario PoE normal, tapi sering terjadi waktu debugging pakai adaptor/USB beda.

```text
Master reboot
Slave tetap hidup di 0x03
  |
  v
Master boot
  |
  v
Master coba recovery ke 247 dulu
  |
  v
Tidak ada slave di 247, karena slave masih di 0x03
  |
  v
Recovery gagal
```

Untuk kondisi debugging seperti ini:

```text
Klik Discover
  |
  v
4 detik awal Discover cek known address lama
  |
  v
Slave di 0x03 bisa dikonfirmasi alive
```

Kenapa bukan default boot behavior? Karena di installasi PoE normal, kalau master mati maka slave juga mati. Jadi saat boot normal, asumsi paling benar adalah slave ada di `247`, bukan address lama.

## Log Serial Yang Perlu Dibaca

| Label | Arti |
|---|---|
| `AUTO_RECOVERY` | Normal loop mencoba restore known slave via `247` |
| `Recovery write TX dst=0xF7 reg=0x00F4 qty=4` | Master mengirim MAC + saved address ke `247` |
| `AUTO_RECOVERY_CONFIRM` | Master confirm ke address saved setelah recovery |
| `PAIR_KNOWN_IDENTITY` | Discover sedang cek address lama yang tersimpan |
| `PAIR_IDENTITY` | Discover sedang scan identity di `247` |
| `PAIR_CAPABILITY` | Discover baca capability candidate di `247` |
| `PAIR_SET_ASSIGN` | Master tulis assignment register ke candidate |
| `PAIR_SET_ADDRESS` | Master tulis address baru ke candidate |
| `IDENTITY_SYNC` | Polling normal refresh identity ke address saved |
| `CAPABILITY_SYNC` | Polling normal refresh capability ke address saved |
| `SENSOR_BLOCK_POLL` | Polling normal baca sensor block |

## Cara Menjawab Pertanyaan Praktis

### "Pas master boot, dia kirim ke 247 atau address lama?"

Untuk known slave normal v2.1:

```text
Pertama ke 247 untuk recovery.
Lalu ke address lama untuk confirm/poll.
```

Kalau terlihat langsung ke address lama, cek:

- apakah itu log setelah recovery,
- apakah slave sudah online di sesi itu,
- apakah registry MAC kosong/legacy,
- atau apakah flow code berubah.

### "Kalau master diam, apakah dia discover slave baru terus?"

```text
Tidak untuk unknown slave.
Master hanya mencoba recovery known slave yang offline/lost tiap 10 detik.
Recovery bukan Discover.
```

### "Kalau kabel dicabut lalu dipasang lagi, kok bisa terbaca?"

```text
Slave tidak pindah address.
Slave tetap di assigned address, misalnya 0x03.
Master juga tetap menyimpan dan polling 0x03.

Saat kabel terputus:
poll 0x03 gagal.

Saat kabel tersambung:
poll 0x03 berikutnya berhasil.

Bukan Discover.
Bukan recovery 247.
Hanya request dan response pada address lama yang akhirnya tersambung lagi.
```

### "Kenapa slave muncul otomatis tanpa klik Discover?"

```text
Karena slave itu kemungkinan known slave.
MAC-nya sudah tersimpan.
Saat dicolok, dia boot di 247.
Master auto recovery dan restore ke address saved.
Setelah itu master apply ulang profile/sensor assignment yang tersimpan.
```

### "Bagaimana kalau mau lupain slave lama?"

```text
Buka Device Detail.
Tekan DELETE.
Master hapus MAC/address/profile/assignment slave itu dari registry.
Mapping dashboard yang menunjuk ke slave itu ikut dibersihkan.
Setelah itu slave tersebut tidak akan auto recovery lagi sampai dipair ulang.
```

### "Kapan wajib klik Discover?"

```text
Saat slave benar-benar baru dan MAC belum ada di registry.
```

### "Kapan master nyerah?"

Known slave:

```text
Tidak nyerah permanen.
Retry recovery tiap 10 detik selama dia masih dianggap lost/offline.
```

Unknown slave:

```text
Tidak dicari di normal loop.
Discover aktif 30 detik setelah diklik.
Kalau timeout, harus klik Discover lagi.
```
