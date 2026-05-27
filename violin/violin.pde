import ddf.minim.*;
import ddf.minim.ugens.*;
import processing.serial.*;

Minim minim;
AudioOutput out;
Serial myPort;

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

float[] vlnAmps = {0.31, 0.14, 0.04, 0.1, 0.06, 0.06, 0.21, 0.07};

Oscil[] waves = new Oscil[frequencies.length];

int currentNote = -1;
int currentDuration = 0;
int noteStartTime = 0;
boolean notePlaying = false;

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
  return new Oscil(freq, 0.5f, table);
}

void setup() {
  size(400, 200);
  minim = new Minim(this);
  out = minim.getLineOut();

  for (int i = 0; i < frequencies.length; i++) {
    waves[i] = generateWave(frequencies[i]);
  }

  printArray(Serial.list());
  myPort = new Serial(this, Serial.list()[1], 9600);
  myPort.bufferUntil('\n');
}

void draw() {
  background(30);
  fill(255);
  textAlign(CENTER, CENTER);
  textSize(20);

  if (notePlaying) {
    if (millis() - noteStartTime >= currentDuration) {
      waves[currentNote].unpatch(out);
      notePlaying = false;
    }
    text("Note: " + currentNote + "  (" + frequencies[currentNote] + " Hz)", width/2, height/2);
  } else {
    text("waiting...", width/2, height/2);
  }
}

void serialEvent(Serial port) {
  String data = port.readStringUntil('\n');
  if (data == null) return;
  data = trim(data);

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
