import ddf.minim.*;
import ddf.minim.ugens.*;
import processing.serial.*;

// =====================================================================
//  コントラバス・プレイヤー (チェロ完成形 + コントラバス音色)
//
//  ベース構造はチェロ版「完成形」のまま:
//    Oscil(ウェーブテーブル) → ビブラートLFO → BowEnv(大中大) → Reverb → out
//    シリアル受信で演奏 / 指揮者UI(BPM・Bar・拍) / 音量バー
//
//  音色を決める部分だけコントラバス版に差し替え:
//    ・BRIGHTNESS          2000 → 2800  (輪郭をくっきり)
//    ・倍音数              9000/f → 7000/f, 上限90→120 (低音楽器なので多め)
//    ・bodyResonance()     チェロ胴 → コントラバス胴 (超低域に共鳴が集中)
//    ・ビブラート          5.2Hz/1.3% → 4.3Hz/0.5% (遅く控えめ)
//    ・弓の食いつき(bowGrip) 0.18 → 0.22  (ゴリッとした弓の存在感)
//    ・Reverb              feedback/damp を 0.84/0.30 に (暗め長め)
// =====================================================================

Minim minim;
AudioOutput out;
Serial myPort;

// ベースパターン8音（Arduino の melody[][0] のインデックスと1:1対応）
//  0=レ(D4) 1=ラ(A3) 2=シ(B3) 3=ファ#(F#3) 4=ソ(G3) 5=レ_(D3) 6=ソ(G3) 7=ラ(A3)
//  ※ 0,5 は元の音程のまま。1〜4, 6〜7 を 1 オクターブ下げ
float[] frequencies = {
  293.66, 220.00, 246.94, 185.00, 196.00, 146.83, 196.00, 220.00
};

float volume = 0.8;

// ===== コントラバス音色パラメータ ====================================
float BRIGHTNESS  = 2800;       // 高域ロールオフ[Hz]。コントラバスは輪郭くっきり
float pitchShift  = 1.0;       // frequencies[] に正しい音高を入れたので変換不要
// =====================================================================

// ビブラート (LFOでサンプル単位に揺らす / コントラバスは遅く控えめ)
float vibratoRate  = 4.3;       // 揺れの速さ [Hz]
float vibratoDepth = 0.005;     // ピッチ変動の割合 (±0.5%)

// 音量カーブ (1音の中の膨らみ = 中→大)
float fadeIn     = 0.06;        // 立ち上がり (大きな胴・重い弓でやや緩め)
float fadeOut    = 0.15;        // 終わり    (余韻長め)
float swellDepth = 0.20;        // 中→大の振り幅

// 弓の食いつき (弾いている感 / コントラバスは存在感が強い)
float attackBite = 2.2;         // 立ち上がりの鋭さ
float bowGrip    = 0.22;        // 弾き始めの擦過ノイズ量 (ゴリッと)
float gripMs     = 0;          // 擦過ノイズが出る時間 [ms]

// 発音の粒立ち (弓を返す感じ)
float gateRatio  = 0.9;         // 音符の長さに対する発音割合
float minGapMs   = 40;          // 音と音の最小すき間 [ms]

float noise = 0.000;

// --- 音声グラフ ------------------------------------------------------
Wavetable[] tables = new Wavetable[frequencies.length];
Oscil    osc;          // 現在発音中のオシレータ
Oscil    vibratoLFO;   // ビブラート用LFO
Constant vibBase;      // ビブラートの中心周波数
Summer   freqSum;      // 中心周波数 + LFO を合算して osc.frequency へ
BowEnv   bowEnv;       // 1音の音量カーブ(大中大)
Reverb   reverb;       // リアルタイム残響

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

  for (int i = 0; i < frequencies.length; i++) {
    tables[i] = makeContrabassTable(frequencies[i] * pitchShift);
  }

  // --- 音声グラフを組む ---
  vibBase    = new Constant(frequencies[0] * pitchShift);
  vibratoLFO = new Oscil(vibratoRate, frequencies[0] * pitchShift * vibratoDepth, Waves.SINE);
  freqSum    = new Summer();
  bowEnv     = new BowEnv();
  reverb     = new Reverb();

  vibBase.patch(freqSum);
  vibratoLFO.patch(freqSum);          // freqSum = 中心周波数 + 揺れ
  bowEnv.patch(reverb).patch(out);    // 出力側は常時接続

  printArray(Serial.list());
  myPort = new Serial(this, Serial.list()[7], 9600);
  myPort.bufferUntil('\n');
}

