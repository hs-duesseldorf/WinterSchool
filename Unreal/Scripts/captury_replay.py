#!/usr/bin/env python3
"""Captury Replay stand-in for Linux: streams recorded takes through the Captury network protocol (TCP 2101 + UDP
poses, as in Captury's open RemoteCaptury library), so Unreal's Captury Live Link plugin connects to this exactly as
it would to Captury Live in the studio. Use it to test the live pipeline at home (Simulate).

  Scripts/captury_replay.py Saved/Mocap/Takes/Dab.json [more takes...] [--speed 1] [--once] [--port 2101]

Every actor of every take is its own Captury actor, Live Link subjects Performer1, Performer2, ... (or --name). All loop,
the frame rate is the take's own. Takes come from Scripts/export_mocap_take.sh (any anim sequence in the project).
In Unreal: Window > Live Link > Source > Captury Live, IP 127.0.0.1 (or this machine's address).

--selftest decodes the generated packets the way the plugin does and checks that the result reproduces the take.
"""
import argparse, json, math, socket, struct, sys, threading, time
import numpy as np

# packet types (RemoteCaptury.h)
ACTORS, ACTOR2, STREAM, STREAM_ACK, GET_TIME, TIME, POSE2 = 1, 59, 5, 6, 16, 17, 54
GET_TIME2, TIME2, HELLO, GET_FRAMERATE, FRAMERATE = 68, 69, 73, 78, 79
GET_STATUS, STATUS = 55, 56
STREAM_POSES, STREAM_TCP, STREAM_ONLY_ROOT_TRANSLATION = 0x0001, 0x1000, 0x2000

S = np.diag([1.0, -1.0, 1.0])                                  # Unreal (left-handed) <-> right-handed mirror
RX = np.array([[1.0, 0, 0], [0, 0, -1.0], [0, 1.0, 0]])        # +90 deg about X: Captury Y-up -> Z-up
TO_CAPTURY = RX.T @ S                                           # Unreal vector (cm) -> Captury vector (/10 mm)


def quat_to_mat(q):  # (..., 4) xyzw -> (..., 3, 3)
    x, y, z, w = np.moveaxis(q / np.linalg.norm(q, axis=-1, keepdims=True), -1, 0)
    return np.stack([np.stack([1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)], -1),
                     np.stack([2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)], -1),
                     np.stack([2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)], -1)], -2)


def mat_to_quat(m):  # (3, 3) -> xyzw, w >= 0
    t = np.trace(m)
    if t > 0:
        s = math.sqrt(t + 1) * 2; q = [(m[2, 1] - m[1, 2]) / s, (m[0, 2] - m[2, 0]) / s, (m[1, 0] - m[0, 1]) / s, s / 4]
    else:
        i = int(np.argmax(np.diag(m))); j, k = (i + 1) % 3, (i + 2) % 3
        s = math.sqrt(1 + m[i, i] - m[j, j] - m[k, k]) * 2
        q = [0.0] * 4; q[i] = s / 4; q[j] = (m[j, i] + m[i, j]) / s; q[k] = (m[k, i] + m[i, k]) / s
        q[3] = (m[k, j] - m[j, k]) / s
    q = np.array(q)
    return -q if q[3] < 0 else q


def euler_zyx(m):  # (..., 3, 3) with R = Rz(z) Ry(y) Rx(x) -> (..., 3) degrees x, y, z
    y = np.arcsin(np.clip(-m[..., 2, 0], -1, 1))
    x = np.arctan2(m[..., 2, 1], m[..., 2, 2]); z = np.arctan2(m[..., 1, 0], m[..., 0, 0])
    return np.degrees(np.stack([x, y, z], -1))


