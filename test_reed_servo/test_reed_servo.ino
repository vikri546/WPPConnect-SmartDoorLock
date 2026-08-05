#include <ESP32Servo.h> // Gunakan library Servo yang sesuai dengan Arduino IDE Anda

Servo testServo;

// ---- Definisi Pin (Sesuai dengan project SmartLock) ----
const int PIN_SERVO = 18;
const int PIN_REED = 5; // D0 sensor hubungkan ke GPIO 5
const int PIN_LED = 2;  // LED onboard ESP32 (biasanya GPIO 2)
const int PIN_BUZ = 4;  // Pin Buzzer

void setup() {
  Serial.begin(115200);

  // Konfigurasi pin input/output
  pinMode(PIN_REED, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_BUZ, OUTPUT);

  // Menginisialisasi Servo
  testServo.attach(PIN_SERVO);
  testServo.write(0); // Posisi awal: 0 derajat (Lock)

  Serial.println("=================================================");
  Serial.println("Program Uji Coba Sensor Reed Switch & Servo ESP32");
  Serial.println("=================================================");
  Serial.println("Petunjuk:");
  Serial.println("1. Hubungkan Pin D0 Sensor ke GPIO 5 ESP32.");
  Serial.println("2. Hubungkan Pin Signal Servo ke GPIO 18 ESP32.");
  Serial.println("3. Dekatkan/jauhkan magnet pada sensor untuk tes.");
  Serial.println("=================================================");

  // Bunyi buzzer sebentar saat menyala untuk test buzzer
  digitalWrite(PIN_BUZ, HIGH);
  delay(200);
  digitalWrite(PIN_BUZ, LOW);
}

void loop() {
  // Membaca status logika dari pin D0 sensor
  int reedState = digitalRead(PIN_REED);

  Serial.print("[BACA SENSOR] Status Pin 5: ");

  if (reedState == LOW) {
    // Kondisi LOW berarti magnet terdeteksi (pintu rapat/tertutup)
    Serial.println("LOW -> PINTU TERTUTUP (Magnet Dekat)");

    // Indikator visual: LED onboard menyala
    digitalWrite(PIN_LED, HIGH);

    // Servo diputar ke posisi 0 derajat (Terkunci)
    testServo.write(0);
    Serial.println("[SERVO] Bergerak ke posisi: TERKUNCI (0°)");
  } else {
    // Kondisi HIGH berarti magnet tidak terdeteksi (pintu terbuka)
    Serial.println("HIGH -> PINTU TERBUKA (Magnet Jauh)");

    // Indikator visual: LED onboard mati
    digitalWrite(PIN_LED, LOW);

    // Servo diputar ke posisi 90 derajat (Terbuka)
    testServo.write(90);
    Serial.println("[SERVO] Bergerak ke posisi: TERBUKA (90°)");
  }

  Serial.println("-------------------------------------------------");
  delay(1000); // Ulangi pembacaan setiap 1 detik
}
