#!/usr/bin/env python3
"""
loadtest.py
Lanza N hilos-publisher que se conectan al gateway y envían M mensajes cada uno.

Uso:
  python3 loadtest.py -n 50 -m 100 -i 0.1 --host 127.0.0.1 --port 6000

Los publishers usan el protocolo:
  HELLO PUBLISHER <id>\n
  PUB <topic> <len>\n
  [4-byte BE len][payload JSON bytes]

El payload JSON contiene campo "ts" en milisegundos (epoch ms) para medir latencia.
"""
import argparse
import socket
import struct
import threading
import time
import json
import sys

TOPIC = "sensors/test/environment"

def send_all(sock, data, timeout=5.0):
    sock.settimeout(timeout)
    total = 0
    while total < len(data):
        try:
            sent = sock.send(data[total:])
        except socket.timeout:
            return False
        except Exception:
            return False
        if sent == 0:
            return False
        total += sent
    return True

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
    return data.decode().strip()

def publisher_thread(thread_id, host, port, messages, interval, results):
    node_id = f"lt-{thread_id}"
    try:
        s = socket.create_connection((host, port), timeout=5)
    except Exception as e:
        print(f"[P{thread_id}] connect error: {e}")
        results['connect_fail'] += 1
        return
    # HELLO
    hello = f"HELLO PUBLISHER {node_id}\n".encode()
    if not send_all(s, hello):
        print(f"[P{thread_id}] failed send HELLO")
        s.close(); results['connect_fail'] += 1; return
    resp = read_line(s, timeout=5.0)
    if resp is None or resp.strip() != "OK":
        print(f"[P{thread_id}] HELLO no OK: {resp}")
        s.close(); results['connect_fail'] += 1; return
    results['connected'] += 1
    for i in range(messages):
        ts_ms = int(time.time() * 1000)
        payload = {
            "node": node_id,
            "ts": ts_ms,
            "topic": TOPIC,
            "data": {"temp": 20 + (thread_id % 10), "hum": 30 + (i % 50)}
        }
        pl = json.dumps(payload, separators=(',', ':')).encode()
        header = f"PUB {TOPIC} {len(pl)}\n".encode()
        # send header
        if not send_all(s, header):
            print(f"[P{thread_id}] send header failed at msg {i}")
            results['send_fail'] += 1
            break
        # send 4-byte BE len
        be = struct.pack('!I', len(pl))
        if not send_all(s, be):
            print(f"[P{thread_id}] send len failed at msg {i}")
            results['send_fail'] += 1
            break
        # send payload
        if not send_all(s, pl):
            print(f"[P{thread_id}] send payload failed at msg {i}")
            results['send_fail'] += 1
            break
        # optional: read OK from gateway (but don't block long)
        ok = read_line(s, timeout=1.0)
        if ok != "OK":
            # not fatal - we still count as sent
            pass
        results['sent'] += 1
        if interval > 0:
            time.sleep(interval)
    try:
        s.close()
    except Exception:
        pass

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("-n", "--num", type=int, default=10, help="Número de publishers (hilos)")
    parser.add_argument("-m", "--msgs", type=int, default=10, help="Mensajes por publisher")
    parser.add_argument("-i", "--interval", type=float, default=0.1, help="Intervalo entre mensajes (s)")
    parser.add_argument("--host", type=str, default="127.0.0.1", help="Gateway host")
    parser.add_argument("--port", type=int, default=6000, help="Gateway port")
    args = parser.parse_args()

    results = {'connected':0, 'connect_fail':0, 'sent':0, 'send_fail':0}
    threads = []
    start = time.time()
    for t in range(args.num):
        th = threading.Thread(target=publisher_thread, args=(t, args.host, args.port, args.msgs, args.interval, results), daemon=True)
        th.start()
        threads.append(th)
        time.sleep(0.01)  # small stagger to avoid SYN bursts

    for th in threads:
        th.join()

    dur = time.time() - start
    print("=== Test finished ===")
    print(f"Threads: {args.num}, Msgs/thread: {args.msgs}, interval: {args.interval}s")
    print(f"Connected: {results['connected']}, connect_fail: {results['connect_fail']}")
    print(f"Sent messages: {results['sent']}, send_fail: {results['send_fail']}")
    print(f"Duration: {dur:.2f}s, throughput (msg/s): {results['sent']/dur:.2f}")

if __name__ == '__main__':
    main()
