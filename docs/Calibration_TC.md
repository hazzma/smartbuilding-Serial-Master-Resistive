# Calibration TC

Dokumen ini mencatat hasil pengukuran dan koreksi koordinat touchscreen
resistif XPT2046 pada Master HMI.

## 1. Hardware dan Konfigurasi

| Item | Nilai |
|---|---|
| Display | ILI9488 SPI, 480x320 |
| Display driver | LovyanGFX `Panel_ILI9488` |
| Touch controller | XPT2046 resistif SPI |
| Touch CS | GPIO46 |
| Shared SPI | SCLK GPIO6, MOSI GPIO7, MISO GPIO4 |
| Display rotation | `3` |
| Touch IRQ | Tidak digunakan / polling |
| Poll interval | 20 ms / 50 Hz |

Konfigurasi dasar LovyanGFX:

```cpp
x_min = 3900;
x_max = 300;
y_min = 400;
y_max = 3900;
offset_rotation = 0;
```

Overlay resistif terpasang berlawanan 180 derajat terhadap orientasi display.
Setelah `tft.getTouch()` menghasilkan koordinat layar, firmware membalik kedua
sumbu:

```cpp
x = 479 - x;
y = 319 - y;
```

## 2. Mode Alignment Test

Mode ini hanya menampilkan koordinat pixel layar final. Nilai raw ADC XPT2046
yang dapat mencapai sekitar `0..4095` tidak ditampilkan sebagai pixel.

| Perintah serial | Fungsi |
|---|---|
| `k` atau `K` | Membuka Touch Alignment Test |
| `b` atau `B` | Kembali ke dashboard |

Mode alignment tidak memakai `tft.calibrateTouch()`, tidak menyimpan data ke
NVS, dan tidak memiliki tombol BACK agar seluruh area layar dapat diuji.

Target pengukuran:

```text
35,70    240,70    445,70
35,180              445,180
35,290   240,290   445,290
```

Target sengaja dibuat masuk dari pojok fisik agar mudah disentuh.

## 3. Hasil Pengukuran Awal

Deviasi dihitung dengan:

```text
deviasi X = X terbaca - X target
deviasi Y = Y terbaca - Y target
```

| Target | Terbaca | Deviasi X | Deviasi Y | Catatan |
|---|---:|---:|---:|---|
| `445,70` | `488,63` | `+43 px` | `-7 px` | Sisi kanan terlalu jauh ke kanan |
| `240,290` | `223,294` | `-17 px` | `+4 px` | Tengah bawah sedikit ke kiri |
| `35,290` | `1,305` | `-34 px` | `+15 px` | Kiri bawah terlalu kiri dan bawah |
| `35,180` | `0,184` | `-35 px` | `+4 px` | Kiri tengah terlalu kiri |
| `35,70` | `0,61` | `-35 px` | `-9 px` | Kiri atas terlalu kiri dan atas |

Kesimpulan pengukuran awal:

- Distorsi X bukan offset tetap.
- Sisi kiri melebar sekitar `34..35 px` ke luar layar.
- Titik tengah bawah meleset `17 px` ke kiri.
- Sisi kanan melebar `43 px` ke luar layar.
- Deviasi Y lebih kecil, tetapi berubah antara area atas, tengah, dan bawah.
- Karena distorsi tidak linear sempurna, koreksi menggunakan dua segmen per
  sumbu, bukan satu offset global.

## 4. Koreksi Piecewise Pertama

Anchor koreksi pertama:

```text
X: 0 -> 35, 223 -> 240, 488 -> 445
Y: 62 -> 70, 184 -> 180, 300 -> 290
```

Setelah koreksi pertama, pengukuran sisi kanan adalah:

| Target | Terbaca | Deviasi X | Deviasi Y |
|---|---:|---:|---:|
| `445,70` | `414,70` | `-31 px` | `0 px` |
| `445,180` | `414,175` | `-31 px` | `-5 px` |
| `445,290` | `414,281` | `-31 px` | `-9 px` |

Kesimpulan:

- Koreksi Y sudah cukup dekat dan konsisten.
- Seluruh sisi kanan X menjadi konsisten pada `414`, tetapi masih kurang
  `31 px` dari target `445`.
- Konsistensi tersebut memberikan anchor yang cukup untuk memperbaiki hanya
  segmen kanan X tanpa mengubah sisi kiri, tengah, atau sumbu Y.

## 5. Koreksi Aktif Saat Ini

Koreksi aktif berada di `src/touch.cpp`.

```cpp
static int32_t touch_correct_screen_x(int32_t x) {
    if (x <= 223) {
        return 35 + (x * 205) / 223;
    }
    return 240 + ((x - 223) * 205) / 225;
}

static int32_t touch_correct_screen_y(int32_t y) {
    if (y <= 184) {
        return 70 + ((y - 62) * 110) / 122;
    }
    return 180 + ((y - 184) * 110) / 116;
}
```

Anchor aktif:

```text
X: 0 -> 35, 223 -> 240, 448 -> 445
Y: 62 -> 70, 184 -> 180, 300 -> 290
```

Perubahan terakhir hanya memperbesar skala segmen kanan X. Sisi kiri, tengah,
dan sumbu Y tidak diubah.

Status verifikasi:

| Area | Status |
|---|---|
| Kiri | Sudah cukup baik setelah koreksi pertama |
| Tengah | Sudah cukup baik setelah koreksi pertama |
| Y atas/tengah/bawah | Sudah cukup dekat |
| Kanan | Anchor terbaru sudah diflash; angka final perlu dicatat saat tes ulang |
| Keyboard | Menggunakan koordinat final yang sama, tanpa offset khusus keyboard |

## 6. Perlindungan Runtime

- Koordinat UI hanya diterima dalam X `0..479` dan Y `0..319`.
- Koordinat hasil mapping di luar layar ditolak dan tidak dikirim ke handler UI.
- Raw ADC hanya digunakan untuk log diagnosis.
- `bus_mutex` melindungi pembacaan touch pada shared SPI3.
- Event touch memakai fase `DOWN`, `MOVE`, dan `UP` dengan debounce minimum
  80 ms.

## 7. Prosedur Verifikasi Ulang

1. Buka Serial Monitor pada `115200`.
2. Kirim `k`.
3. Tekan pusat setiap target satu per satu.
4. Catat target, hasil terbaca, serta deviasi X/Y.
5. Prioritaskan tiga target kanan untuk memverifikasi anchor terbaru:

```text
445,70
445,180
445,290
```

6. Kirim `b` untuk kembali ke dashboard.
7. Uji keyboard, terutama tombol berdekatan seperti `T/Y/U`, `I/O`, `M`, dan
   delete.

## 8. Aturan Perubahan Berikutnya

- Jangan mengubah driver LovyanGFX atau raw XPT2046 jika UI normal masih dapat
  disentuh.
- Jangan memakai raw ADC `0..4095` sebagai koordinat pixel.
- Jangan menambahkan offset khusus keyboard; perbaiki mapping final yang dipakai
  seluruh UI.
- Ubah hanya segmen yang terbukti meleset berdasarkan hasil alignment test.
- Setelah koreksi, catat hasil sebelum dan sesudah pada dokumen ini.
