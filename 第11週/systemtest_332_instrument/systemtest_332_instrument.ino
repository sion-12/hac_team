// ============================================================
// test_332_instrument.ino
// システムテスト 332 — 指示反映テスト（楽器側）
//
// [テスト内容] (各 10 回試行)
//   BPM 遅延テスト : UDP パケット内の millis() タイムスタンプを使い
//                   送信〜受信の片道遅延を計測
//   音量遅延テスト : 同様に "VOL:XX:T:millis" パケットで UDP 遅延を計測
//                   ※ 実際の音量変更はレーザ経由、この計測はネットワーク層のみ
//
// [判定基準]
//   遅延時間 ≤ 100ms → PASS
//   遅延時間 > 100ms → FAIL
//   合格基準 : BPM / 音量ともに 10/10 PASS
//
// [基本動作]
//   var10 と同じ演奏動作を維持しつつ、遅延計測ロジックを追加。
//   演奏中に指揮者が BPM ボタンや音量ボタンを操作することで自動計測される。
//
// [使い方]
//   1. このスケッチを楽器側 Arduino に書き込む
//   2. test_332_conductor.ino を指揮者側 Arduino に書き込む
//   3. 演奏を開始した後、指揮者が BPM ボタン 10 回・音量ボタン 10 回操作する
//   4. このシリアルモニタで遅延と PASS/FAIL を確認する
//
// [注意]
//   BPM UDP フォーマット  : "BPM:120:T:12345"  (test_332_conductor が送信)
//   音量 UDP フォーマット : "VOL:80:T:12345"   (test_332_conductor が送信)
//   通常の conductor との互換性: "BPM:120" (タイムスタンプなし) も受け付ける
// ============================================================

#include <WiFiS3.h>
#include <WiFiUdp.h>
#include <Servo.h>

// ============================================================
// WiFi 設定
// ============================================================
char ssid[] = "hackathon010-WPA2";
char pass[] = "hackathon010";

// ============================================================
// UDP 設定
// ============================================================
WiFiUDP Udp;
const unsigned int localPort = 8080;

// ============================================================
// センサ設定
// ============================================================
const int sensorPin = A0;
int threshold = 150;

// ============================================================
// サーボ設定
// ============================================================
Servo myServo;
const int PIN_SERVO  = 9;
const int ANGLE_REST = 90;
const int ANGLE_PLAY = 75;

// ============================================================
// コマンド定義
// ============================================================
const byte CMD_START    = 0b001;
const byte CMD_VOL_UP   = 0b010;
const byte CMD_VOL_DOWN = 0b011;

// ============================================================
// ステートマシン
// ============================================================
enum SystemState {
  STATE_STANDBY,
  STATE_COUNTING,
  STATE_PLAY_WAIT,
  STATE_PLAY,
  STATE_DONE
};
SystemState currentState = STATE_STANDBY;

// ============================================================
// BPM・拍管理
// ============================================================
float currentBPM = 120.0;
unsigned long beatInterval = 500;

unsigned long anchorMillis    = 0;
long          anchorBeatIdx   = 1;
long          lastServicedIdx = 0;

int globalBeat   = 1;
int globalBar    = 1;
int nextBlockBar = 0;
long playStartIdx = 0;

// ============================================================
// 音量管理
// ============================================================
int volumeLevel = 80;
unsigned long lastVolChangeTime = 0;
const unsigned long VOL_COOLDOWN_MS = 2000;

// ============================================================
// 楽譜データ（フルート版 var10 と同じ）
// ============================================================
int melody[][2] = {
  {8,2},{6,2},{5,2},{4,2},
  {3,2},{2,2},{3,2},{4,2},
  {5,2},{4,2},{3,2},{2,2},
  {1,2},{0,2},{1,2},{2,2},
  {5,4},{8,4},{10,4},{9,4},
  {8,4},{5,4},{8,4},{6,4},
  {5,4},{3,4},{5,4},{10,4},
  {9,4},{11,4},{10,4},{9,4},
  {8,4},{5,4},{6,4},{12,4},
  {13,4},{15,4},{17,4},{10,4},
  {11,4},{9,4},{10,4},{8,4},
  {5,4},{13,4},{13,4},{12,4},
  {13,4},{5,4},{4,4},{12,4},
  {11,4},{3,4},{2,4},{10,4},
  {9,4},{16,4},{15,4},{8,4},
  {6,4},{11,4},{6,4},{14,4},
  {15,1},
};
int melodyLength;

