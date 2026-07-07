// ============================================================
// Ketugou_conductor_var5.ino
// 指揮者側Arduino — 新仕様
//
// [動作フロー]
//   ①スタートボタン（1回目）
//       → 全レーザに START コマンド
//       → 全楽器がカウント開始（音なし）
//       → conductor 側も拍カウント開始
//   ②楽器選択ボタン（instPins[0〜3]）
//       → targetLaser を設定
//   ③スタートボタン（2回目以降）
//       → 選択中の楽器に START コマンド
//       → その楽器が「次の 8 小節ブロック」から演奏開始
//
// [サーボ動作]
//   拍ごとに 15° → 0° のパルス動作（BEAT_HOLD_MS ms 間傾く）
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
// ピン設定
// ============================================================
const int laserPins[]    = {A0, A1, A2, A3};  // レーザ出力（4ch）
const int startButtonPin = 2;                  // スタート/演奏開始ボタン
const int instPins[]     = {3, 4, 5, 6};       // 楽器選択（4ch）
const int volUpPin       = 7;
const int volDownPin     = 8;
const int bpmUpPin       = 9;
const int bpmDownPin     = 10;
const int PIN_LED_DELAY  = 13;                 // 遅延確認LED

// ============================================================
// UDP設定
// ============================================================
WiFiUDP Udp;
const unsigned int localPort = 9090;
const unsigned int destPort  = 8080;
IPAddress broadcastIP;

// ============================================================
// BPM管理
// ============================================================
float currentBPM  = 120.0;
const float BPM_MIN  = 40.0;
const float BPM_MAX  = 240.0;
const float BPM_STEP = 5.0;

// ============================================================
// カウント管理
//   isCounting : 全体カウントが動いているか
// ============================================================
bool isCounting  = false;
int  currentBeat = 1;   // 1〜4
int  currentBar  = 1;   // 1〜
unsigned long previousTime = 0;

// ============================================================
// ターゲットレーザ
// ============================================================
int targetLaser = -1;

// ============================================================
// エッジ検出用
// ============================================================
bool lastStartState   = HIGH;
bool lastBpmUpState   = HIGH;
bool lastBpmDownState = HIGH;
bool lastVolUpState   = HIGH;
bool lastVolDownState = HIGH;
bool lastInstPinState[4] = {HIGH, HIGH, HIGH, HIGH};

// ============================================================
// 遅延計測用
// ============================================================
unsigned long lastSendTime = 0;

// ============================================================
// 音量操作クールダウン用
// ============================================================
unsigned long lastVolChangeTime = 0;
const unsigned long VOL_COOLDOWN_MS = 2000; // 音量変更後のクールダウン時間 (ms)

// ============================================================
// サーボ設定
// ============================================================
Servo myServo;
const int PIN_SERVO      = 11;   // サーボ接続ピン（空きピン）
const int ANGLE_REST     = 0;    // 通常位置
const int ANGLE_BEAT     = 15;   // 拍に合わせて傾ける角度
const unsigned long BEAT_HOLD_MS = 120; // 傾きを保持する時間 (ms)

bool beatActive = false;          // 拍パルス実行中フラグ
unsigned long beatTriggerTime = 0; // 拍パルス開始時刻

// ============================================================
// setup
// ============================================================
void setup() {
  Serial.begin(9600);

  for (int i = 0; i < 4; i++) {
    pinMode(laserPins[i], OUTPUT);
    digitalWrite(laserPins[i], LOW);
  }

  pinMode(startButtonPin, INPUT_PULLUP);
  for (int i = 0; i < 4; i++) pinMode(instPins[i], INPUT_PULLUP);
  pinMode(volUpPin,      INPUT_PULLUP);
  pinMode(volDownPin,    INPUT_PULLUP);
  pinMode(bpmUpPin,      INPUT_PULLUP);
  pinMode(bpmDownPin,    INPUT_PULLUP);
  pinMode(PIN_LED_DELAY, OUTPUT);

  myServo.attach(PIN_SERVO);
  myServo.write(ANGLE_REST);

  Serial.print("Connecting to WiFi...");
  while (WiFi.begin(ssid, pass) != WL_CONNECTED) {
    delay(1000);
    Serial.print(".");
  }
  Serial.println(" Connected!");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  IPAddress ip     = WiFi.localIP();
  IPAddress subnet = WiFi.subnetMask();
  for (int i = 0; i < 4; i++) {
    broadcastIP[i] = (ip[i] & subnet[i]) | (~subnet[i] & 0xFF);
  }
  Serial.print("Broadcast IP: ");
  Serial.println(broadcastIP);

  Udp.begin(localPort);
  broadcastTempoData();

  // 起動時に自動で全体 START → 全楽器カウント開始
  delay(500); // 楽器側の起動を待つ余裕
  sendLaserCommand(0b001, -1);
  isCounting   = true;
  currentBeat  = 1;
  currentBar   = 1;
  previousTime = millis();
  Serial.println("=== ALL COUNT START (AUTO) ===");
}

// ============================================================
// loop
// ============================================================
void loop() {
  checkButtons();
  countBeats();
  updateServo();
  receiveACK();
}

