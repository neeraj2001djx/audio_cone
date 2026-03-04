#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <SD.h>
#include "driver/i2s.h"

/************************************************************
   ESP32 Audio + WiFi Setup + MQTT
   DEVICE_ID: cone_left
************************************************************/

#define DEVICE_ID "audio_cone_001"

String topicCommand = "devices/" + String(DEVICE_ID) + "/command";
String topicVolume  = "devices/" + String(DEVICE_ID) + "/volume";
String topicState   = "devices/" + String(DEVICE_ID) + "/state";
String topicStatus  = "devices/" + String(DEVICE_ID) + "/status";

unsigned long lastWifiAttempt = 0;
const unsigned long WIFI_RETRY_INTERVAL = 10000; // 10 seconds

bool wifiConnecting = false;
unsigned long wifiStartAttempt = 0;
const unsigned long WIFI_CONNECT_TIMEOUT = 15000;

File audioFile;
bool audioOpen = false;


unsigned long resetWindowStart = 0;
int resetPressCount = 0;

const int RESET_PRESS_TARGET = 20;
const unsigned long RESET_WINDOW = 20000; // 20 seconds

//////////////////////////////////////////////////////////////
// AP CONFIG
//////////////////////////////////////////////////////////////

const char* ap_ssid     = "ESP32_SETUP";
const char* ap_password = "12345678";

WebServer server(80);
DNSServer dnsServer;
const byte DNS_PORT = 53;

Preferences preferences;

//////////////////////////////////////////////////////////////
// MQTT CONFIG
//////////////////////////////////////////////////////////////

String mqtt_server = "192.168.31.164";
int mqtt_port      = 1883;

WiFiClient wifiClient;
PubSubClient client(wifiClient);

//////////////////////////////////////////////////////////////
// PINS
//////////////////////////////////////////////////////////////

#define SD_CS   5
#define SD_SCK  18
#define SD_MISO 19
#define SD_MOSI 23

#define I2S_BCLK 26
#define I2S_LRC  25
#define I2S_DOUT 22

#define BUTTON_PIN 4

//////////////////////////////////////////////////////////////

uint8_t audioBuffer[16384];
float volumeGain = 1.0;

String pendingFile     = "";
bool downloadRequested = false;
bool downloading       = false;
bool playing           = false;
bool wifiConnected     = false;
bool lastButtonState   = HIGH;

//////////////////////////////////////////////////////////////
// I2S
//////////////////////////////////////////////////////////////

void setupI2S() {

  i2s_config_t config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = 44100,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = 0,
    .dma_buf_count = 8,
    .dma_buf_len = 256,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };

  i2s_pin_config_t pins = {
    .bck_io_num   = I2S_BCLK,
    .ws_io_num    = I2S_LRC,
    .data_out_num = I2S_DOUT,
    .data_in_num  = I2S_PIN_NO_CHANGE
  };

  i2s_driver_install(I2S_NUM_0, &config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pins);
}

//////////////////////////////////////////////////////////////
// VOLUME
//////////////////////////////////////////////////////////////

void applyVolume(uint8_t* buffer, size_t length) {

  int16_t* samples = (int16_t*)buffer;
  int count = length / 2;

  for (int i = 0; i < count; i++) {

    int32_t scaled = samples[i] * volumeGain;

    if (scaled > 32767) scaled = 32767;
    if (scaled < -32768) scaled = -32768;

    samples[i] = scaled;
  }
}

//////////////////////////////////////////////////////////////
// SETTINGS
//////////////////////////////////////////////////////////////

void loadSettings() {

  preferences.begin("config", true);

  mqtt_server = preferences.getString("mqtt_server", "192.168.31.164");
  mqtt_port   = preferences.getInt("mqtt_port", 1883);

  preferences.end();
}

void saveSettings(String ssid, String pass,
                  String mqttServer, String mqttPort) {

  preferences.begin("config", false);

  preferences.putString("ssid", ssid);
  preferences.putString("pass", pass);
  preferences.putString("mqtt_server", mqttServer);
  preferences.putInt("mqtt_port", mqttPort.toInt());

  preferences.end();
}

void saveVolume() {

  File f = SD.open("/volume.txt", FILE_WRITE);

  if (!f) {
    Serial.println("Failed to save volume");
    return;
  }

  f.seek(0);
  f.print(volumeGain);
  f.close();

  Serial.println("Volume saved to SD");
}

void loadVolumeFromSD() {

  if (!SD.exists("/volume.txt")) {

    Serial.println("No volume file, using default");
    volumeGain = 1.0;
    return;
  }

  File f = SD.open("/volume.txt");

  if (!f) {
    Serial.println("Failed to read volume file");
    volumeGain = 1.0;
    return;
  }

  String v = f.readString();
  volumeGain = v.toFloat();

  f.close();

  Serial.print("Volume loaded from SD: ");
  Serial.println(volumeGain);
}

