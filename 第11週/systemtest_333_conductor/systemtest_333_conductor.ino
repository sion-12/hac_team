// ============================================================
// test_332_conductor.ino
// システムテスト 332 — 指示反映テスト（指揮者側）
//
// [テスト内容] (各 10 回試行)
//   BPM 反映テスト  : BPM 変更 → 全楽器で値が一致するか確認
//   BPM 遅延テスト  : BPM UDP に millis() タイムスタンプを埋め込み片道遅延を計測
//   音量反映テスト  : 音量変更 → 指定楽器で値が反映されるか確認
//   音量遅延テスト  : 音量変更直後に UDP "VOL:XX:T:millis" を送信し遅延を計測
//
// [基本動作]
//   conductor_var5 と同じ動作を維持しつつテスト用タイムスタンプを追加。
//   BPM UDP フォーマット  : "BPM:120:T:12345"  (末尾に送信 millis() を付加)
//   音量 UDP フォーマット : "VOL:80:T:12345"   (音量ボタン押下時に別途 UDP 送信)
//
// [使い方]
//   1. このスケッチを指揮者側 Arduino に書き込む
//   2. test_332_instrument.ino を全楽器側 Arduino に書き込む
//   3. 演奏を開始する (AUTO COUNT START → 楽器選択 → 再生ボタン)
//   4. 演奏中に BPM ボタン / 音量ボタンを 10 回ずつ操作する
//   5. 各楽器のシリアルモニタで遅延を確認する
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
// ピン設定
// ============================================================
const int laserPins[]    = {A0, A1, A2, A3};
const int startButtonPin = 2;
const int instPins[]     = {3, 4, 5, 6};
const int volUpPin       = 7;
const int volDownPin     = 8;
const int bpmUpPin       = 9;    // ※ PIN_SERVO(11) と重複しないよう注意
const int bpmDownPin     = 10;
const int PIN_LED_DELAY  = 13;

// ============================================================
// UDP 設定
// ============================================================
WiFiUDP Udp;
const unsigned int localPort = 9090;
const unsigned int destPort  = 8080;
IPAddress broadcastIP;

// ============================================================
// BPM 管理
// ============================================================
float currentBPM  = 120.0;
const float BPM_MIN  = 40.0;
const float BPM_MAX  = 240.0;
const float BPM_STEP = 5.0;

// ============================================================
// カウント管理
// ============================================================
bool isCounting  = false;
int  currentBeat = 1;
int  currentBar  = 1;
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
// 遅延計測用 (RTT/2 方式 — 指揮者側で完結)
// ============================================================
const int    NUM_TRIALS       = 10;
const long   PASS_THRESHOLD_MS = 100;   // 片道 100ms 以内で PASS

// BPM 遅延
unsigned long currentBpmTs  = 0;     // 最後に送った BPM の送信時刻
bool          bpmAcked       = true;  // 既に ACK 受信済みか（重複排除）
int           bpmTrialCount  = 0;
int           bpmPassCount   = 0;
long          bpmDelays[NUM_TRIALS];

// 音量遅延
unsigned long currentVolTs  = 0;
bool          volAcked       = true;
int           volTrialCount  = 0;
int           volPassCount   = 0;
long          volDelays[NUM_TRIALS];

// ============================================================
// 音量操作クールダウン
// ============================================================
unsigned long lastVolChangeTime = 0;
const unsigned long VOL_COOLDOWN_MS = 2000;

// ============================================================
// サーボ設定
// ============================================================
Servo myServo;
const int PIN_SERVO      = 11;
const int ANGLE_REST     = 0;
const int ANGLE_BEAT     = 15;
const unsigned long BEAT_HOLD_MS = 120;
bool beatActive = false;
unsigned long beatTriggerTime = 0;

// BPM/音量 ボタン押下回数カウンタ（表示用）
int bpmBtnCount = 0;
int volBtnCount = 0;

