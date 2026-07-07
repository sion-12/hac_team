import processing.serial.*;

Serial port;

int bar = 0;
int beat = 0;
int bpm = 120;
int instrument = -1;

String[] instrumentNames = {
  "バイオリン",
  "コントラバス",
  "チェロ",
  "フルート"
};

void setup() {
  size(800, 600);

  printArray(Serial.list());

  port = new Serial(this, Serial.list()[7], 115200);
  port.bufferUntil('\n');
  
  PFont font = createFont("MS Gothic", 32);
  // フォント適用
  textFont(font);
  
  conductor_drawData(bpm, bar, beat, instrument);
}

void draw(){
  conductor_drawData(bpm, bar, beat, instrument);
}

void serialEvent(Serial p) {

  String line = p.readStringUntil('\n');
  if (line == null) return;
  line = trim(line);

  if (line.startsWith("BAR:")) {
    bar = int(line.substring(4));
  }
  else if (line.startsWith("BEAT:")) {
    beat = int(line.substring(5));
  }
  else if (line.startsWith("Broadcasted BPM:")) {
    bpm = int(line.substring(16));
  }
  else if (line.startsWith("Target laser:")) {
    instrument = int(trim(line.substring(13)));
  }
  
  
  
}




void conductor_drawData( int bpm , int bar , float beat , int instrument){
  
  
  background(255);
  
  //BPM表示
  fill(0);                // 文字の色
  textSize(height * 0.17 );           // 文字のサイズ
  textAlign(CENTER);
  text( "BPM:" + bpm, width / 2, height *1/4); // 内容, x座標, y座標
  
  //小節表示
  fill(0);                // 文字の色
  textSize(height * 0.15);           // 文字のサイズ
  textAlign(CENTER);
  text( "Bar:" + bar, width / 2, height *5/13); // 内容, x座標, y座標
  
  for(int i = 1; i<5; i++ ){
  
    if((int)beat == i){
      fill(0);
    }else{
      fill(255);
    }
    
    stroke(0);          
    circle(width* (i)/5, height *1/2, height * 0.1); 
  }
  
  fill(0);                // 文字の色
  textSize(height * 0.08 );           // 文字のサイズ
  textAlign(CENTER);
  text( "選択中の楽器", width / 2, height *5/7); // 内容, x座標, y座標
  
  if(instrument<0){
    
    fill(0);                // 文字の色
    textSize(height * 0.13 );           // 文字のサイズ
    textAlign(CENTER);
    text( "未選択", width / 2, height *6/7); // 内容, x座標, y座標
  }else{
    
  
    fill(0);                // 文字の色
    textSize(height * 0.13 );           // 文字のサイズ
    textAlign(CENTER);
    text( instrumentNames[instrument], width / 2, height *6/7); // 内容, x座標, y座標
  }
  
}
