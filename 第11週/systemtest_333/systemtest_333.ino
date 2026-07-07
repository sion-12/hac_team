// ============================================================
// test_333_sync_instrument.ino
// システムテスト 333 — 全体演奏テスト（楽器側）
//
// [テスト内容]
//   ① 楽器間同期テスト : 演奏開始タイミング差 < 20ms
//   ② 拍一致テスト     : 全楽器が同じ小節から演奏を開始するか確認
//
// [計測方式]
//   STATE_PLAY_WAIT → STATE_PLAY に遷移した瞬間（最初の発音拍）の
//   micros() を記録し、全楽器で比較する。
//   また、その時点の startBar (nextBlockBar) も記録する。
//
// [試行数]
//   NUM_TRIALS = 10  (指揮者が演奏を 10 回開始させる)
//
// [合格基準]
//   楽器間同期 : 全楽器の t_play の最大差 < 20ms (20000 us)  成功率 95% 以上
//   拍一致     : 全楽器の startBar が一致している回数 10/10 = 100%
//
// [使い方]
//   1. このスケッチを全楽器側 Arduino に書き込む
//   2. 指揮者側は通常の Ketugou_conductor_var5 を使う（テスト専用 conductor 不要）
//   3. 指揮者から ALL_START → 楽器選択 → 再生ボタンを 10 回実行する
//   4. 各楽器のシリアルモニタで t_play と startBar を収集して比較する
//
// [出力フォーマット]
//   [n/10] bar=XX  t_play=YYYYYY us
//
// [test_322 との違い]
//   ・試行数 50 → 10
//   ・計測点 : CMD_START 受信時刻 → 「演奏開始拍 (nextBlockBar)」の micros()
//   ・var10 の完全な演奏機能を維持（実際に音符を送信する）
//   ・startBar（演奏開始小節番号）を追加出力
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
const int ANGLE_REST = 60;
const int ANGLE_PLAY = 30;

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
// テスト計測データ (333 全体演奏テスト)
// ============================================================
const int NUM_TRIALS  = 10;

// allStartMicros : ALL_START レーザ受信時刻 (基準点)
// tPlayRel       : STATE_PLAY 開始時刻 - allStartMicros (相対時間)
//   → 2台で比較するのはこの相対値。絶対 micros() は Arduino ごとに異なるため使えない。
unsigned long allStartMicros = 0;
unsigned long tPlayRel[NUM_TRIALS];
int           startBars[NUM_TRIALS];

int trialCount = 0;
bool awaitingPlayStart = false;
bool needsRestart = false;   // emitPlay→startCounting の再入を防ぐフラグ

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
  Serial.println("=== 333 全体演奏テスト（楽器側） ===");
  Serial.println("  指揮者から ALL_START を受け取ってください");
  Serial.println("  その後、10 回演奏を割り当ててください");
  Serial.println("--------------------------------------------");
}

// ============================================================
// loop
// ============================================================
void loop() {
  // serviceBeats() 内から startCounting() を直接呼ぶと while ループが
  // 古い cur 値で走り続けるため、フラグ経由で loop() レベルから呼ぶ
  if (needsRestart) {
    needsRestart = false;
    startCounting();
  }

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
        awaitingPlayStart = true;   // 次の PLAY 開始拍で計測する
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
      // 何もしない（演奏完了後は次の試行まで待機）
    } break;
  }
}

// ============================================================
// カウント開始
// ============================================================
void startCounting() {
  allStartMicros  = micros();   // ALL_START 受信時刻を基準点として記録
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
        // ----------------------------------------------------------------
        // [計測ポイント] PLAY_WAIT → PLAY 遷移 = 演奏開始の瞬間
        //   ここで micros() を記録することで「この楽器がいつ演奏を開始したか」
        //   を記録する。全楽器でこの値を比較し、差が 20ms 未満なら合格。
        // ----------------------------------------------------------------
        unsigned long tNow = micros();

        currentState = STATE_PLAY;
        playStartIdx = idx;
        sentInit     = false;
        Serial.println("RESET");

        // 計測データの記録（ALL_START からの相対時間で比較）
        if (awaitingPlayStart && trialCount < NUM_TRIALS) {
          unsigned long rel = tNow - allStartMicros;   // 相対時間
          tPlayRel[trialCount] = rel;
          startBars[trialCount] = nextBlockBar;
          trialCount++;
          awaitingPlayStart = false;

          // 結果出力（他の楽器と比較するためのログ）
          Serial.print("[");
          if (trialCount < 10) Serial.print(" ");
          Serial.print(trialCount);
          Serial.print("/");
          Serial.print(NUM_TRIALS);
          Serial.print("] bar=");
          Serial.print(nextBlockBar);
          Serial.print("  t_play=");
          Serial.print(rel);
          Serial.println(" us  (ALL_START からの相対時間)");
          Serial.println("  ※ 他の楽器の t_play, bar と比較してください");
          Serial.println("  ※ t_play 差 < 20000us かつ bar が一致なら合格");

          if (trialCount >= NUM_TRIALS) {
            printTestSummary();
          }
        }

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

    // 直接 startCounting() を呼ぶと serviceBeats() の while ループが
    // 古い cur 値で走り続けて bar が爆速で増えるため、フラグだけ立てる
    if (trialCount < NUM_TRIALS) {
      Serial.println("次の試行に備えて COUNTING を再開します");
      needsRestart = true;   // loop() から startCounting() を呼ぶ
    }
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
// UDP 受信 (BPM)
//   タイムスタンプ付き "BPM:120:T:12345" も旧形式 "BPM:120" も受け付ける
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
// テスト集計出力（10 試行完了後）
// ============================================================
void printTestSummary() {
  Serial.println("");
  Serial.println("============================================");
  Serial.println("  333 全体演奏テスト 計測完了");
  Serial.println("============================================");
  Serial.println("  [楽器間同期テスト] 以下の t_play を他楽器と比較：");
  Serial.println("  (ALL_START 受信からの相対時間 — 絶対値ではなく差分を見る)");
  for (int i = 0; i < NUM_TRIALS; i++) {
    Serial.print("    試行 ");
    if (i + 1 < 10) Serial.print(" ");
    Serial.print(i + 1);
    Serial.print(" : bar=");
    Serial.print(startBars[i]);
    Serial.print("  t_play=");
    Serial.print(tPlayRel[i]);
    Serial.println(" us");
  }
  Serial.println("");
  Serial.println("  [判定基準]");
  Serial.println("  ① 楽器間の t_play 最大差 < 20000us (20ms) → 合格");
  Serial.println("  ② 各試行で全楽器の bar 値が一致 → 合格");
  Serial.println("  合格基準: ① ② ともに 10/10 回一致");
  Serial.println("============================================");
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
  Serial.print("threshold = "); Serial.println(threshold);
}