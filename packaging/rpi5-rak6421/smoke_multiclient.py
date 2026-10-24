#!/usr/bin/env python3
"""Two-concurrent-client smoke test for the multi-client meshtasticd TCP API.

Speaks the raw 0x94 0xC3 framing directly so we can assert per-message
behavior (config_complete_id per client, queue-status id remapping, fan-out,
owner-only response routing, same-id collision, want_ack DMs).

Sections (which apply on hardware vs sim are printed at startup):
  1. both clients complete independent config handshakes        [hw + sim]
  2. A sends a packet; A gets its own queue status + echo       [SIM ONLY]
  3. B also receives A's packet (live-traffic fan-out)          [SIM ONLY]
  4. B sends a packet; B gets its own status; A does NOT        [hw + sim]
  5. same-id collision: both clients send the same client id    [hw + sim]
  6. want_ack DM: the routing response is owner-routed          [hw + sim]
  7. A disconnects; B's session is undisturbed                  [hw + sim]
  8. a new client can connect after A left                      [hw + sim]
  9. zero-id send gets a server-assigned id, owner-routed (F4)  [hw + sim]
 10. ~30 idle TCP connections: the admission cap rejects the
    excess and the daemon stays up, a fresh client still works   [hw + sim]

Sections 2-3 are sim-only: on stock code the multi-client fan-out is only
triggered by the radio RX path, and the simulated radio (-s) is the only one
that echoes sent packets back. On hardware they would time out, so they are
skipped (and not counted as failures) unless --sim is given.

Exit codes:
  0  all *applicable* checks passed (skipped sim-only sections don't count)
  1  one or more checks failed (a FAILS summary is printed)
  2  setup failure (daemon unreachable, protobufs missing) — no traceback
"""
import argparse
import os
import socket
import struct
import sys
import threading
import time

try:
    MP = os.path.dirname(os.path.abspath(__import__("meshtastic").__file__))
    sys.path.insert(0, os.path.join(MP, "protobuf"))
    import mesh_pb2  # noqa: E402
    import portnums_pb2  # noqa: E402
except Exception as e:  # noqa: BLE001
    print(f"SETUP FAILURE: cannot import the meshtastic package's protobufs: {e}")
    print("Install it with: pip install meshtastic")
    sys.exit(2)

PORTNUM_TEXT = portnums_pb2.TEXT_MESSAGE_APP
PORTNUM_ROUTING = portnums_pb2.ROUTING_APP
SIM_ECHO_PORTNUM = portnums_pb2.SIMULATOR_APP  # the sim echoes as Compressed blobs
ROUTING_NONE = mesh_pb2.Routing.Error.NONE
ROUTING_MAX_RETRANSMIT = mesh_pb2.Routing.Error.MAX_RETRANSMIT
# The daemon rate-limits TEXT_MESSAGE_APP to one packet per 2 s per connection
# (PhoneAPI), which is also why the sim's own NAK is expected: a want_ack DM to
# a nonexistent node is retransmitted until it fails with MAX_RETRANSMIT.
TEXT_MIN_INTERVAL = 2.3

FAILS = []


def check(cond, what):
    status = "PASS" if cond else "FAIL"
    print(f"  [{status}] {what}")
    if not cond:
        FAILS.append(what)


