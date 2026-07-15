/*
  CuveGuard - GROUPE4
  ESP32 + HC-SR04 + DHT22 + relais (pompe) + LED alerte + buzzer
  Publie la telemetrie vers ThingsBoard (MQTT) et traite les RPC
  setPump / setManualMode envoyees depuis ThingsBoard (dashboard ou agent Python).
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <ArduinoJson.h>

// ---------- Wi-Fi (reseau simule Wokwi) ----------
const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASS = "";

// ---------- ThingsBoard ----------
const char* TB_SERVER = "eu.thingsboard.cloud";
const int   TB_PORT   = 1883;
// Token du device "ESP32 CuveGuard" cree dans ThingsBoard.
// NE JAMAIS committer le vrai token : remplace via un define local
// ou un fichier non versionne. Ici on met un placeholder volontairement.
const char* TB_TOKEN  = "exib7nTNimuHVnSlI2Q0";

// ---------- Broches ----------
#define TRIG_PIN     5
#define ECHO_PIN     18
#define DHT_PIN      4
#define DHT_TYPE     DHT22
#define RELAY_PIN    26   // relais (pompe) + LED indicatrice cablee en parallele
#define ALERT_LED_PIN 25  // LED rouge : niveau bas
#define BUZZER_PIN   33   // buzzer : niveau critique

// ---------- Calibration de la cuve ----------
// Distance capteur -> surface quand la cuve est PLEINE (cm)
const float TANK_FULL_DISTANCE_CM  = 5.0;
// Distance capteur -> surface (ou fond) quand la cuve est VIDE (cm)
const float TANK_EMPTY_DISTANCE_CM = 100.0;

// ---------- Seuils d'alerte locale ----------
const float ALERT_ATTENTION_PCT = 30.0;
const float ALERT_CRITIQUE_PCT  = 15.0;

// ---------- Periodes ----------
const unsigned long TELEMETRY_PERIOD_MS = 3000;
const unsigned long SENSOR_PERIOD_MS    = 1000;
const unsigned long BUZZER_TOGGLE_MS    = 400;

DHT dht(DHT_PIN, DHT_TYPE);
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// ---------- Etat courant ----------
float distanceCm    = 0;
float waterLevelPct = 0;
float temperature   = 0;
float humidity      = 0;
bool  pumpOn        = false;
bool  manualMode    = false;
int   alertLevel    = 0;

unsigned long lastTelemetry = 0;
unsigned long lastSensorRead = 0;
unsigned long lastBuzzerToggle = 0;
bool buzzerState = false;

// ---------------------------------------------------------------
void connectWiFi() {
  Serial.print("Connexion WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print(".");
  }
  Serial.println(" OK");
  Serial.println(WiFi.localIP());
}

float readDistanceCm() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 30000UL); // timeout 30ms ~ 5m
  if (duration == 0) {
    return distanceCm; // pas d'echo -> on garde la derniere valeur valide
  }
  return duration * 0.0343f / 2.0f;
}

float distanceToPct(float d) {
  float pct = (TANK_EMPTY_DISTANCE_CM - d) / (TANK_EMPTY_DISTANCE_CM - TANK_FULL_DISTANCE_CM) * 100.0f;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  return pct;
}

void updateAlertLevel() {
  if (waterLevelPct < ALERT_CRITIQUE_PCT) alertLevel = 2;
  else if (waterLevelPct < ALERT_ATTENTION_PCT) alertLevel = 1;
  else alertLevel = 0;
}

void applyLocalAlert() {
  // LED rouge : allumee fixe des l'etat "attention", buzzer uniquement en "critique"
  digitalWrite(ALERT_LED_PIN, alertLevel >= 1 ? HIGH : LOW);

  if (alertLevel == 2) {
    if (millis() - lastBuzzerToggle > BUZZER_TOGGLE_MS) {
      buzzerState = !buzzerState;
      digitalWrite(BUZZER_PIN, buzzerState ? HIGH : LOW);
      lastBuzzerToggle = millis();
    }
  } else {
    digitalWrite(BUZZER_PIN, LOW);
    buzzerState = false;
  }
}

void applyPumpOutput() {
  digitalWrite(RELAY_PIN, pumpOn ? HIGH : LOW);
}

// ---------------------------------------------------------------
void sendRpcResponse(const String& requestId, JsonDocument& respDoc) {
  String topic = "v1/devices/me/rpc/response/" + requestId;
  char buffer[128];
  size_t n = serializeJson(respDoc, buffer);
  mqttClient.publish(topic.c_str(), (uint8_t*)buffer, n);
}

void handleRpc(const String& requestId, JsonDocument& doc) {
  const char* method = doc["method"] | "";
  StaticJsonDocument<64> resp;

  if (strcmp(method, "setPump") == 0) {
    pumpOn = doc["params"]["value"] | false;
    applyPumpOutput();
    Serial.print("RPC setPump -> ");
    Serial.println(pumpOn);
    resp["value"] = pumpOn;
  } else if (strcmp(method, "setManualMode") == 0) {
    manualMode = doc["params"]["value"] | false;
    Serial.print("RPC setManualMode -> ");
    Serial.println(manualMode);
    resp["value"] = manualMode;
  } else {
    resp["error"] = "unknown method";
  }
  sendRpcResponse(requestId, resp);
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String topicStr(topic);
  String payloadStr;
  payloadStr.reserve(length);
  for (unsigned int i = 0; i < length; i++) payloadStr += (char)payload[i];

  // topic attendu : v1/devices/me/rpc/request/{requestId}
  int idx = topicStr.lastIndexOf('/');
  String requestId = topicStr.substring(idx + 1);

  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, payloadStr);
  if (err) {
    Serial.println("Erreur parsing RPC JSON");
    return;
  }
  handleRpc(requestId, doc);
}

void reconnectMqtt() {
  while (!mqttClient.connected()) {
    Serial.print("Connexion MQTT ThingsBoard...");
    String clientId = "ESP32CuveGuard-" + String(random(0xffff), HEX);
    if (mqttClient.connect(clientId.c_str(), TB_TOKEN, NULL)) {
      Serial.println(" OK");
      mqttClient.subscribe("v1/devices/me/rpc/request/+");
    } else {
      Serial.print(" echec, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" -> nouvel essai dans 2s");
      delay(2000);
    }
  }
}

void publishTelemetry() {
  StaticJsonDocument<256> doc;
  doc["distanceCm"]    = distanceCm;
  doc["waterLevelPct"] = waterLevelPct;
  doc["temperature"]   = temperature;
  doc["humidity"]      = humidity;
  doc["pumpOn"]        = pumpOn;
  doc["manualMode"]    = manualMode;
  doc["alertLevel"]    = alertLevel;

  char buffer[256];
  size_t n = serializeJson(doc, buffer);
  mqttClient.publish("v1/devices/me/telemetry", (uint8_t*)buffer, n);

  Serial.print("Telemetrie envoyee: ");
  Serial.println(buffer);
}

// ---------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(ALERT_LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  digitalWrite(RELAY_PIN, LOW);
  digitalWrite(ALERT_LED_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  dht.begin();
  connectWiFi();

  mqttClient.setServer(TB_SERVER, TB_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(512);
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }
  if (!mqttClient.connected()) {
    reconnectMqtt();
  }
  mqttClient.loop();

  unsigned long now = millis();

  if (now - lastSensorRead >= SENSOR_PERIOD_MS) {
    lastSensorRead = now;
    distanceCm = readDistanceCm();
    waterLevelPct = distanceToPct(distanceCm);

    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t)) temperature = t;
    if (!isnan(h)) humidity = h;

    updateAlertLevel();
    applyLocalAlert();
  }

  if (now - lastTelemetry >= TELEMETRY_PERIOD_MS) {
    lastTelemetry = now;
    publishTelemetry();
  }
}
