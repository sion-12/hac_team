// ============================================================
// Ketugou_instrument_var9.ino
// 演奏者側Arduino — ローカル時計自走方式（UDP遅延250msに対応）
//
// [var8からの変更点]
//   レーザコマンド検出(checkLaserCommand/parseBitStream)は最大数百ms
//   Arduinoをブロックする。var8ではこの間 serviceBeats() が呼ばれず
//   拍表示(Bの送信)が一時停止→復帰後にバーストで送信される現象があり、
//   「演奏開始(レーザ受信)の瞬間だけ拍表示がズレて見える」原因になっていた。
//   本版では delay() の代わりに beatServicingDelay() を使い、
//   待機中も serviceBeats() を呼び続けることで、ブロック中も拍が
//   本来のタイミング通りProcessingへ送られ続けるようにした。
//
// [なぜこの方式か]
//   UDPには約250msの遅延があるため、拍クロックには使えない。
//   拍は各楽器のローカル millis()（水晶発振＝ほぼ無遅延・無ジッタ）で
//   自走させる。全楽器はレーザALL STARTをほぼ同時に受け取るので、
//   あとは各自の正確なローカル時計で刻めば楽器間は数ms精度で揃い続ける。
//   UDPはBPM通知（と遅延計測ACK）だけに使う。
//
// [ズレ対策]
//   ① コマンド受信ブロッキング(parseBitStream 最大~375ms)で拍を取りこぼしても、
//      音符位置は「演奏開始からの経過拍数」から都度算出し、取りこぼしぶんは
//      位置だけ最新へジャンプ・発音は最新拍のみ → 追いつきで走らない。
//   ② BPM変更時は現在拍の頭を基準点に位相を張り直す(アンカー) → 位相連続。
//
// [ステートマシン]（遷移条件は従来通り）
//   STANDBY   : ALL START → COUNTING
//   COUNTING  : 毎拍 "B"。選択START → nextBlockBar算出 → PLAY_WAIT
//   PLAY_WAIT : 次ブロック先頭(bar==nextBlockBar && beat==1) → PLAY
//   PLAY      : 音符/拍を送信、終端 → COUNTING
//
// [Processingへのシリアルプロトコル]（従来通り／変更不要）
//   "RESET\n" / "T,120\n" / "V,80\n" / "B\n" / "8,1000\n"
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
const int ANGLE_REST = 75;
const int ANGLE_PLAY = 45;

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
  STATE_PLAY,
  STATE_DONE      // 演奏完了（カウント停止・レーザ監視なし）
};
SystemState currentState = STATE_STANDBY;

// ============================================================
// BPM・拍管理（ローカル時計自走）
//   通算拍 idx : ALL START後の第1拍=1, 第2拍=2, ...
//   第idx拍の開始時刻 = anchorMillis + (idx - anchorBeatIdx) * beatInterval
// ============================================================
float currentBPM = 120.0;
unsigned long beatInterval = 500;       // ms/拍

unsigned long anchorMillis    = 0;      // アンカー時刻
long          anchorBeatIdx   = 1;      // アンカー時刻に始まる拍のidx
long          lastServicedIdx = 0;      // 最後に処理した拍idx

// 表示・ブロック判定用（idxから算出した現在位置）
int globalBeat   = 1;   // 1〜4
int globalBar    = 1;   // 1〜
int nextBlockBar = 0;   // 演奏開始を待つ小節
long playStartIdx = 0;  // 演奏を開始した拍のidx（経過拍の基準）

// ============================================================
// 音量管理
// ============================================================
int volumeLevel = 80;
unsigned long lastVolChangeTime = 0;
const unsigned long VOL_COOLDOWN_MS = 2000;

// ============================================================
// 楽譜データ
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
  serviceBeats();          // ローカル時計で拍を進める

  switch (currentState) {

    // ---- 待機 ----
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

    // ---- 全体カウント中 ----
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
        nextBlockBar = ((globalBar - 1) / 4 + 1) * 4 + 1;
        currentState = STATE_PLAY_WAIT;
        Serial.print(">>> PLAY_WAIT: will start at bar=");
        Serial.println(nextBlockBar);
      } else {
        handleVolCommand(cmd);
      }
    } break;

    // ---- 演奏開始待機 ----
    case STATE_PLAY_WAIT: {
      byte cmd = checkLaserCommand();
      handleVolCommand(cmd);
    } break;

    // ---- 演奏中 ----
    case STATE_PLAY: {
      byte cmd = checkLaserCommand();
      handleVolCommand(cmd);
    } break;

    // ---- 演奏完了（カウント停止・レーザ監視なし） ----
    case STATE_DONE: {
      // 何もしない
    } break;
  }
}

