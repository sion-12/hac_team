// ============================================================
// Ketugou_instrument_var6.ino
// 演奏者側Arduino — 新仕様
//
// [ステートマシン]
//   STATE_STANDBY
//     → CMD_START 受信 → STATE_COUNTING
//
//   STATE_COUNTING  (全体カウント中・音なし)
//     毎拍 "B\n" をProcessingへ送信して拍表示を更新
//     → CMD_START 受信 → nextBlockBar を計算 → STATE_PLAY_WAIT
//
//   STATE_PLAY_WAIT  (演奏開始を次ブロック境界まで待機)
//     カウントを継続しながら nextBlockBar を待つ
//     → 次ブロック境界（globalBar == nextBlockBar && globalBeat == 1）
//        → STATE_PLAY
//
//   STATE_PLAY  (演奏中)
//     音符データを Processing へ送信
//     → 楽譜終端 → STATE_COUNTING（カウント継続）
//
// [ブロック単位での合流]
//   ブロック1 : 小節 1〜 4
//   ブロック2 : 小節 5〜 8
//   ブロック3 : 小節 9〜12
//   ...（BLOCK_SIZE_BARS 小節ごと。下部の定数で変更可）
//   CMD_START を受信した時点の小節から「次のブロック先頭小節」を計算：
//     nextBlockBar = ((globalBar - 1) / BLOCK_SIZE_BARS + 1) * BLOCK_SIZE_BARS + 1
//
// [Processingへのシリアルプロトコル]
//   "RESET\n"  → bar=1, beat=-1 にリセット（演奏開始前）
//   "T,120\n"  → BPM通知
//   "V,80\n"   → 音量通知 (0-100)
//   "B\n"      → 拍ティック（毎拍、カウント中も送信）
//   "8,1000\n" → 音符番号,デュレーション(ms)
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
const int ANGLE_PLAY = 75;  // 演奏中の傾き角度

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
// 演奏開始ブロックの単位（何小節ごとに合流できるか）
//   例: 4 なら 1-4, 5-8, 9-12... の各先頭小節から合流可能
// ============================================================
const int BLOCK_SIZE_BARS = 4;

// ============================================================
// レーザー受信ステートマシン（非ブロッキング）
//   従来の checkLaserCommand()/parseBitStream() は delay() と
//   busy-wait で最大約650msブロックしており、その間 loop() が
//   完全に止まる → 拍カウント(lastBeatTime)判定も止まる →
//   コマンドを受けた楽器だけ拍が欠落し、楽器間でズレる原因になっていた。
//   ここでは millis() ベースの状態機械にして、ブロックを一切発生させない。
// ============================================================
enum LaserRxState { LZ_IDLE, LZ_HEADER_ACTIVE, LZ_BIT_ACTIVE };
LaserRxState  lzState        = LZ_IDLE;
unsigned long lzHeaderStart  = 0;   // ヘッダHIGHを検知した時刻
unsigned long lzFrameStart   = 0;   // ヘッダがLOWに戻った時刻（=ビット列の基準t=0）
int           lzBitIdx       = 0;   // 0〜2（MSBから順）
byte          lzCommand      = 0;
int           lzVotesHigh    = 0;
int           lzVotesTotal   = 0;
byte          lzPendingCommand = 0;
bool          lzCommandReady   = false;

// 送信側(conductor)のタイミングと対応する定数
const unsigned long LZ_GAP_MS       = 50;   // ヘッダ後の消灯期間
const unsigned long LZ_BIT_SLOT_MS  = 100;  // 1ビットあたりの時間（パルス50ms+消灯50ms）
const unsigned long LZ_SAMPLE_FROM  = 10;   // 各ビット窓内、サンプリング開始オフセット
const unsigned long LZ_SAMPLE_TO    = 40;   // サンプリング終了オフセット

// ============================================================
// ステートマシン
// ============================================================
enum SystemState {
  STATE_STANDBY,    // 待機
  STATE_COUNTING,   // 全体カウント中（音なし）
  STATE_PLAY_WAIT,  // 演奏開始待機（次ブロック境界待ち）
  STATE_PLAY        // 演奏中
};
SystemState currentState = STATE_STANDBY;

// ============================================================
// BPM・拍管理
// ============================================================
float currentBPM = 120.0;
unsigned long beatInterval = 500;
unsigned long lastBeatTime = 0;

// グローバルカウント（全体同期用）
int globalBeat   = 0;  // 現在の拍（1〜4）
int globalBar    = 0;  // 現在の小節（1〜）
int nextBlockBar = 0;  // 演奏開始を待つ小節番号

// ============================================================
// 音量管理
// ============================================================
int volumeLevel = 80;

