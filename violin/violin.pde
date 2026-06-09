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

float[] vlnAmps = {0.31, 0.14, 0.04, 0.1, 0.06, 0.06, 0.21, 0.07};

float volume = 0.8;
float vibratoRate  = 5.5;
float vibratoDepth = 25.0;

Oscil[] waves = new Oscil[frequencies.length];

int currentNote     = -1;
int currentDuration = 0;
int noteStartTime   = 0;
boolean notePlaying = false;

float bpm = 100;
float beatS;
int bar = 1;
float beat = -1;

Oscil generateWave(float freq) {
  float[] wave = new float[1024];
  for (int i = 0; i < 1024; i++) {
    float sum = 0;
    for (int n = 1; n <= vlnAmps.length; n++) {
      sum += vlnAmps[n-1] * sin(TWO_PI * n * ((float)i / 1024));
    }
    wave[i] = sum;
  }
  float maxVal = 0;
  for (float v : wave) if (abs(v) > maxVal) maxVal = abs(v);
  if (maxVal > 0) {
    for (int i = 0; i < wave.length; i++) wave[i] /= maxVal;
  }
  Wavetable table = new Wavetable(wave);
  return new Oscil(freq, volume, table);
}

void applyVibrato(Oscil oscil, float freq, float rate, float depth) {
  float depthHz = freq * (pow(2, depth / 1200.0) - 1);
  Oscil lfo = new Oscil(rate, depthHz, Waves.SINE);
  Constant baseFreq = new Constant(freq);
  Summer summer = new Summer();
  baseFreq.patch(summer);
  lfo.patch(summer);
  summer.patch(oscil.frequency);
}

void setup() {
  size(600, 400);
  minim = new Minim(this);
  out = minim.getLineOut();

  beatS = 60.0 / bpm;

  for (int i = 0; i < frequencies.length; i++) {
    waves[i] = generateWave(frequencies[i]);
    applyVibrato(waves[i], frequencies[i], vibratoRate, vibratoDepth);
  }

  printArray(Serial.list());
  myPort = new Serial(this, Serial.list()[1], 9600);
  myPort.bufferUntil('\n');
}

void draw() {
  background(255);
  conductor_drawData(beatS, bar, beat);
  volumeBar(volume * 100);

  if (notePlaying && millis() - noteStartTime >= currentDuration) {
    waves[currentNote].unpatch(out);
    notePlaying = false;
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
    if ((int)beat == i) {
      fill(0);
    } else {
      fill(255);
    }
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
    for (int i = 0; i < waves.length; i++) {
      waves[i].setAmplitude(volume);
    }
    return;
  }
  // 拍信号
  if (data.equals("B")) {
    beat++;
    if (beat >= 4) {
      beat = 0;
      bar++;
    }
    return;
  }

  // 音符データ
  String[] parts = split(data, ',');
  if (parts.length != 2) return;

  int note     = int(parts[0]);
  int duration = int(parts[1]);

  if (note < 0 || note >= frequencies.length) return;

  if (notePlaying) {
    waves[currentNote].unpatch(out);
  }

  currentNote     = note;
  currentDuration = duration;
  noteStartTime   = millis();
  notePlaying     = true;
  waves[currentNote].patch(out);
}