// ============================================================
// タクトスイッチ処理
// ============================================================
void checkButtons() {

  // --- BPMアップ ---
  bool bpmUpState = digitalRead(bpmUpPin);
  if (lastBpmUpState == HIGH && bpmUpState == LOW) {
    currentBPM = min(currentBPM + BPM_STEP, BPM_MAX);
    broadcastTempoData();
    delay(50);
  }
  lastBpmUpState = bpmUpState;

  // --- BPMダウン ---
  bool bpmDownState = digitalRead(bpmDownPin);
  if (lastBpmDownState == HIGH && bpmDownState == LOW) {
    currentBPM = max(currentBPM - BPM_STEP, BPM_MIN);
    broadcastTempoData();
    delay(50);
  }
  lastBpmDownState = bpmDownState;

  // --- 再生開始ボタン（選択中の楽器を次ブロックから演奏）---
  bool startState = digitalRead(startButtonPin);
  if (lastStartState == HIGH && startState == LOW) {
    if (targetLaser != -1) {
      sendLaserCommand(0b001, targetLaser);
      Serial.print(">>> PLAY NEXT BLOCK: laser=");
      Serial.println(targetLaser);
      targetLaser = -1;  // 送信完了 → 楽器選択をリセット
    } else {
      Serial.println("!! 楽器が選択されていません (instPins で選択してください)");
    }
    delay(50);
  }
  lastStartState = startState;

  // --- 楽器選択（4チャンネル）---
  for (int i = 0; i < 4; i++) {
    bool instState = digitalRead(instPins[i]);
    if (lastInstPinState[i] == HIGH && instState == LOW) {
      targetLaser = i;
      Serial.print("Target laser: ");
      Serial.println(i);
      delay(50);
    }
    lastInstPinState[i] = instState;
  }

  // --- 音量アップ ---
  bool volUpState = digitalRead(volUpPin);
  if (lastVolUpState == HIGH && volUpState == LOW && targetLaser != -1) {
    if (millis() - lastVolChangeTime >= VOL_COOLDOWN_MS) {
      sendLaserCommand(0b010, targetLaser);
      lastVolChangeTime = millis();
      targetLaser = -1;  // 送信完了 → 楽器選択をリセット
      delay(50);
    } else {
      Serial.println("!! 音量操作クールダウン中 (2秒待ってください)");
    }
  }
  lastVolUpState = volUpState;

  // --- 音量ダウン ---
  bool volDownState = digitalRead(volDownPin);
  if (lastVolDownState == HIGH && volDownState == LOW && targetLaser != -1) {
    if (millis() - lastVolChangeTime >= VOL_COOLDOWN_MS) {
      sendLaserCommand(0b011, targetLaser);
      lastVolChangeTime = millis();
      targetLaser = -1;  // 送信完了 → 楽器選択をリセット
      delay(50);
    } else {
      Serial.println("!! 音量操作クールダウン中 (2秒待ってください)");
    }
  }
  lastVolDownState = volDownState;
}

// ============================================================
// 拍・小節カウント
// 拍ごとにサーボパルスをトリガする
// ============================================================
void countBeats() {
  if (!isCounting) return;

  float beatTime = 60000.0 / currentBPM;
  unsigned long currentTime = millis();

  if (currentTime - previousTime >= (unsigned long)beatTime) {
    previousTime += (unsigned long)beatTime;

    Serial.print("COUNT bar=");
    Serial.print(currentBar);
    Serial.print(" beat=");
    Serial.println(currentBeat);

    // サーボパルスをトリガ
    myServo.write(ANGLE_BEAT);
    beatActive      = true;
    beatTriggerTime = millis();

    currentBeat++;
    if (currentBeat > 4) {
      currentBeat = 1;
      currentBar++;
    }
  }
}

// ============================================================
// サーボ制御
// 拍トリガから BEAT_HOLD_MS ms 経過したら 0° に戻す
// ============================================================
void updateServo() {
  if (beatActive && millis() - beatTriggerTime >= BEAT_HOLD_MS) {
    myServo.write(ANGLE_REST);
    beatActive = false;
  }
}

// ============================================================
// BPM UDP ブロードキャスト
// ============================================================
void broadcastTempoData() {
  Udp.beginPacket(broadcastIP, destPort);
  Udp.print("BPM:");
  Udp.print((int)currentBPM);
  Udp.endPacket();
  lastSendTime = millis();
  Serial.print("Broadcasted BPM:");
  Serial.println((int)currentBPM);
}

// ============================================================
// ACK受信・遅延計測
// ============================================================
void receiveACK() {
  int packetSize = Udp.parsePacket();
  if (packetSize > 0) {
    char ackBuf[16] = {0};
    int len = Udp.read(ackBuf, sizeof(ackBuf) - 1);
    if (len > 0) {
      ackBuf[len] = '\0';
      if (strcmp(ackBuf, "ACK") == 0) {
        unsigned long delayTime = (millis() - lastSendTime) / 2;
        Serial.print("【通信遅延】約 ");
        Serial.print(delayTime);
        Serial.println(" ms");
        digitalWrite(PIN_LED_DELAY, delayTime < 20 ? HIGH : LOW);
      }
    }
  }
}

// ============================================================
// レーザ送信
//   target=-1 → 全レーザ同時
//   target=0〜3 → 指定チャンネルのみ
// ============================================================
void sendLaserCommand(byte command, int target) {
  // 開始合図：300ms点灯 → 50ms消灯
  setLaserState(target, HIGH);
  delay(300);
  setLaserState(target, LOW);
  delay(50);

  // 3ビット MSB から順に送信
  for (int bit = 2; bit >= 0; bit--) {
    int bitValue = (command >> bit) & 0x01;
    setLaserState(target, bitValue == 1 ? HIGH : LOW);
    delay(50);
    setLaserState(target, LOW);
    delay(50);
  }
}

void setLaserState(int target, int state) {
  if (target == -1) {
    for (int i = 0; i < 4; i++) digitalWrite(laserPins[i], state);
  } else {
    digitalWrite(laserPins[target], state);
  }
}