//////////////////////////////////////////////////////////////
// WIFI
//////////////////////////////////////////////////////////////

bool startWiFiConnection() {

  preferences.begin("config", true);

  String ssid = preferences.getString("ssid", "");
  String pass = preferences.getString("pass", "");

  preferences.end();

  if (ssid == "") return false;

  Serial.println("Starting WiFi connection...");

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());

  wifiConnecting = true;
  wifiStartAttempt = millis();

  return true;
}

//////////////////////////////////////////////////////////////
// AP MODE
//////////////////////////////////////////////////////////////

void startAPMode() {

  WiFi.disconnect(true);
  delay(500);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(ap_ssid, ap_password);

  IPAddress IP = WiFi.softAPIP();

  dnsServer.start(DNS_PORT, "*", IP);

  server.on("/", HTTP_GET, []() {

    server.send(200, "text/html",
      "<h2>ESP32 Setup</h2>"
      "<form action='/save'>"
      "SSID:<br><input name='ssid'><br>"
      "Password:<br><input name='pass'><br>"
      "MQTT:<br><input name='mqtt'><br>"
      "Port:<br><input name='port'><br>"
      "<input type='submit'>"
      "</form>");
  });

  server.on("/save", HTTP_GET, []() {

    saveSettings(
      server.arg("ssid"),
      server.arg("pass"),
      server.arg("mqtt"),
      server.arg("port")
    );

    server.send(200, "text/html", "Saved. Rebooting...");
    delay(2000);
    ESP.restart();
  });

  server.onNotFound([]() {
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
  });

  server.begin();
}

//////////////////////////////////////////////////////////////
// MQTT
//////////////////////////////////////////////////////////////

void callback(char* topic, byte* payload, unsigned int length) {

  String message;

  for (int i = 0; i < length; i++)
    message += (char)payload[i];

  if (String(topic).endsWith("/volume")) {

    volumeGain = message.toFloat();
    saveVolume();
    return;
  }

  pendingFile = message;
  downloadRequested = true;
}

void connectMQTT() {

  if (client.connected()) return;

  client.setServer(mqtt_server.c_str(), mqtt_port);

  if (client.connect(
        DEVICE_ID,
        topicStatus.c_str(),
        1,
        true,
        "offline")) {

    client.subscribe(topicCommand.c_str());
    client.subscribe(topicVolume.c_str());

    client.publish(topicStatus.c_str(), "online", true);
    client.publish(topicState.c_str(), "idle", true);

    client.publish(topicVolume.c_str(),
                   String(volumeGain).c_str(),
                   true);
  }
}

//////////////////////////////////////////////////////////////
// DOWNLOAD AUDIO
//////////////////////////////////////////////////////////////

void downloadAudio(String filename) {

  downloading = true;

  client.publish(topicState.c_str(), "downloading", true);

  WiFiClient httpClient;
  HTTPClient http;

  String url = "http://" + mqtt_server + ":5000/audio/" + filename;

  http.begin(httpClient, url);

  int code = http.GET();

  if (code != 200) {

    downloading = false;
    http.end();
    return;
  }

  if (SD.exists("/audio.wav"))
    SD.remove("/audio.wav");

  File file = SD.open("/audio.wav", FILE_WRITE);

  WiFiClient* stream = http.getStreamPtr();

  while (http.connected() || stream->available()) {

    size_t available = stream->available();

    if (available) {

      int bytes = stream->readBytes(
        audioBuffer,
        min(available, sizeof(audioBuffer))
      );

      if (bytes > 0)
        file.write(audioBuffer, bytes);
    }

    delay(1);
  }

  file.close();
  http.end();

  downloading = false;

  client.publish(topicState.c_str(), "idle", true);
}

//////////////////////////////////////////////////////////////
// PLAY AUDIO
//////////////////////////////////////////////////////////////

void playAudio() {

  if (!SD.exists("/audio.wav")) return;

  audioFile = SD.open("/audio.wav");

  if (!audioFile) return;

  audioFile.seek(44);

  audioOpen = true;
  playing = true;

  if (wifiConnected)
    client.publish(topicState.c_str(), "playing", true);
}


void factoryReset() {

  Serial.println("FACTORY RESET TRIGGERED");

  preferences.begin("config", false);
  preferences.clear();   // erase WiFi + MQTT settings
  preferences.end();

  delay(1000);

  ESP.restart();
}


//////////////////////////////////////////////////////////////
// SETUP
//////////////////////////////////////////////////////////////

