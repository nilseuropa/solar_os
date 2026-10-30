"""SC2-1 reference implementation in CPython (independent of the C core).

Primitives come from libsodium through PyNaCl (Ed25519, X25519,
XChaCha20-Poly1305, SHA-256) and from the Python standard library (HMAC,
HKDF). Everything SC2 builds on top (labels, byte layouts, X3DH, the double
ratchet, sender chains, safety numbers, padding, frames, state blobs) is
written out here a second time, so the generated vectors are a real
cross-check of protocol/sc2.

SPDX-License-Identifier: MIT
"""

import base64
import binascii
import hashlib
import hmac
import json
import struct

import nacl.bindings as nb
import nacl.exceptions

SUITE = 1
NO_OPK = 0xFFFFFFFF
MAX_SKIP = 32
MAX_STORED = 32
SKIP_TTL = 7 * 24 * 3600
BUCKETS = (256, 512, 1024, 2048, 4096, 8192)

L_DK = b"SC2-DK\x01"
L_SPK = b"SC2-SPK\x01"
L_AUTH = b"SC2-AUTH\x01"
L_X3DH = b"SC2-X3DH\x01"
L_RK = b"SC2-RK\x01"
L_SENDER = b"SC2-SENDER\x01"
L_ROOM = b"SC2-ROOM\x01"
L_PUB = b"SC2-PUB\x01"
L_SAFETY = b"SC2-SAFETY\x01"
L_SAFETY_FULL = b"SC2-SAFETY-FULL\x01"
L_COMMIT = b"SC2-COMMIT\x01"
L_MOD = b"SC2-MOD\x01"
L_INVITE = b"SC2-INVITE\x01"
L_JOIN = b"SC2-JOIN\x01"
COMMIT_OPS = {"create": 1, "add": 2, "remove": 3, "promote": 4, "demote": 5, "rotate": 6}
MOD_ACTIONS = {"remove": 1, "ban": 2, "unban": 3}
ROOM_MEMBERS_MAX = 32


class Rng:
    """Deterministic byte source that records what was drawn."""

    def __init__(self, data: bytes):
        self.data = data
        self.pos = 0

    def __call__(self, n: int) -> bytes:
        if self.pos + n > len(self.data):
            raise ValueError("rng exhausted")
        out = self.data[self.pos:self.pos + n]
        self.pos += n
        return out

    def done(self) -> bool:
        return self.pos == len(self.data)


def stream(label: str, n: int) -> bytes:
    """Reproducible pseudo-random test bytes (not used by the C code)."""
    out = b""
    i = 0
    while len(out) < n:
        out += hashlib.sha256(b"SC2-TEST-STREAM\x00" + label.encode() + struct.pack(">I", i)).digest()
        i += 1
    return out[:n]


# ---------------- hashing ----------------

def sha256(b: bytes) -> bytes:
    h = hashlib.sha256(b).digest()
    assert h == nb.crypto_hash_sha256(b)
    return h


def hmac256(key: bytes, msg: bytes) -> bytes:
    return hmac.new(key, msg, hashlib.sha256).digest()


def hkdf_extract(salt: bytes, ikm: bytes) -> bytes:
    if not salt:
        salt = bytes(32)
    return hmac256(salt, ikm)


def hkdf_expand(prk: bytes, info: bytes, n: int) -> bytes:
    out, t, c = b"", b"", 1
    while len(out) < n:
        t = hmac256(prk, t + info + bytes([c]))
        out += t
        c += 1
    return out[:n]


def hkdf(salt: bytes, ikm: bytes, info: bytes, n: int) -> bytes:
    return hkdf_expand(hkdf_extract(salt, ikm), info, n)


# ---------------- curves / AEAD ----------------

def ed_public(seed: bytes) -> bytes:
    pk, _ = nb.crypto_sign_seed_keypair(seed)
    return pk


def ed_sign(seed: bytes, msg: bytes) -> bytes:
    pk, sk = nb.crypto_sign_seed_keypair(seed)
    return nb.crypto_sign(msg, sk)[:64]


