Kode ini adalah bagian dari buku pertanian pintar yang diterbitkan oleh Common Room Networks Foundation.

Pada bagian ini, disampaikan kode firmware untuk node sensor kelembapan tanah berbasis ESP32 untuk sistem irigasi pintar sederhana. Node bertugas membaca data dari sensor kelembapan resistif, lalu mengirimkan data tersebut secara nirkabel ke sebuah collector (perangkat penerima terpusat) menggunakan protokol ESP-NOW -protokol komunikasi peer-to-peer milik Espressif yang bekerja tanpa infrastruktur WiFi router-.

Salah satu keunggulan kode ini dibandingkan implementasi konvensional adalah fitur Auto Channel Scan. Dengan fitur ini, node tidak perlu dikonfigurasi secara manual untuk menyesuaikan channel WiFi yang digunakan oleh collector. Saat pertama kali dinyalakan, node akan secara otomatis melakukan pemindaian channel, menemukan collector yang tersedia, kemudian mengunci channel yang sesuai untuk membangun komunikasi yang stabil dan andal.

Jika ada kekurangan, kesalahan atau potensi perbaikan pada kode, silahkan lakukan kolaborasi untuk hasil yang lebih baik.
