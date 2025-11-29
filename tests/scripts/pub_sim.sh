#!/usr/bin/env bash
# Simula un gateway que envía 3 mensajes PUB al broker
python3 - <<'PY'
import socket, struct, json, time
HOST='127.0.0.1'
PORT=5000
s=socket.create_connection((HOST,PORT))
s.sendall(b'HELLO GATEWAY gw-sim\n')
print("SERVER:", s.recv(1024))
topic='sensors/test/environment'
for i in range(3):
    payload=json.dumps({"node":"sim","ts":int(time.time()),"topic":topic,"data":{"temp":20+i,"hum":50+i}})
    hdr=f'PUB {topic} {len(payload)}\n'.encode()
    s.sendall(hdr)
    # send 4-byte len BE then payload
    s.sendall(struct.pack('!I', len(payload)))
    s.sendall(payload.encode())
    print("sent", i)
    time.sleep(1)
s.close()
PY
