#include "driver/i2s.h"
#include "SD.h"
#include "SPI.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <base64.h>
#include <Stepper.h>

const char *ssid = "WiFi Name";
const char *password = "WiFi password";   // 用自己的熱點或用隊輔的
const char *apiKey = "API_KEY";           // 要換成自己的
const char *server = "generativelanguage.googleapis.com";
const int port = 443;

#define IN1 15
#define IN2 13
#define IN3 12
#define IN4 14
#define stepsPerRevolution 2048

Stepper myStepper(stepsPerRevolution, IN1, IN3, IN2, IN4);

int position[32] = { 0, -64, -128, -192, -256, -320, -384, -448, -512, -576, -640,
                     -704, -768, -832, -896, -960, -1024, -1088, -1152, -1216, -1280, -1344, -1408,
                     -1472, -1536, -1600, -1664, -1728, -1792, -1856, -1920, -1984 };
int cur_pos = position[0]; // 記得改定位

void move_to(int tar) {
  tar--;
  int diff = position[tar] - cur_pos;
  if (diff > 0)
    diff = diff - 2048;
  myStepper.step(diff);
  cur_pos = position[tar];
}

WiFiClientSecure client;

#define I2S_WS 25   // I2S Word Select (LRCLK)
#define I2S_SCK 33  // I2S Clock (BCLK)
#define I2S_SD 32   // I2S Data Input

#define SD_CS 5  // SD card chip select pin
#define BUTTON_PIN 4

#define SAMPLE_RATE 44100
#define BITS_PER_SAMPLE 16
#define BUFFER_SIZE 64

#define IR_PIN 27

File audioFile;
bool recording = false;
int dataSize = 0;
const char *audioPath = "/recording.wav";
const int chunkSize = 3000;

long fileSize = 0;

int getTranscribeInline();
String readResponse();
String parseJson(String response);
void writeWAVHeader(File file, int sampleRate, int bitsPerSample, int numChannels, int dataSize);
void startRecording();
void stopRecording();

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  if (!SD.begin(SD_CS)) {
    Serial.println("SD Card initialization failed!");
    return;
  }

  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = BUFFER_SIZE,
    .use_apll = false
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_SD
  };

  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pin_config);

  WiFi.begin(ssid, password);

  Serial.print("Connecting to WiFi...");
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.print(".");
  }
  Serial.println("\nConnected!");

  client.setInsecure();

  myStepper.setSpeed(5);

  pinMode(IR_PIN, INPUT);
  while (digitalRead(IR_PIN) == HIGH) {
    myStepper.step(-1);
    delay(10);
  }
  myStepper.step(0);
}

int pre = HIGH;
int now = HIGH;

void loop() {
  pre = now;
  now = digitalRead(BUTTON_PIN);
  if (now == LOW && pre == HIGH && !recording) {
    startRecording();
  } else if (now == HIGH && pre == LOW && recording) {
    stopRecording();
    delay(1000);
    int res = getTranscribeInline();
    if (res != -1)
      move_to(res);
  }

  if (recording) {
    int16_t sampleBuffer[BUFFER_SIZE];
    size_t bytesRead;
    i2s_read(I2S_NUM_0, sampleBuffer, BUFFER_SIZE * sizeof(int16_t), &bytesRead, portMAX_DELAY);
    audioFile.write((byte *)sampleBuffer, bytesRead);
    dataSize += bytesRead;
  }
}