// ============================================================
// 演奏進行管理
// ============================================================
bool sentInit = false;
unsigned long lastHeartbeat = 0;

// ============================================================
// テスト計測データ (332 指示反映テスト)
// ============================================================
// [計測方針]
//   指揮者側のタイムスタンプをそのままエコーバックし、RTT は指揮者側で計測する。
//   楽器側は遅延計算を行わない（millis() がボード間で非同期のため）。

// ============================================================
// setup
// ============================================================
void setup() {
  Serial.begin(9600);
  pinMode(sensorPin, INPUT);

  melodyLength = sizeof(melody) / sizeof(melody[0]);

  calibrateThreshold();

  myServo.attach(PIN_SERVO);
  myServo.write(ANGLE_REST);

  Serial.print("Connecting...");
  while (WiFi.begin(ssid, pass) != WL_CONNECTED) {
    delay(1000);
    Serial.print(".");
  }
  Serial.println(" Connected!");
  Serial.print("IP: "); Serial.println(WiFi.localIP());
  Udp.begin(localPort);

  currentState = STATE_STANDBY;
  Serial.println("Ready. Waiting for ALL START laser...");
  Serial.println("=== 332 指示反映テスト 開始待機中 ===");
}

// ============================================================
// loop
// ============================================================
void loop() {
  receiveUDP();      // BPM + VOL パケットを受信し、遅延を計測
  updateServo();
  checkSerialFromPC();
  serviceBeats();

  switch (currentState) {

    case STATE_STANDBY: {
      if (millis() - lastHeartbeat >= 2000) {
        lastHeartbeat = millis();
        Serial.print("[STANDBY] sensor=");
        Serial.print(analogRead(sensorPin));
        Serial.print(" threshold=");
        Serial.println(threshold);
      }
      byte cmd = checkLaserCommand();
      if (cmd == CMD_START) {
        startCounting();
      }
    } break;

    case STATE_COUNTING: {
      if (millis() - lastHeartbeat >= 2000) {
        lastHeartbeat = millis();
        Serial.print("[COUNTING] bar=");
        Serial.print(globalBar);
        Serial.print(" beat=");
        Serial.println(globalBeat);
      }
      byte cmd = checkLaserCommand();
      if (cmd == CMD_START) {
        nextBlockBar = ((globalBar - 1) / 8 + 1) * 8 + 1;
        currentState = STATE_PLAY_WAIT;
        Serial.print(">>> PLAY_WAIT: will start at bar=");
        Serial.println(nextBlockBar);
      } else {
        handleVolCommand(cmd);
      }
    } break;

    case STATE_PLAY_WAIT: {
      byte cmd = checkLaserCommand();
      handleVolCommand(cmd);
    } break;

    case STATE_PLAY: {
      byte cmd = checkLaserCommand();
      handleVolCommand(cmd);
    } break;

    case STATE_DONE: {
      // 何もしない
    } break;
  }
}

// ============================================================
// カウント開始
// ============================================================
void startCounting() {
  anchorMillis    = millis();
  anchorBeatIdx   = 1;
  lastServicedIdx = 0;
  globalBar       = 1;
  globalBeat      = 1;
  currentState    = STATE_COUNTING;
  Serial.print("T,"); Serial.println((int)currentBPM);
  Serial.print("V,"); Serial.println(volumeLevel);
  Serial.println("=== COUNTING START ===");
}

// ============================================================
// 拍を進める
// ============================================================
void serviceBeats() {
  if (currentState == STATE_STANDBY || currentState == STATE_DONE) return;

  long cur = anchorBeatIdx + (long)((millis() - anchorMillis) / beatInterval);
  while (lastServicedIdx < cur) {
    lastServicedIdx++;
    processBeat(lastServicedIdx, (lastServicedIdx == cur));
  }
}