// 現在の音量 (楽器側と同じ初期値)
int currentVolume = 80;

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
  Serial.print("IP: "); Serial.println(WiFi.localIP());

  IPAddress ip     = WiFi.localIP();
  IPAddress subnet = WiFi.subnetMask();
  for (int i = 0; i < 4; i++) {
    broadcastIP[i] = (ip[i] & subnet[i]) | (~subnet[i] & 0xFF);
  }
  Serial.print("Broadcast: "); Serial.println(broadcastIP);

  Udp.begin(localPort);
  broadcastTempoData();

  delay(500);
  sendLaserCommand(0b001, -1);   // 全楽器カウント開始
  isCounting   = true;
  currentBeat  = 1;
  currentBar   = 1;
  previousTime = millis();

  // setup() 内の broadcastTempoData() に対する ACK はバッファに滞留するため
  // ループ開始前にフラグをリセットして計測対象外にする
  bpmAcked = true;
  volAcked = true;

  Serial.println("=== ALL COUNT START ===");
  Serial.println("--- 332 システムテスト 開始 ---");
  Serial.println("演奏開始後、BPM ボタンを 10 回、音量ボタンを 10 回操作してください");
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
// ボタン処理
// ============================================================
void checkButtons() {

  // --- BPM アップ ---
  bool bpmUpState = digitalRead(bpmUpPin);
  if (lastBpmUpState == HIGH && bpmUpState == LOW) {
    currentBPM = min(currentBPM + BPM_STEP, BPM_MAX);
    broadcastTempoData();   // タイムスタンプ付きで送信
    bpmBtnCount++;
    Serial.print("[BPM操作 ");
    Serial.print(bpmBtnCount);
    Serial.print("/10] BPM=");
    Serial.println((int)currentBPM);
    delay(50);
  }
  lastBpmUpState = bpmUpState;

  // --- BPM ダウン ---
  bool bpmDownState = digitalRead(bpmDownPin);
  if (lastBpmDownState == HIGH && bpmDownState == LOW) {
    currentBPM = max(currentBPM - BPM_STEP, BPM_MIN);
    broadcastTempoData();
    bpmBtnCount++;
    Serial.print("[BPM操作 ");
    Serial.print(bpmBtnCount);
    Serial.print("/10] BPM=");
    Serial.println((int)currentBPM);
    delay(50);
  }
  lastBpmDownState = bpmDownState;

  // --- 再生開始ボタン ---
  bool startState = digitalRead(startButtonPin);
  if (lastStartState == HIGH && startState == LOW) {
    if (targetLaser != -1) {
      sendLaserCommand(0b001, targetLaser);
      Serial.print(">>> PLAY NEXT BLOCK: laser=");
      Serial.println(targetLaser);
      targetLaser = -1;
    } else {
      Serial.println("!! 楽器が選択されていません");
    }
    delay(50);
  }
  lastStartState = startState;

  // --- 楽器選択 ---
  for (int i = 0; i < 4; i++) {
    bool instState = digitalRead(instPins[i]);
    if (lastInstPinState[i] == HIGH && instState == LOW) {
      targetLaser = i;
      Serial.print("Target laser: "); Serial.println(i);
      delay(50);
    }
    lastInstPinState[i] = instState;
  }

  // --- 音量アップ ---
  bool volUpState = digitalRead(volUpPin);
  if (lastVolUpState == HIGH && volUpState == LOW && targetLaser != -1) {
    if (millis() - lastVolChangeTime >= VOL_COOLDOWN_MS) {
      unsigned long ts = millis();
      currentVolume = min(currentVolume + 10, 100);

      // ① 音量変更 UDP タイムスタンプ送信（遅延計測用）
      sendVolumeTimestamp(currentVolume, ts);

      // ② 通常のレーザコマンド送信（実際の音量変更）
      sendLaserCommand(0b010, targetLaser);

      lastVolChangeTime = millis();
      volBtnCount++;
      Serial.print("[VOL操作 ");
      Serial.print(volBtnCount);
      Serial.print("/10] VOL=");
      Serial.print(currentVolume);
      Serial.print(" T=");
      Serial.println(ts);
      targetLaser = -1;
      delay(50);
    } else {
      Serial.println("!! 音量操作クールダウン中");
    }
  }
  lastVolUpState = volUpState;

  // --- 音量ダウン ---
  bool volDownState = digitalRead(volDownPin);
  if (lastVolDownState == HIGH && volDownState == LOW && targetLaser != -1) {
    if (millis() - lastVolChangeTime >= VOL_COOLDOWN_MS) {
      unsigned long ts = millis();
      currentVolume = max(currentVolume - 10, 0);

      sendVolumeTimestamp(currentVolume, ts);
      sendLaserCommand(0b011, targetLaser);

      lastVolChangeTime = millis();
      volBtnCount++;
      Serial.print("[VOL操作 ");
      Serial.print(volBtnCount);
      Serial.print("/10] VOL=");
      Serial.print(currentVolume);
      Serial.print(" T=");
      Serial.println(ts);
      targetLaser = -1;
      delay(50);
    } else {
      Serial.println("!! 音量操作クールダウン中");
    }
  }
  lastVolDownState = volDownState;
}

