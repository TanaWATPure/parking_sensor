#include <WiFi.h>
#include <PubSubClient.h>
#include <MFRC522.h>
#include <SPI.h>
#include <NewPing.h>
#include <time.h>

const char* ssid = "Frankenstyle";
const char* pwd = "1212312121";
const char* mqtt_srv = "172.20.10.3";
const int mqtt_port = 1883;
const char* mqtt_user = "mqtt_test";
const char* mqtt_pwd = "mqtt_test";

const char* topic_stat = "parking/status";
const char* topic_rfid = "parking/rfid";
const char* topic_alert = "parking/alert";
const char* topic_exit = "parking/exit_time";
const char* topic_unknown_exit = "parking/unknown_exit";  

#define TRIG_PIN 12
#define ECHO_PIN 14
#define SS_PIN 5
#define RST_PIN 4

#define MAX_DIST 50
#define CAR_ABSENT_TIME 10000
#define RFID_WAIT_TIME 20000 // Changed to 20 seconds
#define NTP_SYNC_INTERVAL 3600000 // 1 hour in milliseconds

const int PARKING_SLOT = 1;  // Parking slot number

WiFiClient espClient;
PubSubClient client(espClient);
MFRC522 rfid(SS_PIN, RST_PIN);
NewPing sonar(TRIG_PIN, ECHO_PIN, MAX_DIST);

unsigned long lastDetectTime = 0;
unsigned long rfidWaitStart = 0;
unsigned long lastNTPSync = 0;
bool carPresent = false;
bool waitRFID = false;
String lastRFID = "";

struct ParkingRecord {
  String rfid;
  time_t entryTime;
  time_t exitTime;
  long duration;
};

ParkingRecord currentRecord;

void setup() {
  Serial.begin(115200);
  
  WiFi.begin(ssid, pwd);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected");

  client.setServer(mqtt_srv, mqtt_port);
  client.setCallback(callback);

  SPI.begin();
  rfid.PCD_Init();

  configTime(7 * 3600, 0, "pool.ntp.org");
  syncTime();
}

void loop() {
  if (!client.connected()) reconnect();
  client.loop();

  if (millis() - lastNTPSync > NTP_SYNC_INTERVAL) {
    syncTime();
  }

  int dist = sonar.ping_cm();
  
  if (dist > 0 && dist <= MAX_DIST) {
    if (!carPresent) {
      carPresent = waitRFID = true;
      rfidWaitStart = millis();
      currentRecord.entryTime = time(nullptr);
      currentRecord.exitTime = 0;
      currentRecord.duration = 0;
      currentRecord.rfid = "";
      pubStatus("occupied");
      Serial.println("Car detected. Wait RFID scan.");
    }
    lastDetectTime = millis();
  } else if (carPresent && (millis() - lastDetectTime > CAR_ABSENT_TIME)) {
    currentRecord.exitTime = time(nullptr);
    currentRecord.duration = currentRecord.exitTime - currentRecord.entryTime;
    pubCarExit();
    resetSys();
  }

  if (waitRFID) {
    if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
      String tag = "";
      for (byte i = 0; i < rfid.uid.size; i++) {
        tag += String(rfid.uid.uidByte[i] < 0x10 ? "0" : "");
        tag += String(rfid.uid.uidByte[i], HEX);
      }
      tag.toUpperCase();
      
      if (tag != lastRFID) {
        lastRFID = tag;
        currentRecord.rfid = tag;
        pubRFID(tag);
        waitRFID = false;
        Serial.println("RFID scanned: " + tag);
      } else {
        Serial.println("Same RFID. Ignoring.");
      }
      
      rfid.PICC_HaltA();
      rfid.PCD_StopCrypto1();
    }
    
    if (millis() - rfidWaitStart > RFID_WAIT_TIME) {
      waitRFID = false;
      currentRecord.rfid = "unknown";
      // Set entry and exit times as "unknown" if no RFID detected
      currentRecord.entryTime = 0; // Setting times to zero to indicate unknown
      currentRecord.exitTime = 0;  // This will be handled in pubCarExit
      pubAlert("No RFID in 20s");
      Serial.println("No RFID in 20s. Recorded as unknown.");
    }
  }
}

void syncTime() {
  configTime(7 * 3600, 0, "pool.ntp.org");
  time_t now = time(nullptr);
  while (now < 8 * 3600 * 2) {
    delay(500);
    Serial.print(".");
    now = time(nullptr);
  }
  Serial.println("\nTime synchronized");
  lastNTPSync = millis();

  char timeStr[20];
  strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", localtime(&now));
  Serial.printf("Current time: %s\n", timeStr);
}

void callback(char* topic, byte* payload, unsigned int length) {
  // Handle MQTT messages
}

void reconnect() {
  while (!client.connected()) {
    Serial.print("Connecting MQTT...");
    if (client.connect("ESP32Client", mqtt_user, mqtt_pwd)) {
      Serial.println("connected");
      client.subscribe(topic_stat);
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" retry in 5s");
      delay(5000);
    }
  }
}

void pubStatus(const char* status) {
  client.publish(topic_stat, status);
}

void pubRFID(String rfid) {
  client.publish(topic_rfid, rfid.c_str());
}

void pubAlert(const char* msg) {
  client.publish(topic_alert, msg);
}

void pubCarExit() {
  char entryTime[20];
  char exitTime[20];
  
  // If RFID is "unknown", set entry and exit times to "unknown" as well
  if (currentRecord.rfid == "unknown") {
    strcpy(entryTime, "unknown");
    strcpy(exitTime, "unknown");
    
    // Create message for unknown RFID
    char unknownExitMsg[300];
    snprintf(unknownExitMsg, sizeof(unknownExitMsg), 
             "{'slot':%d,'rfid':'%s','entry':'%s','exit':'%s','duration':%ld}", 
             PARKING_SLOT, "unknown", "unknown", "unknown", 0);
    
    // Publish to separate topic for unknown exits
    client.publish(topic_unknown_exit, unknownExitMsg);
    Serial.println(unknownExitMsg);
  } else {
    // Format the entry and exit times for known RFID
    strftime(entryTime, sizeof(entryTime), "%Y-%m-%d %H:%M:%S", localtime(&currentRecord.entryTime));
    strftime(exitTime, sizeof(exitTime), "%Y-%m-%d %H:%M:%S", localtime(&currentRecord.exitTime));
    
    // Create message for known RFID
    char exitMsg[300];
    snprintf(exitMsg, sizeof(exitMsg), 
             "{'slot':%d,'rfid':'%s','entry':'%s','exit':'%s','duration':%ld}", 
             PARKING_SLOT, currentRecord.rfid.c_str(), entryTime, exitTime, currentRecord.duration);
    
    // Publish to the normal exit topic
    client.publish(topic_exit, exitMsg);
    Serial.println(exitMsg);
  }
}

void resetSys() {
  carPresent = waitRFID = false;
  lastRFID = "";
  currentRecord = {String(""), 0, 0, 0};
  pubStatus("available");
  Serial.println("System reset. Ready for next car.");
}
