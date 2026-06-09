const int laserPins[] = {A0, A1, A2, A3};

const int startButtonPin = 2;
const int instPins[] = {3, 4, 5, 6};
const int volUpPin = 7;
const int volDownPin = 8;
const int bpmUpPin = 9;
const int bpmDownPin = 10;
const int resetPin = 11; // ★ リセットボタン用のピン（D11）を追加

int targetLaser = -1;

// [FIX] BPM管理変数を追加
int bpm = 120;
const int BPM_MIN  = 40;
const int BPM_MAX  = 240;
const int BPM_STEP = 5;

void setup() {
  Serial.begin(9600);

  for (int i = 0; i < 4; i++) {
    pinMode(laserPins[i], OUTPUT);
    digitalWrite(laserPins[i], LOW);
  }

  pinMode(startButtonPin, INPUT_PULLUP);
  for (int i = 0; i < 4; i++) pinMode(instPins[i], INPUT_PULLUP);
  pinMode(volUpPin, INPUT_PULLUP);
  pinMode(volDownPin, INPUT_PULLUP);
  pinMode(bpmUpPin, INPUT_PULLUP);
  pinMode(bpmDownPin, INPUT_PULLUP);
  pinMode(resetPin, INPUT_PULLUP); // ★ リセットピンの入力設定を追加

  // [FIX] 起動時に初期BPMをPCへ送信
  broadcastTempoData();
}

void loop() {
  if (digitalRead(startButtonPin) == LOW) {
    sendLaserCommand(0b001, -1);
    delay(500);
  }

  for (int i = 0; i < 4; i++) {
    if (digitalRead(instPins[i]) == LOW) {
      targetLaser = i;
      delay(200);
    }
  }

  if (digitalRead(volUpPin) == LOW && targetLaser != -1) {
    sendLaserCommand(0b010, targetLaser);
    delay(500);
  }
  if (digitalRead(volDownPin) == LOW && targetLaser != -1) {
    sendLaserCommand(0b011, targetLaser);
    delay(500);
  }

  // [FIX] BPMアップ/ダウンを実装
  if (digitalRead(bpmUpPin) == LOW) {
    bpm = min(bpm + BPM_STEP, BPM_MAX);
    broadcastTempoData();
    delay(500);
  }
  if (digitalRead(bpmDownPin) == LOW) {
    bpm = max(bpm - BPM_STEP, BPM_MIN);
    broadcastTempoData();
    delay(500);
  }

  // ★ リセットボタンの処理を追加
  if (digitalRead(resetPin) == LOW) {
    sendLaserCommand(0b111, -1); // 全てのレーザ(-1)に対して 0b111(リセット) を送信
    delay(500);
  }
}

// [FIX] BPMをシリアル経由でPCに送信する関数
void broadcastTempoData() {
  Serial.print("BPM:");
  Serial.println(bpm);
}

void sendLaserCommand(byte command, int target) {
  // 開始合図：300ms連続点灯 → 50ms消灯
  setLaserState(target, HIGH);
  delay(300);
  setLaserState(target, LOW);
  delay(50);

  // 3ビットをMSBから順に送信（1=点灯50ms, 0=消灯50ms）
  for (int bit = 2; bit >= 0; bit--) {
    int bitValue = (command >> bit) & 0x01;

    if (bitValue == 1) {
      setLaserState(target, HIGH);
    } else {
      setLaserState(target, LOW);
    }
    delay(50);

    // ビット間の消灯インターバル
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