// ============================================================
// 拍カウント
// ============================================================
void countBeats() {
  if (!isCounting) return;
  float beatTime = 60000.0 / currentBPM;
  unsigned long currentTime = millis();
  if (currentTime - previousTime >= (unsigned long)beatTime) {
    previousTime += (unsigned long)beatTime;
    myServo.write(ANGLE_BEAT);
    beatActive      = true;
    beatTriggerTime = millis();
    currentBeat++;
    if (currentBeat > 4) { currentBeat = 1; currentBar++; }
  }
}

// ============================================================
// サーボ制御
// ============================================================
void updateServo() {
  if (beatActive && millis() - beatTriggerTime >= BEAT_HOLD_MS) {
    myServo.write(ANGLE_REST);
    beatActive = false;
  }
}

// ============================================================
// BPM UDP ブロードキャスト（タイムスタンプ付き）
//   フォーマット: "BPM:120:T:12345"
// ============================================================
void broadcastTempoData() {
  currentBpmTs = millis();
  bpmAcked     = false;   // 次の ACK を待つ
  Udp.beginPacket(broadcastIP, destPort);
  Udp.print("BPM:");
  Udp.print((int)currentBPM);
  Udp.print(":T:");
  Udp.print(currentBpmTs);
  Udp.endPacket();
  Serial.print("UDP BPM:"); Serial.print((int)currentBPM);
  Serial.print(":T:"); Serial.println(currentBpmTs);
}

// ============================================================
// 音量タイムスタンプ UDP 送信（計測用）
//   フォーマット: "VOL:80:T:12345"
//   ※ レーザによる実際の音量変更とは別に、UDP で遅延計測用タイムスタンプを送る
// ============================================================
void sendVolumeTimestamp(int vol, unsigned long ts) {
  currentVolTs = ts;
  volAcked     = false;   // 次の ACK を待つ
  Udp.beginPacket(broadcastIP, destPort);
  Udp.print("VOL:");
  Udp.print(vol);
  Udp.print(":T:");
  Udp.print(ts);
  Udp.endPacket();
}

