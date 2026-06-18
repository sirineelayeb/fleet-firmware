#define TINY_GSM_MODEM_SIM800

#include <TinyGsmClient.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// ─────────────────────────────────────────────
// SERIAL CONFIG
// ─────────────────────────────────────────────
#define SerialMon Serial
#define SerialAT  Serial2

// ─────────────────────────────────────────────
// SIM808 — GPRS + GPS + MQTT on Serial2
// ─────────────────────────────────────────────
const char apn[]      = "internet.ooredoo.tn";
const char gprsUser[] = "";
const char gprsPass[] = "";

const char* mqttBroker = "broker.hivemq.com";
const int   mqttPort   = 1883;

TinyGsm       modem(SerialAT);
TinyGsmClient gsmClient(modem);
PubSubClient  mqtt(gsmClient);

// ─────────────────────────────────────────────
// DEVICE
// ─────────────────────────────────────────────
String deviceId;
String mqttTopic;

unsigned long lastPublish    = 0;
const unsigned long PUBLISH_INTERVAL = 15000;

// ─────────────────────────────────────────────
// AT HELPER
// ─────────────────────────────────────────────
String sendATDirect(String cmd, int timeout = 2000) {
  while (SerialAT.available()) SerialAT.read();
  SerialAT.println(cmd);

  String resp  = "";
  long   start = millis();
  while (millis() - start < timeout) {
    while (SerialAT.available()) resp += (char)SerialAT.read();
  }

  SerialMon.println(">> " + cmd);
  SerialMon.println(resp);
  return resp;
}

// ─────────────────────────────────────────────
// CSV FIELD PARSER  (AT+CGNSINF / AT+CBC)
// ─────────────────────────────────────────────
String getField(String data, int n) {
  int count = 0, start = 0;
  for (int i = 0; i <= (int)data.length(); i++) {
    if (i == (int)data.length() || data[i] == ',') {
      if (count == n) return data.substring(start, i);
      count++;
      start = i + 1;
    }
  }
  return "";
}

// ─────────────────────────────────────────────
// DEVICE ID
// ─────────────────────────────────────────────
String getDeviceId() {
  uint64_t chipid = ESP.getEfuseMac();
  char id[20];
  snprintf(id, sizeof(id),
           "ESP32-%04X%08X",
           (uint16_t)(chipid >> 32),
           (uint32_t)(chipid));
  return String(id);
}

// ─────────────────────────────────────────────
// BATTERY — reads from SIM808 via AT+CBC
// No GPIO/voltage divider needed
// AT+CBC returns: +CBC: <status>,<percent>,<voltage_mV>
// ─────────────────────────────────────────────
float readBatteryPercent() {
  String resp = sendATDirect("AT+CBC", 2000);

  int idx = resp.indexOf("+CBC:");
  if (idx == -1) {
    SerialMon.println("Battery read failed");
    return -1;  // backend ignores -1
  }

  String data = resp.substring(idx + 5);
  data.trim();

  // field 0 = charge status, field 1 = percent, field 2 = voltage mV
  String spct = getField(data, 1);
  float  pct  = spct.toFloat();

  SerialMon.println("Battery: " + String(pct) + "%");
  return constrain(pct, 0.0, 100.0);
}

// ─────────────────────────────────────────────
// TIMESTAMP — real GSM network time
// ─────────────────────────────────────────────
String getTimestamp() {
  String t = modem.getGSMDateTime(DATE_FULL);
  if (t.length() > 0) return t;
  return "unknown";
}

// ─────────────────────────────────────────────
// GPS — polls AT+CGNSINF, waits up to 5 min for fix
// Extracts real heading, satellites, hdop
//
// AT+CGNSINF field map:
//  0  GNSS run status
//  1  Fix status (1 = valid)
//  2  UTC datetime
//  3  Latitude
//  4  Longitude
//  5  Altitude (m)
//  6  Speed (km/h)
//  7  Course / heading (degrees)
//  8  Fix mode
//  9  Reserved
//  10 HDOP
//  11 PDOP
//  12 VDOP
//  13 Reserved
//  14 Satellites in view
// ─────────────────────────────────────────────
bool getGPS(float &lat, float &lng, float &speed,
            float &altitude, float &heading,
            int &satellites, float &hdop) {

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

    String fix     = getField(data, 1);
    String slat    = getField(data, 3);
    String slng    = getField(data, 4);
    String salt    = getField(data, 5);
    String sspeed  = getField(data, 6);
    String scourse = getField(data, 7);
    String shdop   = getField(data, 10);
    String ssats   = getField(data, 14);

    SerialMon.println("Fix: " + fix +
                      " | Attempt " + String(attempt + 1) + "/60" +
                      " | Sats: " + ssats);

    if (fix == "1" && slat.length() >= 3 && slng.length() >= 3) {
      lat        = slat.toFloat();
      lng        = slng.toFloat();
      altitude   = salt.toFloat();
      speed      = sspeed.toFloat();
      heading    = scourse.toFloat();
      hdop       = shdop.toFloat();
      satellites = ssats.toInt();

      SerialMon.println("GPS Fix: " + slat + ", " + slng +
                        " @ " + sspeed + " km/h | HDG " + scourse +
                        " | Sats " + ssats);
      return true;
    }

    delay(5000);
  }

  SerialMon.println("GPS fix timeout after 5 min");
  return false;
}

