// ============================================================
// Ketugou_contrabass.ino
// コントラバス専用 — 8音ベースパターンを無限ループ再生
//
// [var10(フルート版)からの変更点]
//   ・melody を8音ベースパターン専用に差し替え
//   ・emitPlay() でモジュロ演算 (elapsed % melodyTotalBeats) を使い
//     メロディ末尾に達しても STATE_DONE に入らず永遠にループ
//   ・STATE_DONE / "END" 送信は使わない
//
// [Processing側 contrabass_player.pde の frequencies[] との対応]
//   index 0 = 293.66 Hz  (D4  / レ )
//   index 1 = 440.00 Hz  (A4  / ラ )
//   index 2 = 493.88 Hz  (B4  / シ )
//   index 3 = 369.99 Hz  (F#4 / ファ#)
//   index 4 = 392.00 Hz  (G4  / ソ )
//   index 5 = 146.83 Hz  (D3  / レ_)
//   index 6 = 392.00 Hz  (G4  / ソ )
//   index 7 = 440.00 Hz  (A4  / ラ )
// ============================================================

#include <WiFiS3.h>
#include <WiFiUdp.h>
#include <Servo.h>

// ============================================================
// WiFi設定
// ============================================================
char ssid[] = "hackathon010-WPA2";
char pass[] = "hackathon010";

// ============================================================
// UDP設定
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
// デバッグ設定
// ============================================================
// #define PLOTTER_MODE
// #define DEBUG_MODE

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
  STATE_PLAY
  // STATE_DONE は使わない（コントラバスは永遠にループ）
};
SystemState currentState = STATE_STANDBY;

// ============================================================
// BPM・拍管理（ローカル時計自走）
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
// 楽譜データ（8音ベースパターン / duration=2は2拍=半音符）
//   再生順: レ→ラ→シ→ファ#→ソ→レ_→ソ→ラ を永遠に繰り返す
// ============================================================
int melody[][2] = {
  {0, 2},  // D4  レ
  {1, 2},  // A4  ラ
  {2, 2},  // B4  シ
  {3, 2},  // F#4 ファ#
  {4, 2},  // G4  ソ
  {5, 2},  // D3  レ_（1オクターブ下のレ）
  {6, 2},  // G4  ソ
  {7, 2},  // A4  ラ
};
int melodyLength;
int melodyTotalBeats;   // 8音 × 2拍 = 16拍 (ループ周期)

// ============================================================
// 演奏進行管理
// ============================================================
bool sentInit = false;
unsigned long lastHeartbeat = 0;

// ============================================================
// setup
// ============================================================
void setup() {
  Serial.begin(9600);
  pinMode(sensorPin, INPUT);

  melodyLength = sizeof(melody) / sizeof(melody[0]);

  // ループ周期を算出（duration 2 → 4/2 = 2拍/音 × 8音 = 16拍）
  melodyTotalBeats = 0;
  for (int i = 0; i < melodyLength; i++) {
    melodyTotalBeats += 4 / melody[i][1];
  }

  calibrateThreshold();

  myServo.attach(PIN_SERVO);
  myServo.write(ANGLE_REST);

  Serial.print("Connecting...");
  while (WiFi.begin(ssid, pass) != WL_CONNECTED) {
    delay(1000);
    Serial.print(".");
  }
  Serial.println(" Connected!");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
  Udp.begin(localPort);

  currentState = STATE_STANDBY;
  Serial.println("Ready. Waiting for ALL START laser...");
}

// ============================================================
// loop
// ============================================================
void loop() {

#ifdef PLOTTER_MODE
  Serial.print(analogRead(sensorPin));
  Serial.print(",");
  Serial.println(threshold);
  delay(20);
  return;
#endif

  receiveBPM();
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
// ローカル時計で拍を進める
// ============================================================
void serviceBeats() {
  if (currentState == STATE_STANDBY) return;  // STATE_DONE がないのでここだけ除外

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
        // RESET は送らない：ALL START からの通算小節数をそのまま継続する
        Serial.print(">>> PLAY START at bar=");
        Serial.println(globalBar);
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
// 演奏中の1拍を送出（モジュロでループ）
//
//   elapsedRaw : 演奏開始からの通算拍数
//   elapsed    : elapsedRaw % melodyTotalBeats → 常に [0, 16) に収まる
//   → ni が melodyLength に達しないので STATE_DONE に入らない
// ============================================================
void emitPlay(long idx, bool allowNote) {
  int elapsedRaw = (int)(idx - playStartIdx);
  if (elapsedRaw < 0) return;

  int elapsed = elapsedRaw % melodyTotalBeats;  // ← ループの核心

  int acc = 0, ni = 0, bin = 0;
  while (ni < melodyLength) {
    int bpn = 4 / melody[ni][1];
    if (elapsed < acc + bpn) { bin = elapsed - acc; break; }
    acc += bpn;
    ni++;
  }
  // elapsed % melodyTotalBeats により ni < melodyLength が保証される

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
// 音量コマンド処理
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
// BPMをアンカー方式で更新
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
// UDP経由でBPMを受信
// ============================================================
void receiveBPM() {
  int packetSize = Udp.parsePacket();
  if (packetSize == 0) return;

  char buf[64] = {0};
  int len = Udp.read(buf, sizeof(buf) - 1);
  if (len <= 0) return;
  buf[len] = '\0';

  if (strncmp(buf, "BPM:", 4) == 0) {
    int bpmValue = atoi(buf + 4);
    if (bpmValue >= 30 && bpmValue <= 240) {
      applyBPM(bpmValue);
      if (currentState == STATE_PLAY) {
        Serial.print("T,"); Serial.println(bpmValue);
      }
      Udp.beginPacket(Udp.remoteIP(), Udp.remotePort());
      Udp.print("ACK");
      Udp.endPacket();
    }
  }
}

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
  Serial.print("threshold = ");
  Serial.println(threshold);
}