void draw() {
  background(255);
  conductor_drawData(beatS, bar, beat);
  volumeBar(volume * 100);
  // 音作り(包絡・ビブラート)はすべて音声スレッド任せ。draw()は描画のみ。
}

// =====================================================================
//  ノート発音: 音声レートで滑らかに鳴らす
// =====================================================================
void playNote(int note, int duration) {
  if (note < 0 || note >= frequencies.length) return;

  float freq = frequencies[note] * pitchShift;

  // 直前の音を外して差し替える
  if (osc != null) {
    freqSum.unpatch(osc);
    osc.unpatch(bowEnv);
  }

  osc = new Oscil(freq, 0.8, tables[note]);
  vibBase.setConstant(freq);                      // 中心周波数
  vibratoLFO.setAmplitude(freq * vibratoDepth);   // 揺れ幅も音高に比例
  freqSum.patch(osc.frequency);                   // osc.frequency = 中心 + LFO
  osc.patch(bowEnv);                              // osc → 音量カーブ

  bowEnv.noteOn(duration / 1000.0, volume);       // 音符の長さで大中大を描く

  currentNote = note;
}

// =====================================================================
//  BowEnv: 1音の音量カーブを描く UGen
//   音符の長さにわたって「中 → 大」へ膨らませる(弓を伸ばして歌い上げる)。
//   両端には短いフェードを入れてプチノイズを防ぐ。
// =====================================================================
class BowEnv extends UGen {
  UGenInput audio;
  float sr = 44100;
  boolean active = false;
  int pos = 0, durSamp = 1, atkSamp = 1, relSamp = 1, gripSamp = 1;
  float vol = 0.8, swell = 0.20;
  float noiseLP = 0;     // 擦過ノイズ平滑化用

  BowEnv() {
    audio = new UGenInput(InputType.AUDIO);
  }

  void sampleRateChanged() {
    sr = sampleRate();
  }

  void noteOn(float durSec, float volume) {
    vol     = volume;
    swell   = swellDepth;

    // 実際に鳴らす時間を音符の長さより少し短くして、後ろに弓離れの隙間を作る
    float gap     = max(minGapMs / 1000.0, durSec * (1.0 - gateRatio));
    float soundSec = max(fadeIn + fadeOut, durSec - gap);

    durSamp = max(1, (int)(soundSec * sr));
    atkSamp = max(1, (int)(fadeIn  * sr));
    relSamp = max(1, (int)(fadeOut * sr));
    gripSamp = max(1, (int)(gripMs / 1000.0 * sr));
    pos     = 0;
    active  = true;
  }

  void uGenerate(float[] channels) {
    float g = 0;
    if (active) {
      float x = constrain((float) pos / (float) durSamp, 0, 1);

      // 中→大カーブ: x=0で(1-2*swell) → x=1で1.0 へ向かって膨らむ
      float shape = (1.0 - 2.0 * swell) + 2.0 * swell * x;

      // 両端のフェード(プチノイズ防止)。立ち上がりは角を付けて弓の食いつきを出す
      float fade = 1;
      if (pos < atkSamp) {
        float a = (float) pos / atkSamp;
        fade = pow(a, 1.0 / attackBite);   // 速く立ち上げて「キュッ」とした出音に
      } else if (pos > durSamp - relSamp) {
        fade = constrain((float)(durSamp - pos) / relSamp, 0, 1);
      }

      g = vol * shape * fade;
      pos++;
      if (pos >= durSamp) active = false;
    }

    float in = audio.getLastValues()[0];

    // 弾き始めの弓の擦過ノイズ (食いつき感)。平滑化して耳障りを抑える
    if (active && pos < gripSamp) {
      noiseLP = noiseLP * 0.85 + random(-1, 1) * 0.15;
      float gripEnv = 1.0 - (float) pos / gripSamp;   // 出だしで強く→すぐ消える
      in += noiseLP * bowGrip * gripEnv;
    }

    in *= g;
    for (int i = 0; i < channels.length; i++) channels[i] = in;
  }
}

