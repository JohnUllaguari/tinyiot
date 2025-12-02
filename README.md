# Tiny_IoT Broker

Sistema completo de mensajería IoT pub/sub implementado en C con arquitectura de múltiples capas para hardware de bajo poder computacional.

## 📋 Descripción General

**Tiny_IoT** es un sistema distribuido de mensajería basado en el patrón publicador-suscriptor (pub/sub) diseñado específicamente para Internet de las Cosas. El proyecto implementa una arquitectura completa de tres capas que permite la comunicación eficiente entre dispositivos IoT, agregadores de datos y clientes consumidores.

### Características Principales

- 🚀 **Alto Rendimiento**: Arquitectura non-blocking con `epoll` para manejar miles de conexiones simultáneas
- 🔄 **Protocolo Ligero**: Protocolo binario optimizado con overhead mínimo
- 📡 **Arquitectura Escalable**: Diseño modular con broker, gateway y publishers independientes
- 🛡️ **Robusto**: Manejo completo de errores, reconexión automática y buffers de salida para evitar bloqueos
- 🔧 **Compatible con ESP32**: Publisher implementado con FreeRTOS para microcontroladores
- 📊 **Testing Incluido**: Scripts de carga y medición de latencia

## 🏗️ Arquitectura del Sistema

```
┌─────────────────┐
│  ESP32 Sensors  │ (Publishers)
│  - Temperature  │
│  - Humidity     │
└────────┬────────┘
         │ WiFi
         ▼
┌─────────────────┐
│     Gateway     │ (Agregador)
│  - Buffer       │
│  - Forward      │
└────────┬────────┘
         │ TCP
         ▼
┌─────────────────┐
│     Broker      │ (Servidor Central)
│  - Topics       │
│  - Pub/Sub      │
│  - Non-blocking │
└────────┬────────┘
         │
    ┌────┴────┬─────────┐
    ▼         ▼         ▼
┌────────┐ ┌───────┐ ┌──────┐
│CLI Sub │ │Web UI │ │  DB  │
└────────┘ └───────┘ └──────┘
```

### Componentes

1. **Publishers (ESP32)**: Nodos con sensores que generan datos
2. **Gateway**: Agregador que recibe de múltiples publishers y envía al broker
3. **Broker**: Servidor central que maneja tópicos y distribuye mensajes
4. **Subscribers**: Clientes que consumen datos (CLI, web, base de datos)

## 📂 Estructura del Proyecto

```
tiny_iot/
├── broker/                 # Servidor central
│   ├── src/
│   │   ├── main.c         # Loop principal con epoll
│   │   ├── broker.c       # Lógica pub/sub y manejo de conexiones
│   │   ├── proto.c        # Funciones de protocolo
│   │   └── proto.h        # Definiciones compartidas
│   └── Makefile
│
├── gateway/               # Agregador de publishers
│   ├── gateway.c         # Gateway con queue thread-safe
│   ├── publisher_sim.c   # Simulador de publisher en C
│   └── Makefile
│
├── esp32/                # Publisher para ESP32
│   ├── publisher_esp32.ino
│   └── wokwi_project.json
│
├── loadtest/             # Pruebas de carga
│   ├── loadtest.py       # Test con N publishers
│   ├── sub_client_latency.py  # Subscriber con medición
│   └── analyze_latencies.py   # Análisis estadístico
│
├── tests/scripts/        # Scripts de prueba
│   ├── sub_client.py     # Subscriber CLI robusto
│   ├── send_pub.py       # Publisher de prueba
│   └── *.sh              # Scripts auxiliares
│
└── README.md
```

## 🚀 Compilación e Instalación

### Requisitos

- **Sistema Operativo**: Linux/Unix (usa `epoll`)
- **Compilador**: GCC 7.0+
- **Herramientas**: Make, Python 3.7+
- **Opcional**: ESP32 con Arduino IDE o PlatformIO para el publisher físico

### Compilar el Broker

```bash
cd broker/
make
# Ejecutar
./brokerd [puerto]
```

### Compilar el Gateway

```bash
cd gateway/
make
# Ejecutar gateway (conecta a broker en 127.0.0.1:5000)
./gatewayd

# Ejecutar simulador de publisher
./publisher_sim
```