// ============================================================
// 音量操作クールダウン用
// ============================================================
unsigned long lastVolChangeTime = 0;
const unsigned long VOL_COOLDOWN_MS = 2000; // 音量変更後のクールダウン時間 (ms)

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
int  noteIndex  = 0;
int  beatInNote = 0;
bool sentInit   = false;

// ハートビート
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
  updateLaserRx();   // 非ブロッキングでレーザー受信を1ステップ進める

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
      byte cmd = getLaserCommand();
      if (cmd == CMD_START) {
        // 全体カウント開始
        globalBeat   = 0;
        globalBar    = 1;
        // 即座に第1拍を送信するために1拍分先行させる
        lastBeatTime = millis() - beatInterval;
        currentState = STATE_COUNTING;
        // Processing に BPM/音量を通知
        Serial.print("T,"); Serial.println((int)currentBPM);
        Serial.print("V,"); Serial.println(volumeLevel);
        Serial.println("=== COUNTING START ===");
      }
    } break;

    // ---- 全体カウント中（音なし） ----
    case STATE_COUNTING: {
      if (millis() - lastHeartbeat >= 2000) {
        lastHeartbeat = millis();
        Serial.print("[COUNTING] bar=");
        Serial.print(globalBar);
        Serial.print(" beat=");
        Serial.println(globalBeat);
      }

      // 拍カウント・Processing へ "B" 送信
      // while化: 万一処理が遅延して複数拍分経過していても取りこぼさない
      while (millis() - lastBeatTime >= beatInterval) {
        lastBeatTime += beatInterval;
        Serial.println("B");
        advanceGlobalCount();
      }

      // レーザコマンド監視
      byte cmd = getLaserCommand();
      if (cmd == CMD_START) {
        // 次のブロック境界を計算
        nextBlockBar = ((globalBar - 1) / BLOCK_SIZE_BARS + 1) * BLOCK_SIZE_BARS + 1;
        currentState = STATE_PLAY_WAIT;
        Serial.print(">>> PLAY_WAIT: will start at bar=");
        Serial.println(nextBlockBar);
      } else if (cmd == CMD_VOL_UP) {
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
    } break;

    // ---- 演奏開始待機（次ブロック境界まで） ----
    case STATE_PLAY_WAIT: {
      // 拍カウント継続
      // while化: currentStateがSTATE_PLAYに変わったら自動的に抜ける
      while (currentState == STATE_PLAY_WAIT && millis() - lastBeatTime >= beatInterval) {
        lastBeatTime += beatInterval;
        Serial.println("B");
        advanceGlobalCount();

        // ブロック境界チェック：次のブロック先頭拍に到達したか
        if (globalBar == nextBlockBar && globalBeat == 1) {
          // 演奏開始！
          currentState = STATE_PLAY;
          noteIndex    = 0;
          beatInNote   = 0;
          sentInit     = false;
          // 即座に最初の音符を送信するために1拍分先行
          lastBeatTime -= beatInterval;
          // Processing 側のbar/beatをリセット
          Serial.println("RESET");
          Serial.print(">>> PLAY START at bar=");
          Serial.println(globalBar);
        }
      }

      // 音量コマンド監視（STATE_PLAYに遷移していたら次ループでそちらのcaseへ）
      byte cmd = (currentState == STATE_PLAY_WAIT) ? getLaserCommand() : 0;
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
    } break;

    // ---- 演奏中 ----
    case STATE_PLAY: {

      // BPM・音量を最初に1回送信
      if (!sentInit) {
        Serial.print("T,"); Serial.println((int)currentBPM);
        Serial.print("V,"); Serial.println(volumeLevel);
        sentInit = true;
      }

      // 拍ティック
      // while化: 楽譜終端でSTATE_STANDBYに変わったら自動的に抜ける
      while (currentState == STATE_PLAY && millis() - lastBeatTime >= beatInterval) {
        lastBeatTime += beatInterval;

        if (noteIndex < melodyLength) {
          // 音符先頭拍：音符番号とデュレーションを送信
          if (beatInNote == 0) {
            int note     = melody[noteIndex][0];
            int noteType = melody[noteIndex][1];
            int duration = (int)((4.0 / noteType) * (float)beatInterval);
            Serial.print(note);
            Serial.print(",");
            Serial.println(duration);
          }

          // 毎拍「B」を送信
          Serial.println("B");
          advanceGlobalCount();  // グローバルカウントも進める

          // 拍を進める
          beatInNote++;
          int beatsInNote = 4 / melody[noteIndex][1];
          if (beatInNote >= beatsInNote) {
            beatInNote = 0;
            noteIndex++;
          }

        } else {
          // 楽譜終端 → 演奏終了・停止
          Serial.println("Melody done. STOP.");
          currentState = STATE_STANDBY;
        }
      }

      // 音量コマンド監視（演奏終了でSTATE_STANDBYに遷移していたらスキップ）
      byte cmd = (currentState == STATE_PLAY) ? getLaserCommand() : 0;
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

    } break;
  }
}

