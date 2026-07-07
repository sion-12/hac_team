// ============================================================
// test_332_laser_instrument.ino
// 332 レーザ遅延テスト — 楽器側
//
// [動作]
//   1. BPM UDP を受信して指揮者の IP / ポートを記憶する
//   2. レーザコマンドを受信したら即座に UDP "LASER_ACK" を返送する
//   3. 指揮者側が (T2 - T1) を遅延として記録する
//
// [出力]
//   [n] cmd=0b010  LASER_ACK 送信
// ============================================================

#include <WiFiS3.h>
#include <WiFiUdp.h>

// ---- WiFi ----
char ssid[] = "hackathon010-WPA2";
char pass[] = "hackathon010";

// ---- UDP ----
WiFiUDP Udp;
const unsigned int localPort = 8080;

// ---- センサ ----
const int sensorPin = A0;
int threshold = 200;

// ---- 指揮者情報（BPM 受信時に記憶） ----
IPAddress conductorIP;
uint16_t  conductorPort  = 9090;
bool      conductorKnown = false;

int trialCount = 0;

// ============================================================
void setup() {
  Serial.begin(9600);
  pinMode(sensorPin, INPUT);

  calibrateThreshold();

  Serial.print("WiFi 接続中...");
  while (WiFi.begin(ssid, pass) != WL_CONNECTED) { delay(1000); Serial.print("."); }
  Serial.println(" 接続完了");
  Serial.print("IP: "); Serial.println(WiFi.localIP());
  Udp.begin(localPort);

  Serial.println("=== 332 レーザ遅延テスト (楽器側) ===");
  Serial.println("指揮者から BPM を受信後、レーザコマンドを待機します");
  Serial.println("------------------------------------------");
}

// ============================================================
void loop() {
  receiveBPM();

  byte cmd = checkLaserCommand();
  if (cmd != 0) {
    if (!conductorKnown) {
      Serial.println("!! 指揮者 IP 未取得 — BPM を先に受信してください");
      return;
    }

    // レーザ受信直後に ACK 送信（遅延を最小化）
    Udp.beginPacket(conductorIP, conductorPort);
    Udp.print("LASER_ACK");
    Udp.endPacket();

    trialCount++;
    Serial.print("["); Serial.print(trialCount); Serial.print("] ");
    Serial.print("cmd=0b");
    Serial.print((cmd >> 2) & 1);
    Serial.print((cmd >> 1) & 1);
    Serial.print(cmd & 1);
    Serial.println("  LASER_ACK 送信");
  }
}

// ============================================================
// BPM 受信 → 指揮者 IP 記憶
// ============================================================
void receiveBPM() {
  int ps = Udp.parsePacket();
  if (ps == 0) return;
  char buf[32] = {0};
  int len = Udp.read(buf, sizeof(buf) - 1);
  if (len <= 0) return;

  if (strncmp(buf, "BPM:", 4) == 0) {
    conductorIP   = Udp.remoteIP();
    conductorPort = Udp.remotePort();
    conductorKnown = true;
    Serial.print("指揮者 IP 記憶: ");
    Serial.print(conductorIP);
    Serial.print(":");
    Serial.println(conductorPort);
  }
}

// ============================================================
// レーザコマンド検出
// ============================================================
byte checkLaserCommand() {
  if (readLaser() != HIGH) return 0;

  unsigned long t0 = millis();
  while (readLaser() == HIGH) {
    if (millis() - t0 > 1000) {
      while (readLaser() == HIGH);
      return 0;
    }
  }
  unsigned long dur = millis() - t0;
  if (dur < 250 || dur > 350) {
    Serial.print("[PULSE_REJECTED] dur="); Serial.print(dur); Serial.println("ms");
    return 0;
  }
  return parseBitStream();
}

byte parseBitStream() {
  byte cmd = 0;
  delay(75);
  for (int i = 2; i >= 0; i--) {
    int v = 0;
    v += readLaser(); delay(5);
    v += readLaser(); delay(5);
    v += readLaser();
    cmd |= ((v >= 2 ? 1 : 0) << i);
    delay(90);
  }
  return cmd;
}

int readLaser() {
  return (analogRead(sensorPin) >= threshold) ? HIGH : LOW;
}

// ============================================================
// 閾値キャリブレーション
// ============================================================
void calibrateThreshold() {
  Serial.println("キャリブレーション中...");
  int sum = 0;
  for (int i = 0; i < 20; i++) { sum += analogRead(sensorPin); delay(50); }
  int dark = sum / 20;
  threshold = constrain(dark + dark / 5 + 30, 100, 900);
  Serial.print("  閾値 = "); Serial.println(threshold);
}