int getTranscribeInline() {
  if (!client.connect(server, port)) {
    Serial.println("Connection failed!");
    return -1;
  }
  File file = SD.open(audioPath, "r");
  if (!file) {
    Serial.println("Failed to open file for chunked upload!");
    return -1;
  }
  fileSize = file.size();
  long bytesUploaded = 0;
  uint8_t buffer[chunkSize];

  String prompt = R"rawliteral(
你現在是一本 **「答案之書」**，你的回答方式會完全採用下列提供的 **句子清單** 中的語句。

你的任務如下：

1. **讀取音檔，並理解我說的話或提問的內容。**
2. **根據對話內容，從以下提供的句子中選擇最符合情境的一句。**
3. **直接回傳所選語句對應的編號，不需要回覆其他文字或解釋。**

---

**以下是你可以選擇的句子：**

1. 加入交大創客俱樂部
2. 這時就該吃點東西
3. 這很顯然吧
4. 去問你媽
5. 聽不懂。
6. 是又怎樣
7. 你會怕？
8. 我很感動
9. 益友，可以有
10. 迷霧之中會有路徑顯現
11. 看似無盡黑暗才是真正轉機
12. 此路不通！
13. 欲速則不達
14. 燒肉
15. 怎麼想怎麼賺
16. AI不是萬能的
17. 錢包會哭喔
18. 你說得對
19. 不如原神
20. 好啊哪次不好
21. 要不你再想一下？
22. 這根本情緒勒索
23. 孩子，去讀書
24. 重點 要活著
25. 寶，去睡覺
26. 食之無味 棄之可惜
27. 因為春日影是一首好歌
28. 對健康不好喔
29. 拜拜 下次見
30. 早中晚 安
31. 這根本算犯罪
32. 看看過程的風景

---

**舉例說明：**
如果我說「我肚子好餓」，你應該選擇回覆「2」——這時就該吃點東西。
如果我說「我今天失敗了」，你可以選擇「11」——看似無盡黑暗才是真正轉機。

有時候你也可以選擇最具幽默感或反諷效果的句子作為回應，但要確保回應與語意有合理關聯。
只需回傳數字（句子的編號），不要附加任何其他文字。

---

請依照此格式開始執行。
  )rawliteral";
  String jsonLeft = "{ \"contents\": [ { \"parts\": [ { \"text\": \"" + prompt + "\" }, { \"inline_data\": { \"mime_type\": \"audio/wav\", \"data\": \"";
  String jsonRight = "\" } } ] } ] }";
  String url = "/v1beta/models/gemini-2.0-flash:generateContent?key=" + String(apiKey);
  Serial.print("Requesting URL: ");
  Serial.println(url);

  client.println("POST " + url + " HTTP/1.1");
  client.println("Host: " + String(server));
  client.println("Content-Type: application/json");
  client.print("Content-Length: ");
  client.println((fileSize + 2) / 3 * 4 + jsonLeft.length() + jsonRight.length());
  client.println("Connection: close");
  client.println();

  client.print(jsonLeft);
  while (bytesUploaded < fileSize) {
    size_t bytesToRead = min((long)chunkSize, fileSize - bytesUploaded);
    String rawFileContent = "";
    for (int i = 0; i < bytesToRead; i++) {
      rawFileContent += char(file.read());
    }
    String encodedData = base64::encode(rawFileContent);
    if (bytesToRead == 0) {
      Serial.println("Failed to read chunk from file!");
      file.close();
      return -1;
    }

    client.print(encodedData);
    bytesUploaded += bytesToRead;
  }
  client.println(jsonRight);

  String response = readResponse();
  Serial.println(response);
  int chosen_num = parseJson(response).toInt();
  client.stop();
  return chosen_num;
}

String readResponse() {
  String response = "";
  long startTime = millis();

  while (millis() - startTime < 10000) {  // timeout after 10s
    while (client.available()) {
      char c = client.read();
      response += c;
    }
    if (response.length() > 0)
      break;
  }

  return response;
}

String parseJson(String response) {
  int jsonStart = response.indexOf("{");
  if (jsonStart == -1) {
    Serial.println("Invalid JSON response.");
    return "";
  }

  String jsonData = response.substring(jsonStart);

  StaticJsonDocument<1024> doc;
  DeserializationError error = deserializeJson(doc, jsonData);

  if (error) {
    Serial.print("JSON Parsing failed: ");
    Serial.println(error.f_str());
    return "";
  }

  JsonArray candidates = doc["candidates"];
  for (JsonObject candidate : candidates) {
    JsonArray parts = candidate["content"]["parts"];
    for (JsonObject part : parts) {
      const char *text = part["text"];
      if (text) {
        Serial.println("Extracted Text: ");
        Serial.println(text);
        return text;
      }
    }
  }
  return "";
}

void writeWAVHeader(File file, int sampleRate, int bitsPerSample, int numChannels, int dataSize) {
  file.seek(0);
  file.write((byte *)&"RIFF", 4);
  uint32_t fileSize = dataSize + 36;
  file.write((byte *)&fileSize, 4);
  file.write((byte *)&"WAVEfmt ", 8);
  uint32_t subChunk1Size = 16;
  uint16_t audioFormat = 1;
  file.write((byte *)&subChunk1Size, 4);
  file.write((byte *)&audioFormat, 2);
  file.write((byte *)&numChannels, 2);
  file.write((byte *)&sampleRate, 4);
  uint32_t byteRate = sampleRate * numChannels * bitsPerSample / 8;
  file.write((byte *)&byteRate, 4);
  uint16_t blockAlign = numChannels * bitsPerSample / 8;
  file.write((byte *)&blockAlign, 2);
  file.write((byte *)&bitsPerSample, 2);
  file.write((byte *)&"data", 4);
  file.write((byte *)&dataSize, 4);
}

void startRecording() {
  audioFile = SD.open("/recording.wav", FILE_WRITE);
  if (!audioFile) {
    Serial.println("Failed to create file!");
    return;
  }
  writeWAVHeader(audioFile, SAMPLE_RATE, BITS_PER_SAMPLE, 1, 0);
  Serial.println("Recording started...");
  recording = true;
  dataSize = 0;
}

void stopRecording() {
  if (recording) {
    Serial.println("Recording finished!");
    audioFile.seek(4);
    fileSize = dataSize + 36;
    audioFile.write((byte *)&fileSize, 4);
    audioFile.seek(40);
    audioFile.write((byte *)&dataSize, 4);
    audioFile.close();
    Serial.println("File saved!");
    recording = false;
  }
}
