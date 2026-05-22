#define TINY_GSM_MODEM_SIM800

#include <TinyGsmClient.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// -------------------------------------------------
// SERIAL CONFIG
// -------------------------------------------------
#define SerialMon Serial
#define SerialAT  Serial2   // SIM808 -- single UART for BOTH GSM and GPS

// -------------------------------------------------
// SIM808 (MQTT + GPRS + GPS -- all on Serial2)
// -------------------------------------------------
const char apn[]      = "internet.ooredoo.tn";
const char gprsUser[] = "";
const char gprsPass[] = "";

const char* mqttBroker = "broker.hivemq.com";
const int   mqttPort   = 1883;

TinyGsm       modem(SerialAT);
TinyGsmClient gsmClient(modem);
PubSubClient  mqtt(gsmClient);

// -------------------------------------------------
// DEVICE
// -------------------------------------------------
String deviceId;
String mqttTopic;

unsigned long lastPublish = 0;
const unsigned long PUBLISH_INTERVAL = 15000; // 15s -- give GPS time between polls

// -------------------------------------------------
// AT HELPER -- uses SerialAT directly (bypasses TinyGSM)
// -------------------------------------------------
String sendATDirect(String cmd, int timeout = 2000) {
  while (SerialAT.available()) SerialAT.read();

  SerialAT.println(cmd);

  String resp = "";
  long start = millis();
  while (millis() - start < timeout) {
    while (SerialAT.available()) {
      resp += (char)SerialAT.read();
    }
  }

  SerialMon.println(">> " + cmd);
  SerialMon.println(resp);

  return resp;
}

// -------------------------------------------------
// CSV FIELD PARSER
// -------------------------------------------------
String getField(String data, int n) {
  int count = 0;
  int start = 0;

  for (int i = 0; i <= (int)data.length(); i++) {
    if (i == (int)data.length() || data[i] == ',') {
      if (count == n) return data.substring(start, i);
      count++;
      start = i + 1;
    }
  }
  return "";
}

// -------------------------------------------------
// GPS -- polls AT+CGNSINF, waits up to 5 min for fix
// -------------------------------------------------
bool getGPS(float &lat, float &lng, float &speed, float &altitude,
            float &heading, int &satellites, float &hdop) {

  sendATDirect("AT+CGNSPWR=1", 2000);
  delay(2000);

  for (int attempt = 0; attempt < 60; attempt++) {

    String resp = sendATDirect("AT+CGNSINF", 2000);

    int idx = resp.indexOf("+CGNSINF:");
    if (idx == -1) {
      SerialMon.println("No +CGNSINF in response, retrying...");
      delay(5000);
      continue;
    }

    String data = resp.substring(idx + 9);
    data.trim();

    String fix        = getField(data, 1);
    String slat       = getField(data, 3);
    String slng       = getField(data, 4);
    String salt       = getField(data, 5);
    String sspeed     = getField(data, 6);
    String sheading   = getField(data, 7);
    String shdop      = getField(data, 10);
    String ssatellites = getField(data, 14);

    SerialMon.println("Fix status: " + fix + " | attempt " + String(attempt + 1) + "/60");

    if (fix == "1" && slat.length() >= 3 && slng.length() >= 3) {
      lat        = slat.toFloat();
      lng        = slng.toFloat();
      altitude   = salt.toFloat();
      speed      = sspeed.toFloat();
      heading    = sheading.toFloat();
      hdop       = shdop.toFloat();
      satellites = ssatellites.toInt();
      SerialMon.println("GPS Fix: " + slat + ", " + slng);
      return true;
    }

    delay(5000);
  }

  SerialMon.println("GPS fix timeout after 5 min");
  return false;
}

// -------------------------------------------------
// DEVICE ID
// -------------------------------------------------
String getDeviceId() {
  uint64_t chipid = ESP.getEfuseMac();
  char id[20];
  snprintf(id, sizeof(id),
           "ESP32-%04X%08X",
           (uint16_t)(chipid >> 32),
           (uint32_t)(chipid));
  return String(id);
}