// ─────────────────────────────────────────────
// GPRS CONNECT
// restart() only on first connect — preserves GPS engine on reconnects
// ─────────────────────────────────────────────
void connectGPRS() {
  if (modem.isGprsConnected()) return;

  SerialMon.println("Connecting GPRS...");

  modem.init();
  delay(1000);

  SerialMon.print("Waiting for network...");
  if (!modem.waitForNetwork(60000L)) {
    SerialMon.println(" Network timeout");
    return;
  }
  SerialMon.println(" OK");

  if (!modem.gprsConnect(apn, gprsUser, gprsPass)) {
    SerialMon.println("GPRS FAILED");
  } else {
    SerialMon.println("GPRS CONNECTED");
  }
}

// ─────────────────────────────────────────────
// MQTT CONNECT
// ─────────────────────────────────────────────
void connectMQTT() {
  mqtt.setServer(mqttBroker, mqttPort);
  mqtt.setBufferSize(512);

  int tries = 0;
  while (!mqtt.connected() && tries < 5) {
    SerialMon.print("Connecting MQTT...");
    String clientId = deviceId + "-" + String(random(0xffff), HEX);

    if (mqtt.connect(clientId.c_str())) {
      SerialMon.println("CONNECTED");
    } else {
      SerialMon.print("FAILED rc=");
      SerialMon.println(mqtt.state());
      delay(3000);
      tries++;
    }
  }
}

// ─────────────────────────────────────────────
// PUBLISH GPS
// ─────────────────────────────────────────────
void publishGPS() {
  float lat, lng, speed, altitude, heading, hdop;
  int   satellites;

  if (!getGPS(lat, lng, speed, altitude, heading, satellites, hdop)) {
    SerialMon.println("Skipping publish — no GPS fix");
    return;
  }

  float battery = readBatteryPercent();

  StaticJsonDocument<400> doc;
  doc["deviceId"] = deviceId;

  JsonObject location = doc.createNestedObject("location");
  location["lat"] = lat;
  location["lng"] = lng;

  doc["altitude"]     = altitude;
  doc["speed"]        = speed;
  doc["heading"]      = heading;
  doc["satellites"]   = satellites;
  doc["hdop"]         = hdop;
  doc["batteryLevel"] = battery;
  // doc["deviceTimestamp"] = getTimestamp();

  char payload[400];
  serializeJson(doc, payload);

  SerialMon.println("Publishing:");
  SerialMon.println(payload);

  // bool ok = mqtt.publish(mqttTopic.c_str(), payload, true);
  // Publish GPS data to MQTT broker (false = do NOT retain message on broker)
  bool ok = mqtt.publish(mqttTopic.c_str(), payload, false);
  SerialMon.println(ok ? "Published OK" : "Publish FAILED");
}

// ─────────────────────────────────────────────
// SETUP
// ─────────────────────────────────────────────
void setup() {
  SerialMon.begin(115200);
  SerialAT.begin(9600, SERIAL_8N1, 16, 17);

  randomSeed(analogRead(0));

  deviceId  = getDeviceId();
  mqttTopic = "fleet/" + deviceId + "/gps";

  SerialMon.println("Device: " + deviceId);
  SerialMon.println("Topic : " + mqttTopic);

  connectGPRS();
  connectMQTT();

  // Power GPS engine once — stays warm between publishes
  sendATDirect("AT+CGNSPWR=1", 2000);
  delay(2000);
  SerialMon.println("GPS engine powered on");
}

// ─────────────────────────────────────────────
// LOOP
// ─────────────────────────────────────────────
void loop() {
  if (!modem.isGprsConnected()) connectGPRS();
  if (!mqtt.connected())        connectMQTT();

  mqtt.loop();

  if (millis() - lastPublish > PUBLISH_INTERVAL) {
    lastPublish = millis();
    publishGPS();
  }
}