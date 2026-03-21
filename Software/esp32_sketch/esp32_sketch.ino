// Stable Version V0.8
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <SPI.h>
#include <SD.h>
#include <PubSubClient.h>
#include <HTTPClient.h>

#include <AudioFileSourceSD.h>
#include <AudioGeneratorWAV.h>
#include <AudioOutputI2S.h>

#define BUTTON_PIN 4
#define SD_CS 5

#define I2S_BCLK 26
#define I2S_LRC 25
#define I2S_DOUT 22

#define LED_RED 32
#define LED_YELLOW 33
#define LED_GREEN 27

Preferences prefs;
WebServer server(80);
DNSServer dns;

WiFiClient espClient;
PubSubClient mqttClient(espClient);

String device_id="Audio_Cone_001";

String ssid;
String pass;
String server_ip;
int server_port;

bool wifiConfigured=false;
bool apModeRunning=false;
bool audioPlaying=false;
bool downloadingAudio=false;

bool lastButtonState=HIGH;
unsigned long debounceTime=0;

unsigned long lastWifiAttempt=0;
bool wifiConnecting=false;

unsigned long lastMQTTAttempt=0;

bool sdMounted=false;

unsigned long yellowBlinkTimer=0;
unsigned long greenBlinkTimer=0;

bool yellowBlinkState=false;
bool greenBlinkState=false;

float volumeLevel=0.8;

AudioGeneratorWAV *wav;
AudioFileSourceSD *file;
AudioOutputI2S *out;

uint8_t downloadBuffer[4096];

int pressCount=0;
unsigned long firstPressTime=0;

unsigned long lastInteractionTime = 0;
const unsigned long interactionCooldown = 5000; // 5 seconds


/* ================= AUDIO ================= */

void startPlayback() {
    stopPlayback();
    Serial.println("AUDIO: Starting playback");
    
    if(!SD.exists("/audio.wav")) {
        Serial.println("AUDIO: File missing");
        return;
    }
    
    file = new AudioFileSourceSD("/audio.wav");
    out = new AudioOutputI2S(0,1);
    
    out->SetPinout(I2S_BCLK,I2S_LRC,I2S_DOUT);
    out->SetGain(volumeLevel);
    
    out->SetBuffers(8, 1024);   // prevents beat drop crash
    
    wav = new AudioGeneratorWAV();
    
    if(!wav->begin(file,out))
    Serial.println("AUDIO: Begin failed");
}

void stopPlayback() {
    Serial.println("AUDIO: Stopping playback");
    
    if(wav){wav->stop();delete wav;wav=NULL;}
    if(file){delete file;file=NULL;}
    if(out){delete out;out=NULL;}
}

/* ================= DOWNLOAD ================= */
void downloadAudio(String filename) {
    downloadingAudio=true;
    Serial.println("DOWNLOAD: Start");
    
    if(mqttClient.connected())
        mqttClient.publish(("devices/"+device_id+"/state").c_str(),"downloading",true);
    
    String url="http://"+server_ip+":5000/audio/"+filename;
    HTTPClient http;
    http.begin(url);
    
    int httpCode=http.GET();
    
    if(httpCode==HTTP_CODE_OK) {
        Serial.println("DOWNLOAD: HTTP OK");
        
        if(SD.exists("/audio.wav"))
            SD.remove("/audio.wav");
        
        File file=SD.open("/audio.wav",FILE_WRITE,true);
        WiFiClient *stream=http.getStreamPtr();
        
        size_t bufferFill=0;
        
        while(http.connected()||stream->available()) {
            handleLEDs();
            if(mqttClient.connected())
                mqttClient.loop();
            
            int available=stream->available();
            if(available) {
                int len=stream->readBytes(downloadBuffer+bufferFill, min(available,(int)(sizeof(downloadBuffer)-bufferFill)));
                bufferFill+=len;
                
                if(bufferFill>=sizeof(downloadBuffer)) {
                    file.write(downloadBuffer,bufferFill);
                    bufferFill=0;
                }
            }
            yield();
        }
        
        if(bufferFill>0)
            file.write(downloadBuffer,bufferFill);
        
        file.close();
        Serial.println("DOWNLOAD: Completed");
    }
    
    else {
        Serial.println("DOWNLOAD: HTTP FAILED");
    }
    
    http.end();
    downloadingAudio=false;
    
    if(mqttClient.connected())
        mqttClient.publish(("devices/"+device_id+"/state").c_str(),"idle",true);
}


/* ================= RESET ================= */

void resetMemory() {
    Serial.println("RESET: Clearing configuration");
    
    prefs.clear();
    delay(1000);
    
    ESP.restart();
}

