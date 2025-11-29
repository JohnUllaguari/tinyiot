#!/usr/bin/env python3
# tests/scripts/send_pub.py (verboso)
import socket, struct, json, time

HOST='127.0.0.1'
PORT=5000
TOPIC='sensors/test/environment'

def read_line(sock, timeout=5.0):
    sock.settimeout(timeout)
    data = b''
    try:
        while True:
            c = sock.recv(1)
            if not c:
                return None
            data += c
            if c == b'\n':
                break
    except Exception:
        return None
    finally:
        sock.settimeout(None)
    return data.decode().strip()

s = socket.create_connection((HOST, PORT))
try:
    s.settimeout(5.0)
    s.sendall(b'HELLO GATEWAY gw-py\n')
    resp = read_line(s)
    print("BROKER:", resp)
    topic = TOPIC
    payload = json.dumps({"node":"py","ts":int(time.time()),"topic":topic,"data":{"temp":42,"hum":10}})
    hdr = f'PUB {topic} {len(payload)}\n'.encode()
    print("-> SEND HEADER:", hdr)
    s.sendall(hdr)
    # send 4-byte len BE then payload
    s.sendall(struct.pack('!I', len(payload)))
    s.sendall(payload.encode())
    print("sent payload, len=", len(payload))
finally:
    s.close()
