# Tiny_IoT Broker

Sistema de broker MQTT ligero para IoT implementado en C, diseñado para hardware de bajo poder computacional.

## 📋 Descripción

Este proyecto implementa un **broker de mensajería pub/sub** basado en tópicos para sistemas IoT. Permite la comunicación entre:
- **Publishers** (ESP32/NodeMCU con sensores)
- **Gateways** (agregadores de datos)
- **Subscribers** (clientes CLI/UI que consumen datos)

## 🏗️ Arquitectura

```
┌─────────────┐
│  Publisher  │─┐
│  (ESP32)    │ │
└─────────────┘ │
                ├──> ┌──────────┐      ┌──────────┐
┌─────────────┐ │    │ Gateway  │─────>│  Broker  │
│  Publisher  │─┘    │  (RPi)   │      │ (Server) │
│  (ESP32)    │      └──────────┘      └─────┬────┘
└─────────────┘                              │
                                             │
                                    ┌────────┴────────┐
                                    │                 │
                              ┌─────▼─────┐   ┌──────▼──────┐
                              │Subscriber │   │ Subscriber  │
                              │  (CLI)    │   │   (UI/DB)   │
                              └───────────┘   └─────────────┘
```

## 📂 Estructura del Proyecto

```
broker/
├── src/
│   ├── proto.h         # Definiciones y prototipos
│   ├── proto.c         # Implementación de funciones de protocolo
│   ├── broker.c        # Lógica del broker (tópicos, pub/sub)
│   └── main.c          # Loop principal con epoll
├── Makefile            # Compilación del proyecto
├── test_subscriber.sh  # Script de prueba para subscribers
├── test_publisher.py   # Script de prueba para publishers
└── README.md           # Esta documentación
```

## 🚀 Compilación

### Requisitos
- GCC (GNU Compiler Collection)
- Make
- Linux/Unix (usa epoll)

### Compilar

```bash
# Compilar el proyecto
make

# Limpiar archivos de compilación
make clean

# Compilar y ejecutar
make run
```

## 📖 Uso

### 1. Iniciar el Broker

```bash
# Puerto por defecto (5000)
./broker

# Puerto personalizado
./broker 8080
```

### 2. Conectar un Subscriber

**Opción A: Usando el script de prueba**
```bash
chmod +x test_subscriber.sh
./test_subscriber.sh
```

**Opción B: Usando netcat manualmente**
```bash
nc localhost 5000
HELLO SUBSCRIBER sub1
SUB sensor/temperature
SUB sensor/humidity
```

### 3. Conectar un Publisher/Gateway

**Opción A: Usando el script de prueba**
```bash
chmod +x test_publisher.py
python3 test_publisher.py
```

**Opción B: Manualmente (ejemplo con Python)**
```python
import socket
import json
import struct

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.connect(('localhost', 5000))

# Autenticación
sock.sendall(b'HELLO GATEWAY gw1\n')
print(sock.recv(1024).decode())

# Publicar datos
topic = 'sensor/temperature'
data = {'value': 25.5, 'unit': 'C'}
payload = json.dumps(data).encode()
length = len(payload)

# Enviar comando PUB
sock.sendall(f'PUB {topic} {length}\n'.encode())

# Enviar longitud en big-endian (4 bytes)
sock.sendall(struct.pack('!I', length))

# Enviar payload
sock.sendall(payload)

sock.close()
```

## 🔌 Protocolo de Comunicación

### Comandos del Cliente → Broker

| Comando | Formato | Descripción | Respuesta |
|---------|---------|-------------|-----------|
| `HELLO` | `HELLO <ROLE> <NODE_ID>` | Autenticación inicial | `OK` |
| `SUB` | `SUB <TOPIC>` | Suscribirse a un tópico | `OK` |
| `UNSUB` | `UNSUB <TOPIC>` | Desuscribirse de un tópico | `OK` |
| `PUB` | `PUB <TOPIC> <LENGTH>` | Publicar mensaje (seguido de datos) | N/A |
| `PING` | `PING` | Verificar conexión | `PONG` |
| `BYE` | `BYE` | Cerrar conexión | `OK` |

### Roles Soportados
- `PUBLISHER` - Nodos con sensores (ESP32)
- `GATEWAY` - Agregadores de datos
- `SUBSCRIBER` - Clientes que reciben datos

### Formato de Mensajes PUB