class Client:
    """One raw TCP API client.

    Every frame is recorded in self.history as it arrives, so all checks
    consume it order-independently (a frame seen by an earlier check is still
    visible to a later one, regardless of when the daemon sent it).
    self.alive reflects the connection and guards the negative checks: a
    dead connection would trivially "not receive" a frame, which is exactly
    the stock single-client failure mode this suite must not accept.
    """

    def __init__(self, name, nonce, host, port):
        self.name = name
        self.nonce = nonce
        self.sock = socket.create_connection((host, port), timeout=5)
        self.sock.settimeout(0.2)
        self.buf = b""
        self.history = []  # [(kind, obj)] every frame, arrival order
        self._cond = threading.Condition()
        self.alive = False
        self._last_text_send = 0.0
        self.t = threading.Thread(target=self._reader, daemon=True)
        self.t.start()
        # kick off the config handshake
        tr = mesh_pb2.ToRadio(want_config_id=nonce)
        self._send(tr.SerializeToString())

    def _send(self, payload: bytes):
        frame = b"\x94\xc3" + struct.pack(">H", len(payload)) + payload
        self.sock.sendall(frame)

    def _record(self, kind, obj):
        with self._cond:
            self.history.append((kind, obj))
            self._cond.notify_all()

    def _reader(self):
        while True:
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                break
            if not chunk:
                break
            self.buf += chunk
            while True:
                # scan for framing
                i = self.buf.find(b"\x94\xc3")
                if i < 0:
                    # Keep a trailing 0x94: it may be the first half of the
                    # next frame's magic, split across TCP reads.
                    self.buf = self.buf[-1:] if self.buf.endswith(b"\x94") else b""
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
                    self._record("MSG", fr)
                except Exception as e:  # noqa: BLE001
                    self._record("BADFRAME", payload)
                    print(f"[{self.name}] BAD FRAME: {e}")
        self.alive = False
        self._record("DISCONNECTED", None)

    def wait_for(self, pred, timeout=20, desc="message"):
        """Return the first FromRadio matching pred; None on timeout/close.

        Scans the recorded history first, so the result does not depend on
        which earlier check consumed a frame.
        """
        deadline = time.time() + timeout
        with self._cond:
            while True:
                for kind, fr in self.history:
                    if kind == "DISCONNECTED":
                        print(f"[{self.name}] *** socket closed by server ***")
                        return None
                    if kind == "MSG" and pred(fr):
                        return fr
                remaining = deadline - time.time()
                if remaining <= 0:
                    return None
                self._cond.wait(min(remaining, 0.5))

    def wait_config(self, timeout=30):
        fr = self.wait_for(lambda m: m.HasField("config_complete_id"), timeout=timeout)
        if fr is None:
            return None
        self.alive = True
        check(
            fr.config_complete_id == self.nonce,
            f"{self.name}: config_complete_id {fr.config_complete_id:#x} == our nonce {self.nonce:#x}",
        )
        print(f"[{self.name}] config handshake complete (id={fr.config_complete_id:#x})")
        return fr

    def send_text(self, text, pkt_id, want_ack=False, to=0xFFFFFFFF):
        # Pace sends on this connection: the daemon rate-limits text to
        # one packet per 2 s per client (a RATE_LIMIT_EXCEEDED NAK would
        # otherwise show up where we expect a normal send).
        gap = self._last_text_send + TEXT_MIN_INTERVAL - time.time()
        if gap > 0:
            time.sleep(gap)
        self._last_text_send = time.time()
        data = mesh_pb2.Data(portnum=PORTNUM_TEXT, payload=text.encode(), want_response=False)
        pkt = mesh_pb2.MeshPacket(id=pkt_id, to=to, decoded=data, want_ack=want_ack)
        tr = mesh_pb2.ToRadio()
        tr.packet.CopyFrom(pkt)
        self._send(tr.SerializeToString())
        return pkt_id

    # -- recorded-frame queries (order-independent) -------------------------

    def frames(self, pred):
        """All MSG frames matching pred so far, in arrival order."""
        return [fr for kind, fr in self.history if kind == "MSG" and pred(fr)]

    def queue_statuses(self, pkt_id):
        """All queue-status frames carrying pkt_id (client id, restored)."""
        return [
            fr.queueStatus
            for fr in self.frames(
                lambda m: m.HasField("queueStatus") and m.queueStatus.mesh_packet_id == pkt_id
            )
        ]

    def routing_responses(self, request_id):
        """All ROUTING_APP frames (ACK/NAK) referencing request_id."""
        return [
            fr.packet
            for fr in self.frames(
                lambda m: m.HasField("packet")
                and m.packet.decoded.portnum == PORTNUM_ROUTING
                and m.packet.decoded.request_id == request_id
            )
        ]

    def saw_echo(self, pkt_id, payload):
        """True if we saw the sim-mode echo of a packet with id pkt_id
        carrying the original text `payload`.

        SimRadio rewrites sent packets into SIMULATOR_APP blobs whose
        Compressed payload carries the original portnum + text. Matching on
        the payload (not just the id) matters in the collision case, where
        two clients' echoes share the same restored client id.
        """
        for fr in self.frames(
            lambda m: m.HasField("packet")
            and m.packet.id == pkt_id
            and m.packet.decoded.portnum == SIM_ECHO_PORTNUM
        ):
            c = mesh_pb2.Compressed()
            try:
                c.ParseFromString(fr.packet.decoded.payload)
            except Exception:  # noqa: BLE001
                continue
            if c.portnum == PORTNUM_TEXT and c.data == payload:
                return True
        return False

    def routing_error_reason(self, pkt):
        r = mesh_pb2.Routing()
        try:
            r.ParseFromString(pkt.decoded.payload)
        except Exception:  # noqa: BLE001
            return -1
        return r.error_reason

    def drain(self, seconds=3.0):
        """Wait `seconds`, collecting every frame that arrives.

        Returns the complete MSG history (the record all checks draw on).
        """
        deadline = time.time() + seconds
        with self._cond:
            while time.time() < deadline:
                self._cond.wait(min(0.5, max(0.0, deadline - time.time())))
        return [fr for kind, fr in self.history if kind == "MSG"]

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def negative_ok(client, pred, window, what):
    """Liveness-guarded negative check.

    Waits `window` seconds, then asserts that no matching frame arrived AND
    that the connection was still alive (a dead connection would trivially
    pass the negative — the single-client behavior this suite must detect).
    """
    client.drain(window)
    hits = client.frames(pred)
    alive = client.alive
    if not alive:
        print(f"  ! [{client.name}] connection dropped during the {window}s negative window — result not trustworthy")
    if hits:
        print(f"  ! [{client.name}] unexpectedly received {len(hits)} frame(s) that only the sender should see")
    check(alive and not hits, what)