class Actor:
    """One streamed person: Captury skeleton definition plus every frame's pose values."""

    def __init__(self, actor_id, name, a):
        self.id, self.name = actor_id, name
        self.bones, self.parents = a["bones"], a["parents"]
        ref = np.array(a["ref"], dtype=np.float64)                       # (J, 7) pos cm + quat, Unreal
        fr = np.array(a["frames"], dtype=np.float64).reshape(len(a["frames"]), len(self.bones), 7)
        uref, u = quat_to_mat(ref[:, 3:]), quat_to_mat(fr[..., 3:])
        # Captury's bind orientation B = Rx^T S Uref S, pose rotation P = Rx^T S U Uref^T S Rx (see the plugin's newPose)
        self.bind = [mat_to_quat(RX.T @ S @ m @ S) for m in uref]
        p = RX.T @ S @ u @ np.swapaxes(uref, -1, -2)[None] @ S @ RX
        self.rot = euler_zyx(p).astype(np.float32)                       # (F, J, 3) degrees
        self.pos = (10 * fr[..., :3] @ TO_CAPTURY.T).astype(np.float32)  # (F, J, 3) mm, global
        refpos = ref[:, :3]
        self.offsets = [10 * TO_CAPTURY @ (refpos[i] - (refpos[q] if q >= 0 else 0)) for i, q in enumerate(self.parents)]
        self.num_frames = fr.shape[0]
        self.ref, self.frames_raw = ref, fr

    def actor_packet(self):
        body = struct.pack("<32sii", self.name.encode()[:31], self.id, len(self.bones))
        for b, q, off, bq in zip(self.bones, self.parents, self.offsets, self.bind):
            body += struct.pack("<i3f3f", q, *off, *bq[:3]) + b.encode() + b"\0"
        return struct.pack("<ii", ACTOR2, 8 + len(body)) + body

    def values(self, f, only_root):
        if only_root:
            v = np.concatenate([self.pos[f, 0], self.rot[f].reshape(-1)])
            return v, -len(v)
        v = np.concatenate([self.pos[f], self.rot[f]], -1).reshape(-1)
        return v, len(v)

    def pose_packet(self, f, timestamp_us, only_root):
        v, n = self.values(f, only_root)
        head = struct.pack("<iiiQBBBBi", POSE2, 0, self.id, timestamp_us, 100, 100, 0, 0, n)
        data = head + v.astype("<f4").tobytes()
        return data[:4] + struct.pack("<i", len(data)) + data[8:]


