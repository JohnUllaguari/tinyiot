/*
 publisher_esp32.ino
 Sketch Arduino para ESP32 que:
 - usa FreeRTOS tasks para simular lectura de sensores
 - agrupa lecturas en JSON y las envía al gateway por TCP
 - protocolo: HELLO PUBLISHER <id>\n  |  PUB <topic> <len>\n + 4-byte BE len + payload JSON
 - Adjust: GATEWAY_IP, GATEWAY_PORT, WIFI_SSID, WIFI_PASS
*/

#include <Arduino.h>
#include <WiFi.h>
#include <sys/socket.h> // not used directly but useful to know
#include <stdint.h>

// ---------- CONFIG ----------
#define WIFI_SSID     "YOUR_SSID"
#define WIFI_PASS     "YOUR_PASSWORD"

#define GATEWAY_IP    "192.168.1.100"   // <- Cambia a la IP del gateway (o hostname)
#define GATEWAY_PORT  6000

#define PUBLISHER_ID  "esp32-01"
#define TOPIC         "sensors/test/environment"

#define QUEUE_LENGTH  8
#define MAX_PAYLOAD   1024

// ---------- FreeRTOS objects ----------
static QueueHandle_t sensorQueue = NULL;

// Structure to hold a measurement
typedef struct {
  unsigned long ts;
  float temp;
  float hum;
} sensor_sample_t;

// ---------- Utility: build JSON payload ----------
static void build_payload(char *buf, size_t buflen, const char *node_id, unsigned long ts, float temp, float hum) {
  // Very small manual JSON build to avoid external libs
  // Example:
  // {"node":"esp32-01","ts":123456,"topic":"sensors/test/environment","data":{"temp":23,"hum":45}}
  int n = snprintf(buf, buflen,
    "{\"node\":\"%s\",\"ts\":%lu,\"topic\":\"%s\",\"data\":{\"temp\":%.2f,\"hum\":%.2f}}",
    node_id, ts, TOPIC, temp, hum);
  if (n < 0 || n >= (int)buflen) {
    // truncated — ensure null terminated
    buf[buflen-1] = '\0';
  }
}

// ---------- Networking helper: send_all using WiFiClient ----------
static bool send_all(WiFiClient &c, const uint8_t *data, size_t len, unsigned long timeout_ms=5000) {
  unsigned long start = millis();
  size_t sent = 0;
  while (sent < len) {
    size_t w = c.write(data + sent, len - sent);
    if (w > 0) {
      sent += w;
      continue;
    }
    // if write returns 0, wait a bit but not indefinitely
    if (!c.connected()) return false;
    if (millis() - start > timeout_ms) return false;
    delay(5);
  }
  return true;
}

// ---------- Tasks ----------

// Simulated sensor reader: produce samples every 'interval_ms' and push to queue
static void task_read_sensors(void *param) {
  (void)param;
  TickType_t lastWake = xTaskGetTickCount();
  const TickType_t interval = pdMS_TO_TICKS(1000); // every 1s produce simulated sample
  while (1) {
    sensor_sample_t s;
    s.ts = (unsigned long)time(nullptr);
    // simulate sensor values (simple noise)
    s.temp = 20.0 + (random(0, 100) / 10.0); // 20.0 - 29.9
    s.hum  = 30.0 + (random(0, 700) / 10.0); // 30.0 - 99.9
    // push to queue (overwrite if full -> block short time then drop)
    if (xQueueSend(sensorQueue, &s, pdMS_TO_TICKS(100)) != pdTRUE) {
      // queue full, drop oldest: receive one and send again
      sensor_sample_t tmp;
      xQueueReceive(sensorQueue, &tmp, 0);
      xQueueSend(sensorQueue, &s, 0);
    }
    vTaskDelayUntil(&lastWake, interval);
  }
}