// ============================================================
// グローバルカウントを1拍進める
// ============================================================
void advanceGlobalCount() {
  globalBeat++;
  if (globalBeat > 4) {
    globalBeat = 1;
    globalBar++;
  }
}

// ============================================================
// Processing からのシリアルコマンド受信
//   'R' → BPM/音量を再送（起動タイミングズレの補正）
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
// UDP経由でBPMを受信してbeatIntervalを更新
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
      currentBPM   = (float)bpmValue;
      beatInterval = (unsigned long)(60000.0 / currentBPM);

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
// STATE_PLAY 中は 15° に静止、それ以外は 0° に戻す
// ============================================================
void updateServo() {
  if (currentState == STATE_PLAY) {
    myServo.write(ANGLE_PLAY);
  } else {
    myServo.write(ANGLE_REST);
  }
}

// ============================================================
// レーザコマンド検出（非ブロッキング）
//
//   updateLaserRx() : loop()から毎回呼ぶ。状態機械を1ステップ進めるだけ
//                      で、delay()もbusy-waitも一切行わない。
//   getLaserCommand(): 1コマンド分の受信が完了していればそれを返し、
//                      フラグをクリアする。未完了なら0を返す。
//
//   送信側(conductor)のタイミング:
//     ヘッダ 300ms点灯 → 50ms消灯 → [ビット(パルス50ms+消灯50ms)]×3
//   これに合わせて、ヘッダLOW検知(t=0)を基準に
//     ビットi(0=MSB)のパルス窓 = [50+100*i, 100+100*i] ms
//   の範囲でサンプリングし多数決判定する。
// ============================================================
int readLaserState() {
  return (analogRead(sensorPin) > threshold) ? 1 : 0;
}

void updateLaserRx() {
  int level = readLaserState();
  unsigned long now = millis();

  switch (lzState) {

    case LZ_IDLE:
      if (level == 1) {
        lzHeaderStart = now;
        lzState = LZ_HEADER_ACTIVE;
      }
      break;

    case LZ_HEADER_ACTIVE:
      if (level == 0) {
        unsigned long dur = now - lzHeaderStart;
        if (dur >= 250 && dur <= 350) {
          // 有効なヘッダ → ビット受信フェーズへ
          lzFrameStart = now;   // 以後この時刻を基準(t=0)にビット窓を判定
          lzCommand    = 0;
          lzBitIdx     = 0;
          lzVotesHigh  = 0;
          lzVotesTotal = 0;
          lzState = LZ_BIT_ACTIVE;
        } else {
          // 短すぎ/長すぎ → ノイズとして破棄
          lzState = LZ_IDLE;
        }
      } else if (now - lzHeaderStart > 400) {
        // 張り付き異常（1秒待たずに早期に見切る）
        lzState = LZ_IDLE;
      }
      break;

    case LZ_BIT_ACTIVE: {
      unsigned long elapsed    = now - lzFrameStart;
      unsigned long windowStart = LZ_GAP_MS + (unsigned long)lzBitIdx * LZ_BIT_SLOT_MS;
      unsigned long sampleFrom  = windowStart + LZ_SAMPLE_FROM;
      unsigned long sampleTo    = windowStart + LZ_SAMPLE_TO;
      unsigned long slotEnd     = windowStart + LZ_BIT_SLOT_MS;

      if (elapsed >= sampleFrom && elapsed <= sampleTo) {
        lzVotesHigh += level;
        lzVotesTotal++;
      }

      if (elapsed >= slotEnd) {
        int bitValue = (lzVotesTotal > 0 && lzVotesHigh * 2 >= lzVotesTotal) ? 1 : 0;
        int shift = 2 - lzBitIdx;   // bitIdx 0→MSB(shift2) ... 2→LSB(shift0)
        lzCommand |= (bitValue << shift);

        lzBitIdx++;
        lzVotesHigh  = 0;
        lzVotesTotal = 0;

        if (lzBitIdx >= 3) {
          lzPendingCommand = lzCommand;
          lzCommandReady    = true;
          lzState = LZ_IDLE;
        }
      }
      break;
    }
  }
}

byte getLaserCommand() {
  if (!lzCommandReady) return 0;
  lzCommandReady = false;
  return lzPendingCommand;
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