void checkMultiPress() {
    unsigned long now=millis();
    
    if(pressCount==0)
        firstPressTime=now;
    
    pressCount++;
    
    Serial.print("RESET COUNTER: ");
    Serial.println(pressCount);
    
    if(pressCount>=20) {
        if(now-firstPressTime<=20000)
            resetMemory();
        else
            pressCount=0;
    }
}

void checkPressTimeout() {
    if(pressCount==0)return;
    if(millis()-firstPressTime>20000) {
        Serial.println("RESET WINDOW EXPIRED");
        pressCount=0;
    }
}

/* ================= SD ================= */
void mountSD() {
    Serial.println("SD: Mounting");
    SPI.begin(18,19,23,SD_CS);
    
    if(!SD.begin(SD_CS,SPI,4000000)) {
        Serial.println("SD: FAILED");
        sdMounted=false;
    }
    else {
        Serial.println("SD: Mounted");
        sdMounted=true;
    }
}

/* ================= LED ================= */
void handleLEDs() {
    unsigned long now=millis();
    digitalWrite(LED_RED,digitalRead(BUTTON_PIN)==LOW);
    
    if(!sdMounted)
        digitalWrite(LED_YELLOW,LOW);
    else if(downloadingAudio) {

        if(now-yellowBlinkTimer>300) {
            yellowBlinkTimer=now;
            yellowBlinkState=!yellowBlinkState;
            digitalWrite(LED_YELLOW,yellowBlinkState);
        }
    }
    else
        digitalWrite(LED_YELLOW,HIGH);
    
    if(WiFi.status()!=WL_CONNECTED)
        digitalWrite(LED_GREEN,LOW);
    else if(!mqttClient.connected()) {
        if(now-greenBlinkTimer>500) {
            greenBlinkTimer=now;
            greenBlinkState=!greenBlinkState;
            digitalWrite(LED_GREEN,greenBlinkState);
        }
    }
    else
        digitalWrite(LED_GREEN,HIGH);
}

/* ================= PORTAL ================= */
void handleSave() {
    Serial.println("PORTAL: Saving config");
    
    ssid=server.arg("ssid");
    pass=server.arg("pass");
    server_ip=server.arg("ip");
    server_port=server.arg("port").toInt();
    
    prefs.putString("ssid",ssid);
    prefs.putString("pass",pass);
    prefs.putString("ip",server_ip);
    prefs.putInt("port",server_port);
    
    server.send(200,"text/plain","Saved. Rebooting...");
    delay(1000);
    ESP.restart();
}

void startAPMode() {
    Serial.println("PORTAL: Starting AP");
    
    WiFi.mode(WIFI_AP_STA);
    String apName = device_id + " Setup";
    WiFi.softAP(apName.c_str(), "12345678");
    
    IPAddress IP=WiFi.softAPIP();
    dns.start(53,"*",IP);
    
    apModeRunning=true;
    
    server.on("/",[](){
        String page=
        "<html><body>"
        "<h2>ESP Setup</h2>"
        "<form action='/save'>"
        "SSID:<input name='ssid'><br>"
        "PASS:<input name='pass'><br>"
        "Server IP:<input name='ip'><br>"
        "Port:<input name='port'><br>"
        "<input type='submit'>"
        "</form>"
        "</body></html>";
    
        server.send(200,"text/html",page);
    });
    
    server.on("/generate_204",[](){
        server.sendHeader("Location","/",true);
        server.send(302,"text/plain","");
    });
    
    server.on("/hotspot-detect.html",[](){
        server.sendHeader("Location","/",true);
        server.send(302,"text/plain","");
    });
    
    server.on("/save",handleSave);
    server.begin();
}

/* ================= CONFIG ================= */
void loadConfig() {
    Serial.println("CONFIG: Loading");
    ssid=prefs.getString("ssid","");
    pass=prefs.getString("pass","");
    server_ip=prefs.getString("ip","");
    server_port=prefs.getInt("port",0);
    
    if(ssid!="" && server_ip!="" && server_port!=0) {
        wifiConfigured=true;
        Serial.println("CONFIG: Found");
    }
    else {
        Serial.println("CONFIG: Missing");
    }
}