// ============================================================
// カウント開始（ALL START検出時刻を拍の原点にする）
// ============================================================
void startCounting() {
  anchorMillis    = millis();
  anchorBeatIdx   = 1;
  lastServicedIdx = 0;   // 次のserviceBeatsで第1拍を処理
  globalBar       = 1;
  globalBeat      = 1;
  currentState    = STATE_COUNTING;
  Serial.print("T,"); Serial.println((int)currentBPM);
  Serial.print("V,"); Serial.println(volumeLevel);
  Serial.println("=== COUNTING START ===");
}

// ============================================================
// ローカル時計で拍を進める
//   取りこぼし(ブロッキング)があっても位置は最新へジャンプ。
//   発音は最新拍のみ(isLatest)なので追いつきで走らない。
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
        // RESET は送らない：ALL START からの通算小節数をそのまま継続する
        Serial.print(">>> PLAY START at bar=");
        Serial.println(globalBar);
        emitPlay(idx, true);         // 先頭音符は必ず発音
      } else {
        Serial.println("B");         // 演奏開始までのカウントイン
      }
      break;

    case STATE_PLAY:
      emitPlay(idx, isLatest);       // 中間拍(isLatest=false)は発音せず位置だけ通過
      break;

    default:
      break;
  }
}

// ============================================================
// 演奏中の1拍を送出（位置は経過拍から算出）
// ============================================================
void emitPlay(long idx, bool allowNote) {
  int elapsed = (int)(idx - playStartIdx);
  if (elapsed < 0) return;

  // elapsed拍目がどの音符・音符内の何拍目か
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
    Serial.println("END");        // Processingに演奏終了を通知（bar=0 に戻す）
    Serial.println("Melody done. → STATE_DONE");
    return;
  }

  if (!sentInit) {
    Serial.print("T,"); Serial.println((int)currentBPM);
    Serial.print("V,"); Serial.println(volumeLevel);
    sentInit = true;
  }

  // 音符の先頭拍だけノートデータを送る（取りこぼした中間拍の音符はスキップ）
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
    } else {
      Serial.println("!! 音量操作クールダウン中");
    }
  } else if (cmd == CMD_VOL_DOWN) {
    if (millis() - lastVolChangeTime >= VOL_COOLDOWN_MS) {
      volumeLevel = max(volumeLevel - 10, 0);
      lastVolChangeTime = millis();
      Serial.print("V,"); Serial.println(volumeLevel);
    } else {
      Serial.println("!! 音量操作クールダウン中");
    }
  }
}

// ============================================================
// BPMをアンカー方式で更新（位相を連続させる）
// ============================================================
void applyBPM(int bpm) {
  if (currentState != STATE_STANDBY) {
    // 現在拍の頭を新しいアンカーにしてから間隔を変える
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
    if (bpmValue >= 30 && bpmValue <= 200) {  // 指揮者側BPM_MAXの修正(240→200)に合わせて上限を統一
      applyBPM(bpmValue);

      if (currentState == STATE_PLAY) {
        Serial.print("T,"); Serial.println(bpmValue);
      }
      // 遅延計測用ACK
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
//   ※検出処理は数百ms要するため、待機中も beatServicingDelay() で
//     serviceBeats() を呼び続け、拍がフリーズ→バーストしないようにする
// ============================================================
byte checkLaserCommand() {
  if (readLaserState() != 1) return 0;

  unsigned long startTime = millis();
  bool timedOut = false;

  while (readLaserState() == 1) {
    if (millis() - startTime > 1000) { timedOut = true; break; }
    serviceBeats();   // 待機中も拍を進める（フリーズ防止）
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

// delay(ms) の代替：待っている間も拍サービスを止めない
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