// ============================================================
// ACK 受信・RTT 計測
//
//   "ACK:BPM:{ts}" → BPM 片道遅延 = (millis() - ts) / 2
//   "ACK:VOL:{ts}" → 音量片道遅延 = (millis() - ts) / 2
//   "ACK"          → 旧形式（計測スキップ）
// ============================================================
void receiveACK() {
  int packetSize = Udp.parsePacket();
  if (packetSize <= 0) return;

  char ackBuf[32] = {0};
  int len = Udp.read(ackBuf, sizeof(ackBuf) - 1);
  if (len <= 0) return;
  ackBuf[len] = '\0';

  unsigned long now = millis();

  // --- BPM ACK ---
  if (strncmp(ackBuf, "ACK:BPM:", 8) == 0 && !bpmAcked && bpmTrialCount < NUM_TRIALS) {
    unsigned long echoTs = (unsigned long)atol(ackBuf + 8);
    if (echoTs == currentBpmTs) {
      long rtt     = (long)(now - echoTs);
      long oneWay  = rtt / 2;
      bpmDelays[bpmTrialCount] = oneWay;
      bool pass = (oneWay <= PASS_THRESHOLD_MS);
      if (pass) bpmPassCount++;
      bpmTrialCount++;
      bpmAcked = true;
      digitalWrite(PIN_LED_DELAY, pass ? HIGH : LOW);

      Serial.print("[BPM遅延 ");
      Serial.print(bpmTrialCount);
      Serial.print("/");
      Serial.print(NUM_TRIALS);
      Serial.print("] RTT=");
      Serial.print(rtt);
      Serial.print("ms 片道≈");
      Serial.print(oneWay);
      Serial.print("ms → ");
      Serial.println(pass ? "PASS" : "FAIL");

      if (bpmTrialCount >= NUM_TRIALS) printBPMSummary();
    }
  }

  // --- VOL ACK ---
  else if (strncmp(ackBuf, "ACK:VOL:", 8) == 0 && !volAcked && volTrialCount < NUM_TRIALS) {
    unsigned long echoTs = (unsigned long)atol(ackBuf + 8);
    if (echoTs == currentVolTs) {
      long rtt     = (long)(now - echoTs);
      long oneWay  = rtt / 2;
      volDelays[volTrialCount] = oneWay;
      bool pass = (oneWay <= PASS_THRESHOLD_MS);
      if (pass) volPassCount++;
      volTrialCount++;
      volAcked = true;
      digitalWrite(PIN_LED_DELAY, pass ? HIGH : LOW);

      Serial.print("[音量遅延 ");
      Serial.print(volTrialCount);
      Serial.print("/");
      Serial.print(NUM_TRIALS);
      Serial.print("] RTT=");
      Serial.print(rtt);
      Serial.print("ms 片道≈");
      Serial.print(oneWay);
      Serial.print("ms → ");
      Serial.println(pass ? "PASS" : "FAIL");

      if (volTrialCount >= NUM_TRIALS) printVolSummary();
    }
  }
}

// ============================================================
// 集計出力 — BPM 遅延テスト
// ============================================================
void printBPMSummary() {
  Serial.println("");
  Serial.println("============================================");
  Serial.println("  BPM 遅延テスト 集計  (テスト番号 332)");
  Serial.println("============================================");
  long sum = 0, maxD = 0;
  for (int i = 0; i < NUM_TRIALS; i++) {
    Serial.print("  試行 "); Serial.print(i + 1); Serial.print(" : ");
    Serial.print(bpmDelays[i]); Serial.print(" ms → ");
    Serial.println(bpmDelays[i] <= PASS_THRESHOLD_MS ? "PASS" : "FAIL");
    sum += bpmDelays[i];
    if (bpmDelays[i] > maxD) maxD = bpmDelays[i];
  }
  Serial.print("  平均遅延 : "); Serial.print(sum / NUM_TRIALS); Serial.println(" ms");
  Serial.print("  最大遅延 : "); Serial.print(maxD); Serial.println(" ms");
  Serial.print("  PASS 率  : "); Serial.print(bpmPassCount); Serial.print("/"); Serial.println(NUM_TRIALS);
  Serial.print("  判定     : "); Serial.println(bpmPassCount == NUM_TRIALS ? "合格" : "不合格");
  Serial.println("============================================");
  Serial.println("");
}

// ============================================================
// 集計出力 — 音量遅延テスト
// ============================================================
void printVolSummary() {
  Serial.println("");
  Serial.println("============================================");
  Serial.println("  音量遅延テスト 集計  (テスト番号 332)");
  Serial.println("============================================");
  long sum = 0, maxD = 0;
  for (int i = 0; i < NUM_TRIALS; i++) {
    Serial.print("  試行 "); Serial.print(i + 1); Serial.print(" : ");
    Serial.print(volDelays[i]); Serial.print(" ms → ");
    Serial.println(volDelays[i] <= PASS_THRESHOLD_MS ? "PASS" : "FAIL");
    sum += volDelays[i];
    if (volDelays[i] > maxD) maxD = volDelays[i];
  }
  Serial.print("  平均遅延 : "); Serial.print(sum / NUM_TRIALS); Serial.println(" ms");
  Serial.print("  最大遅延 : "); Serial.print(maxD); Serial.println(" ms");
  Serial.print("  PASS 率  : "); Serial.print(volPassCount); Serial.print("/"); Serial.println(NUM_TRIALS);
  Serial.print("  判定     : "); Serial.println(volPassCount == NUM_TRIALS ? "合格" : "不合格");
  Serial.println("============================================");
  Serial.println("");
}

// ============================================================
// レーザ送信
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