// -------------------------------------------------
// GPRS CONNECT
// -------------------------------------------------
void connectGPRS() {
  if (modem.isGprsConnected()) return;

  SerialMon.println("Connecting GPRS...");
  modem.restart();
  delay(3000);

  if (!modem.gprsConnect(apn, gprsUser, gprsPass)) {
    SerialMon.println("GPRS FAILED");
  } else {
    SerialMon.println("GPRS CONNECTED");
  }
}

// -------------------------------------------------
// MQTT CONNECT
// -------------------------------------------------
void connectMQTT() {
  mqtt.setServer(mqttBroker, mqttPort);
  mqtt.setBufferSize(512);

  int tries = 0;
  while (!mqtt.connected() && tries < 5) {
    SerialMon.print("Connecting MQTT...");
    String clientId = deviceId + "-" + String(random(0xffff), HEX);

    if (mqtt.connect(clientId.c_str())) {
      SerialMon.println(" CONNECTED");
    } else {
      SerialMon.print(" FAILED rc=");
      SerialMon.println(mqtt.state());
      delay(3000);
      tries++;
    }
  }
}

// -------------------------------------------------
// TIMESTAMP -- reads from SIM808 modem clock
// -------------------------------------------------
String getTimestamp() {
  String time = modem.getGSMDateTime(DATE_FULL);
  if (time.length() == 0) return "1970-01-01T00:00:00Z";

  // GSM format: "YY/MM/DD,HH:MM:SS+TZ"
  // Output ISO 8601: "20YY-MM-DDTHH:MM:SSZ"
  String year   = "20" + time.substring(0, 2);
  String month  = time.substring(3, 5);
  String day    = time.substring(6, 8);
  String hour   = time.substring(9, 11);
  String minute = time.substring(12, 14);
  String second = time.substring(15, 17);

  return year + "-" + month + "-" + day + "T" + hour + ":" + minute + ":" + second + "Z";
}

// -------------------------------------------------
// PUBLISH GPS
// -------------------------------------------------
void publishGPS() {
  float lat, lng, speed, altitude, heading, hdop;
  int satellites;

  if (!getGPS(lat, lng, speed, altitude, heading, satellites, hdop)) {
    SerialMon.println("Skipping publish -- no GPS fix");
    return;
  }

  StaticJsonDocument<300> doc;
  doc["deviceId"] = deviceId;

  JsonObject location = doc.createNestedObject("location");
  location["lat"] = lat;
  location["lng"] = lng;

  doc["altitude"]   = altitude;
  doc["speed"]      = speed;
  doc["heading"]    = heading;
  doc["satellites"] = satellites;
  doc["hdop"]       = hdop;
  doc["timestamp"]  = getTimestamp();

  char payload[300];
  serializeJson(doc, payload);

  SerialMon.println("Publishing:");
  SerialMon.println(payload);

  bool ok = mqtt.publish(mqttTopic.c_str(), payload, true);
  SerialMon.println(ok ? "Published" : "Publish failed");
}

// -------------------------------------------------
// SETUP
// -------------------------------------------------
void setup() {
  SerialMon.begin(115200);
  SerialAT.begin(9600, SERIAL_8N1, 16, 17);

  deviceId  = getDeviceId();
  mqttTopic = "fleet/" + deviceId + "/gps";

  SerialMon.println("Device: " + deviceId);
  SerialMon.println("Topic : " + mqttTopic);

  connectGPRS();
  connectMQTT();
}

// -------------------------------------------------
// LOOP
// -------------------------------------------------
void loop() {
  if (!modem.isGprsConnected()) connectGPRS();
  if (!mqtt.connected())        connectMQTT();

  mqtt.loop();

  if (millis() - lastPublish > PUBLISH_INTERVAL) {
    lastPublish = millis();
    publishGPS();
  }
}