def ed_verify(pk: bytes, msg: bytes, sig: bytes) -> bool:
    try:
        nb.crypto_sign_open(sig + msg, pk)
        return True
    except (nacl.exceptions.BadSignatureError, nacl.exceptions.ValueError, nacl.exceptions.CryptoError):
        return False


def x_public(priv: bytes) -> bytes:
    return nb.crypto_scalarmult_base(priv)


def x_dh(priv: bytes, peer: bytes) -> bytes:
    """Raises ValueError for all-zero / small-order results (libsodium)."""
    try:
        out = nb.crypto_scalarmult(priv, peer)
    except nacl.exceptions.RuntimeError as e:
        raise ValueError("x25519 rejected") from e
    if out == bytes(32):
        raise ValueError("x25519 all-zero")
    return out


def aead_seal(key: bytes, nonce: bytes, pt: bytes, ad: bytes) -> bytes:
    return nb.crypto_aead_xchacha20poly1305_ietf_encrypt(pt, ad, nonce, key)


def aead_open(key: bytes, nonce: bytes, ct: bytes, ad: bytes) -> bytes:
    return nb.crypto_aead_xchacha20poly1305_ietf_decrypt(ct, ad, nonce, key)


# ---------------- codecs ----------------

def b64(b: bytes) -> str:
    return base64.b64encode(b).decode()


def unb64(s: str) -> bytes:
    return base64.b64decode(s, validate=True)


def pad(b: bytes) -> bytes:
    for size in BUCKETS:
        if len(b) + 1 <= size:
            return b + b"\x80" + bytes(size - len(b) - 1)
    raise ValueError("too large")


def unpad(b: bytes) -> bytes:
    if len(b) not in BUCKETS:
        raise ValueError("bad bucket")
    s = b.rstrip(b"\x00")
    if not s or s[-1] != 0x80:
        raise ValueError("bad padding")
    out = s[:-1]
    if len(pad(out)) != len(b):
        raise ValueError("not minimal")
    return out


def dumps(obj) -> str:
    return json.dumps(obj, separators=(",", ":"), ensure_ascii=False)


# ---------------- identity ----------------

def device_id(ik: bytes) -> bytes:
    return sha256(ik)[:16]


class Identity:
    def __init__(self, rng):
        self.ik_seed = rng(32)
        self.dk_priv = rng(32)
        self.ik_pub = ed_public(self.ik_seed)
        self.dk_pub = x_public(self.dk_priv)
        self.dk_sig = ed_sign(self.ik_seed, L_DK + self.dk_pub)
        self.id = device_id(self.ik_pub)


class Spk:
    def __init__(self, spk_id, ident, rng):
        self.id = spk_id
        self.priv = rng(32)
        self.pub = x_public(self.priv)
        self.sig = ed_sign(ident.ik_seed, L_SPK + struct.pack(">I", spk_id) + self.pub)


class Opk:
    def __init__(self, opk_id, rng):
        self.id = opk_id
        self.priv = rng(32)
        self.pub = x_public(self.priv)


def auth_bytes(relay_id: str, challenge: bytes, ik_pub: bytes) -> bytes:
    r = relay_id.encode()
    return L_AUTH + bytes([len(r)]) + r + challenge + ik_pub


# ---------------- X3DH ----------------

def x3dh_initiate(alice, bundle, rng):
    assert ed_verify(bundle["ik"], L_DK + bundle["dk"], bundle["dk_sig"])
    assert ed_verify(bundle["ik"], L_SPK + struct.pack(">I", bundle["spk_id"]) + bundle["spk"], bundle["spk_sig"])
    ek = rng(32)
    ek_pub = x_public(ek)
    dhs = [x_dh(alice.dk_priv, bundle["spk"]), x_dh(ek, bundle["dk"]), x_dh(ek, bundle["spk"])]
    if bundle["opk_id"] != NO_OPK:
        dhs.append(x_dh(ek, bundle["opk"]))
    sk = hkdf(bytes(32), b"\xff" * 32 + b"".join(dhs), L_X3DH, 32)
    ad = alice.ik_pub + bundle["ik"]
    return sk, ad, {"ek": ek_pub, "spk_id": bundle["spk_id"], "opk_id": bundle["opk_id"]}