### Configurar ESP32 Publisher

1. Abrir `esp32/publisher_esp32.ino` en Arduino IDE
2. Configurar credenciales WiFi y dirección del gateway:
   ```cpp
   #define WIFI_SSID "tu_red"
   #define WIFI_PASS "tu_password"
   #define GATEWAY_HOST "192.168.1.100"  // IP del gateway
   #define GATEWAY_PORT 6000
   ```
3. Compilar y flashear al ESP32

**Simulación con Wokwi**: Usa [Wokwi](https://wokwi.com/) con `ngrok` para tunelizar el gateway a Internet.

## 📖 Uso del Sistema

### 1. Iniciar el Broker

```bash
cd broker/
./brokerd 5000
```

**Salida esperada:**
```
brokerd listening on port 5000
```

### 2. Iniciar el Gateway

```bash
cd gateway/
./gatewayd
```

**Salida esperada:**
```
[G] listening publishers on port 6000
[G] connected to broker 127.0.0.1:5000 fd=4
```

### 3. Conectar un Subscriber

**Usando el cliente Python robusto:**
```bash
cd tests/scripts/
python3 sub_client.py
```

**Salida esperada:**
```
Connecting to broker 127.0.0.1 5000
-> SEND: b'HELLO SUBSCRIBER cli-py\n'
Waiting server response to HELLO...
BROKER: OK
-> SEND: b'SUB sensors/test/environment\n'
BROKER: OK
Esperando mensajes en topic: sensors/test/environment
```

### 4. Publicar Datos

**Opción A: Usando el simulador del Gateway**
```bash
cd gateway/
./publisher_sim
```

**Opción B: Usando script Python**
```bash
cd tests/scripts/
python3 send_pub.py
```

**Opción C: N ppublishers**
```bash
#Correr previamente un subscriber
$ python3 tests/scripts/sub_client.py
#loadtest
$ python3 loadtest/loadtest.py -n 10 -m 50 -i 0.5
```

**Opción D: ESP32 real** (flasheado previamente)
```
El ESP32 se conectará automáticamente y comenzará a enviar datos
```

## 🔌 Protocolo de Comunicación

### Comandos Cliente → Servidor

| Comando | Formato | Descripción | Respuesta |
|---------|---------|-------------|-----------|
| `HELLO` | `HELLO <ROLE> <NODE_ID>\n` | Autenticación inicial | `OK\n` |
| `SUB` | `SUB <TOPIC>\n` | Suscribirse a tópico | `OK\n` |
| `UNSUB` | `UNSUB <TOPIC>\n` | Desuscribirse | `OK\n` |
| `PUB` | `PUB <TOPIC> <LEN>\n` + datos | Publicar mensaje | `OK\n` |
| `PING` | `PING\n` | Verificar conexión | `PONG\n` |
| `BYE` | `BYE\n` | Cerrar conexión | `OK\n` |

### Roles Soportados

- **`PUBLISHER`**: Nodos ESP32 con sensores
- **`GATEWAY`**: Agregadores intermedios
- **`SUBSCRIBER`**: Clientes consumidores

### Formato del Comando PUB

El protocolo usa un formato binario eficiente:

```
1. Cliente envía: "PUB <topic> <length>\n"
2. Cliente envía: 4 bytes en big-endian con la longitud del payload
3. Cliente envía: payload (JSON recomendado)
```

**Ejemplo:**
```
PUB sensors/test/environment 98\n
[0x00 0x00 0x00 0x62]  (4 bytes = 98 decimal)
{"node":"esp32-01","ts":1234567890,"data":{"temp":25.5,"hum":60.2}}
```

### Formato del Payload JSON (Recomendado)

```json
{
  "node": "esp32-01",
  "ts": 1701234567890,
  "topic": "sensors/test/environment",
  "data": {
    "temp": 25.5,
    "hum": 60.2
  }
}
```

## 🧪 Pruebas de Carga

### Ejecutar Load Test

```bash
cd loadtest/
python3 loadtest.py -n 50 -m 100 -i 0.1 --host 127.0.0.1 --port 6000
```

**Parámetros:**
- `-n`: Número de publishers concurrentes
- `-m`: Mensajes por publisher
- `-i`: Intervalo entre mensajes (segundos)
- `--host`: Host del gateway
- `--port`: Puerto del gateway

**Ejemplo de salida:**
```
=== Test finished ===
Threads: 50, Msgs/thread: 100, interval: 0.1s
Connected: 50, connect_fail: 0
Sent messages: 5000, send_fail: 0
Duration: 52.34s, throughput (msg/s): 95.53
```

### Medir Latencia

**Terminal 1 - Subscriber con medición:**
```bash
cd loadtest/
python3 sub_client_latency.py
```

**Terminal 2 - Generar carga:**
```bash
python3 loadtest.py -n 10 -m 50 -i 0.05
```

**Terminal 3 - Analizar resultados:**
```bash
python3 analyze_latencies.py
```

**Salida esperada:**
```
count: 500
mean: 12.45 ms
median: 11.20 ms
p90: 18.30 ms
p95: 22.10 ms
max: 45.60 ms
```

## 🔧 Características Técnicas Avanzadas

### Broker

- **I/O Non-blocking**: Todas las operaciones usan `O_NONBLOCK` con `epoll`
- **Buffers de Salida**: Sistema de buffering por conexión para evitar bloqueos en escritura
- **EPOLLOUT Dinámico**: Solo se registra cuando hay datos pendientes
- **Máquina de Estados**: Parsing robusto con estados `AWAIT_LINE`, `AWAIT_LEN`, `AWAIT_PAYLOAD`
- **Límites Configurables**:
  - `MAX_FD_LIMIT`: 10,000 descriptores
  - `TINY_MAX_PAYLOAD`: 8,192 bytes por mensaje
  - `LISTEN_BACKLOG`: 128 conexiones pendientes

### Gateway

- **Queue Thread-Safe**: Cola FIFO con mutex y condition variables
- **Thread Dedicado**: Un thread para enviar al broker sin bloquear publishers
- **Reconexión Automática**: Se reconecta al broker si se cae la conexión
- **Límite de Cola**: `QUEUE_MAX_ITEMS` (20,000) para prevenir memory exhaustion
- **Epoll Multi-conexión**: Maneja múltiples publishers simultáneamente

### ESP32 Publisher

- **FreeRTOS**: Tres tareas concurrentes:
  - `temp_task`: Lee sensor de temperatura (700ms)
  - `hum_task`: Lee sensor de humedad (1100ms)
  - `sender_task`: Envía datos al gateway
- **Queue Inter-task**: Comunicación entre tareas vía `xQueueSend`/`xQueueReceive`
- **WiFi Resiliente**: Reconexión automática WiFi y TCP
- **Timestamps Precisos**: Usa `esp_timer_get_time()` para timestamps en microsegundos

## 📊 Ejemplo de Flujo Completo

### Terminal 1: Broker
```bash
$ cd broker && ./brokerd 5000
brokerd listening on port 5000
[INFO] accepted fd=4 from 127.0.0.1:45678
[INFO] fd=4 HELLO role=2 node=gw1
[INFO] accepted fd=5 from 127.0.0.1:45679
[INFO] fd=5 HELLO role=3 node=sub1
[INFO] fd=5 SUB sensors/test/environment
[INFO] fd=4 PUB header topic=sensors/test/environment expected_len=98
[INFO] published topic=sensors/test/environment -> 1 subscribers
```

### Terminal 2: Gateway
```bash
$ cd gateway && ./gatewayd
[G] listening publishers on port 6000
[G] connected to broker 127.0.0.1:5000 fd=4
[G] accepted fd=5 from 192.168.1.42:54321
[G] fd=5 PUB header topic=sensors/test/environment expected_len=98
[G] queued topic=sensors/test/environment len=98 from fd=5
```

### Terminal 3: Subscriber
```bash
$ cd tests/scripts && python3 sub_client.py
Connecting to broker 127.0.0.1 5000
BROKER: OK
BROKER: OK
Esperando mensajes en topic: sensors/test/environment
[2024-11-30 15:23:45] recibidos 98 bytes -> {"node":"esp32-01","ts":1701357825,"data":{"temp":25.5,"hum":60.2}}
```

### Terminal 4: ESP32 (Serial Monitor)
```
ESP32 publisher (FreeRTOS) starting...
WiFi connected, IP: 192.168.1.42
[SENDER] connecting to gateway 192.168.1.100:6000 ...
[SENDER] connected, sending HELLO
[SENDER] gateway replied: 'OK'
[SENDER] sent payload len=98
```

## 🐛 Troubleshooting

### Error: "Address already in use"
```bash
# Esperar 60s o usar otro puerto
./brokerd 5001
# O forzar liberación
sudo fuser -k 5000/tcp
```

### Subscriber no recibe mensajes
1. Verificar que el subscriber se conectó **antes** de que lleguen mensajes
2. Verificar que el tópico coincide exactamente (case-sensitive)
3. Revisar logs del broker para confirmar publicación
4. Usar `tcpdump` para inspeccionar tráfico:
   ```bash
   sudo tcpdump -i lo -A port 5000
   ```

### Gateway no conecta al broker
```bash
# Verificar que el broker está corriendo
netstat -tuln | grep 5000
# Verificar conectividad
telnet 127.0.0.1 5000
```

### ESP32 no se conecta al Gateway
1. Verificar WiFi: SSID y contraseña correctos
2. Verificar IP del gateway: usar `ifconfig` o `ip addr`
3. Si usas Wokwi: configurar `ngrok` correctamente
4. Revisar firewall: `sudo ufw allow 6000/tcp`

### Errores de compilación
```bash
# Verificar GCC
gcc --version  # Requiere 7.0+

# Limpiar y recompilar
make clean && make

# En Mac (usar clang)
CC=clang make
```

## 📈 Optimizaciones y Mejoras Futuras

### Implementadas ✅
- [x] Non-blocking I/O con epoll
- [x] Buffers de salida por conexión
- [x] EPOLLOUT dinámico
- [x] Gateway con queue thread-safe
- [x] Reconexión automática
- [x] Publisher ESP32 con FreeRTOS
- [x] Scripts de load testing
- [x] Medición de latencia

### Planeadas 🚧
- [ ] **TLS/SSL**: Encriptación de comunicaciones
- [ ] **Autenticación**: Sistema de tokens o certificados
- [ ] **Persistencia**: Almacenar mensajes en disco (SQLite, RocksDB)
- [ ] **QoS Levels**: Garantías de entrega (at-most-once, at-least-once, exactly-once)
- [ ] **Wildcards**: Suscripciones con `+` y `#` (estilo MQTT)
- [ ] **Retained Messages**: Último mensaje retenido por tópico
- [ ] **Dashboard Web**: Interfaz React para monitoreo en tiempo real
- [ ] **Integración con Grafana**: Visualización de métricas
- [ ] **Dockerización**: Contenedores para despliegue fácil

## 📚 Referencias y Recursos

- [MQTT Protocol Specification](https://mqtt.org/mqtt-specification/)
- [epoll(7) - Linux Man Pages](https://man7.org/linux/man-pages/man7/epoll.7.html)
- [FreeRTOS Documentation](https://www.freertos.org/Documentation/RTOS_book.html)
- [ESP32 Arduino Core](https://docs.espressif.com/projects/arduino-esp32/)
- [Wokwi ESP32 Simulator](https://wokwi.com/)
- [The C10K Problem](http://www.kegel.com/c10k.html)
- [Beej's Guide to Network Programming](https://beej.us/guide/bgnet/)

## 🎓 Contexto Académico

Este proyecto fue desarrollado como parte de la materia **Sistemas Operativos**, demostrando conceptos avanzados de:

- **Programación concurrente**: Epoll, threads, sincronización
- **Protocolos de red**: Diseño e implementación de protocolos binarios
- **Sistemas embebidos**: FreeRTOS en ESP32
- **Arquitectura de software**: Diseño modular y escalable
- **Performance**: Optimización para alto throughput y baja latencia

## 👨‍💻 Autor

John Lopez, desarrollado con ❤️ para aprender sobre sistemas distribuidos y programación de sistemas.

## 📄 Licencia

Este proyecto es de uso académico y educativo. Siéntete libre de usarlo para aprender y experimentar.

---

**¿Preguntas o problemas?** Revisa la sección de Troubleshooting o abre un issue con logs detallados.