1. Cliente envía: `PUB <topic> <length>\n`
2. Cliente envía: 4 bytes (big-endian) con la longitud del payload
3. Cliente envía: payload (JSON recomendado)

**Ejemplo:**
```
PUB sensor/temp 25\n
[4 bytes: 0x00 0x00 0x00 0x19]
{"value":25.5,"unit":"C"}
```

## 📊 Ejemplo de Flujo Completo

### Terminal 1: Broker
```bash
$ ./broker 5000
========================================
  Tiny_IoT Broker Started
  Listening on port: 5000
  Press Ctrl+C to stop
========================================
```

### Terminal 2: Subscriber
```bash
$ nc localhost 5000
HELLO SUBSCRIBER sub1
OK
SUB sensor/temperature
OK
SUB sensor/humidity
OK
# Esperando mensajes...
```

### Terminal 3: Publisher
```bash
$ python3 test_publisher.py
==================================================
  Tiny_IoT Test Publisher/Gateway
  Connecting to localhost:5000
  Node ID: test_gateway_1234
==================================================

Connected to broker at localhost:5000

  -> HELLO GATEWAY test_gateway_1234
  <- OK
  -> PING
  <- PONG

==================================================
  Publishing sensor data from test_gateway_1234...
==================================================

  -> PUB sensor/temperature 98
  -> [4-byte length: 98]
  -> [payload: {"node_id": "test_gateway_1234", ...}]
  Published to 'sensor/temperature': {...}
```

### Output en Terminal 2 (Subscriber):
```
[4 bytes length prefix]
{"node_id": "test_gateway_1234", "sensor": "temperature", "value": 28.42, ...}
```

## 🔧 Características Implementadas

✅ **Broker funcional**
- Servidor TCP non-blocking con epoll
- Manejo de múltiples clientes simultáneos
- Sistema de tópicos dinámico

✅ **Protocolo pub/sub**
- Suscripción/desuscripción a tópicos
- Publicación de mensajes con payload binario
- Autenticación básica (HELLO)

✅ **Gestión de conexiones**
- Detección de desconexiones
- Limpieza automática de recursos
- Manejo de errores robusto

✅ **Estado por conexión**
- Máquina de estados para parsing de mensajes
- Buffer de entrada por conexión
- Manejo de mensajes fragmentados

## 🐛 Troubleshooting

### Error: "Address already in use"
```bash
# Esperar unos segundos o cambiar el puerto
./broker 5001
```

### El subscriber no recibe mensajes
- Verificar que el subscriber se suscribió ANTES de que lleguen mensajes
- Verificar que el tópico coincide exactamente (case-sensitive)

### Errores de compilación
```bash
# Verificar que tienes GCC instalado
gcc --version

# Limpiar y recompilar
make clean
make
```

## 📝 Notas Técnicas

### Formato de Payload
- El broker NO interpreta el contenido del payload
- Se recomienda usar JSON para compatibilidad
- Longitud máxima: 8192 bytes (TINY_MAX_PAYLOAD)

### Límites
- Máximo de FDs: 10000 (MAX_FD_LIMIT)
- Líneas de comando: 1024 bytes (TINY_MAX_LINE)
- Backlog de listen: 128 conexiones

### Non-blocking I/O
- Todas las operaciones de socket son non-blocking
- Usa epoll para multiplexación eficiente
- Manejo de EAGAIN/EWOULDBLOCK

## 🎯 Próximos Pasos

Para completar el proyecto Tiny_IoT:

1. **Publisher (ESP32)**
   - Implementar con FreeRTOS
   - Leer sensores (DHT22, etc.)
   - Conectar al Gateway vía WiFi

2. **Gateway (RPi/Linux)**
   - Recibir datos de múltiples publishers
   - Agregar/filtrar datos
   - Enviar al Broker

3. **Subscribers avanzados**
   - Base de datos (InfluxDB, MongoDB)
   - Dashboard web (Grafana-like)
   - Alertas y notificaciones

## 📚 Referencias

- [MQTT Protocol](https://mqtt.org/)
- [epoll(7) - Linux man page](https://man7.org/linux/man-pages/man7/epoll.7.html)
- [FreeRTOS](https://www.freertos.org/)
- [Wokwi ESP32 Simulator](https://wokwi.com/)

## 👨‍💻 Autor

Proyecto desarrollado para la materia de Sistemas Operativos.

---

**Licencia:** Uso académico