// ============================================================
// 1拍分の処理
// ============================================================
void processBeat(long idx, bool isLatest) {
  globalBeat = (int)((idx - 1) % 4) + 1;
  globalBar  = (int)((idx - 1) / 4) + 1;

  switch (currentState) {

    case STATE_COUNTING:
      Serial.println("B");
      break;

    case STATE_PLAY_WAIT:
      if (globalBar == nextBlockBar && globalBeat == 1) {
        currentState = STATE_PLAY;
        playStartIdx = idx;
        sentInit     = false;
        Serial.println("RESET");
        Serial.print(">>> PLAY START at bar="); Serial.println(globalBar);
        emitPlay(idx, true);
      } else {
        Serial.println("B");
      }
      break;

    case STATE_PLAY:
      emitPlay(idx, isLatest);
      break;

    default:
      break;
  }
}

// ============================================================
// 演奏中の1拍を送出
// ============================================================
void emitPlay(long idx, bool allowNote) {
  int elapsed = (int)(idx - playStartIdx);
  if (elapsed < 0) return;

  int acc = 0, ni = 0, bin = 0;
  while (ni < melodyLength) {
    int bpn = 4 / melody[ni][1];
    if (elapsed < acc + bpn) { bin = elapsed - acc; break; }
    acc += bpn;
    ni++;
  }

  if (ni >= melodyLength) {
    globalBar  = 0;
    globalBeat = 0;
    currentState = STATE_DONE;
    Serial.println("END");
    Serial.println("Melody done. → STATE_DONE");
    return;
  }

  if (!sentInit) {
    Serial.print("T,"); Serial.println((int)currentBPM);
    Serial.print("V,"); Serial.println(volumeLevel);
    sentInit = true;
  }

  if (bin == 0 && allowNote) {
    int note     = melody[ni][0];
    int noteType = melody[ni][1];
    int duration = (int)((4.0 / noteType) * (float)beatInterval);
    Serial.print(note);
    Serial.print(",");
    Serial.println(duration);
  }

  Serial.println("B");
}

// ============================================================
// 音量コマンド処理（レーザ経由）
// ============================================================
void handleVolCommand(byte cmd) {
  if (cmd == CMD_VOL_UP) {
    if (millis() - lastVolChangeTime >= VOL_COOLDOWN_MS) {
      volumeLevel = min(volumeLevel + 10, 100);
      lastVolChangeTime = millis();
      Serial.print("V,"); Serial.println(volumeLevel);
    }
  } else if (cmd == CMD_VOL_DOWN) {
    if (millis() - lastVolChangeTime >= VOL_COOLDOWN_MS) {
      volumeLevel = max(volumeLevel - 10, 0);
      lastVolChangeTime = millis();
      Serial.print("V,"); Serial.println(volumeLevel);
    }
  }
}

// ============================================================
// BPM アンカー更新
// ============================================================
void applyBPM(int bpm) {
  if (currentState != STATE_STANDBY) {
    long cur = anchorBeatIdx + (long)((millis() - anchorMillis) / beatInterval);
    unsigned long curStart = anchorMillis + (unsigned long)((cur - anchorBeatIdx) * (long)beatInterval);
    anchorMillis  = curStart;
    anchorBeatIdx = cur;
  }
  currentBPM   = (float)bpm;
  beatInterval = (unsigned long)(60000.0 / currentBPM);
}

// ============================================================
// Processing からのシリアルコマンド受信
// ============================================================
void checkSerialFromPC() {
  if (Serial.available() > 0) {
    char c = Serial.read();
    if (c == 'R' && currentState != STATE_STANDBY) {
      Serial.print("T,"); Serial.println((int)currentBPM);
      Serial.print("V,"); Serial.println(volumeLevel);
    }
  }
}

