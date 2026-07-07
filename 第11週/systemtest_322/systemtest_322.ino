// ============================================================
// test_322_sync_instrument.ino
// 再生タイミング同期テスト - 楽器側 [テスト番号: 322]
//
// [動作]
//   CMD_START レーザ受信時に micros() を記録する (基準時刻)．
//   最初のビート tick が来た時刻を micros() で記録する (発音時刻)．
//   差分 (発音までのオフセット) をシリアルに出力する．
//   複数台の楽器で実行し，各台の発音時刻差 (= ログの t_note 値の差) が
//   20ms 未満であれば合格．
//
// [テスト手順]
//   1. このスケッチを楽器側 Arduino 全台に書き込む (全台シリアル同時に確認)
//   2. BPM 設定のため指揮者側 Arduino (conductor_var3) を起動しておく
//   3. 指揮者側から STATE_COUNTING→CMD_START を送信する (楽器選択→再生ボタン)
//   4. 各楽器のシリアルに出力された「t_note」の値を比較する
//   5. 最大値 - 最小値 が 20ms 未満なら合格
//
// [出力フォーマット]
//   [n/50] t_ref=XXXXXX us  t_note=YYYYYY us  offset=ZZZ us  diff_ms=W.Wms
//
// [合格基準]
//   楽器間 t_note 差 20ms 未満 (成功率 95%)
// ============================================================

#include <WiFiS3.h>
#include <WiFiUdp.h>

// ---- WiFi 設定 ----
char ssid[] = "hackathon010-WPA2";
char pass[] = "hackathon010";

// ---- UDP ----
WiFiUDP Udp;
const unsigned int localPort = 8080;

// ---- センサ設定 ----
const int sensorPin = A0;
int threshold = 200;

// ---- コマンド定義 ----
const byte CMD_START    = 0b001;
const byte CMD_VOL_UP   = 0b010;
const byte CMD_VOL_DOWN = 0b011;

// ---- BPM 管理 ----
float currentBPM    = 120.0;
unsigned long beatInterval = 500;
unsigned long lastBeatTime = 0;
bool counting = false;

// ---- タイミング計測 ----
unsigned long refTimeMicros  = 0;   // CMD_START 受信時刻 (micros)
unsigned long noteTimeMicros = 0;   // 最初の発音時刻 (micros)
bool refSet  = false;
bool noteSet = false;

// ---- 試行カウント ----
const int NUM_TRIALS = 50;
int trialCount = 0;
int passCount  = 0;   // ※ 単体では合否不明。複数台比較が必要

// ============================================================
// 閾値キャリブレーション
// ============================================================
void calibrateThreshold() {
  Serial.println("[準備] キャリブレーション中...");
  int sum = 0;
  for (int i = 0; i < 20; i++) { sum += analogRead(sensorPin); delay(50); }
  int dark = sum / 20;
  threshold = constrain(dark + dark / 5 + 30, 100, 900);
  Serial.print("  閾値="); Serial.println(threshold);
}

int readLaser() { return (analogRead(sensorPin) >= threshold) ? HIGH : LOW; }

// ============================================================
// UDP BPM 受信
// ============================================================
void receiveBPM() {
  int ps = Udp.parsePacket();
  if (ps == 0) return;
  IPAddress senderIP   = Udp.remoteIP();
  uint16_t  senderPort = Udp.remotePort();
  char buf[64] = {0};
  int len = Udp.read(buf, sizeof(buf) - 1);
  if (len <= 0) return;
  if (strncmp(buf, "BPM:", 4) == 0) {
    int bpm = atoi(buf + 4);
    if (bpm >= 30 && bpm <= 240) {
      currentBPM   = bpm;
      beatInterval = (unsigned long)(60000.0 / currentBPM);
    }
    Udp.beginPacket(senderIP, senderPort);
    Udp.print("ACK");
    Udp.endPacket();
  }
}

// ============================================================
// レーザコマンド受信 (micros() 版)
// ============================================================
byte checkLaserCommand(unsigned long *refMicros) {
  if (readLaser() != HIGH) return 0;

  *refMicros = micros();   // [重要] レーザ立ち上がり時刻を記録
  unsigned long t0 = millis();

  while (readLaser() == HIGH) {
    if (millis() - t0 > 1000) {
      while (readLaser() == HIGH);
      return 0;
    }
  }

  unsigned long dur = millis() - t0;
  if (dur < 250 || dur > 350) return 0;

  return parseBitStream();
}

byte parseBitStream() {
  byte cmd = 0;
  delay(70);
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

// ============================================================
// 1試行の結果を出力
// ============================================================
void outputTrialResult() {
  long offsetUs = (long)(noteTimeMicros - refTimeMicros);
  float offsetMs = offsetUs / 1000.0;

  trialCount++;
  Serial.print("["); Serial.print(trialCount);
  if (trialCount < 10) Serial.print(" ");
  Serial.print("/"); Serial.print(NUM_TRIALS); Serial.print("] ");
  Serial.print("t_ref="); Serial.print(refTimeMicros);
  Serial.print("us  t_note="); Serial.print(noteTimeMicros);
  Serial.print("us  offset="); Serial.print(offsetUs);
  Serial.print("us  ("); Serial.print(offsetMs, 1); Serial.println("ms)");
  Serial.println("  ※ 他の楽器の t_note と差が 20ms 未満なら合格");

  if (trialCount >= NUM_TRIALS) {
    Serial.println("--------------------------------------------");
    Serial.println("50 試行完了。各楽器の t_note を比較してください");
    Serial.println("[合格基準] 楽器間の t_note 最大差 < 20000us (20ms)");
  }
}

// ============================================================
// setup
// ============================================================
void setup() {
  Serial.begin(9600);
  pinMode(sensorPin, INPUT);

  Serial.println("=== 322 再生タイミング同期テスト (楽器側) ===");
  calibrateThreshold();

  Serial.print("WiFi 接続中...");
  while (WiFi.begin(ssid, pass) != WL_CONNECTED) { delay(1000); Serial.print("."); }
  Serial.println(" 接続完了");
  Serial.print("IP: "); Serial.println(WiFi.localIP());
  Udp.begin(localPort);

  Serial.println("指揮者側からの CMD_START を待機中...");
  Serial.println("--------------------------------------------");
}

// ============================================================
// loop
// ============================================================
void loop() {
  if (trialCount >= NUM_TRIALS) return;

  receiveBPM();

  // 待機: CMD_START を検出
  unsigned long refMicros = 0;
  byte cmd = checkLaserCommand(&refMicros);

  if (cmd == CMD_START) {
    refTimeMicros = refMicros;
    refSet  = true;
    noteSet = false;

    // カウント開始: 最初のビート tick まで待つ
    lastBeatTime = millis() - beatInterval;   // 即時発火
    counting = true;
    Serial.println("CMD_START 受信。最初の発音まで待機...");
  }

  // ビート tick: 最初のビート = 発音時刻
  if (counting && !noteSet) {
    if (millis() - lastBeatTime >= beatInterval) {
      lastBeatTime += beatInterval;
      noteTimeMicros = micros();   // 発音時刻
      noteSet = true;
      counting = false;
      outputTrialResult();

      // 次の試行まで待機 (指揮者側が次の CMD_START を送るまで)
      delay(500);
    }
  }
}