// ============================================================
// test_332_laser_conductor.ino
// 332 レーザ遅延テスト — 指揮者側
//
// [計測方式]
//   T1 = millis() → sendLaserCommand() (~650ms ブロック)
//   楽器がレーザ受信直後に UDP "LASER_ACK" を返送
//   T2 = millis() on ACK receive
//   遅延 = T2 - T1  (レーザ送信〜楽器反映確認まで)
//
// [期待値]
//   レーザプロトコル時間 ~650ms + UDP ~10ms = 約 660ms
//
// [合格基準]
//   遅延 < 1000ms → PASS  (10/10 回)
//
// [手順]
//   1. 楽器選択ボタンで対象楽器を選ぶ
//   2. 送信ボタンを押す (10回繰り返す)
//   3. 指揮者シリアルモニタで遅延を確認する
// ============================================================

#include <WiFiS3.h>
#include <WiFiUdp.h>

// ---- WiFi ----
char ssid[] = "hackathon010-WPA2";
char pass[] = "hackathon010";

// ---- ピン ----
const int laserPins[] = {A0, A1, A2, A3};
const int instPins[]  = {3, 4, 5, 6};
const int sendPin     = 2;    // 送信トリガーボタン

// ---- UDP ----
WiFiUDP Udp;
const unsigned int localPort = 9090;
const unsigned int destPort  = 8080;
IPAddress broadcastIP;

// ---- テスト設定 ----
const int  NUM_TRIALS     = 10;
const long PASS_THRESHOLD = 1000;   // ms

// ---- 計測データ ----
int  trialCount = 0;
int  passCount  = 0;
long delays[NUM_TRIALS];

// ---- 状態 ----
int  targetLaser  = -1;
bool waitingAck   = false;
unsigned long T1  = 0;

bool lastSendState    = HIGH;
bool lastInstState[4] = {HIGH, HIGH, HIGH, HIGH};

// ============================================================
void setup() {
  Serial.begin(9600);

  for (int i = 0; i < 4; i++) {
    pinMode(laserPins[i], OUTPUT);
    digitalWrite(laserPins[i], LOW);
  }
  for (int i = 0; i < 4; i++) pinMode(instPins[i], INPUT_PULLUP);
  pinMode(sendPin, INPUT_PULLUP);

  Serial.print("WiFi 接続中...");
  while (WiFi.begin(ssid, pass) != WL_CONNECTED) { delay(1000); Serial.print("."); }
  Serial.println(" 接続完了");
  Serial.print("IP: "); Serial.println(WiFi.localIP());

  IPAddress ip     = WiFi.localIP();
  IPAddress subnet = WiFi.subnetMask();
  for (int i = 0; i < 4; i++)
    broadcastIP[i] = (ip[i] & subnet[i]) | (~subnet[i] & 0xFF);

  Udp.begin(localPort);

  // 楽器が指揮者 IP を覚えるために BPM 送信
  Udp.beginPacket(broadcastIP, destPort);
  Udp.print("BPM:120");
  Udp.endPacket();
  delay(1000);   // 楽器の起動・受信を待つ

  Serial.println("=== 332 レーザ遅延テスト (指揮者側) ===");
  Serial.println("合格基準: レーザ遅延 < 1000ms (期待値 ~660ms)");
  Serial.println("手順: 楽器選択ボタン → 送信ボタン を 10 回");
  Serial.println("------------------------------------------");
}

// ============================================================
void loop() {
  if (trialCount >= NUM_TRIALS) return;

  // ---- 楽器選択 ----
  for (int i = 0; i < 4; i++) {
    bool st = digitalRead(instPins[i]);
    if (lastInstState[i] == HIGH && st == LOW) {
      targetLaser = i;
      Serial.print("楽器選択: laser="); Serial.println(i);
      delay(50);
    }
    lastInstState[i] = st;
  }

  // ---- 送信トリガー ----
  bool sendState = digitalRead(sendPin);
  if (lastSendState == HIGH && sendState == LOW && !waitingAck) {
    if (targetLaser == -1) {
      Serial.println("!! 楽器が選択されていません");
    } else {
      Serial.print("[試行 "); Serial.print(trialCount + 1);
      Serial.print("/"); Serial.print(NUM_TRIALS);
      Serial.print("] レーザ送信中 (laser="); Serial.print(targetLaser); Serial.println(")...");

      T1 = millis();
      sendLaserCommand(0b010, targetLaser);   // CMD_VOL_UP
      waitingAck = true;
      // sendLaserCommand() が ~650ms ブロック。
      // その間に楽器が LASER_ACK を送信 → バッファに溜まっている
    }
    delay(50);
  }
  lastSendState = sendState;

  // ---- ACK 受信 ----
  if (waitingAck) {
    int ps = Udp.parsePacket();
    if (ps > 0) {
      char buf[32] = {0};
      Udp.read(buf, sizeof(buf) - 1);

      if (strncmp(buf, "LASER_ACK", 9) == 0) {
        unsigned long T2 = millis();
        long d = (long)(T2 - T1);

        delays[trialCount] = d;
        bool pass = (d <= PASS_THRESHOLD);
        if (pass) passCount++;
        trialCount++;
        waitingAck = false;

        Serial.print("  遅延 = "); Serial.print(d);
        Serial.println(pass ? " ms → PASS" : " ms → FAIL");

        if (trialCount >= NUM_TRIALS) printSummary();
      }
    }

    // タイムアウト 2000ms
    if (waitingAck && millis() - T1 > 2000) {
      Serial.println("  !! タイムアウト (ACK 未着)");
      delays[trialCount] = 2000;
      trialCount++;
      waitingAck = false;
      if (trialCount >= NUM_TRIALS) printSummary();
    }
  }
}

// ============================================================
void printSummary() {
  long sum = 0, maxD = 0;
  Serial.println("");
  Serial.println("============================================");
  Serial.println("  レーザ遅延テスト 集計  (テスト番号 332)");
  Serial.println("============================================");
  for (int i = 0; i < NUM_TRIALS; i++) {
    Serial.print("  試行 "); Serial.print(i + 1); Serial.print(" : ");
    Serial.print(delays[i]); Serial.print(" ms → ");
    Serial.println(delays[i] <= PASS_THRESHOLD ? "PASS" : "FAIL");
    sum += delays[i];
    if (delays[i] > maxD) maxD = delays[i];
  }
  Serial.print("  平均遅延 : "); Serial.print(sum / NUM_TRIALS); Serial.println(" ms");
  Serial.print("  最大遅延 : "); Serial.print(maxD); Serial.println(" ms");
  Serial.print("  PASS 率  : "); Serial.print(passCount);
  Serial.print("/"); Serial.println(NUM_TRIALS);
  Serial.print("  判定     : ");
  Serial.println(passCount == NUM_TRIALS ? "合格" : "不合格");
  Serial.println("============================================");
}

// ============================================================
void sendLaserCommand(byte command, int target) {
  setLaserState(target, HIGH); delay(300);
  setLaserState(target, LOW);  delay(50);
  for (int bit = 2; bit >= 0; bit--) {
    int bitValue = (command >> bit) & 0x01;
    setLaserState(target, bitValue == 1 ? HIGH : LOW); delay(50);
    setLaserState(target, LOW); delay(50);
  }
}

void setLaserState(int target, int state) {
  if (target == -1) {
    for (int i = 0; i < 4; i++) digitalWrite(laserPins[i], state);
  } else {
    digitalWrite(laserPins[target], state);
  }
}
