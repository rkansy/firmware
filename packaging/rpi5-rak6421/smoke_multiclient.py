#!/usr/bin/env python3
"""Two-concurrent-client smoke test for the multi-client meshtasticd TCP API.

Speaks the raw 0x94 0xC3 framing directly so we can assert per-message
behavior (config_complete_id per client, queue-status id remapping, fan-out).
"""
import os
import queue
import socket
import struct
import sys
import threading
import time

MP = os.path.dirname(os.path.abspath(__import__("meshtastic").__file__))
sys.path.insert(0, os.path.join(MP, "protobuf"))
import mesh_pb2  # noqa: E402
import portnums_pb2  # noqa: E402

PORTNUM_TEXT = portnums_pb2.TEXT_MESSAGE_APP

HOST = "127.0.0.1"
PORT = 4403
FAILS = []


class Client:
    def __init__(self, name, nonce):
        self.name = name
        self.nonce = nonce
        self.sock = socket.create_connection((HOST, PORT), timeout=5)
        self.sock.settimeout(0.2)
        self.buf = b""
        self.msgs = queue.Queue()
        self.alive = False
        self.t = threading.Thread(target=self._reader, daemon=True)
        self.t.start()
        # kick off the config handshake
        tr = mesh_pb2.ToRadio(want_config_id=nonce)
        self._send(tr.SerializeToString())

    def _send(self, payload: bytes):
        frame = b"\x94\xc3" + struct.pack(">H", len(payload)) + payload
        self.sock.sendall(frame)

    def _reader(self):
        while True:
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                self.msgs.put(("DISCONNECTED", None))
                return
            if not chunk:
                self.msgs.put(("DISCONNECTED", None))
                return
            self.buf += chunk
            while True:
                # scan for framing
                i = self.buf.find(b"\x94\xc3")
                if i < 0:
                    self.buf = b""
                    break
                if i > 0:
                    self.buf = self.buf[i:]
                if len(self.buf) < 4:
                    break
                (length,) = struct.unpack(">H", self.buf[2:4])
                if length > 1024:  # bogus, resync
                    self.buf = self.buf[1:]
                    continue
                if len(self.buf) < 4 + length:
                    break
                payload = self.buf[4 : 4 + length]
                self.buf = self.buf[4 + length:]
                try:
                    fr = mesh_pb2.FromRadio()
                    fr.ParseFromString(payload)
                    self.msgs.put(("MSG", fr))
                except Exception as e:  # noqa: BLE001
                    self.msgs.put(("BADFRAME", payload))
                    print(f"[{self.name}] BAD FRAME: {e}")

    def wait_for(self, pred, timeout=20, desc="message"):
        """Return the first FromRadio matching pred; None on timeout/disconnect."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                kind, fr = self.msgs.get(timeout=0.2)
            except queue.Empty:
                continue
            if kind == "DISCONNECTED":
                print(f"[{self.name}] *** socket closed by server ***")
                return None
            if kind != "MSG":
                continue
            if pred(fr):
                return fr
        return None

    def wait_config(self, timeout=30):
        fr = self.wait_for(lambda m: m.HasField("config_complete_id"), timeout=timeout)
        if fr is None:
            return None
        self.alive = True
        assert fr.config_complete_id == self.nonce, (
            f"{self.name}: config_complete_id {fr.config_complete_id} != our nonce {self.nonce}"
        )
        print(f"[{self.name}] config handshake complete (id={fr.config_complete_id})")
        return fr

    def send_text(self, text, pkt_id, want_ack=False):
        data = mesh_pb2.Data(
            portnum=PORTNUM_TEXT, payload=text.encode(), want_response=False
        )
        pkt = mesh_pb2.MeshPacket(id=pkt_id, to=0xFFFFFFFF, decoded=data, want_ack=want_ack)
        tr = mesh_pb2.ToRadio()
        tr.packet.CopyFrom(pkt)
        self._send(tr.SerializeToString())
        return pkt_id

    def drain(self, seconds=3.0):
        """Collect messages for `seconds`; return (queue_statuses, packets)."""
        qs, pkts = [], []
        deadline = time.time() + seconds
        while time.time() < deadline:
            try:
                kind, fr = self.msgs.get(timeout=0.2)
            except queue.Empty:
                continue
            if kind != "MSG":
                continue
            if fr.HasField("queueStatus"):
                qs.append(fr.queueStatus)
            elif fr.HasField("packet"):
                pkts.append(fr.packet)
        return qs, pkts

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def check(cond, what):
    status = "PASS" if cond else "FAIL"
    print(f"  [{status}] {what}")
    if not cond:
        FAILS.append(what)


def main():
    A = Client("A", nonce=0xAAAA0001)
    time.sleep(1.0)
    B = Client("B", nonce=0xBBBB0002)  # connect while A is attached

    print("== 1. both clients complete their independent config handshakes ==")
    cfgA = A.wait_config()
    cfgB = B.wait_config()
    check(cfgA is not None, "client A completed config (with B connected)")
    check(cfgB is not None, "client B completed config (with A connected)")
    check(cfgA is not None and cfgA.config_complete_id == 0xAAAA0001, "A got its own config_complete_id")
    check(cfgB is not None and cfgB.config_complete_id == 0xBBBB0002, "B got its own config_complete_id")

    print("== 2. A sends a packet; A receives its own queue status + echo with its own id ==")
    A.send_text("hello from A", pkt_id=0x1111)
    qa = A.wait_for(lambda m: m.HasField("queueStatus") and m.queueStatus.mesh_packet_id == 0x1111, timeout=15)
    check(qa is not None, "A got queue status for its own packet id 0x1111")
    pa = A.wait_for(lambda m: m.HasField("packet") and m.packet.id == 0x1111, timeout=15)
    check(pa is not None, "A saw its own packet echoed back with id 0x1111")

    print("== 3. B also receives A's packet (live traffic fan-out) ==")
    pb = B.wait_for(lambda m: m.HasField("packet") and m.packet.id == 0x1111, timeout=10)
    check(pb is not None, "B received A's broadcast packet (fan-out, id restored)")

    print("== 4. B sends a packet; B gets its own status; A does NOT get B's ==")
    B.send_text("hello from B", pkt_id=0x2222)
    qb = B.wait_for(lambda m: m.HasField("queueStatus") and m.queueStatus.mesh_packet_id == 0x2222, timeout=15)
    check(qb is not None, "B got queue status for its own packet id 0x2222")
    qa_stolen = A.wait_for(lambda m: m.HasField("queueStatus") and m.queueStatus.mesh_packet_id == 0x2222, timeout=4)
    check(qa_stolen is None, "A did NOT receive B's queue status (owner routing)")

    print("== 5. A disconnects; B's session is undisturbed ==")
    A.close()
    time.sleep(1.5)
    B.send_text("still here", pkt_id=0x3333)
    qb3 = B.wait_for(lambda m: m.HasField("queueStatus") and m.queueStatus.mesh_packet_id == 0x3333, timeout=15)
    check(qb3 is not None, "B still works after A dropped (own status for 0x3333)")

    print("== 6. a new client can connect after A left ==")
    C = Client("C", nonce=0xCCCC0003)
    cfgC = C.wait_config()
    check(cfgC is not None and cfgC.config_complete_id == 0xCCCC0003, "C connected + config after A left")

    B.close()
    C.close()

    print()
    if FAILS:
        print(f"SMOKE RESULT: {len(FAILS)} FAILURE(S):")
        for f in FAILS:
            print(f"  - {f}")
        sys.exit(1)
    print("SMOKE RESULT: ALL CHECKS PASSED")


if __name__ == "__main__":
    main()
