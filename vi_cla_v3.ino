const int sensorPin = A0;

// 手動設定の閾値
int threshold = 200;

#define PLOTTER_MODE

// デバッグ出力のON/OFF切り替え
// ※注意：プロッタ表示中は自動的に文字出力をストップさせてグラフの乱れを防ぎます．
#define DEBUG_MODE

const byte CMD_START    = 0b001;
const byte CMD_VOL_UP   = 0b010;
const byte CMD_VOL_DOWN = 0b011;
const byte CMD_RESET    = 0b111;

enum SystemState {
  STATE_STANDBY,
  STATE_PREPARE,
  STATE_PLAY
};
SystemState currentState = STATE_STANDBY;

unsigned long beatInterval = 500;
unsigned long lastBeatTime = 0;
int beatCount = 0;

void setup() {
  Serial.begin(9600);
  pinMode(sensorPin, INPUT);

  currentState = STATE_STANDBY;
}

void loop() {

  #ifdef PLOTTER_MODE
    int currentVal = analogRead(sensorPin);
    Serial.print(currentVal);  // 青線のデータ
    Serial.print(",");
    Serial.println(threshold); // オレンジ線のデータ
    delay(20);                 // 描画を滑らかにするためのウェイト
    return;                    // プロッタ表示中は，これ以降の通信処理をスキップします
  #endif



  switch (currentState) {

    case STATE_STANDBY:
      {
        byte cmd = checkLaserCommand();
        if (cmd == CMD_START) {
          currentState = STATE_PREPARE;
          beatCount = 0;
          lastBeatTime = millis();
        }
      }
      break;

    case STATE_PREPARE:
      if (millis() - lastBeatTime >= beatInterval) {
        lastBeatTime += beatInterval;
        beatCount++;

        if (beatCount >= 16) {
          currentState = STATE_PLAY;
          sendToMac(CMD_START);
        }
      }
      break;

    case STATE_PLAY:
      {
        byte cmd = checkLaserCommand();
        if (cmd == CMD_VOL_UP || cmd == CMD_VOL_DOWN) {
          sendToMac(cmd);
        } else if (cmd == CMD_RESET) {
          currentState = STATE_STANDBY;
        }
      }
      break;
  }
}

byte checkLaserCommand() {
  if (readLaserState() == 1) {
    unsigned long startTime = millis();

    while (readLaserState() == 1) {
      if (millis() - startTime > 1000) return 0;  // タイムアウト
    }
    unsigned long duration = millis() - startTime;

    #if defined(DEBUG_MODE) && !defined(PLOTTER_MODE)
    Serial.print("Start pulse duration: ");
    Serial.print(duration);
    Serial.println("ms");
    #endif

    // 300ms開始合図の検出（±50ms許容）
    if (duration >= 250 && duration <= 350) {
      return parseBitStream();
    }
  }
  return 0;
}

int readLaserState() {
  int val = analogRead(sensorPin);
  return (val > threshold) ? 1 : 0;
}

// 各ビットを3回サンプリングして多数決で判定（ノイズ耐性向上）
byte parseBitStream() {
  byte command = 0;

  // 送信側：消灯50ms → ビット開始
  // 受信側：75ms待機でビット点灯期間（50〜100ms）の中心をサンプリング
  delay(75);

  for (int i = 2; i >= 0; i--) {
    int votes = 0;
    votes += readLaserState();
    delay(5);
    votes += readLaserState(); // タイポを修正しました
    delay(5);
    votes += readLaserState();

    int bitValue = (votes >= 2) ? 1 : 0;
    command |= (bitValue << i);

    #if defined(DEBUG_MODE) && !defined(PLOTTER_MODE)
    Serial.print("Bit ");
    Serial.print(i);
    Serial.print(": ");
    Serial.print(bitValue);
    Serial.print(" (votes=");
    Serial.print(votes);
    Serial.println(")");
    #endif

    delay(90);
  }

  #if defined(DEBUG_MODE) && !defined(PLOTTER_MODE)
  Serial.print("Parsed command: 0b");
  Serial.println(command, BIN);
  #endif

  return command;
}

void sendToMac(byte command) {
  Serial.write(command);
}