// =====================================================================
//  makeContrabassTable: コントラバス音色の単一周期ウェーブテーブルを作る
//   各倍音の振幅 = (1/h) * ボディ共鳴(h*freq) * 高域ロールオフ
//   低音楽器なので倍音を多めに取る (7000/freq, 20〜120本)。
// =====================================================================
Wavetable makeContrabassTable(float freq) {
  int N = 1024;
  float[] wave = new float[N];

  int numH = constrain((int)(7000.0 / freq), 20, 120);
  float[] amp = new float[numH + 1];
  for (int h = 1; h <= numH; h++) {
    float hf = freq * h;
    float tilt = 1.0 / (1.0 + (hf / BRIGHTNESS));
    amp[h] = (1.0 / h) * bodyResonance(hf) * tilt;
  }

  for (int i = 0; i < N; i++) {
    float ph = TWO_PI * ((float) i / N);
    float sum = 0;
    for (int h = 1; h <= numH; h++) sum += amp[h] * sin(h * ph);
    sum += random(-noise, noise);
    wave[i] = sum;
  }

  float maxVal = 0;
  for (float v : wave) if (abs(v) > maxVal) maxVal = abs(v);
  if (maxVal > 0) for (int i = 0; i < N; i++) wave[i] /= maxVal;

  return new Wavetable(wave);
}

// =====================================================================
//  ボディ共鳴 (フォルマント): コントラバス胴体の共鳴特性
//   最大級の胴のため共鳴が超低域に集まる。60〜250Hzの厚みが
//   重く太い響きを、600〜1200Hzの山が弓の存在感を生む。
// =====================================================================
float bodyResonance(float f) {
  float g = 0.22;
  g += resonancePeak(f,   60,  30, 0.8);   // 空気共鳴 (やや抑えてぼやけ防止)
  g += resonancePeak(f,  100,  40, 1.2);   // 主木部共鳴 (重さの芯)
  g += resonancePeak(f,  160,  55, 1.0);
  g += resonancePeak(f,  250, 100, 0.8);   // 低中音域の太さ
  g += resonancePeak(f,  600, 250, 0.9);   // 弓の輪郭
  g += resonancePeak(f, 1200, 450, 0.8);   // 張り・力強さ
  g += resonancePeak(f, 2200, 700, 0.5);   // 倍音の明瞭さ
  return g;
}

float resonancePeak(float f, float center, float bw, float gain) {
  float x = (f - center) / bw;
  return gain / (1 + x * x);
}

// =====================================================================
//  Reverb: リアルタイム残響 UGen (Schroeder/Freeverb型)
//   コントラバスは余韻が長いので feedback/damp を暗め長めに。
//   ※ 残響を切りたいときは wet = 0
// =====================================================================
class Reverb extends UGen {
  UGenInput audio;

  float[] combMs = {29.7, 37.1, 41.1, 43.7};
  float feedback = 0.84;        // チェロ 0.83 → コントラバス 0.84 (長め)
  float damp     = 0.30;        // チェロ 0.28 → コントラバス 0.30 (暗め)
  float[][] cbuf;
  int[] cpos, clen;
  float[] cstore;

  float[] apMs = {5.0, 1.7};
  float apFb = 0.5;
  float[][] abuf;
  int[] apos, alen;

  float dry = 0.78, wet = 0.32, inGain = 0.4;

  Reverb() {
    audio = new UGenInput(InputType.AUDIO);
  }

  void sampleRateChanged() {
    float sr = sampleRate();
    int nc = combMs.length;
    cbuf = new float[nc][];  cpos = new int[nc];  clen = new int[nc];  cstore = new float[nc];
    for (int c = 0; c < nc; c++) {
      clen[c] = max(1, (int)(combMs[c] * 0.001 * sr));
      cbuf[c] = new float[clen[c]];
    }
    int na = apMs.length;
    abuf = new float[na][];  apos = new int[na];  alen = new int[na];
    for (int a = 0; a < na; a++) {
      alen[a] = max(1, (int)(apMs[a] * 0.001 * sr));
      abuf[a] = new float[alen[a]];
    }
  }

  void uGenerate(float[] channels) {
    float input = audio.getLastValues()[0];
    if (cbuf == null) {
      for (int i = 0; i < channels.length; i++) channels[i] = input;
      return;
    }
    float fed = input * inGain;

    float mono = 0;
    for (int c = 0; c < combMs.length; c++) {
      int p = cpos[c];
      float y = cbuf[c][p];
      cstore[c] = y * (1 - damp) + cstore[c] * damp;
      cbuf[c][p] = fed + cstore[c] * feedback;
      cpos[c] = (p + 1) % clen[c];
      mono += y;
    }
    for (int a = 0; a < apMs.length; a++) {
      int p = apos[a];
      float bufout = abuf[a][p];
      float y = -mono + bufout;
      abuf[a][p] = mono + bufout * apFb;
      apos[a] = (p + 1) % alen[a];
      mono = y;
    }

    float outSample = dry * input + wet * mono;
    for (int i = 0; i < channels.length; i++) channels[i] = outSample;
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
