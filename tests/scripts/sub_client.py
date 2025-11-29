#!/usr/bin/env python3
# tests/scripts/sub_client.py (versión robusta)
import socket, struct, time, sys, json

HOST='127.0.0.1'
PORT=5000
TOPIC='sensors/test/environment'
RECV_TIMEOUT = 10.0  # segundos, aumentado para evitar timeouts cortos

def read_nbytes(sock, n):
    data = b''
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            raise ConnectionError("peer closed")
        data += chunk
    return data

def read_line(sock, timeout=RECV_TIMEOUT):
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
    print("Connecting to broker", HOST, PORT)
    s = socket.create_connection((HOST, PORT))
    try:
        # handshake
        msg = f"HELLO SUBSCRIBER cli-py\n".encode()
        print("-> SEND:", msg)
        s.sendall(msg)

        print("Waiting server response to HELLO (timeout", RECV_TIMEOUT, "s)...")
        try:
            resp = read_line(s, timeout=RECV_TIMEOUT)
        except TimeoutError:
            print("ERROR: timeout waiting server response to HELLO")
            s.close()
            return
        print("BROKER:", resp)

        # subscribe
        cmd = f"SUB {TOPIC}\n".encode()
        print("-> SEND:", cmd)
        s.sendall(cmd)

        print("Waiting server response to SUB (timeout", RECV_TIMEOUT, "s)...")
        try:
            resp = read_line(s, timeout=RECV_TIMEOUT)
        except TimeoutError:
            print("ERROR: timeout waiting server response to SUB")
            s.close()
            return
        print("BROKER:", resp)

        print("Esperando mensajes en topic:", TOPIC)
        while True:
            # read 4-byte big-endian length
            hdr = read_nbytes(s, 4)
            (llen,) = struct.unpack('!I', hdr)
            payload = read_nbytes(s, llen)
            try:
                obj = json.loads(payload.decode())
            except Exception:
                obj = payload.decode(errors='replace')
            ts = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime())
            print(f"[{ts}] recibidos {llen} bytes ->", json.dumps(obj, ensure_ascii=False))
    except KeyboardInterrupt:
        print("Saliendo por Ctrl-C")
    except ConnectionError as e:
        print("Conexión cerrada:", e)
    finally:
        s.close()

if __name__ == "__main__":
    main()