/* ================= WIFI ================= */
void handleWiFi() {
    // BLOCK during interaction
    if(millis() - lastInteractionTime < interactionCooldown)
        return;
    
    if(downloadingAudio)return;
    if(!wifiConfigured)return;
    if(audioPlaying)return;
    
    if(WiFi.status()==WL_CONNECTED) {
        if(wifiConnecting) {
            Serial.print("WIFI: Connected IP=");
            Serial.println(WiFi.localIP());
            wifiConnecting=false;
        }
        return;
    }
    
    if(millis()-lastWifiAttempt<3000)
        return;
    
    lastWifiAttempt=millis();
    Serial.println("WIFI: Attempting connection");
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(ssid.c_str(),pass.c_str());
    
    wifiConnecting=true;
}

/* ================= MQTT ================= */
void mqttCallback(char* topic,byte* payload,unsigned int length) {
    String msg;
    
    for(int i=0;i<length;i++)
        msg+=(char)payload[i];
    
    Serial.print("MQTT RX: ");
    Serial.println(topic);
    
    if(String(topic)=="devices/"+device_id+"/command") {
        Serial.println("MQTT: Download command");
        downloadAudio(msg);
    }
    
    if(String(topic)=="devices/"+device_id+"/volume") {
        volumeLevel=msg.toFloat();
        Serial.print("MQTT: Volume=");
        Serial.println(volumeLevel);
        
        if(out)
            out->SetGain(volumeLevel);
    }
}

void handleMQTT() {
    if(millis() - lastInteractionTime < interactionCooldown)
        return;
    
    if(WiFi.status()!=WL_CONNECTED) return;
    
    // DO NOT block if button is pressed
    if(digitalRead(BUTTON_PIN) == LOW) return;

    if(mqttClient.connected()) {
        mqttClient.loop();
        return;
    }

    if(millis()-lastMQTTAttempt<5000) return; // increase retry gap

    lastMQTTAttempt=millis();

    Serial.println("MQTT: Connecting");

    mqttClient.setServer(server_ip.c_str(),server_port);

    if(mqttClient.connect(device_id.c_str(), ("devices/"+device_id+"/status").c_str(), 1,true,"offline")) {
        Serial.println("MQTT: Connected");

        mqttClient.publish(("devices/"+device_id+"/status").c_str(),"online",true);
        mqttClient.publish(("devices/"+device_id+"/state").c_str(),"idle",true);

        mqttClient.subscribe(("devices/"+device_id+"/command").c_str());
        mqttClient.subscribe(("devices/"+device_id+"/volume").c_str());
    }
}


/* ================= BUTTON ================= */
void handleButtonPriority() {
    if(downloadingAudio) return;
        bool button = digitalRead(BUTTON_PIN);

    if(button == LOW && lastButtonState == HIGH) {
        if(millis() - debounceTime > 50) {
            debounceTime = millis();
            Serial.println("BUTTON: Pressed");
            lastInteractionTime = millis();  //ADD  THIS
            checkMultiPress();

            if(!audioPlaying) {
                audioPlaying = true;
                Serial.println("STATE: playing");

                if(mqttClient.connected())
                    mqttClient.publish(("devices/"+device_id+"/state").c_str(),"playing",true);

                if(SD.exists("/audio.wav"))
                    startPlayback();

                if(apModeRunning) {
                    dns.stop();
                    server.stop();
                }
            }
        }
    }

    if(button == HIGH && lastButtonState == LOW) {
        Serial.println("BUTTON: Released");
        lastInteractionTime = millis();  // ADD THIS

        if(audioPlaying) {
            audioPlaying = false;
            Serial.println("STATE: idle");

            if(mqttClient.connected())
                mqttClient.publish(("devices/"+device_id+"/state").c_str(),"idle",true);

            stopPlayback();

            if(apModeRunning) {
                IPAddress IP = WiFi.softAPIP();
                dns.start(53,"*",IP);
                server.begin();
            }
        }
    }
    lastButtonState = button;
}

/* ================= SETUP ================= */
void setup() {
    Serial.begin(115200);
    Serial.println("BOOT: Audio Cone");

    pinMode(BUTTON_PIN,INPUT_PULLUP);
    pinMode(LED_RED,OUTPUT);
    pinMode(LED_YELLOW,OUTPUT);
    pinMode(LED_GREEN,OUTPUT);

    mountSD();

    prefs.begin("config",false);

    loadConfig();

    mqttClient.setCallback(mqttCallback);
    mqttClient.setSocketTimeout(1);

    if(!wifiConfigured)
        startAPMode();
}

/* ================= LOOP ================= */
void loop() {

    handleLEDs();
    handleButtonPriority();
    checkPressTimeout();

    if(audioPlaying) {
        if(wav && wav->isRunning())
            wav->loop();

        delay(1);
        return;
    }

    handleWiFi();
    handleMQTT();

    if(apModeRunning) {
        dns.processNextRequest();
        server.handleClient();
    }
}