def x3dh_respond(bob, spk, opk, alice_ik, alice_dk, alice_dk_sig, hdr):
    assert ed_verify(alice_ik, L_DK + alice_dk, alice_dk_sig)
    assert hdr["spk_id"] == spk.id
    dhs = [x_dh(spk.priv, alice_dk), x_dh(bob.dk_priv, hdr["ek"]), x_dh(spk.priv, hdr["ek"])]
    if hdr["opk_id"] != NO_OPK:
        assert opk is not None and opk.id == hdr["opk_id"]
        dhs.append(x_dh(opk.priv, hdr["ek"]))
    sk = hkdf(bytes(32), b"\xff" * 32 + b"".join(dhs), L_X3DH, 32)
    return sk, alice_ik + bob.ik_pub


# ---------------- Double Ratchet ----------------

def chain_step(ck: bytes):
    return hmac256(ck, b"\x01"), hmac256(ck, b"\x02")


def kdf_rk(rk: bytes, dh: bytes):
    out = hkdf(rk, dh, L_RK, 64)
    return out[:32], out[32:]


def dm_header(h, frm: bytes, to: bytes) -> bytes:
    out = b"SC2D" + bytes([SUITE]) + frm + to + h["dh"] + struct.pack(">II", h["pn"], h["n"])
    if h.get("x3dh"):
        x = h["x3dh"]
        out += b"\x01" + x["ek"] + struct.pack(">II", x["spk_id"], x["opk_id"])
    else:
        out += b"\x00"
    return out


F_SEND, F_RECV, F_X3DH, F_INIT = 1, 2, 4, 8


