#!/usr/bin/env python3
"""
sub_client_latency.py
Subscriber que recibe mensajes y calcula latencia = recv_time_ms - payload.ts (ts en ms).
Guarda resultados en latencies.csv y los imprime por pantalla.
"""
import socket, struct, json, time, csv, sys

HOST='127.0.0.1'
PORT=5000
TOPIC='sensors/test/environment'
CSV='latencies.csv'

def read_nbytes(sock, n):
    data = b''
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            raise ConnectionError("peer closed")
        data += chunk
    return data

def read_line(sock, timeout=5.0):
    sock.settimeout(timeout)
    data = b''
    try:
        while True:
            c = sock.recv(1)
            if not c:
                raise ConnectionError("peer closed while reading line")
            data += c
            if c == b'\n':
                break
    except socket.timeout:
        raise TimeoutError("read_line timeout")
    finally:
        sock.settimeout(None)
    return data.decode(errors='replace').rstrip('\r\n')

def main():
    s = socket.create_connection((HOST, PORT))
    try:
        s.sendall(b'HELLO SUBSCRIBER lt-sub\n')
        print("HELLO sent, waiting OK...")
        print("BROKER:", read_line(s))
        s.sendall(f"SUB {TOPIC}\n".encode())
        print("SUB sent, waiting OK...")
        print("BROKER:", read_line(s))
        print("Esperando mensajes en topic:", TOPIC)
        # open CSV
        with open(CSV, 'w', newline='') as csvf:
            writer = csv.writer(csvf)
            writer.writerow(["recv_time_ms","node","topic","latency_ms"])
            while True:
                hdr = read_nbytes(s, 4)
                (llen,) = struct.unpack('!I', hdr)
                payload = read_nbytes(s, llen)
                recv_ms = int(time.time()*1000)
                try:
                    obj = json.loads(payload.decode())
                except Exception:
                    obj = None
                if obj and 'ts' in obj:
                    latency = recv_ms - int(obj['ts'])
                else:
                    latency = None
                node = obj.get('node') if obj else ""
                topic = obj.get('topic') if obj else ""
                print(f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] recv {llen} bytes node={node} latency_ms={latency}")
                writer.writerow([recv_ms, node, topic, latency])
                csvf.flush()
    except KeyboardInterrupt:
        print("Interrupted")
    finally:
        s.close()

if __name__ == '__main__':
    main()