// ============================================================
// UDP 受信（BPM + VOL のタイムスタンプをエコーバック）
//
//   BPM パケット : "BPM:120:T:12345"  → BPM 適用 + "ACK:BPM:12345" をエコー
//   VOL パケット : "VOL:80:T:12345"   → "ACK:VOL:12345" をエコー
//   旧形式互換   : "BPM:120"          → BPM 適用 + "ACK" を返送
//
//   ★ 遅延計測は指揮者側が RTT/2 で行う（楽器 millis() は非同期のため）
// ============================================================
void receiveUDP() {
  int packetSize = Udp.parsePacket();
  if (packetSize == 0) return;

  char buf[64] = {0};
  int len = Udp.read(buf, sizeof(buf) - 1);
  if (len <= 0) return;
  buf[len] = '\0';

  IPAddress  senderIP   = Udp.remoteIP();
  uint16_t   senderPort = Udp.remotePort();

  // ---- BPM パケット ----
  if (strncmp(buf, "BPM:", 4) == 0) {
    int bpmValue = atoi(buf + 4);
    if (bpmValue < 30 || bpmValue > 240) return;

    applyBPM(bpmValue);
    if (currentState == STATE_PLAY) {
      Serial.print("T,"); Serial.println(bpmValue);
    }

    char* tPtr = strstr(buf, ":T:");
    if (tPtr != NULL) {
      // タイムスタンプ付き → エコーバック "ACK:BPM:{ts}"
      char ackBuf[32];
      snprintf(ackBuf, sizeof(ackBuf), "ACK:BPM:%s", tPtr + 3);
      Udp.beginPacket(senderIP, senderPort);
      Udp.print(ackBuf);
      Udp.endPacket();
      Serial.print("[ACK] "); Serial.println(ackBuf);
    } else {
      // 旧形式 → 単純 ACK
      Udp.beginPacket(senderIP, senderPort);
      Udp.print("ACK");
      Udp.endPacket();
    }
  }

  // ---- VOL パケット ----
  else if (strncmp(buf, "VOL:", 4) == 0) {
    char* tPtr = strstr(buf, ":T:");
    if (tPtr != NULL) {
      // エコーバック "ACK:VOL:{ts}"
      char ackBuf[32];
      snprintf(ackBuf, sizeof(ackBuf), "ACK:VOL:%s", tPtr + 3);
      Udp.beginPacket(senderIP, senderPort);
      Udp.print(ackBuf);
      Udp.endPacket();
      Serial.print("[ACK] "); Serial.println(ackBuf);
    }
  }
}

// ============================================================
// 遅延計測はすべて指揮者側で RTT/2 として行う。
// 楽器側は ACK エコーのみ担当。
// ============================================================

// ============================================================
// サーボ制御
// ============================================================
void updateServo() {
  if (currentState == STATE_PLAY) {
    myServo.write(ANGLE_PLAY);
  } else {
    myServo.write(ANGLE_REST);
  }
}

// ============================================================
// レーザコマンド検出
// ============================================================
byte checkLaserCommand() {
  if (readLaserState() != 1) return 0;

  unsigned long startTime = millis();
  bool timedOut = false;

  while (readLaserState() == 1) {
    if (millis() - startTime > 1000) { timedOut = true; break; }
    serviceBeats();
  }
  if (timedOut) return 0;

  unsigned long duration = millis() - startTime;

  if (duration >= 250 && duration <= 350) {
    return parseBitStream();
  }

  Serial.print("[PULSE REJECTED] duration=");
  Serial.print(duration);
  Serial.println("ms");
  return 0;
}

int readLaserState() {
  return (analogRead(sensorPin) > threshold) ? 1 : 0;
}

void beatServicingDelay(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) {
    serviceBeats();
  }
}

byte parseBitStream() {
  byte command = 0;
  beatServicingDelay(75);
  for (int i = 2; i >= 0; i--) {
    int votes = 0;
    votes += readLaserState(); beatServicingDelay(5);
    votes += readLaserState(); beatServicingDelay(5);
    votes += readLaserState();
    int bitValue = (votes >= 2) ? 1 : 0;
    command |= (bitValue << i);
    beatServicingDelay(90);
  }
  return command;
}

// ============================================================
// 閾値キャリブレーション
// ============================================================
void calibrateThreshold() {
  Serial.println("閾値キャリブレーション中...");
  int sum = 0;
  for (int i = 0; i < 20; i++) {
    sum += analogRead(sensorPin);
    delay(50);
  }
  int darkLevel = sum / 20;
  threshold = constrain(darkLevel + darkLevel / 5, 100, 900);
  Serial.print("threshold = "); Serial.println(threshold);
}