class Ratchet:
    def __init__(self):
        self.ad = bytes(64)
        self.self_id = bytes(16)
        self.peer_id = bytes(16)
        self.rk = bytes(32)
        self.dhs_priv = bytes(32)
        self.dhs_pub = bytes(32)
        self.dhr = bytes(32)
        self.cks = bytes(32)
        self.ckr = bytes(32)
        self.ns = self.nr = self.pn = 0
        self.flags = 0
        self.x3dh = {"ek": bytes(32), "spk_id": 0, "opk_id": NO_OPK}
        self.skipped = []  # [dh, n, mk, stored_at], oldest first

    @classmethod
    def initiator(cls, sk, ad, self_id, peer_id, peer_spk, x3dh, rng):
        s = cls()
        s.ad, s.self_id, s.peer_id, s.rk, s.dhr = ad, self_id, peer_id, sk, peer_spk
        s.dhs_priv = rng(32)
        s.dhs_pub = x_public(s.dhs_priv)
        s.rk, s.cks = kdf_rk(s.rk, x_dh(s.dhs_priv, s.dhr))
        s.flags = F_SEND | F_X3DH | F_INIT
        s.x3dh = dict(x3dh)
        return s

    @classmethod
    def responder(cls, sk, ad, self_id, peer_id, spk):
        s = cls()
        s.ad, s.self_id, s.peer_id, s.rk = ad, self_id, peer_id, sk
        s.dhs_priv, s.dhs_pub = spk.priv, spk.pub
        return s

    def copy(self):
        c = Ratchet()
        c.__dict__.update({k: (list(map(list, v)) if k == "skipped" else (dict(v) if isinstance(v, dict) else v))
                           for k, v in self.__dict__.items()})
        return c

    def encrypt(self, pt: bytes, rng):
        assert self.flags & F_SEND
        nonce = rng(24)
        h = {"dh": self.dhs_pub, "pn": self.pn, "n": self.ns,
             "x3dh": dict(self.x3dh) if self.flags & F_X3DH else None}
        mk, ck = chain_step(self.cks)
        ct = aead_seal(mk, nonce, pad(pt), self.ad + dm_header(h, self.self_id, self.peer_id))
        self.cks = ck
        self.ns += 1
        return h, nonce, ct

    def _expire(self, now):
        self.skipped = [e for e in self.skipped if not (now >= e[3] and now - e[3] >= SKIP_TTL)]

    def _skip(self, until, now):
        if not self.flags & F_RECV or until <= self.nr:
            return
        if until - self.nr > MAX_SKIP:
            raise ValueError("skip")
        while self.nr < until:
            mk, self.ckr = chain_step(self.ckr)
            if len(self.skipped) == MAX_STORED:
                self.skipped.pop(0)
            self.skipped.append([self.dhr, self.nr, mk, now])
            self.nr += 1

    def _open(self, h, mk, nonce, ct):
        pt = aead_open(mk, nonce, ct, self.ad + dm_header(h, self.peer_id, self.self_id))
        return unpad(pt)

    def decrypt(self, h, nonce, ct, now, rng):
        """Returns plaintext; on any failure raises and leaves self unchanged."""
        w = self.copy()
        w._expire(now)
        for i, e in enumerate(w.skipped):
            if e[1] == h["n"] and e[0] == h["dh"]:
                pt = w._open(h, e[2], nonce, ct)
                del w.skipped[i]
                w.flags &= ~F_X3DH
                self.__dict__.update(w.__dict__)
                return pt
        if not w.flags & F_RECV or h["dh"] != w.dhr:
            w._skip(h["pn"], now)
            w.pn, w.ns, w.nr = w.ns, 0, 0
            w.dhr = h["dh"]
            w.rk, w.ckr = kdf_rk(w.rk, x_dh(w.dhs_priv, w.dhr))
            w.flags |= F_RECV
            w.dhs_priv = rng(32)
            w.dhs_pub = x_public(w.dhs_priv)
            w.rk, w.cks = kdf_rk(w.rk, x_dh(w.dhs_priv, w.dhr))
            w.flags |= F_SEND
        elif h["n"] < w.nr:
            raise ValueError("replay")
        w._skip(h["n"], now)
        mk, w.ckr = chain_step(w.ckr)
        w.nr = h["n"] + 1
        pt = w._open(h, mk, nonce, ct)
        w.flags &= ~F_X3DH
        self.__dict__.update(w.__dict__)
        return pt

    def serialize(self) -> bytes:
        out = b"SC2S" + bytes([1, self.flags]) + self.ad + self.self_id + self.peer_id
        out += self.rk + self.dhs_priv + self.dhs_pub + self.dhr + self.cks + self.ckr
        out += struct.pack(">III", self.ns, self.nr, self.pn)
        out += self.x3dh["ek"] + struct.pack(">II", self.x3dh["spk_id"], self.x3dh["opk_id"])
        out += bytes([len(self.skipped)])
        for dh, n, mk, at in self.skipped:
            out += dh + struct.pack(">I", n) + mk + struct.pack(">Q", at)
        return out + struct.pack(">I", binascii.crc32(out) & 0xFFFFFFFF)


# ---------------- rooms ----------------

def room_chain(epoch_key: bytes, room: bytes, epoch: int, sender: bytes) -> bytes:
    return hkdf(room + struct.pack(">I", epoch), epoch_key, L_SENDER + sender, 32)


def room_header(room: bytes, epoch: int, frm: bytes, n: int) -> bytes:
    return b"SC2R" + bytes([SUITE]) + room + struct.pack(">I", epoch) + frm + struct.pack(">I", n)


def room_seal(mk, nonce, hdr, pt, ik_seed):
    ct = aead_seal(mk, nonce, pad(pt), hdr)
    sig = ed_sign(ik_seed, L_ROOM + hdr + nonce + ct)
    return ct, sig


# ---------------- membership commits (fixed 130-byte records) ----------------

def roster_hash(roster) -> bytes:
    """roster: {device_id(16 bytes): role} with role 1 = admin, 0 = member."""
    return sha256(b"".join(d + bytes([roster[d]]) for d in sorted(roster)))


def commit_record(room, epoch, prev, by, op, target, count, rhash) -> bytes:
    rec = (L_COMMIT + bytes([SUITE]) + room + struct.pack(">I", epoch) + prev + by + bytes([COMMIT_OPS[op]]) +
           target + bytes([count]) + rhash)
    assert len(rec) == 130
    return rec