def main():
    ap = argparse.ArgumentParser(description="multi-client meshtasticd TCP API smoke test")
    ap.add_argument("--host", default="127.0.0.1", help="daemon host (default 127.0.0.1)")
    ap.add_argument("--port", type=int, default=4403, help="daemon TCP API port (default 4403)")
    ap.add_argument(
        "-s",
        "--sim",
        action="store_true",
        help="daemon was started with -s (simulated radio): enables the sim-only sections 2-3",
    )
    args = ap.parse_args()

    print(f"smoke_multiclient: {args.host}:{args.port}  mode={'sim' if args.sim else 'hardware'}")
    if args.sim:
        print("  applicable sections: 1-10 (all)")
    else:
        print("  applicable sections: 1, 4, 5, 6, 7, 8, 9, 10")
        print("  skipped (sim-only): 2 (own echo), 3 (fan-out) — stock fan-out needs")
        print("    real radio RX traffic; the sim echo (-s) is the only trigger.")
        print("    Run the daemon with -s and pass -s here to exercise them.")

    try:
        A = Client("A", nonce=0xAAAA0001, host=args.host, port=args.port)
        time.sleep(1.0)
        B = Client("B", nonce=0xBBBB0002, host=args.host, port=args.port)  # connect while A is attached
    except OSError as e:  # ConnectionRefused/timeout/etc.
        print(f"SETUP FAILURE: cannot connect to meshtasticd at {args.host}:{args.port}: {e}")
        print("Is the daemon running? (on hardware, or in sim mode: meshtasticd -s)")
        print("Exit codes: 0 = all applicable checks passed, 1 = check failed, 2 = setup failure.")
        sys.exit(2)

    print("== 1. both clients complete their independent config handshakes ==")
    cfgA = A.wait_config()
    cfgB = B.wait_config()
    check(cfgA is not None, "client A completed config (with B connected)")
    check(cfgB is not None, "client B completed config (with A connected)")

    if args.sim:
        print("== 2. A sends a packet; A receives its own queue status + echo (SIM ONLY) ==")
        A.send_text("hello from A", pkt_id=0x1111)
        qa = A.wait_for(
            lambda m: m.HasField("queueStatus") and m.queueStatus.mesh_packet_id == 0x1111, timeout=15
        )
        check(qa is not None, "A got queue status for its own packet id 0x1111")
        pa = A.wait_for(lambda m: m.HasField("packet") and m.packet.id == 0x1111, timeout=15)
        check(pa is not None, "A saw its own packet echoed back with id 0x1111")
        check(A.saw_echo(0x1111, b"hello from A"), "the echo carries A's original text payload")

        print("== 3. B also receives A's packet (live traffic fan-out, SIM ONLY) ==")
        pb = B.wait_for(lambda m: m.HasField("packet") and m.packet.id == 0x1111, timeout=10)
        check(pb is not None, "B received A's broadcast packet (fan-out, id restored)")
        check(
            B.saw_echo(0x1111, b"hello from A"),
            "B's copy of the echo is A's text (fan-out delivers the real traffic)",
        )
    else:
        print("== 2. A's own echo (SIM ONLY — skipped on hardware) ==")
        print("== 3. live-traffic fan-out (SIM ONLY — skipped on hardware) ==")

    print("== 4. B sends a packet; B gets its own status; A does NOT get B's ==")
    B.send_text("hello from B", pkt_id=0x2222)
    qb = B.wait_for(
        lambda m: m.HasField("queueStatus") and m.queueStatus.mesh_packet_id == 0x2222, timeout=15
    )
    check(qb is not None, "B got queue status for its own packet id 0x2222")
    negative_ok(
        A,
        lambda m: m.HasField("queueStatus") and m.queueStatus.mesh_packet_id == 0x2222,
        4,
        "A did NOT receive B's queue status (owner routing)",
    )

    print("== 5. same-id collision: A and B send packets with the SAME client id ==")
    X = 0x5A5A
    A.send_text("collide-A", pkt_id=X)
    B.send_text("collide-B", pkt_id=X)
    # Per-client id remapping means each client's queue statuses come back
    # owner-routed with its own client id restored. Exactly one status is
    # sent per accepted send; in sim the simulated transmit sends one more
    # per packet, so the expected per-client count is mode-dependent.
    expected = 2 if args.sim else 1
    A.drain(8.0)
    B.drain(8.0)
    a_qs = A.queue_statuses(X)
    b_qs = B.queue_statuses(X)
    check(
        len(a_qs) == expected,
        f"A got exactly its own {expected} queue status(es) for shared id {X:#x} (got {len(a_qs)})",
    )
    check(
        len(b_qs) == expected,
        f"B got exactly its own {expected} queue status(es) for shared id {X:#x} (got {len(b_qs)})",
    )
    if args.sim:
        # Without per-client remapping the two packets would share one mesh
        # id and each client could not tell its own echo apart from the other's.
        check(A.saw_echo(X, b"collide-A"), "A saw its own echo (payload collide-A) with id restored")
        check(B.saw_echo(X, b"collide-B"), "B saw its own echo (payload collide-B) with id restored")

    print("== 6. want_ack DM: the routing response is owner-routed (request_id) ==")
    DM = 0x6666
    # A DM (not broadcast) with want_ack, to a node that does not exist, so it
    # is never acked: after retransmissions run out the router emits a routing
    # NAK (MAX_RETRANSMIT) whose decoded.request_id references the DM's id.
    B.send_text("dm from B", pkt_id=DM, want_ack=True, to=0x42424242)
    qd = B.wait_for(
        lambda m: m.HasField("queueStatus") and m.queueStatus.mesh_packet_id == DM, timeout=15
    )
    check(qd is not None, "B got queue status for its want_ack DM id 0x6666")
    nak = B.wait_for(
        lambda m: m.HasField("packet")
        and m.packet.decoded.portnum == PORTNUM_ROUTING
        and m.packet.decoded.request_id == DM,
        timeout=45,
        desc="routing response for the DM (retransmit timeout ~20 s)",
    )
    check(nak is not None, "B received a routing response for its DM (request_id 0x6666 restored)")
    if nak is not None:
        reason = B.routing_error_reason(nak.packet)
        # Sim (no real radio) always ends in MAX_RETRANSMIT; on hardware an
        # unknown-node DM may instead NAK with NO_ROUTE/TIMEOUT, or be ACKed.
        accepted = (
            (ROUTING_MAX_RETRANSMIT,)
            if args.sim
            else (
                ROUTING_NONE,
                ROUTING_MAX_RETRANSMIT,
                mesh_pb2.Routing.Error.NO_ROUTE,
                mesh_pb2.Routing.Error.TIMEOUT,
                mesh_pb2.Routing.Error.GOT_NAK,
            )
        )
        print(f"      (routing error_reason={reason})")
        check(reason in accepted, f"routing response is a sane ACK/NAK (reason={reason})")
    negative_ok(
        A,
        lambda m: m.HasField("packet")
        and m.packet.decoded.portnum == PORTNUM_ROUTING
        and m.packet.decoded.request_id == DM,
        5,
        "A did NOT receive B's DM routing response (owner routing of request_id)",
    )
    check(
        A.alive and not A.queue_statuses(DM),
        "A did NOT receive B's DM queue status (owner routing of the remapped id)",
    )

    print("== 7. A disconnects; B's session is undisturbed ==")
    A.close()
    time.sleep(1.5)
    B.send_text("still here", pkt_id=0x3333)
    qb3 = B.wait_for(
        lambda m: m.HasField("queueStatus") and m.queueStatus.mesh_packet_id == 0x3333, timeout=15
    )
    check(qb3 is not None, "B still works after A dropped (own status for 0x3333)")

    print("== 8. a new client can connect after A left ==")
    try:
        C = Client("C", nonce=0xCCCC0003, host=args.host, port=args.port)
        cfgC = C.wait_config()
        check(cfgC is not None, "C connected + config after A left")
    except OSError as e:  # daemon died mid-run
        check(False, f"C could not connect after A left ({e})")
        C = None

    print("== 9. zero-id send: the server assigns a client-visible id, owner-routed (F4) ==")
    D = None
    try:
        D = Client("D", nonce=0x0D0D0004, host=args.host, port=args.port)
        cfgD = D.wait_config()
        check(cfgD is not None, "D completed config (B and C connected)")
        D.send_text("zero id send", pkt_id=0)
        # D must get a queue status for its zero-id send carrying a NON-zero,
        # server-assigned client-visible id (before F4 the mesh layer generated
        # an untracked id that was fanned out to every client).
        fr9 = D.wait_for(
            lambda m: m.HasField("queueStatus") and m.queueStatus.mesh_packet_id != 0,
            timeout=15,
        )
        check(fr9 is not None, "D got a queue status for its zero-id send")
        if fr9 is not None:
            gen_id = fr9.queueStatus.mesh_packet_id
            check(gen_id != 0, f"D's zero-id send got a server-assigned non-zero id ({gen_id:#x})")
            D.drain(3.0)
            B.drain(3.0)
            check(
                not B.queue_statuses(gen_id),
                "B did NOT receive D's zero-id-send queue status (owner routing, not fan-out)",
            )
            if args.sim:
                check(D.saw_echo(gen_id, b"zero id send"), "D saw its own echo with the assigned id")
                check(
                    B.saw_echo(gen_id, b"zero id send"),
                    "B's fan-out copy of the echo carries the client-visible id (no internal server id on the wire)",
                )
    except OSError as e:
        check(False, f"D could not connect for the zero-id check ({e})")
    if D is not None:
        D.close()

    print("== 10. admission cap: ~30 idle TCP connections, daemon stays up (F1) ==")
    # Open ~30 sequential idle (no API traffic) TCP connections. The daemon
    # caps concurrent sessions at 8; connections beyond the cap must be
    # rejected (closed) by the daemon, and the daemon must stay up.
    MAX_API_CLIENTS = 8
    raw = []
    for i in range(30):
        try:
            s = socket.create_connection((args.host, args.port), timeout=5)
            s.settimeout(0.3)
            raw.append(s)
        except OSError as e:
            check(False, f"raw connection {i} failed ({e})")
    # The daemon's accept loop runs on the cooperative main loop (~100 ms
    # period) and drains the whole backlog in one pass; give it ample time.
    time.sleep(4.0)
    open_n = 0
    closed_n = 0
    for s in raw:
        try:
            data = s.recv(1)
            if data == b"":
                closed_n += 1  # EOF: the daemon rejected (stopped) this connection
            else:
                open_n += 1
        except socket.timeout:
            open_n += 1  # still open and idle
        except OSError:
            closed_n += 1  # reset/closed
    for s in raw:
        try:
            # Linger-0 close: send RST instead of FIN, modeling clients that
            # went away (process exit / network failure). The daemon notices
            # the dead peer on its next socket read; a clean FIN is only
            # detected on the next write or after the 15-min idle timeout
            # (pre-existing upstream WiFiClient behavior, out of scope here).
            s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
            s.close()
        except OSError:
            pass
    check(
        open_n <= MAX_API_CLIENTS,
        f"at most {MAX_API_CLIENTS} idle connections are held open (open {open_n})",
    )
    check(closed_n >= 1, f"connections beyond the cap were rejected/closed ({closed_n} closed)")
    # Let the daemon reap the (now dead) idle sessions it accepted, then a
    # fresh, normal client must be able to complete a config handshake.
    time.sleep(3.0)
    E = None
    try:
        E = Client("E", nonce=0x0E0E0005, host=args.host, port=args.port)
        cfgE = E.wait_config()
        check(cfgE is not None, "fresh client completes config after the connection storm (daemon alive)")
    except OSError as e:
        check(False, f"the daemon no longer accepts API clients after the storm ({e})")
    if E is not None:
        E.close()

    B.close()
    if C is not None:
        C.close()

    print()
    if FAILS:
        print(f"SMOKE RESULT: {len(FAILS)} FAILURE(S):")
        for f in FAILS:
            print(f"  - {f}")
        sys.exit(1)
    print("SMOKE RESULT: ALL CHECKS PASSED")
    if not args.sim:
        print("  (sections 2-3 skipped: sim-only — run with -s against a daemon started with -s)")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\ninterrupted")
        sys.exit(130)
