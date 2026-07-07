import ddf.minim.*;
import ddf.minim.ugens.*;
import processing.serial.*;

Minim minim;
AudioOutput out;
Serial myPort;

float[] frequencies = {
  185.00, 196.00, 220.00, 246.94, 277.18, 293.66,
  329.63, 349.23, 369.99, 392.00, 440.00, 493.88,
  554.37, 587.33, 659.25, 739.99, 783.99, 880.00
};

float volume = 0.8;

// ===== フルート音色パラメータ (最初に送ったコードと同一) =================
float pitchShift = 1.00;   // 音高倍率。インデックスはそのまま。
                           //   1.00 = そのまま / 2.00 = 1オクターブ上げ

// 倍音構成 (基音が支配的 → 急減衰)
float[] FLUTE_HARM = {1.0, 0.28, 0.11, 0.05, 0.025, 0.012};

// ビブラート / トレモロ
float vibRate   = 5.5;     // 揺れの速さ [Hz]
float vibDepth  = 0.004;   // ピッチ変動の割合
float tremDepth = 0.05;    // 音量変動(トレモロ)の割合

// ADSR エンベロープ
float atk = 0.06;          // アタック
float dec = 0.10;          // ディケイ
float sus = 0.85;          // サステイン
float rel = 0.1;          // リリース
// =====================================================================

// --- 音声グラフ ------------------------------------------------------
//  FluteVoice(フルート音色を直接生成) → out  ※リバーブなし
FluteVoice fluteVoice;

int currentNote = -1;

float bpm = 100;
float beatS;
int   bar = 1;
float beat = -1;

void setup() {
  size(600, 400);
  minim = new Minim(this);
  out = minim.getLineOut();

  beatS = 60.0 / bpm;

  fluteVoice = new FluteVoice();
  fluteVoice.patch(out);

  printArray(Serial.list());
  myPort = new Serial(this, Serial.list()[1], 9600);
  myPort.bufferUntil('\n');
}

void draw() {
  background(255);
  conductor_drawData(beatS, bar, beat);
  volumeBar(volume * 100);
  // 音作りはすべて音声スレッド任せ。draw()は描画のみ。
}

// =====================================================================
//  ノート発音: 単音モノフォニック。新しい音が前の音を即座に差し替える。
// =====================================================================
void playNote(int note, int duration) {
  if (note < 0 || note >= frequencies.length) return;

  float freq = frequencies[note] * pitchShift;
  fluteVoice.noteOn(freq, duration / 1000.0, volume);

  currentNote = note;
}

// =====================================================================
//  FluteVoice: 最初のフルートコード generateFluteWave() をそのまま
//  リアルタイム生成する UGen。
//   ・倍音合成 (純粋ハーモニクス。ノイズなし)
//   ・ビブラート(ピッチ)を0.15秒後からフェードイン
//   ・トレモロ(音量)も同じフェードで連動
//   ・ADSR エンベロープ
// =====================================================================
class FluteVoice extends UGen {
  float sr = 44100;
  boolean active = false;

  float freq = 440;
  float amp  = 0.7;
  float durationSec = 1.0;

  float phase = 0;
  float vibPhase = 0;
  float t = 0;            // 経過時間 [s]

  float hsum;

  FluteVoice() {
    hsum = 0;
    for (float a : FLUTE_HARM) hsum += a;
  }

  void sampleRateChanged() {
    sr = sampleRate();
  }

  void noteOn(float f, float durSec, float volume) {
    freq        = f;
    amp         = volume;
    durationSec = max(atk + dec + rel, durSec);   // 短すぎる音でも破綻させない
    phase       = 0;
    vibPhase    = 0;
    t           = 0;
    active      = true;
  }

  void uGenerate(float[] channels) {
    float outv = 0;

    if (active) {
      // ビブラートは0.15秒後から0.3秒かけて立ち上げる
      float vibAmt = constrain((t - 0.15) / 0.3, 0, 1);
      vibPhase += TWO_PI * vibRate / sr;
      if (vibPhase > TWO_PI) vibPhase -= TWO_PI;
      float lfo = sin(vibPhase) * vibAmt;

      // ピッチビブラート
      float instFreq = freq * (1 + lfo * vibDepth);
      phase += TWO_PI * instFreq / sr;
      if (phase > TWO_PI) phase -= TWO_PI;

      // 倍音合成
      float h = 0;
      for (int k = 0; k < FLUTE_HARM.length; k++) h += FLUTE_HARM[k] * sin(phase * (k + 1));
      h /= hsum;

      // トレモロ + ADSR
      float trem = 1 + tremDepth * lfo;
      float env  = adsr(t, durationSec, atk, dec, sus, rel);

      outv = constrain(amp * env * trem * h, -1, 1);

      t += 1.0 / sr;
      if (t >= durationSec) active = false;
    }

    for (int i = 0; i < channels.length; i++) channels[i] = outv;
  }

  float adsr(float t, float dur, float atk, float dec, float sus, float rel) {
    float relStart = max(dur - rel, atk + dec);
    if (t < atk)       return t / atk;
    if (t < atk + dec) return 1 - (1 - sus) * (t - atk) / dec;
    if (t < relStart)  return sus;
    return sus * max(0, 1 - (t - relStart) / rel);
  }
}

void conductor_drawData(float beatS, int bar, float beat) {
  int bpm = (int)(60 / beatS);
  fill(0);
  textSize(height * 0.15);
  textAlign(CENTER);
  text("BPM:" + bpm, width / 2, height * 1/5);

  fill(0);
  textSize(height * 0.12);
  textAlign(CENTER);
  text("Bar:" + bar, width / 2, height * 2/5);

  for (int i = 0; i < 4; i++) {
    if ((int)beat == i) fill(0);
    else fill(255);
    stroke(0);
    circle(width * (i+1)/5, height * 3/5, height * 0.1);
  }
}

void volumeBar(float volumeRate) {
  float barX = width * 0.1;
  float barY = height * 0.82;
  float barW = width * 0.8;
  float barH = height * 0.06;

  fill(200);
  noStroke();
  rect(barX, barY, barW, barH, 5);

  float ratio = constrain(volumeRate / 100.0, 0, 1);
  if (ratio > 0.7) fill(220, 50, 50);
  else if (ratio > 0.4) fill(50, 180, 50);
  else fill(50, 130, 220);
  rect(barX, barY, barW * ratio, barH, 5);

  fill(0);
  textSize(height * 0.05);
  textAlign(CENTER);
  text((int)volumeRate + "%", width / 2, height * 0.97);
}

void serialEvent(Serial port) {
  String data = port.readStringUntil('\n');
  if (data == null) return;
  data = trim(data);

  if (data.startsWith("T,")) {
    bpm = float(split(data, ',')[1]);
    beatS = 60.0 / bpm;
    return;
  }

  if (data.startsWith("V,")) {
    float v = float(split(data, ',')[1]);
    volume = constrain(v / 100.0, 0.0, 1.0);
    return;
  }

  if (data.equals("RESET")) {
    bar = 1;
    beat = -1;
    return;
  }

  if (data.equals("END")) {
    bar = 0;
    beat = -1;
    return;
  }

  if (data.equals("B")) {
    beat++;
    if (beat >= 4) {
      beat = 0;
      bar++;
    }
    return;
  }

  String[] parts = split(data, ',');
  if (parts.length != 2) return;

  int note     = int(parts[0]);
  int duration = int(parts[1]);
  playNote(note, duration);
}