def commit_parse(rec: bytes):
    if len(rec) != 130 or rec[:11] != L_COMMIT or rec[11] != SUITE:
        raise ValueError("bad commit record")
    ops = {v: k for k, v in COMMIT_OPS.items()}
    if rec[80] not in ops:
        raise ValueError("unknown op")
    c = {"room": rec[12:28], "epoch": struct.unpack(">I", rec[28:32])[0], "prev": rec[32:64],
         "by": rec[64:80], "op": ops[rec[80]], "target": rec[81:97], "count": rec[97], "hash": rec[98:130]}
    z16 = bytes(16)
    if not 1 <= c["count"] <= ROOM_MEMBERS_MAX or c["epoch"] == 0 or c["by"] == z16:
        raise ValueError("bad commit fields")
    if c["op"] == "create":
        if c["epoch"] != 1 or c["count"] != 1 or c["prev"] != bytes(32) or c["target"] != c["by"]:
            raise ValueError("bad genesis shape")
    elif c["epoch"] == 1:
        raise ValueError("only a genesis commit may have epoch 1")
    elif (c["op"] == "rotate") != (c["target"] == z16):
        raise ValueError("rotate has no target; other ops need one")
    return c


def commit_id(rec: bytes) -> bytes:
    return sha256(rec)


class Roster:
    """Client view of a private room's signed membership (independent of the C core)."""

    def __init__(self, room: bytes):
        self.room, self.epoch, self.head, self.members = room, 0, bytes(32), {}

    def _after(self, op, target):
        m = dict(self.members)
        if op == "create":
            m[target] = 1
        elif op == "add":
            m[target] = 0
        elif op == "remove":
            del m[target]
        elif op == "promote":
            m[target] = 1
        elif op == "demote":
            m[target] = 0
        return m

    def _check(self, by, op, target):
        if op == "create":
            if self.epoch or self.members or target != by:
                raise ValueError("bad genesis")
            return
        if not self.epoch or self.members.get(by) != 1:
            raise ValueError("not an admin")
        admins = sum(self.members.values())
        if op == "add" and (target in self.members or len(self.members) >= ROOM_MEMBERS_MAX):
            raise ValueError("cannot add")
        if op == "remove" and (target not in self.members or (self.members[target] == 1 and admins <= 1)):
            raise ValueError("cannot remove")
        if op == "promote" and self.members.get(target) != 0:
            raise ValueError("cannot promote")
        if op == "demote" and (self.members.get(target) != 1 or admins <= 1):
            raise ValueError("cannot demote")
        if op == "rotate" and target != bytes(16):
            raise ValueError("rotate has no target")

    def make(self, by, op, target=None):
        target = by if op == "create" else (bytes(16) if op == "rotate" else target)
        self._check(by, op, target)
        m = self._after(op, target)
        return commit_record(self.room, self.epoch + 1, self.head, by, op, target, len(m), roster_hash(m))

    def apply(self, rec):
        c = commit_parse(rec)
        if c["room"] != self.room or c["epoch"] != self.epoch + 1 or c["prev"] != self.head:
            raise ValueError("out of order")
        self._check(c["by"], c["op"], c["target"])
        m = self._after(c["op"], c["target"])
        if len(m) != c["count"] or roster_hash(m) != c["hash"]:
            raise ValueError("roster mismatch")
        self.members, self.epoch, self.head = m, c["epoch"], commit_id(rec)


# ---------------- relay-signed moderation (fixed 122-byte records) ----------------

def mod_record(action, room: str, seq, ts, ref=None, device=None, until=0) -> bytes:
    r = room.encode()
    assert 0 < len(r) <= 63
    rec = (L_MOD + bytes([SUITE, MOD_ACTIONS[action]]) + r + bytes(64 - len(r)) + struct.pack(">QQ", seq, ts) +
           (ref or bytes(8)) + (device or bytes(16)) + struct.pack(">Q", until or 0))
    assert len(rec) == 122
    return rec