// Sender task: connect to gateway and send queued samples
static void task_sender(void *param) {
  (void)param;
  WiFiClient client;
  char payload[MAX_PAYLOAD];
  char header[128];

  for (;;) {
    // ensure WiFi connected
    if (WiFi.status() != WL_CONNECTED) {
      // block until connected
      Serial.println("[SENDER] waiting for WiFi...");
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    // try connect to gateway
    Serial.printf("[SENDER] connecting to gateway %s:%d ...\n", GATEWAY_IP, GATEWAY_PORT);
    if (!client.connect(GATEWAY_IP, GATEWAY_PORT, 5000)) {
      Serial.println("[SENDER] connect failed, retry in 2s");
      client.stop();
      vTaskDelay(pdMS_TO_TICKS(2000));
      continue;
    }
    Serial.println("[SENDER] connected to gateway");
    // send HELLO
    String hello = String("HELLO PUBLISHER ") + PUBLISHER_ID + "\n";
    client.print(hello);
    // wait for OK (simple blocking read with timeout)
    unsigned long tstart = millis();
    String resp = "";
    while (millis() - tstart < 5000) {
      while (client.available()) {
        char ch = client.read();
        if (ch == '\n') goto got_hello_resp;
        resp += ch;
      }
      delay(1);
    }
got_hello_resp:
    resp.trim();
    Serial.printf("[SENDER] gateway replied: '%s'\n", resp.c_str());
    if (resp != "OK") {
      Serial.println("[SENDER] gateway did not reply OK, closing and retrying");
      client.stop();
      vTaskDelay(pdMS_TO_TICKS(2000));
      continue;
    }

    // Now enter sending loop: pop samples from queue and send
    sensor_sample_t sample;
    while (client.connected()) {
      // block until sample available
      if (xQueueReceive(sensorQueue, &sample, pdMS_TO_TICKS(5000)) == pdTRUE) {
        // build payload
        build_payload(payload, sizeof(payload), PUBLISHER_ID, sample.ts, sample.temp, sample.hum);
        size_t len = strlen(payload);
        // header: PUB <topic> <len>\n
        int hn = snprintf(header, sizeof(header), "PUB %s %u\n", TOPIC, (unsigned)len);
        if (hn < 0) { Serial.println("[SENDER] header snprintf error"); continue; }
        // send header text
        if (!send_all(client, (const uint8_t*)header, hn)) { Serial.println("[SENDER] header send failed"); break; }
        // send 4-byte BE len
        uint32_t be = htonl((uint32_t)len);
        if (!send_all(client, (const uint8_t*)&be, sizeof(be))) { Serial.println("[SENDER] len send failed"); break; }
        // send payload text
        if (!send_all(client, (const uint8_t*)payload, len)) { Serial.println("[SENDER] payload send failed"); break; }
        Serial.printf("[SENDER] sent payload len=%u\n", (unsigned)len);
        // wait for OK line from gateway
        String okr = "";
        unsigned long t0 = millis();
        bool got_ok = false;
        while (millis() - t0 < 3000) {
          while (client.available()) {
            char ch = client.read();
            if (ch == '\n') {
              okr.trim();
              if (okr == "OK") got_ok = true;
              break;
            }
            okr += ch;
          }
          if (got_ok) break;
          delay(1);
        }
        if (!got_ok) {
          Serial.println("[SENDER] did not receive OK, continuing (but message already forwarded)");
          // continue; // we keep going; re-send policy can be added
        }
      } else {
        // timeout waiting for measurement -> check connectivity, loop
      }
    } // end while connected
    Serial.println("[SENDER] disconnected from gateway, will retry");
    client.stop();
    vTaskDelay(pdMS_TO_TICKS(2000));
  } // end forever
}

// ---------- setup/loop ----------
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("ESP32 publisher starting...");

  // init random seed
  randomSeed((unsigned)esp_random());

  // create queue
  sensorQueue = xQueueCreate(QUEUE_LENGTH, sizeof(sensor_sample_t));
  if (!sensorQueue) {
    Serial.println("Failed to create queue");
    while (1) delay(1000);
  }

  // connect to WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("Connecting to WiFi '%s' ...\n", WIFI_SSID);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > 15000) {
      Serial.println("WiFi connect timeout; retrying...");
      start = millis();
    }
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("WiFi connected, IP: "); Serial.println(WiFi.localIP());

  // create FreeRTOS tasks
  xTaskCreatePinnedToCore(task_read_sensors, "read_sensors", 4096, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(task_sender, "sender", 8192, NULL, 2, NULL, 1);
}

void loop() {
  // Nothing here; tasks run independently
  delay(1000);
}
