int BPM = 120;
int quarterNote;
int volumeLevel = 10; // 0〜100で指定

int melody[][2] = {
  {8,  2},  {6,  2},  {5,  2},  {4,  2},
  {3,  2},  {2,  2},  {3,  2},  {4,  2},
  {5,  2},  {4,  2},  {3,  2},  {2,  2},
  {1,  2},  {0,  2},  {1,  2},  {2,  2},
  {5,  4},  {8,  4},  {10, 4},  {9,  4},
  {8,  4},  {5,  4},  {8,  4},  {6,  4},
  {5,  4},  {3,  4},  {5,  4},  {10, 4},
  {9,  4},  {11, 4},  {10, 4},  {9,  4},
  {8,  4},  {5,  4},  {6,  4},  {12, 4},
  {13, 4},  {15, 4},  {17, 4},  {10, 4},
  {11, 4},  {9,  4},  {10, 4},  {8,  4},
  {5,  4},  {13, 4},  {13, 4},  {12, 4},
  {13, 4},  {5,  4},  {4,  4},  {12, 4},
  {11, 4},  {3,  4},  {2,  4},  {10, 4},
  {9,  4},  {16, 4},  {15, 4},  {8,  4},
  {6,  4},  {11, 4},  {6,  4},  {14, 4},
  {15, 1},
};

void setup() {
  Serial.begin(9600);

  Serial.print("T,");
  Serial.println(BPM);
  Serial.print("V,");
  Serial.println(volumeLevel);
  
  quarterNote = 60000 / BPM;
  quarterNote = 60000 / BPM;
  int melodyLength = sizeof(melody) / sizeof(melody[0]);

  for (int i = 0; i < melodyLength; i++) {
    int note     = melody[i][0];
    int noteType = melody[i][1];
    int duration = (4.0 / noteType) * quarterNote;

    // 音符の長さを拍数に変換
    int beats = 4 / noteType;

    for (int b = 0; b < beats; b++) {
      if (b == 0) {
        // 最初の拍で音符を送信
        Serial.print(note);
        Serial.print(",");
        Serial.println(duration);
      }
      // 毎拍Bを送信
      Serial.println("B");
      delay(quarterNote);
    }
  }
}

void loop() {
}