def mod_frame(relay_seed, action, room, seq, ts, ref=None, device=None, until=None):
    f = {"type": "room.mod", "room": room, "seq": str(seq), "action": action, "ts": ts}
    if action == "remove":
        f["ref"] = ref.hex()
    f["device"] = device.hex()
    if action == "ban":
        f["until"] = until if until else None
    f["by"] = "relay"
    f["sig"] = b64(ed_sign(relay_seed, mod_record(action, room, seq, ts, ref, device, until or 0)))
    return dumps(f)


# ---------------- invites ----------------

def invite_key(secret: bytes, room: bytes) -> bytes:
    return hmac256(secret, L_INVITE + room)


def invite_proof(key: bytes, joiner: bytes) -> bytes:
    return hmac256(key, L_JOIN + joiner)


def invite_code(room: bytes, secret: bytes, admin_ik: bytes, url: str) -> bytes:
    u = url.encode("ascii")
    assert 0 < len(u) <= 200 and (url.startswith("chat://") or url.startswith("chats://"))
    return b"SC2I\x01" + room + secret + device_id(admin_ik) + admin_ik + bytes([len(u)]) + u


def invite_text(code: bytes) -> str:
    return "sc2i:" + b64(code)


# ---------------- public posts ----------------

def post_signed_bytes(relay_id: str, room: str, p) -> bytes:
    r, m = relay_id.encode(), room.encode()
    name, body = p["name"].encode(), p["body"].encode()
    t = {"text": 1, "del": 2}[p["t"]]
    ref = bytes.fromhex(p["ref"]) if p.get("ref") else bytes(8)
    return (L_PUB + bytes([p["s"], len(r)]) + r + bytes([len(m)]) + m + unb64(p["ik"]) + bytes.fromhex(p["id"]) +
            struct.pack(">QB", p["ts"], t) + bytes([len(name)]) + name + struct.pack(">H", len(body)) + body + ref)


def post_make(relay_id, room, ik_seed, t, msg_id, ts, name, body, ref=None):
    ik = ed_public(ik_seed)
    p = {"s": SUITE, "t": t, "from": device_id(ik).hex(), "ik": b64(ik), "id": msg_id.hex(), "ts": ts,
         "name": name, "body": body}
    if ref is not None:
        p["ref"] = ref.hex()
    p["sig"] = b64(ed_sign(ik_seed, post_signed_bytes(relay_id, room, p)))
    return p


# ---------------- safety numbers ----------------

def safety_hash(a: bytes, b: bytes) -> bytes:
    lo, hi = sorted([a, b])
    return sha256(L_SAFETY + lo + hi)


def _groups(src: bytes, n: int, digits: int) -> str:
    return " ".join("%0*d" % (digits, int.from_bytes(src[5 * i:5 * i + 5], "big") % 10 ** digits) for i in range(n))


def safety_short(h: bytes) -> str:
    return _groups(h, 6, 4)


def safety_full(h: bytes) -> str:
    return _groups(hkdf_expand(h, L_SAFETY_FULL, 60), 12, 5)


def safety_word_indices(h: bytes):
    v = int.from_bytes(h[:11], "big")  # 88 bits
    return [(v >> (88 - 11 * (i + 1))) & 0x7FF for i in range(8)]


# ---------------- frames ----------------

def dm_env(frm: bytes, h, nonce: bytes, ct: bytes):
    env = {"s": SUITE, "from": frm.hex()}
    if h.get("x3dh"):
        x = {"ek": b64(h["x3dh"]["ek"]), "spk": h["x3dh"]["spk_id"]}
        if h["x3dh"]["opk_id"] != NO_OPK:
            x["opk"] = h["x3dh"]["opk_id"]
        env["x3dh"] = x
    env.update({"rk": b64(h["dh"]), "pn": h["pn"], "n": h["n"], "nonce": b64(nonce), "ct": b64(ct)})
    return env


def frame(type_, rid=None, **fields):
    obj = {"type": type_}
    if rid is not None:
        obj["rid"] = rid
    obj.update(fields)
    return dumps(obj)