class Client:
    def __init__(self, server, sock, addr):
        self.server, self.sock, self.addr = server, sock, addr
        self.what, self.udp_target, self.lock = 0, None, threading.Lock()

    def send(self, data):
        with self.lock: self.sock.sendall(data)

    def run(self):
        srv = self.server
        print(f"client {self.addr[0]}:{self.addr[1]} connected")
        try:
            self.send(struct.pack("<iii", ACTORS, 12, len(srv.actors)))
            for a in srv.actors: self.send(a.actor_packet())
            self.send(struct.pack("<iiii", FRAMERATE, 16, *srv.fps))
            self.send(struct.pack("<ii", HELLO, 8))
            while True:
                head = self._recv(8)
                if head is None: break
                typ, size = struct.unpack("<ii", head)
                body = self._recv(size - 8) if size > 8 else b""
                if body is None: break
                self.handle(typ, body)
        except OSError as e:
            print(f"client {self.addr[0]}: {e}")
        finally:
            srv.drop(self)
            print(f"client {self.addr[0]}:{self.addr[1]} disconnected")

    def _recv(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk: return None
            buf += chunk
        return buf

    def handle(self, typ, body):
        if typ == STREAM:
            what = struct.unpack_from("<i", body)[0]
            ip, port = (struct.unpack_from("<IH", body, 8) if len(body) >= 14 else (0, 0))
            host = socket.inet_ntoa(struct.pack("<I", ip)) if ip else self.addr[0]
            port = socket.ntohs(port)
            if host == "0.0.0.0": host = self.addr[0]
            self.udp_target = (host, port) if port else self.udp_target
            self.what = what
            mode = "tcp" if what & STREAM_TCP else f"udp {self.udp_target}"
            print(f"client {self.addr[0]}: stream {what:#x} ({'nothing' if not what & STREAM_POSES else mode})")
            self.send(struct.pack("<ii", STREAM_ACK, 8))
        elif typ == GET_FRAMERATE:
            self.send(struct.pack("<iiii", FRAMERATE, 16, *self.server.fps))
        elif typ == GET_TIME:
            self.send(struct.pack("<iiQ", TIME, 16, self.server.now_us()))
        elif typ == GET_TIME2:
            time_id = struct.unpack_from("<i", body, 8)[0] if len(body) >= 12 else 0
            self.send(struct.pack("<iiQi", TIME2, 20, self.server.now_us(), time_id))
        elif typ == GET_STATUS:
            msg = b"replaying\0"; self.send(struct.pack("<ii", STATUS, 8 + len(msg)) + msg)
        else:
            print(f"client {self.addr[0]}: ignoring request {typ}")


class Server:
    def __init__(self, actors, fps, port, speed, once):
        self.actors, self.fps, self.port, self.speed, self.once = actors, fps, port, speed, once
        self.clients, self.lock, self.t0 = [], threading.Lock(), time.monotonic()
        self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.udp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.udp.bind(("0.0.0.0", port))

    def now_us(self):
        return int((time.monotonic() - self.t0) * 1e6)

    def drop(self, c):
        with self.lock:
            if c in self.clients: self.clients.remove(c)

    def accept_loop(self):
        tcp = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        tcp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        tcp.bind(("0.0.0.0", self.port)); tcp.listen()
        while True:
            s, addr = tcp.accept()
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            c = Client(self, s, addr)
            with self.lock: self.clients.append(c)
            threading.Thread(target=c.run, daemon=True).start()

    def udp_loop(self):  # the plugin sends keep-alive packets to punch through firewalls; nothing to answer
        while True:
            try: self.udp.recvfrom(2048)
            except OSError: time.sleep(0.1)

    def play(self):
        period = self.fps[1] / self.fps[0] / self.speed
        length = max(a.num_frames for a in self.actors)
        frame, start, loops = 0, time.monotonic(), 0
        while True:
            ts = int(frame * self.fps[1] / self.fps[0] * 1e6) + loops * int(length * self.fps[1] / self.fps[0] * 1e6)
            with self.lock: clients = list(self.clients)
            for c in clients:
                if not c.what & STREAM_POSES: continue
                only_root = bool(c.what & STREAM_ONLY_ROOT_TRANSLATION)
                for a in self.actors:
                    pkt = a.pose_packet(min(frame, a.num_frames - 1), ts, only_root)
                    try:
                        if c.what & STREAM_TCP: c.send(pkt)
                        elif c.udp_target: self.udp.sendto(pkt, c.udp_target)
                    except OSError: pass
            frame += 1
            if frame >= length:
                if self.once: print("take finished"); return
                frame, loops = 0, loops + 1
            start += period
            delay = start - time.monotonic()
            if delay > 0: time.sleep(delay)
            else: start = time.monotonic()  # fell behind (suspended?), don't burst


def load_actors(paths, names):
    actors, fps = [], None
    for path in paths:
        take = json.load(open(path))
        for i, a in enumerate(take["actors"]):
            name = names.pop(0) if names else f"Performer{len(actors) + 1}"
            actors.append(Actor(0x1000 + len(actors) + 1, name, a))
            print(f"{path}: {a['name']} as {name}, {len(a['bones'])} joints, {actors[-1].num_frames} frames")
        f = take["fps"]
        if fps and fps != f: print(f"warning: {path} runs at {f[0]}/{f[1]} fps, streaming everything at {fps[0]}/{fps[1]}")
        fps = fps or f
    return actors, fps


def plugin_decode(actor, values, only_root):
    """The plugin's newPose (CapturyLiveLinkSource.cpp) on numpy: Captury joints + pose -> Unreal local transforms."""
    def q_mul(a, b):
        ax, ay, az, aw = a; bx, by, bz, bw = b
        return np.array([aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
                         aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz])
    def q_axis(ax, ang): return np.array([*(np.array(ax) * math.sin(ang / 2)), math.cos(ang / 2)])
    def q_inv(q): return np.array([-q[0], -q[1], -q[2], q[3]])
    def q_rot(q, v): return quat_to_mat(q[None])[0] @ v
    n = len(actor.bones)
    if only_root:
        trans0, rots = values[:3], values[3:].reshape(n, 3)
    else:
        trans0, rots = values.reshape(n, 6)[0, :3], values.reshape(n, 6)[:, 3:]
    out, glob_pose, glob_bind = [], [], []
    for i in range(n):
        rx, ry, rz = np.radians(rots[i])
        pose = q_mul(q_mul(q_axis((0, 0, 1), rz), q_axis((0, 1, 0), ry)), q_axis((1, 0, 0), rx))
        glob_pose.append(pose)
        p = actor.parents[i]
        if p >= 0: pose = q_mul(q_inv(glob_pose[p]), pose)
        b = actor.bind[i]; glob_bind.append(b)
        if i == 0:
            rot = q_mul(q_mul(q_axis((1, 0, 0), math.pi / 2), pose), b)
            trans = q_rot(q_axis((1, 0, 0), math.pi / 2), trans0 * 0.1)
        else:
            trans = q_rot(q_inv(glob_bind[p]), actor.offsets[i] * 0.1)
            rot = q_mul(q_mul(q_mul(q_inv(glob_bind[p]), b), q_inv(b)), q_mul(pose, b))
        rot = np.array([rot[0], -rot[1], rot[2], -rot[3]]); trans = trans * np.array([1, -1, 1])
        out.append((rot, trans))
    return out


def selftest(actors):
    worst_r = worst_t = 0.0
    for a in actors:
        for f in sorted({0, a.num_frames // 2, a.num_frames - 1}):
            for only_root in (True, False):
                v, _ = a.values(f, only_root)
                local = plugin_decode(a, v.astype(np.float64), only_root)
                u = quat_to_mat(a.frames_raw[f, :, 3:]); uref = quat_to_mat(a.ref[:, 3:])
                for i, (q, t) in enumerate(local):  # what the take's sequence has as local bone transform
                    p = a.parents[i]
                    if p < 0: want_m, want_t = u[i], a.frames_raw[f, i, :3]
                    else:  # Captury streams rotations only, so joints below the root keep their reference offset
                        want_m, want_t = u[p].T @ u[i], uref[p].T @ (a.ref[i, :3] - a.ref[p, :3])
                    m = quat_to_mat(q[None])[0]
                    worst_r = max(worst_r, math.degrees(math.acos(np.clip((np.trace(m.T @ want_m) - 1) / 2, -1, 1))))
                    worst_t = max(worst_t, float(np.linalg.norm(t - want_t)))
    print(f"selftest: worst rotation error {worst_r:.4f} deg, worst joint position error {worst_t:.4f} cm")
    return worst_r < 0.05 and worst_t < 0.05


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("takes", nargs="+")
    ap.add_argument("--name", action="append", default=[], help="Live Link subject name per actor (repeatable)")
    ap.add_argument("--port", type=int, default=2101)
    ap.add_argument("--speed", type=float, default=1.0)
    ap.add_argument("--once", action="store_true", help="play once instead of looping")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    actors, fps = load_actors(args.takes, list(args.name))
    if args.selftest: sys.exit(0 if selftest(actors) else 1)
    srv = Server(actors, fps, args.port, args.speed, args.once)
    threading.Thread(target=srv.accept_loop, daemon=True).start()
    threading.Thread(target=srv.udp_loop, daemon=True).start()
    print(f"Captury replay on port {args.port} (tcp + udp), {fps[0] / fps[1]:g} fps, Ctrl+C to stop")
    try: srv.play()
    except KeyboardInterrupt: pass


if __name__ == "__main__":
    main()
