import ddf.minim.*;
import ddf.minim.ugens.*;
import processing.serial.*;

Minim minim;
AudioOutput out;
Serial myPort;

// 各音階の周波数（インデックス0〜17に対応）
float[] frequencies = {
  185.00,  // 0  ファ#_
  196.00,  // 1  ソ_
  220.00,  // 2  ラ_
  246.94,  // 3  シ_
  277.18,  // 4  レ♭
  293.66,  // 5  レ
  329.63,  // 6  ミ
  349.23,  // 7  ファ
  369.99,  // 8  ファ#
  392.00,  // 9  ソ
  440.00,  // 10 ラ
  493.88,  // 11 シ
  554.37,  // 12 レ♭~
  587.33,  // 13 レ~
  659.25,  // 14 ミ~
  739.99,  // 15 ファ#~
  783.99,  // 16 ソ~
  880.00   // 17 ラ~
};

// バイオリン音色の倍音構成（各倍音の振幅）
float[] vlnAmps = {0.31, 0.14, 0.04, 0.1, 0.06, 0.06, 0.21, 0.07};

// 音量（0.0〜1.0）
float volume = 0.8;

// ビブラートの速さ（Hz）と深さ（セント）
float vibratoRate  = 5.5;
float vibratoDepth = 25.0;

// 各音階に対応するオシレーターの配列
Oscil[] waves = new Oscil[frequencies.length];

// 現在再生中の音符の情報
int currentNote     = -1;   // 現在の音階インデックス
int currentDuration = 0;    // 現在の音符の長さ（ミリ秒）
int noteStartTime   = 0;    // 音符の再生開始時刻（ミリ秒）
boolean notePlaying = false; // 音符が再生中かどうか

// 倍音合成でバイオリン音色のオシレーターを生成する関数
Oscil generateWave(float freq) {
  float[] wave = new float[1024];

  // 各サンプル点で倍音を足し合わせる
  for (int i = 0; i < 1024; i++) {
    float sum = 0;
    for (int n = 1; n <= vlnAmps.length; n++) {
      sum += vlnAmps[n-1] * sin(TWO_PI * n * ((float)i / 1024));
    }
    wave[i] = sum;
  }

  // 波形を-1.0〜1.0に正規化する
  float maxVal = 0;
  for (float v : wave) if (abs(v) > maxVal) maxVal = abs(v);
  if (maxVal > 0) {
    for (int i = 0; i < wave.length; i++) wave[i] /= maxVal;
  }

  // 波形テーブルからオシレーターを生成して返す
  Wavetable table = new Wavetable(wave);
  return new Oscil(freq, volume, table);
}

// オシレーターにビブラートをかける関数
void applyVibrato(Oscil oscil, float freq, float rate, float depth) {
  // ビブラートの深さをセントからHzに変換
  float depthHz = freq * (pow(2, depth / 1200.0) - 1);

  // LFO（低周波オシレーター）を生成
  Oscil lfo = new Oscil(rate, depthHz, Waves.SINE);

  // 基準周波数にLFOを足してオシレーターの周波数に接続
  Constant baseFreq = new Constant(freq);
  Summer summer = new Summer();
  baseFreq.patch(summer);
  lfo.patch(summer);
  summer.patch(oscil.frequency);
}

void setup() {
  size(400, 200);
  minim = new Minim(this);
  out = minim.getLineOut();

  // 全音階分のオシレーターを生成してビブラートをかける
  for (int i = 0; i < frequencies.length; i++) {
    waves[i] = generateWave(frequencies[i]);
    applyVibrato(waves[i], frequencies[i], vibratoRate, vibratoDepth);
  }

  // 使用可能なシリアルポートを表示して接続
  printArray(Serial.list());
  myPort = new Serial(this, Serial.list()[1], 9600);
  myPort.bufferUntil('\n'); // 改行まで受信バッファに貯める
}

void draw() {
  background(30);
  fill(255);
  textAlign(CENTER, CENTER);
  textSize(20);

  if (notePlaying) {
    // 音符の長さを超えたら音を止める
    if (millis() - noteStartTime >= currentDuration) {
      waves[currentNote].unpatch(out);
      notePlaying = false;
    }
    // 現在鳴っている音階と周波数を表示
    text("Note: " + currentNote + "  (" + frequencies[currentNote] + " Hz)", width/2, height/2);
  } else {
    text("waiting...", width/2, height/2);
  }
}

// シリアルデータを受信したときに呼ばれる関数
void serialEvent(Serial port) {
  String data = port.readStringUntil('\n');
  if (data == null) return;
  data = trim(data);

  // "音階インデックス,音符の長さ" の形式でパース
  String[] parts = split(data, ',');
  if (parts.length != 2) return;

  int note     = int(parts[0]);
  int duration = int(parts[1]);

  // インデックスが範囲外なら無視
  if (note < 0 || note >= frequencies.length) return;

  // 前の音を止める
  if (notePlaying) {
    waves[currentNote].unpatch(out);
  }

  // 新しい音を鳴らす
  currentNote     = note;
  currentDuration = duration;
  noteStartTime   = millis();
  notePlaying     = true;
  waves[currentNote].patch(out);
}
