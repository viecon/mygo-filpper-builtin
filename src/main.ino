#include "Arduino.h"
#include "driver/i2s.h"
#include "SD.h"
#include "SPI.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <base64.h>

const char* ssid = "SSID";
const char* password = "PASSWORD";
const char* apiKey = "API_KEY";
// Google Gemini API endpoint
const char* server = "generativelanguage.googleapis.com";
const int port = 443;

WiFiClientSecure client;

#define I2S_WS 25   // I2S Word Select (LRCLK)
#define I2S_SCK 33  // I2S Clock (BCLK)
#define I2S_SD 32   // I2S Data Input

#define SD_CS 5  // SD card chip select pin
#define BUTTON_PIN 4

#define SAMPLE_RATE 44100
#define BITS_PER_SAMPLE 16
#define BUFFER_SIZE 64

File audioFile;
bool recording = false;
int dataSize = 0;
const char* audioPath = "/recording.wav";  // Modify if using SPIFFS or LittleFS
const int chunkSize = 3000;

long fileSize = 0;

void getTranscribeInline() {
  if (!client.connect(server, port)) {
    Serial.println("Connection failed!");
    return;
  }
  File file = SD.open(audioPath, "r");
  if (!file) {
    Serial.println("Failed to open file for chunked upload!");
    return;
  }
  fileSize = file.size();
  long bytesUploaded = 0;
  uint8_t buffer[chunkSize];
  String jsonLeft = "{ \"contents\": [ { \"parts\": [ { \"text\": \"Please convert the following speech to text and output it directly. If there is noise, you can ignore it. If it is all noise or unintelligible, please reply with &$%$hu#did\" }, { \"inline_data\": { \"mime_type\": \"audio/wav\", \"data\": \"";
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

  //Serial.print(jsonLeft);
  while (bytesUploaded < fileSize) {
    size_t bytesToRead = min((long)chunkSize, fileSize - bytesUploaded);
    String rawFileContent = "";
    for (int i = 0; i < bytesToRead; i++) {
      rawFileContent += char(file.read());
    }
    // size_t bytesRead = file.read(buffer, bytesToRead);
    String encodedData = base64::encode(rawFileContent);
    if (bytesToRead == 0) {
      Serial.println("Failed to read chunk from file!");
      file.close();
      return;
    }

    client.print(encodedData);
    // Serial.print(encodedData);
    bytesUploaded += bytesToRead;
    //Serial.printf("Uploaded %ld / %ld bytes\n", bytesUploaded, fileSize);
  }
  client.println(jsonRight);
  //Serial.println(jsonRight);
  String response = readResponse();
  Serial.println(response);
  parseJson(response);
  client.stop();
}

String readResponse() {
  String response = "";
  long startTime = millis();

  while (millis() - startTime < 10000) {  // Timeout after 5s
    while (client.available()) {
      char c = client.read();
      response += c;

      if (response.length() > 4096) {  // Prevent excessive memory use
        Serial.println("Response too large! Truncating...");
        return response;
      }
    }
    if (response.length() > 0) break;
  }

  return response;
}

String parseJson(String response) {
  // Find the start of the JSON body
  int jsonStart = response.indexOf("{");
  if (jsonStart == -1) {
    Serial.println("Invalid JSON response.");
    return "";
  }

  // Extract JSON content
  String jsonData = response.substring(jsonStart);

  // Parse JSON
  StaticJsonDocument<1024> doc;
  DeserializationError error = deserializeJson(doc, jsonData);

  if (error) {
    Serial.print("JSON Parsing failed: ");
    Serial.println(error.f_str());
    return "";
  }

  // Extract text fields
  JsonArray candidates = doc["candidates"];
  for (JsonObject candidate : candidates) {
    JsonArray parts = candidate["content"]["parts"];
    for (JsonObject part : parts) {
      const char* text = part["text"];
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
  file.write((byte*)&"RIFF", 4);
  uint32_t fileSize = dataSize + 36;
  file.write((byte*)&fileSize, 4);
  file.write((byte*)&"WAVEfmt ", 8);
  uint32_t subChunk1Size = 16;
  uint16_t audioFormat = 1;
  file.write((byte*)&subChunk1Size, 4);
  file.write((byte*)&audioFormat, 2);
  file.write((byte*)&numChannels, 2);
  file.write((byte*)&sampleRate, 4);
  uint32_t byteRate = sampleRate * numChannels * bitsPerSample / 8;
  file.write((byte*)&byteRate, 4);
  uint16_t blockAlign = numChannels * bitsPerSample / 8;
  file.write((byte*)&blockAlign, 2);
  file.write((byte*)&bitsPerSample, 2);
  file.write((byte*)&"data", 4);
  file.write((byte*)&dataSize, 4);
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
    audioFile.write((byte*)&fileSize, 4);
    audioFile.seek(40);
    audioFile.write((byte*)&dataSize, 4);
    audioFile.close();
    Serial.println("File saved!");
    recording = false;
  }
}

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

  // Set SSL/TLS certificate (skip if insecure)
  client.setInsecure();
}

int pre = LOW;
int now = LOW;

void loop() {
  pre = now;
  now = digitalRead(BUTTON_PIN);
  if (now == HIGH && pre == LOW && !recording) {
    startRecording();
  } else if (now == LOW && pre == HIGH && recording) {
    stopRecording();
    delay(1000);
    getTranscribeInline();
  }

  if (recording) {
    int16_t sampleBuffer[BUFFER_SIZE];
    size_t bytesRead;
    i2s_read(I2S_NUM_0, sampleBuffer, BUFFER_SIZE * sizeof(int16_t), &bytesRead, portMAX_DELAY);
    audioFile.write((byte*)sampleBuffer, bytesRead);
    dataSize += bytesRead;
  }
}