void setup() {

  Serial.begin(115200);

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  WiFi.setSleep(false);

  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

  if (!SD.begin(SD_CS)) {

    Serial.println("SD Failed");
    while (true);
  }

  loadVolumeFromSD();
  loadSettings();
  setupI2S();

  wifiConnected = startWiFiConnection();

  if (!wifiConnected)
    startAPMode();

  client.setCallback(callback);
}

//////////////////////////////////////////////////////////////
// LOOP
//////////////////////////////////////////////////////////////

void loop() {
  if (resetPressCount > 0 &&
      millis() - resetWindowStart > RESET_WINDOW) {

    resetPressCount = 0;
  }
  ////////////////////////////////////////////////////////////
  // WIFI CONNECTION PROGRESS
  ////////////////////////////////////////////////////////////

  if (wifiConnecting) {

    if (WiFi.status() == WL_CONNECTED) {

      wifiConnected = true;
      wifiConnecting = false;

      Serial.println("WiFi Connected!");
      Serial.println(WiFi.localIP());
    }

    if (millis() - wifiStartAttempt > WIFI_CONNECT_TIMEOUT) {

      Serial.println("WiFi connection timeout");

      wifiConnecting = false;
      wifiConnected = false;

      startAPMode();
    }

    // BUTTON HAS PRIORITY → cancel connection attempt
    if (playing) {

      Serial.println("Audio priority: cancelling WiFi connect");

      WiFi.disconnect(true);
      wifiConnecting = false;
      wifiConnected = false;
    }
  }

  ////////////////////////////////////////////////////////////
  // WIFI RECONNECT LOGIC
  ////////////////////////////////////////////////////////////

  if (!wifiConnected && !wifiConnecting && !playing && !downloading) {

    if (millis() - lastWifiAttempt > WIFI_RETRY_INTERVAL) {

      lastWifiAttempt = millis();

      Serial.println("Retrying WiFi connection...");

      if (!startWiFiConnection()) {
        Serial.println("No WiFi credentials saved.");
      }
    }
  }

  ////////////////////////////////////////////////////////////
  // AP MODE HANDLING
  ////////////////////////////////////////////////////////////

  if (!wifiConnected) {

    dnsServer.processNextRequest();
    server.handleClient();
  }

  ////////////////////////////////////////////////////////////
  // WIFI CONNECTED
  ////////////////////////////////////////////////////////////

  if (wifiConnected) {

    if (WiFi.status() != WL_CONNECTED) {

      Serial.println("WiFi lost.");

      wifiConnected = false;

      startAPMode();

    } else {

      connectMQTT();
      client.loop();
    }
  }

  ////////////////////////////////////////////////////////////
  // BUTTON
  ////////////////////////////////////////////////////////////

  bool currentButtonState = digitalRead(BUTTON_PIN);

  if (lastButtonState == HIGH && currentButtonState == LOW) {

    unsigned long now = millis();

    if (resetPressCount == 0) {
      resetWindowStart = now;
    }

    resetPressCount++;

    Serial.print("Reset press count: ");
    Serial.println(resetPressCount);

    if (resetPressCount >= RESET_PRESS_TARGET &&
        now - resetWindowStart <= RESET_WINDOW) {

      factoryReset();
    }

    if (now - resetWindowStart > RESET_WINDOW) {

      resetPressCount = 1;
      resetWindowStart = now;
    }

    if (!playing && !downloading) {
      playAudio();
    }
  }

  if (lastButtonState == LOW && currentButtonState == HIGH) {

    if (playing) {

      playing = false;

      if (audioOpen) {

        audioFile.close();
        audioOpen = false;

        i2s_zero_dma_buffer(I2S_NUM_0);
      }

      if (wifiConnected)
        client.publish(topicState.c_str(), "idle", true);
    }
  }

  lastButtonState = currentButtonState;

  ////////////////////////////////////////////////////////////
  // AUDIO ENGINE
  ////////////////////////////////////////////////////////////

  if (playing && audioOpen) {

    size_t bytesRead = audioFile.read(audioBuffer, sizeof(audioBuffer));

    if (bytesRead <= 0) {

      playing = false;

      audioFile.close();
      audioOpen = false;

      i2s_zero_dma_buffer(I2S_NUM_0);

      if (wifiConnected)
        client.publish(topicState.c_str(), "idle", true);
    }
    else {

      applyVolume(audioBuffer, bytesRead);

      size_t written;

      i2s_write(
        I2S_NUM_0,
        audioBuffer,
        bytesRead,
        &written,
        portMAX_DELAY
      );
    }
  }

  ////////////////////////////////////////////////////////////

  if (downloadRequested && wifiConnected) {

    downloadRequested = false;

    downloadAudio(pendingFile);
  }
}