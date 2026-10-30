#!/usr/bin/env python3
"""Generate SC2-1 test vectors for protocol/sc2.

Requires PyNaCl (libsodium). Writes:
  vectors/sc2_vectors.json  - language-neutral vectors (also used by the Python reference relay, solar_os_sc2-relay)
  vectors/sc2_vectors.inc   - the same data as C tables for tests/host/sc2_*_test.c

Every primitive vector is checked against libsodium (PyNaCl) and, where it
exists, the published RFC value; Ed25519 and HMAC are additionally checked
against the system libsodium when it can be loaded. Run:

  python3 -m venv ~/sc2-venv && ~/sc2-venv/bin/pip install pynacl
  ~/sc2-venv/bin/python protocol/sc2/tools/gen_vectors.py [--check]

python-mnemonic is optional (used to cross-check the committed word list).

--check regenerates in memory and fails if the committed files differ.

SPDX-License-Identifier: MIT
"""

import argparse
import base64
import ctypes
import ctypes.util
import hashlib
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import sc2ref as R  # noqa: E402

import nacl.bindings as nb  # noqa: E402

OUT_DIR = os.path.join(HERE, "..", "vectors")
T0 = 1760889600

# ---------------------------------------------------------------- RFC data
RFC8032 = [
    ("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
     "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
     "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"),
    ("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
     "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
     "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"),
    ("c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
     "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
     "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"),
]
RFC7748 = [
    ("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4",
     "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c",
     "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552"),
    ("4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d",
     "e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493",
     "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957"),
    ("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a",
     "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f",
     "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742"),
    ("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb",
     "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a",
     "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742"),
]
RFC7748_ITER = {1: "422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079",
                1000: "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51"}
RFC4231 = [
    ("0b" * 20, b"Hi There".hex(), "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"),
    (b"Jefe".hex(), b"what do ya want for nothing?".hex(),
     "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"),
    ("aa" * 20, "dd" * 50, "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe"),
    ("0102030405060708090a0b0c0d0e0f10111213141516171819", "cd" * 50,
     "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b"),
    ("aa" * 131, b"Test Using Larger Than Block-Size Key - Hash Key First".hex(),
     "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"),
    ("aa" * 131, b"This is a test using a larger than block-size key and a larger than block-size data. "
                 b"The key needs to be hashed before being used by the HMAC algorithm.".hex(),
     "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2"),
]
RFC5869 = [
    ("0b" * 22, "000102030405060708090a0b0c", "f0f1f2f3f4f5f6f7f8f9", 42,
     "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5",
     "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865"),
    (bytes(range(0x00, 0x50)).hex(), bytes(range(0x60, 0xb0)).hex(), bytes(range(0xb0, 0x100)).hex(), 82,
     "06a6b88c5853361a06104c9ceb35b45cef760014904671014a193f40c15fc244",
     "b11e398dc80327a1c8e7f78c596a49344f012eda2d4efad8a050cc4c19afa97c59045a99cac7827271cb41c65e590e09"
     "da3275600c2f09b8367793a9aca3db71cc30c58179ec3e87c14c01d5c1f3434f1d87"),
    ("0b" * 22, "", "", 42,
     "19ef24a32c717b167f33a91d6f648bdf96596776afdb6377ac434c1c293ccb04",
     "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8"),
]
XCHACHA_A31 = {
    "key": "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f",
    "nonce": "404142434445464748494a4b4c4d4e4f5051525354555657",
    "ad": "50515253c0c1c2c3c4c5c6c7",
    "pt": b"Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, "
          b"sunscreen would be it.".hex(),
    "ct": "bd6d179d3e83d43b9576579493c0e939572a1700252bfaccbed2902c21396cbb731c7f1b0b4aa6440bf3a82f4eda7e39"
          "ae64c6708c54c216cb96b72e1213b4522f8c9ba40db5d945b11b69b982c1bb9e3f3fac2bc369488f76b2383565d3fff9"
          "21f9664c97637da9768812f615c68b13b52e" "c0875924c1c7987947deafd8780acf49",
}

# ------------------------------------------------- system libsodium (optional)


def load_system_sodium():
    name = ctypes.util.find_library("sodium") or "libsodium.so.23"
    try:
        lib = ctypes.CDLL(name)
        if lib.sodium_init() < 0:
            return None
        lib.sodium_version_string.restype = ctypes.c_char_p
        return lib
    except OSError:
        return None


SYS = load_system_sodium()


def sys_verify(pk, msg, sig):
    if SYS is None:
        return None
    return SYS.crypto_sign_verify_detached(sig, msg, ctypes.c_ulonglong(len(msg)), pk) == 0


def sys_hmac(key, msg):
    if SYS is None:
        return None
    st = ctypes.create_string_buffer(512)
    out = ctypes.create_string_buffer(32)
    SYS.crypto_auth_hmacsha256_init(st, key, ctypes.c_size_t(len(key)))
    SYS.crypto_auth_hmacsha256_update(st, msg, ctypes.c_ulonglong(len(msg)))
    SYS.crypto_auth_hmacsha256_final(st, out)
    return out.raw


# ------------------------------------------------------ Edwards arithmetic
P = 2 ** 255 - 19
D = (-121665 * pow(121666, P - 2, P)) % P
LORD = 2 ** 252 + 27742317777372353535851937790883648493
SQRTM1 = pow(2, (P - 1) // 4, P)


def ed_add(a, b):
    (x1, y1), (x2, y2) = a, b
    t = D * x1 * x2 * y1 * y2 % P
    x3 = (x1 * y2 + x2 * y1) * pow(1 + t, P - 2, P) % P
    y3 = (y1 * y2 + x1 * x2) * pow(1 - t, P - 2, P) % P
    return (x3, y3)


def ed_mul(k, pt):
    acc = (0, 1)
    while k:
        if k & 1:
            acc = ed_add(acc, pt)
        pt = ed_add(pt, pt)
        k >>= 1
    return acc


def ed_decode(b):
    y = int.from_bytes(b, "little") & ((1 << 255) - 1)
    sign = b[31] >> 7
    if y >= P:
        return None
    x2 = (y * y - 1) * pow(D * y * y + 1, P - 2, P) % P
    x = pow(x2, (P + 3) // 8, P)
    if (x * x - x2) % P:
        x = x * SQRTM1 % P
    if (x * x - x2) % P:
        return None
    if x & 1 != sign:
        x = (-x) % P
    return (x, y)


def ed_encode(pt, y_override=None):
    x, y = pt
    v = y if y_override is None else y_override
    return (v | ((x & 1) << 255)).to_bytes(32, "little")


def torsion_points():
    # find a point of order 8
    for i in range(2, 200):
        y = i
        pt = ed_decode(y.to_bytes(32, "little"))
        if pt is None:
            continue
        t = ed_mul(LORD, pt)
        if ed_mul(4, t) != (0, 1):
            return [ed_mul(k, t) for k in range(8)]
    raise RuntimeError("no torsion point")


def small_order_encodings():
    encs = set()
    for pt in torsion_points():
        encs.add(ed_encode(pt))
        x, y = pt
        if y + P < 2 ** 255:
            encs.add(ed_encode(pt, y + P))  # non-canonical y
        if x == 0:
            encs.add((y | (1 << 255)).to_bytes(32, "little"))  # "negative zero"
    return sorted(encs)


# ------------------------------------------------------------- generators

def gen_sha256():
    out = [{"msg": "", "digest": hashlib.sha256(b"").hexdigest()},
           {"msg": b"abc".hex(), "digest": "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
           {"msg": b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq".hex(),
            "digest": "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"}]
    for e in out:
        assert R.sha256(bytes.fromhex(e["msg"])).hex() == e["digest"]
    for n in (1, 55, 56, 57, 63, 64, 65, 119, 120, 127, 128, 129, 1000):
        m = R.stream("sha%d" % n, n)
        out.append({"msg": m.hex(), "digest": R.sha256(m).hex()})
    return out


def gen_hmac():
    out = []
    for k, m, mac in RFC4231:
        got = R.hmac256(bytes.fromhex(k), bytes.fromhex(m))
        assert got.hex() == mac
        out.append({"key": k, "msg": m, "mac": mac})
    for kl, ml in ((0, 0), (32, 0), (32, 100), (64, 64), (65, 3), (200, 300)):
        k, m = R.stream("hk%d" % kl, kl), R.stream("hm%d" % ml, ml)
        mac = R.hmac256(k, m)
        s = sys_hmac(k, m)
        assert s is None or s == mac
        out.append({"key": k.hex(), "msg": m.hex(), "mac": mac.hex()})
    return out


def gen_hkdf():
    out = []
    for ikm, salt, info, n, prk, okm in RFC5869:
        got_prk = R.hkdf_extract(bytes.fromhex(salt), bytes.fromhex(ikm))
        got = R.hkdf_expand(got_prk, bytes.fromhex(info), n)
        assert got_prk.hex() == prk and got.hex() == okm
        out.append({"ikm": ikm, "salt": salt, "info": info, "len": n, "prk": prk, "okm": okm})
    for i, n in enumerate((1, 32, 33, 64, 255, 8160)):
        ikm, salt, info = R.stream("ikm%d" % i, 32), R.stream("salt%d" % i, 16 * i), R.stream("info%d" % i, i * 7)
        prk = R.hkdf_extract(salt, ikm)
        out.append({"ikm": ikm.hex(), "salt": salt.hex(), "info": info.hex(), "len": n, "prk": prk.hex(),
                    "okm": R.hkdf_expand(prk, info, n).hex()})
    return out


def gen_ed25519():
    sign = []
    for seed, pk, msg, sig in RFC8032:
        s, m = bytes.fromhex(seed), bytes.fromhex(msg)
        assert R.ed_public(s).hex() == pk and R.ed_sign(s, m).hex() == sig
        sign.append({"seed": seed, "pk": pk, "msg": msg, "sig": sig})
    for i in range(6):
        s, m = R.stream("edseed%d" % i, 32), R.stream("edmsg%d" % i, i * 37)
        sign.append({"seed": s.hex(), "pk": R.ed_public(s).hex(), "msg": m.hex(), "sig": R.ed_sign(s, m).hex()})

    verify = []

    def add(note, pk, msg, sig):
        ok = R.ed_verify(pk, msg, sig)
        so = sys_verify(pk, msg, sig)
        if so is not None and so != ok:
            raise AssertionError("PyNaCl and system libsodium disagree: " + note)
        verify.append({"note": note, "pk": pk.hex(), "msg": msg.hex(), "sig": sig.hex(), "ok": ok})

    seed = R.stream("edstrict", 32)
    pk = R.ed_public(seed)
    msg = b"SC2 strict verify"
    sig = R.ed_sign(seed, msg)
    add("valid", pk, msg, sig)
    add("wrong message", pk, msg + b"!", sig)
    for bit in (0, 100, 255, 256, 300, 511):
        b = bytearray(sig)
        b[bit // 8] ^= 1 << (bit % 8)
        add("sig bit %d flipped" % bit, pk, msg, bytes(b))
    s_int = int.from_bytes(sig[32:], "little")
    add("S + L (malleated)", pk, msg, sig[:32] + (s_int + LORD).to_bytes(32, "little"))
    add("S + 2L", pk, msg, sig[:32] + (s_int + 2 * LORD).to_bytes(32, "little"))
    add("S = L", pk, msg, sig[:32] + LORD.to_bytes(32, "little"))
    add("S top bits set", pk, msg, sig[:63] + bytes([sig[63] | 0xE0]))
    for i, enc in enumerate(small_order_encodings()):
        add("small-order pk #%d" % i, enc, msg, sig)
        add("small-order R #%d" % i, pk, msg, enc + sig[32:])
    # non-canonical encodings of large-order points (y >= p)
    for y in range(2, 19):
        pt = ed_decode(y.to_bytes(32, "little"))
        if pt is None or ed_mul(8, pt) == (0, 1):
            continue
        add("non-canonical pk y=%d+p" % y, ed_encode(pt, y + P), msg, sig)
        add("non-canonical R y=%d+p" % y, pk, msg, ed_encode(pt, y + P) + sig[32:])
    # off-curve public key
    for y in range(2, 40):
        if ed_decode(y.to_bytes(32, "little")) is None:
            add("off-curve pk y=%d" % y, y.to_bytes(32, "little"), msg, sig)
            break
    # mixed-order public key: A + T with T of order 8 (accepted by libsodium if the equation held; here it does not)
    t8 = [p for p in torsion_points() if ed_mul(4, p) != (0, 1)][0]
    a = ed_decode(pk)
    add("mixed-order pk", ed_encode(ed_add(a, t8)), msg, sig)
    assert verify[0]["ok"] and not any(v["ok"] for v in verify[1:])
    return sign, verify


def gen_x25519():
    out = []
    for k, u, o in RFC7748:
        got = nb.crypto_scalarmult(bytes.fromhex(k), bytes.fromhex(u))
        assert got.hex() == o
        out.append({"scalar": k, "u": u, "out": o})
    for i in range(4):
        a, b = R.stream("xa%d" % i, 32), R.stream("xb%d" % i, 32)
        out.append({"scalar": a.hex(), "u": R.x_public(b).hex(), "out": R.x_dh(a, R.x_public(b)).hex()})
    # u with the top bit set must be masked
    a, b = R.stream("xmask", 32), R.stream("xmaskb", 32)
    u = bytearray(R.x_public(b))
    u[31] |= 0x80
    out.append({"scalar": a.hex(), "u": bytes(u).hex(), "out": nb.crypto_scalarmult(a, bytes(u)).hex()})
    # small-order inputs: all-zero output, must be rejected
    bad = []
    so = [0, 1, P - 1, P, P + 1,
          325606250916557431795983626356110631294008115727848805560023387167927233504,
          39382357235489614581723060781553021112529911719440698176882885853963445705823]
    for v in so:
        for top in (0, 1):
            enc = (v | (top << 255)).to_bytes(32, "little")
            try:
                R.x_dh(a, enc)
                rejected = False
            except ValueError:
                rejected = True
            assert rejected, v
            bad.append({"scalar": a.hex(), "u": enc.hex()})
    # iteration test
    k = u = bytes([9]) + bytes(31)
    iters = {}
    for i in range(1, 1001):
        k, u = nb.crypto_scalarmult(k, u), k
        if i in RFC7748_ITER:
            assert k.hex() == RFC7748_ITER[i]
            iters[i] = k.hex()
    return out, bad, iters


def gen_aead():
    out = []
    x = XCHACHA_A31
    got = R.aead_seal(bytes.fromhex(x["key"]), bytes.fromhex(x["nonce"]), bytes.fromhex(x["pt"]), bytes.fromhex(x["ad"]))
    assert got.hex() == x["ct"]
    out.append(dict(x))
    for i, (pl, al) in enumerate(((0, 0), (1, 0), (0, 13), (16, 16), (63, 7), (64, 64), (65, 1), (300, 45),
                                  (256, 0), (8192, 118))):
        key, nonce = R.stream("ak%d" % i, 32), R.stream("an%d" % i, 24)
        pt, ad = R.stream("ap%d" % i, pl), R.stream("aa%d" % i, al)
        out.append({"key": key.hex(), "nonce": nonce.hex(), "ad": ad.hex(), "pt": pt.hex(),
                    "ct": R.aead_seal(key, nonce, pt, ad).hex()})
    return out


def gen_b64():
    good = []
    for n in range(0, 10):
        b = R.stream("b64%d" % n, n)
        good.append({"bin": b.hex(), "b64": R.b64(b)})
    good.append({"bin": "fbff", "b64": "+/8="})
    bad = ["A", "AA", "AAA", "A===", "AA=A", "====", "AB==", "AAB=", "AA==AA==", "Zm9v YmFy", "Zm9v\nYmFy",
           "Zm9-", "Zm9_", "Zg", "Zm8"]
    for s in bad:
        try:
            R.unb64(s)
            if s in ("AB==", "AAB="):  # Python ignores non-zero trailing bits; SC2 is stricter
                continue
            raise AssertionError("python accepted " + s)
        except (ValueError, base64.binascii.Error):
            pass
    return good, bad


def gen_identity():
    rng = R.Rng(R.stream("id-alice", 96))
    a = R.Identity(rng)
    spk = R.Spk(7, a, rng)
    opk_rng = R.Rng(R.stream("id-alice-opk", 32))
    opk = R.Opk(101, opk_rng)
    challenge = R.stream("challenge", 32)
    relay = "relay.example"
    auth = R.ed_sign(a.ik_seed, R.auth_bytes(relay, challenge, a.ik_pub))
    return {"rng": rng.data.hex(), "ik_pub": a.ik_pub.hex(), "dk_pub": a.dk_pub.hex(), "dk_sig": a.dk_sig.hex(),
            "device_id": a.id.hex(), "spk_id": 7, "spk_pub": spk.pub.hex(), "spk_sig": spk.sig.hex(),
            "opk_rng": opk_rng.data.hex(), "opk_id": 101, "opk_pub": opk.pub.hex(), "relay_id": relay,
            "challenge": challenge.hex(), "auth_sig": auth.hex()}


def bundle_of(ident, spk, opk):
    return {"ik": ident.ik_pub, "dk": ident.dk_pub, "dk_sig": ident.dk_sig, "spk_id": spk.id, "spk": spk.pub,
            "spk_sig": spk.sig, "opk_id": opk.id if opk else R.NO_OPK, "opk": opk.pub if opk else bytes(32)}


def gen_x3dh():
    out = []
    for with_opk in (True, False):
        tag = "opk" if with_opk else "noopk"
        arng, brng, erng = (R.Rng(R.stream("x3a" + tag, 64)), R.Rng(R.stream("x3b" + tag, 96)),
                            R.Rng(R.stream("x3e" + tag, 32)))
        alice, bob = R.Identity(arng), R.Identity(brng)
        spk = R.Spk(7, bob, brng)
        opk = R.Opk(101, R.Rng(R.stream("x3o" + tag, 32))) if with_opk else None
        sk, ad, hdr = R.x3dh_initiate(alice, bundle_of(bob, spk, opk), erng)
        sk2, ad2 = R.x3dh_respond(bob, spk, opk, alice.ik_pub, alice.dk_pub, alice.dk_sig, hdr)
        assert sk == sk2 and ad == ad2
        out.append({"alice_rng": arng.data.hex(), "bob_rng": brng.data.hex(),
                    "opk_rng": R.stream("x3o" + tag, 32).hex() if with_opk else "",
                    "ek_rng": erng.data.hex(), "spk_id": 7, "opk_id": 101 if with_opk else R.NO_OPK,
                    "ek_pub": hdr["ek"].hex(), "sk": sk.hex(), "ad": ad.hex()})
    return out


class Party:
    def __init__(self, name):
        self.name = name
        self.draws = R.stream("ratchet-" + name, 4096)
        self.pos = 0
        self.session = None

    def rng(self):
        start = self.pos

        def draw(n):
            if self.pos + n > len(self.draws):
                raise ValueError("exhausted")
            b = self.draws[self.pos:self.pos + n]
            self.pos += n
            return b
        return draw, start


def gen_ratchet():
    A, B = Party("alice"), Party("bob")
    draw, _ = A.rng()
    alice = R.Identity(draw)
    draw, _ = B.rng()
    bob = R.Identity(draw)
    spk = R.Spk(3, bob, draw)
    opk = R.Opk(55, draw)
    setup_a, setup_b = A.pos, B.pos
    draw, _ = A.rng()
    sk, ad, x3 = R.x3dh_initiate(alice, bundle_of(bob, spk, opk), draw)
    A.session = R.Ratchet.initiator(sk, ad, alice.id, bob.id, spk.pub, x3, draw)
    init_a = A.pos
    msgs = []
    steps = []

    def enc(p, pt):
        draw, start = p.rng()
        h, nonce, ct = p.session.encrypt(pt, draw)
        msgs.append((h, nonce, ct, p.name))
        steps.append({"actor": p.name[0].upper(), "op": "E", "pt": pt.hex(), "now": 0, "msg": len(msgs) - 1,
                      "tamper": 0, "rc": 0, "rng_from": start, "rng_to": p.pos,
                      "dh": h["dh"].hex(), "pn": h["pn"], "n": h["n"], "x3dh": 1 if h.get("x3dh") else 0,
                      "nonce": nonce.hex(), "ct": ct.hex()})
        return len(msgs) - 1

    def dec(p, idx, now=T0, tamper=0, expect="ok"):
        h, nonce, ct, _ = msgs[idx]
        if tamper:
            ct = bytearray(ct)
            ct[5] ^= 0x40
            ct = bytes(ct)
        draw, start = p.rng()
        rc, pt = 0, b""
        try:
            if p.session is None:
                x = h["x3dh"]
                sk2, ad2 = R.x3dh_respond(bob, spk, opk if x["opk_id"] == opk.id else None,
                                          alice.ik_pub, alice.dk_pub, alice.dk_sig, x)
                p.session = R.Ratchet.responder(sk2, ad2, bob.id, alice.id, spk)
            pt = p.session.decrypt(h, nonce, ct, now, draw)
        except ValueError as e:
            rc = {"replay": -7, "skip": -8}.get(str(e), -4)
            p.pos = start  # nothing consumed on failure (state rolled back)
        except Exception:  # nacl CryptoError on a bad tag
            rc = -4
            p.pos = start
        want = {"ok": 0, "auth": -4, "replay": -7, "skip": -8}[expect]
        assert rc == want, (idx, rc, want)
        steps.append({"actor": p.name[0].upper(), "op": "D", "pt": pt.hex(), "now": now, "msg": idx,
                      "tamper": tamper, "rc": rc, "rng_from": start, "rng_to": p.pos,
                      "dh": "", "pn": 0, "n": 0, "x3dh": 0, "nonce": "", "ct": ""})

    m0 = enc(A, b'{"t":"text","id":"0000000000000001","body":"a0","ts":1760889600}')
    m1 = enc(A, b'{"t":"text","id":"0000000000000002","body":"a1","ts":1760889601}')
    m2 = enc(A, b'{"t":"text","id":"0000000000000003","body":"a2","ts":1760889602}')
    dec(B, m1)                     # out of order, creates the session
    dec(B, m0)                     # from the skipped store
    dec(B, m0, expect="replay")    # replay
    b0 = enc(B, b'{"t":"text","id":"1000000000000001","body":"b0"}')
    dec(B, m2)
    dec(A, b0)                     # DH ratchet step on A
    a3 = enc(A, b'{"t":"text","id":"0000000000000004","body":"a3 no x3dh"}')
    dec(B, a3, tamper=1, expect="auth")
    dec(B, a3)
    burst = [enc(A, ('{"t":"text","id":"%016x","body":"burst %d"}' % (0x100 + i, i)).encode()) for i in range(34)]
    dec(B, burst[33], expect="skip")   # gap 33 > 32
    dec(B, burst[32])                  # gap 32: stores 32 keys
    dec(B, burst[0])                   # from the store
    dec(B, burst[1], now=T0 + R.SKIP_TTL, expect="replay")  # expired, index already passed
    big = ('{"t":"text","id":"2000000000000001","body":"' + ("\u00e9\u20ac\U0001F600" * 140) + '"}').encode()
    b1 = enc(B, big)
    dec(A, b1)
    b2 = enc(B, b'{"t":"text","id":"2000000000000002","body":"b2"}')
    b3 = enc(B, b'{"t":"text","id":"2000000000000003","body":"b3"}')
    dec(A, b3)                     # b3 first, b2 stored
    a4 = enc(A, b'{"t":"text","id":"0000000000000200","body":"a after b3"}')
    dec(B, a4)                     # new DH step on B
    b4 = enc(B, b'{"t":"text","id":"2000000000000004","body":"b4 new chain"}')
    dec(A, b4)                     # new chain on A
    dec(A, b2)                     # b2 from A's store
    dec(B, burst[33])              # late message of the previous chain, stored via pn on the DH step
    return {
        "alice_rng": A.draws[:A.pos].hex(), "bob_rng": B.draws[:B.pos].hex(),
        "alice_setup": setup_a, "bob_setup": setup_b, "alice_init": init_a,
        "spk_id": 3, "opk_id": 55, "sk": sk.hex(), "ad": ad.hex(),
        "steps": steps,
        "alice_blob": A.session.serialize().hex(), "bob_blob": B.session.serialize().hex(),
        "alice_skipped": len(A.session.skipped), "bob_skipped": len(B.session.skipped),
    }


def gen_room():
    epoch_key, room, sender = R.stream("epoch", 32), R.stream("room", 16), R.stream("sender", 16)
    seed = R.stream("room-ik", 32)
    ck = R.room_chain(epoch_key, room, 6, sender)
    cks, mks = [ck], []
    for _ in range(3):
        mk, ck = R.chain_step(ck)
        mks.append(mk)
        cks.append(ck)
    hdr = R.room_header(room, 6, sender, 2)
    nonce = R.stream("room-nonce", 24)
    pt = b'{"t":"text","id":"00000000000000aa","body":"hello room","ts":1760889600}'
    ct, sig = R.room_seal(mks[2], nonce, hdr, pt, seed)
    return {"epoch_key": epoch_key.hex(), "room": room.hex(), "epoch": 6, "sender": sender.hex(),
            "ck0": cks[0].hex(), "mk": [m.hex() for m in mks], "hdr": hdr.hex(), "seed": seed.hex(),
            "ik_pub": R.ed_public(seed).hex(), "nonce": nonce.hex(), "pt": pt.hex(), "ct": ct.hex(),
            "sig": sig.hex()}


WORDLIST_SHA256 = "2f5eed53a4727b4bf8880d8f3f199efc90e58503646d9ff8eff3a2ed3b24dbda"


def load_wordlist():
    """BIP-39 English list: from python-mnemonic when installed, else from the
    committed C table. Either way it must hash to the published english.txt."""
    try:
        import mnemonic
        words = list(mnemonic.Mnemonic("english").wordlist)
    except ImportError:
        import re
        with open(os.path.join(HERE, "..", "src", "sc2_wordlist_en.c"), encoding="utf-8") as f:
            body = f.read().split("WORDS[SC2_WORDLIST_SIZE] = {", 1)[1].split("};", 1)[0]
        words = re.findall(r'"([a-z]+)"', body)
    digest = hashlib.sha256(("\n".join(words) + "\n").encode()).hexdigest()
    assert len(words) == 2048 and digest == WORDLIST_SHA256, "BIP-39 word list mismatch"
    return words


def gen_safety():
    words = load_wordlist()
    out = []
    for i in range(5):
        a, b = R.ed_public(R.stream("sa%d" % i, 32)), R.ed_public(R.stream("sb%d" % i, 32))
        h = R.safety_hash(a, b)
        assert h == R.safety_hash(b, a)
        idx = R.safety_word_indices(h)
        out.append({"ik_a": a.hex(), "ik_b": b.hex(), "hash": h.hex(), "short": R.safety_short(h),
                    "words": "-".join(words[j] for j in idx), "full": R.safety_full(h),
                    "fp_a": R.safety_short(R.sha256(a))})
    # fixed edge case: identical keys (self-chat) and all-zero / all-ff keys
    for a, b in ((bytes(32), b"\xff" * 32), (b"\x01" * 32, b"\x01" * 32)):
        h = R.safety_hash(a, b)
        out.append({"ik_a": a.hex(), "ik_b": b.hex(), "hash": h.hex(), "short": R.safety_short(h),
                    "words": "-".join(words[j] for j in R.safety_word_indices(h)), "full": R.safety_full(h),
                    "fp_a": R.safety_short(R.sha256(a))})
    return out


def gen_frames(ratchet, room):
    fr = {}
    fr["hello"] = R.frame("hello", v=2, proto="sc2", suites=[1], client="solaros/1.x", admit="code-42")
    ident = gen_identity()
    fr["auth"] = R.frame("auth", rid="a1", ik=R.b64(bytes.fromhex(ident["ik_pub"])),
                         sig=R.b64(bytes.fromhex(ident["auth_sig"])), resume=True)
    st = ratchet["steps"][0]
    h = {"dh": bytes.fromhex(st["dh"]), "pn": st["pn"], "n": st["n"],
         "x3dh": {"ek": bytes(32), "spk_id": 0, "opk_id": 0}}
    # use the real x3dh header of the transcript's first message
    alice_rng = bytes.fromhex(ratchet["alice_rng"])
    # recompute EK pub from the transcript: EK is the first draw after setup
    ek_priv = alice_rng[ratchet["alice_setup"]:ratchet["alice_setup"] + 32]
    h["x3dh"] = {"ek": R.x_public(ek_priv), "spk_id": ratchet["spk_id"], "opk_id": ratchet["opk_id"]}
    alice_id = R.Identity(R.Rng(alice_rng[:64])).id
    bob_id = R.Identity(R.Rng(bytes.fromhex(ratchet["bob_rng"])[:64])).id
    env = R.dm_env(alice_id, h, bytes.fromhex(st["nonce"]), bytes.fromhex(st["ct"]))
    fr["dm_send"] = R.frame("dm.send", rid="d1", to=bob_id.hex(), env=env)
    fr["dm_msg"] = R.frame("dm.msg", seq="981", env=env)
    st2 = ratchet["steps"][9]
    assert st2["op"] == "E" and not st2["x3dh"]
    env2 = R.dm_env(alice_id, {"dh": bytes.fromhex(st2["dh"]), "pn": st2["pn"], "n": st2["n"], "x3dh": None},
                    bytes.fromhex(st2["nonce"]), bytes.fromhex(st2["ct"]))
    fr["dm_send_plain"] = R.frame("dm.send", to=bob_id.hex(), env=env2)
    renv = {"s": 1, "e": room["epoch"], "from": room["sender"], "n": 2, "nonce": R.b64(bytes.fromhex(room["nonce"])),
            "ct": R.b64(bytes.fromhex(room["ct"])), "sig": R.b64(bytes.fromhex(room["sig"]))}
    fr["room_send"] = R.frame("room.send", rid="s1", room=room["room"], env=renv)
    fr["room_msg"] = R.frame("room.msg", room=room["room"], seq="4523", env=renv)
    seed = R.stream("post-ik", 32)
    body = 'hello #general "quoted" \\ back\nnew\ttab \u00e9 \U0001F600 \x01 end'
    post = R.post_make("relay.example", "pub:general", seed, "text", bytes.fromhex("0123456789abcdef"), T0,
                       "Ana", body)
    fr["room_post"] = R.frame("room.post", rid="p1", room="pub:general", post=post)
    fr["room_pub"] = R.frame("room.pub", room="pub:general", seq="99813", post=post)
    fr["post_signed"] = R.post_signed_bytes("relay.example", "pub:general", post).hex()
    fr["post_seed"] = seed.hex()
    fr["post_body"] = body.encode().hex()
    dpost = R.post_make("relay.example", "pub:general", seed, "del", bytes.fromhex("1111111111111111"), T0 + 5,
                        "Ana", "", ref=bytes.fromhex("0123456789abcdef"))
    fr["room_post_del"] = R.frame("room.post", room="pub:general", post=dpost)
    # same post with ASCII-escaped JSON (\\u00e9, surrogate pairs) must decode identically
    fr["room_pub_ascii"] = json.dumps({"type": "room.pub", "room": "pub:general", "seq": "99813", "post": post},
                                      separators=(",", ":"), ensure_ascii=True)
    fr["welcome"] = R.frame("welcome", v=2, proto="sc2", suite=1,
                            relay={"id": "relay.example", "key": R.b64(R.ed_public(R.stream("relay", 32)))},
                            challenge=R.b64(bytes.fromhex(ident["challenge"])), legacy_v1=True,
                            limits={"line": 16384, "msg": 4096, "rooms": 64, "members": 32, "queue": 500,
                                    "queue_bytes": 5242880, "ttl_days": 7})
    fr["relay_key"] = R.ed_public(R.stream("relay", 32)).hex()
    return fr


# ------------------------------------------------- commits, moderation, invites

def _ids(*labels):
    out = {}
    for lab in labels:
        seed = R.stream("commit-" + lab, 32)
        ik = R.ed_public(seed)
        out[lab] = {"seed": seed, "ik": ik, "id": R.device_id(ik)}
    return out


def gen_commit():
    """A signed membership chain plus commits that every implementation must refuse."""
    P = _ids("a", "b", "c", "d", "e")
    room = R.stream("commit-room", 16)
    plan = [("a", "create", None), ("a", "add", "b"), ("a", "add", "c"), ("a", "promote", "b"),
            ("b", "add", "d"), ("a", "remove", "c"), ("b", "demote", "a"), ("b", "rotate", None),
            ("b", "remove", "a")]
    r = R.Roster(room)
    steps = []
    for by, op, tgt in plan:
        rec = r.make(P[by]["id"], op, P[tgt]["id"] if tgt else None)
        sig = R.ed_sign(P[by]["seed"], rec)
        assert R.ed_verify(P[by]["ik"], rec, sig) and sys_verify(P[by]["ik"], rec, sig)
        r.apply(rec)
        steps.append({"by": by, "op": op, "target": tgt or "", "rec": rec.hex(), "sig": sig.hex(),
                      "ik": P[by]["ik"].hex(), "id": R.commit_id(rec).hex(), "count": len(r.members),
                      "roster_hash": R.roster_hash(r.members).hex()})

    def at(k):
        x = R.Roster(room)
        for st in steps[:k]:
            x.apply(bytes.fromhex(st["rec"]))
        return x

    def rec_for(x, by, op, tgt, **over):
        target = P[by]["id"] if op == "create" else (bytes(16) if op == "rotate" else P[tgt]["id"])
        try:
            m = x._after(op, target)
        except KeyError:
            m = dict(x.members)
        f = {"room": room, "epoch": x.epoch + 1, "prev": x.head, "by": P[by]["id"], "target": target,
             "count": len(m), "hash": R.roster_hash(m)}
        f.update(over)
        return R.commit_record(f["room"], f["epoch"], f["prev"], f["by"], op, f["target"], f["count"], f["hash"])

    bad = []

    def add_bad(note, k, rec, signer, fail, ik=None, sig=None):
        sig = sig if sig is not None else R.ed_sign(P[signer]["seed"], rec)
        key = ik if ik is not None else P[signer]["ik"]
        refused = False
        try:  # the reference must refuse it at the same stage
            if fail == "decode":
                R.commit_parse(rec)
            elif fail == "verify":
                R.commit_parse(rec)
                if R.device_id(key) != rec[64:80] or not R.ed_verify(key, rec, sig):
                    raise ValueError("signature")
            else:
                R.commit_parse(rec)
                at(k).apply(rec)
        except ValueError:
            refused = True
        assert refused, "reference accepted bad commit: " + note
        bad.append({"note": note, "at": k, "rec": rec.hex(), "sig": sig.hex(), "ik": key.hex(), "fail": fail})

    x3 = at(3)
    assert rec_for(x3, "a", "promote", "b").hex() == steps[3]["rec"]
    add_bad("wrong prev", 3, rec_for(x3, "a", "promote", "b", prev=bytes(32)), "a", "check")
    add_bad("epoch skips", 3, rec_for(x3, "a", "promote", "b", epoch=x3.epoch + 2), "a", "check")
    add_bad("replayed commit", 3, bytes.fromhex(steps[2]["rec"]), "a", "check")
    add_bad("signer not admin", 3, rec_for(x3, "c", "add", "d"), "c", "check")
    add_bad("roster hash wrong", 3, rec_for(x3, "a", "promote", "b", hash=bytes(32)), "a", "check")
    add_bad("count wrong", 3, rec_for(x3, "a", "promote", "b", count=2), "a", "check")
    add_bad("add existing member", 3, rec_for(x3, "a", "add", "b"), "a", "check")
    add_bad("remove non-member", 3, rec_for(x3, "a", "remove", "e"), "a", "check")
    add_bad("remove last admin", 3, rec_for(x3, "a", "remove", "a"), "a", "check")
    add_bad("demote last admin", 3, rec_for(x3, "a", "demote", "a"), "a", "check")
    add_bad("promote an admin", 3, rec_for(x3, "a", "promote", "a"), "a", "check")
    add_bad("other room", 3, rec_for(x3, "a", "promote", "b", room=bytes(16)), "a", "check")
    add_bad("second genesis", 3, R.commit_record(room, 1, bytes(32), P["a"]["id"], "create", P["a"]["id"], 1,
                                                  R.roster_hash({P["a"]["id"]: 1})), "a", "check")
    for note, off, val in (("unknown op", 80, 9), ("unknown suite", 11, 2), ("bad label", 0, ord("X")),
                           ("zero count", 97, 0), ("count over 32", 97, 33)):
        rec = bytearray(rec_for(x3, "a", "promote", "b"))
        rec[off] = val
        add_bad(note, 3, bytes(rec), "a", "decode")
    add_bad("genesis epoch 2", 0, R.commit_record(room, 2, bytes(32), P["a"]["id"], "create", P["a"]["id"], 1,
                                                   R.roster_hash({P["a"]["id"]: 1})), "a", "decode")
    add_bad("genesis target other", 0, R.commit_record(room, 1, bytes(32), P["a"]["id"], "create", P["b"]["id"],
                                                        1, R.roster_hash({P["b"]["id"]: 1})), "a", "decode")
    add_bad("rotate with target", 3, rec_for(x3, "a", "rotate", None, target=P["b"]["id"]), "a", "decode")
    add_bad("add at epoch 1", 0, R.commit_record(room, 1, bytes(32), P["a"]["id"], "add", P["b"]["id"], 2,
                                                  bytes(32)), "a", "decode")
    good = rec_for(x3, "a", "promote", "b")
    add_bad("key does not match by", 3, good, "a", "verify", ik=P["c"]["ik"],
            sig=R.ed_sign(P["c"]["seed"], good))
    tampered = bytearray(R.ed_sign(P["a"]["seed"], good))
    tampered[5] ^= 1
    add_bad("tampered signature", 3, good, "a", "verify", sig=bytes(tampered))
    other = bytearray(good)
    other[97 + 1] ^= 1  # signature over different bytes
    add_bad("signature over other bytes", 3, good, "a", "verify", sig=R.ed_sign(P["a"]["seed"], bytes(other)))
    return {"room": room.hex(), "members": {k: {"id": v["id"].hex(), "ik": v["ik"].hex()} for k, v in P.items()},
            "steps": steps, "bad": bad, "final_epoch": r.epoch, "final_head": r.head.hex()}


def gen_mod():
    seed = R.stream("relay", 32)
    pub = R.ed_public(seed)
    author = R.device_id(R.ed_public(R.stream("post-ik", 32)))
    cases = [
        ("remove", "pub:general", 99814, T0 + 60, bytes.fromhex("0123456789abcdef"), author, 0),
        ("ban", "pub:general", 99815, T0 + 61, None, author, T0 + 86400),
        ("ban", "pub:general", 99816, T0 + 62, None, author, 0),
        ("unban", "pub:general", 99817, T0 + 63, None, author, 0),
        ("remove", "pub:" + "x" * 59, 1, 2, bytes.fromhex("ffffffffffffffff"), bytes(range(16)), 0),
    ]
    out = []
    for action, room, seq, ts, ref, dev, until in cases:
        rec = R.mod_record(action, room, seq, ts, ref, dev, until)
        sig = R.ed_sign(seed, rec)
        assert sys_verify(pub, rec, sig)
        fr = R.mod_frame(seed, action, room, seq, ts, ref, dev, until)
        assert json.loads(fr)["sig"] == R.b64(sig)
        out.append({"action": action, "room": room, "seq": seq, "ts": ts, "ref": ref.hex() if ref else "",
                    "device": dev.hex(), "until": until, "rec": rec.hex(), "sig": sig.hex(), "frame": fr})
    good = json.loads(out[0]["frame"])
    bad_frames = []

    def bad(note, **chg):
        f = dict(good)
        for k, v in chg.items():
            if v is None:
                f.pop(k, None)
            else:
                f[k] = v
        bad_frames.append({"note": note, "frame": R.dumps(f)})

    bad("remove without ref", ref=None)
    bad("remove without device", device=None)
    bad("until on remove", until=5)
    bad("by not relay", by="admin")
    bad("unknown action", action="mute")
    bad("room too long", room="pub:" + "x" * 60)
    bad("zero ref", ref="0000000000000000")
    bad("seq not a string", seq=99814)
    bad("short sig", sig=R.b64(bytes(63)))
    unban = next(json.loads(c["frame"]) for c in out if c["action"] == "unban")
    assert unban["action"] == "unban"
    f = dict(unban, until=5)
    bad_frames.append({"note": "until on unban", "frame": R.dumps(f)})
    tamper = dict(good, seq="99815")
    return {"relay_seed": seed.hex(), "relay_pub": pub.hex(), "cases": out, "bad_frames": bad_frames,
            "tampered": R.dumps(tamper)}


def gen_invite():
    admin_seed = R.stream("invite-admin", 32)
    admin_ik = R.ed_public(admin_seed)
    room = R.stream("invite-room", 16)
    secret = R.stream("invite-secret", 16)
    joiner = R.device_id(R.ed_public(R.stream("invite-joiner", 32)))
    key = R.invite_key(secret, room)
    proof = R.invite_proof(key, joiner)
    assert key == sys_hmac(secret, b"SC2-INVITE\x01" + room) if SYS else True
    url = "chats://relay.example:7778#pin=sha256/" + R.b64(R.sha256(b"spki"))
    code = R.invite_code(room, secret, admin_ik, url)
    bad = []
    c = bytearray(code)
    c[37] ^= 1
    bad.append({"note": "admin id does not match ik", "code": bytes(c).hex()})
    bad.append({"note": "url not chat(s)://", "code": R.invite_code(room, secret, admin_ik, "chats://x").hex()
                .replace(b"chats://x".hex(), b"https:/xx".hex())})
    bad.append({"note": "length mismatch", "code": (code + b"!").hex()})
    c = bytearray(code)
    c[4] = 2
    bad.append({"note": "unknown version", "code": bytes(c).hex()})
    c = bytearray(code)
    c[86 + 5] = 0x20
    bad.append({"note": "space in url", "code": bytes(c).hex()})
    return {"room": room.hex(), "secret": secret.hex(), "joiner": joiner.hex(), "key": key.hex(),
            "proof": proof.hex(), "admin_seed": admin_seed.hex(), "admin_ik": admin_ik.hex(),
            "admin_id": R.device_id(admin_ik).hex(), "url": url, "code": code.hex(), "text": R.invite_text(code),
            "bad": bad}


def gen_frames_admin(commit, mod, invite):
    fr = {}
    room = commit["room"]
    st = commit["steps"]
    fr["room_create_private"] = R.frame("room.create", rid="r1", room=room, kind="private",
                                        commit=R.b64(bytes.fromhex(st[0]["rec"])),
                                        sig=R.b64(bytes.fromhex(st[0]["sig"])))
    fr["room_commit"] = R.frame("room.commit", rid="c1", room=room, commit=R.b64(bytes.fromhex(st[1]["rec"])),
                                sig=R.b64(bytes.fromhex(st[1]["sig"])))
    fr["room_commit_msg"] = R.frame("room.commit", room=room, seq="4524", commit=R.b64(bytes.fromhex(st[1]["rec"])),
                                    sig=R.b64(bytes.fromhex(st[1]["sig"])))
    fr["room_invite"] = R.frame("room.invite", rid="i1", room=invite["room"],
                                key=R.b64(bytes.fromhex(invite["key"])))
    fr["room_invite_revoke"] = R.frame("room.invite", rid="i2", room=invite["room"],
                                       key=R.b64(bytes.fromhex(invite["key"])), revoke=True)
    fr["room_join_private"] = R.frame("room.join", rid="j1", room=invite["room"],
                                      invite=R.b64(bytes.fromhex(invite["proof"])))
    for i, name in enumerate(("room_mod_remove", "room_mod_ban", "room_mod_ban_indef", "room_mod_unban")):
        fr[name] = mod["cases"][i]["frame"]
    return fr


# ------------------------------------------------------------- C output

def cstr(s):
    out = '"'
    for ch in s.encode():
        if ch in (0x22, 0x5c):
            out += "\\" + chr(ch)
        elif 0x20 <= ch < 0x7f:
            out += chr(ch)
        else:
            out += "\\%03o" % ch
    return out + '"'


def emit_c(v):
    L = ["/* Generated by protocol/sc2/tools/gen_vectors.py from sc2_vectors.json. Do not edit. */",
         "/* clang-format off */", ""]

    def table(name, ctype, fields, rows):
        L.append("static const %s %s[] = {" % (ctype, name))
        for r in rows:
            vals = []
            for f, kind in fields:
                x = r[f]
                if kind == "s":
                    vals.append(cstr(x))
                elif kind == "b":
                    vals.append("1" if x else "0")
                else:
                    vals.append("%du" % x if x >= 0 else "%d" % x)
            L.append("    {" + ", ".join(vals) + "},")
        L.append("};")
        L.append("")

    L += ["typedef struct { const char *msg, *digest; } sc2v_sha256_t;",
          "typedef struct { const char *key, *msg, *mac; } sc2v_hmac_t;",
          "typedef struct { const char *ikm, *salt, *info; unsigned len; const char *prk, *okm; } sc2v_hkdf_t;",
          "typedef struct { const char *seed, *pk, *msg, *sig; } sc2v_ed_sign_t;",
          "typedef struct { const char *note, *pk, *msg, *sig; int ok; } sc2v_ed_verify_t;",
          "typedef struct { const char *scalar, *u, *out; } sc2v_x25519_t;",
          "typedef struct { const char *scalar, *u; } sc2v_x25519_bad_t;",
          "typedef struct { const char *key, *nonce, *ad, *pt, *ct; } sc2v_aead_t;",
          "typedef struct { const char *bin, *b64; } sc2v_b64_t;",
          "typedef struct { const char *ik_a, *ik_b, *hash, *shrt, *words, *full, *fp_a; } sc2v_safety_t;",
          "typedef struct { const char *alice_rng, *bob_rng, *opk_rng, *ek_rng; unsigned spk_id, opk_id;"
          " const char *ek_pub, *sk, *ad; } sc2v_x3dh_t;",
          "typedef struct { char actor, op; const char *pt; unsigned long long now; unsigned msg; int tamper, rc;"
          " unsigned rng_from, rng_to; const char *dh; unsigned pn, n; int x3dh; const char *nonce, *ct; }"
          " sc2v_step_t;", ""]
    table("SC2V_SHA256", "sc2v_sha256_t", [("msg", "s"), ("digest", "s")], v["sha256"])
    table("SC2V_HMAC", "sc2v_hmac_t", [("key", "s"), ("msg", "s"), ("mac", "s")], v["hmac"])
    table("SC2V_HKDF", "sc2v_hkdf_t", [("ikm", "s"), ("salt", "s"), ("info", "s"), ("len", "i"), ("prk", "s"),
                                       ("okm", "s")], v["hkdf"])
    table("SC2V_ED_SIGN", "sc2v_ed_sign_t", [("seed", "s"), ("pk", "s"), ("msg", "s"), ("sig", "s")],
          v["ed25519_sign"])
    table("SC2V_ED_VERIFY", "sc2v_ed_verify_t", [("note", "s"), ("pk", "s"), ("msg", "s"), ("sig", "s"),
                                                 ("ok", "b")], v["ed25519_verify"])
    table("SC2V_X25519", "sc2v_x25519_t", [("scalar", "s"), ("u", "s"), ("out", "s")], v["x25519"])
    table("SC2V_X25519_BAD", "sc2v_x25519_bad_t", [("scalar", "s"), ("u", "s")], v["x25519_reject"])
    L.append("static const char *const SC2V_X25519_ITER_1 = %s;" % cstr(v["x25519_iter"]["1"]))
    L.append("static const char *const SC2V_X25519_ITER_1000 = %s;" % cstr(v["x25519_iter"]["1000"]))
    L.append("")
    table("SC2V_AEAD", "sc2v_aead_t", [("key", "s"), ("nonce", "s"), ("ad", "s"), ("pt", "s"), ("ct", "s")],
          v["aead"])
    table("SC2V_B64", "sc2v_b64_t", [("bin", "s"), ("b64", "s")], v["b64"])
    L.append("static const char *const SC2V_B64_BAD[] = {" + ", ".join(cstr(s) for s in v["b64_bad"]) + "};")
    L.append("")
    sv = [dict(x, shrt=x["short"]) for x in v["safety"]]
    table("SC2V_SAFETY", "sc2v_safety_t", [("ik_a", "s"), ("ik_b", "s"), ("hash", "s"), ("shrt", "s"),
                                           ("words", "s"), ("full", "s"), ("fp_a", "s")], sv)
    table("SC2V_X3DH", "sc2v_x3dh_t", [("alice_rng", "s"), ("bob_rng", "s"), ("opk_rng", "s"), ("ek_rng", "s"),
                                       ("spk_id", "i"), ("opk_id", "i"), ("ek_pub", "s"), ("sk", "s"),
                                       ("ad", "s")], v["x3dh"])
    for k, x in v["identity"].items():
        L.append("#define SC2V_ID_%s %s" % (k.upper(), ("%du" % x) if isinstance(x, int) else cstr(x)))
    L.append("")
    rt = v["ratchet"]
    for k in ("alice_rng", "bob_rng", "sk", "ad", "alice_blob", "bob_blob"):
        L.append("#define SC2V_RT_%s %s" % (k.upper(), cstr(rt[k])))
    for k in ("alice_setup", "bob_setup", "alice_init", "spk_id", "opk_id", "alice_skipped", "bob_skipped"):
        L.append("#define SC2V_RT_%s %du" % (k.upper(), rt[k]))
    L.append("")
    steps = [dict(s, actor=s["actor"], op=s["op"]) for s in rt["steps"]]
    L.append("static const sc2v_step_t SC2V_RT_STEPS[] = {")
    for s in steps:
        L.append("    {'%s', '%s', %s, %dull, %du, %d, %d, %du, %du, %s, %du, %du, %d, %s, %s}," % (
            s["actor"], s["op"], cstr(s["pt"]), s["now"], s["msg"], s["tamper"], s["rc"], s["rng_from"],
            s["rng_to"], cstr(s["dh"]), s["pn"], s["n"], s["x3dh"], cstr(s["nonce"]), cstr(s["ct"])))
    L.append("};")
    L.append("")
    for k, x in v["room"].items():
        if k == "mk":
            for i, m in enumerate(x):
                L.append("#define SC2V_ROOM_MK%d %s" % (i, cstr(m)))
        else:
            L.append("#define SC2V_ROOM_%s %s" % (k.upper(), ("%du" % x) if isinstance(x, int) else cstr(x)))
    L.append("")
    for k, x in v["frames"].items():
        L.append("#define SC2V_FRAME_%s %s" % (k.upper(), cstr(x)))
    L.append("")
    cm = v["commit"]
    L.append("typedef struct { const char *by, *op, *rec, *sig, *ik, *id; unsigned count; const char *roster_hash; }"
             " sc2v_commit_t;")
    L.append("typedef struct { const char *note; unsigned at; const char *rec, *sig, *ik, *fail; } sc2v_commit_bad_t;")
    L.append("typedef struct { const char *action, *room; unsigned long long seq, ts; const char *ref, *device;"
             " unsigned long long until; const char *rec, *sig, *frame; } sc2v_mod_t;")
    L.append("typedef struct { const char *note, *frame; } sc2v_mod_bad_t;")
    L.append("typedef struct { const char *note, *code; } sc2v_invite_bad_t;")
    L.append("")
    L.append("#define SC2V_COMMIT_ROOM %s" % cstr(cm["room"]))
    L.append("#define SC2V_COMMIT_FINAL_EPOCH %du" % cm["final_epoch"])
    L.append("#define SC2V_COMMIT_FINAL_HEAD %s" % cstr(cm["final_head"]))
    for k, m in cm["members"].items():
        L.append("#define SC2V_COMMIT_ID_%s %s" % (k.upper(), cstr(m["id"])))
        L.append("#define SC2V_COMMIT_IK_%s %s" % (k.upper(), cstr(m["ik"])))
    table("SC2V_COMMIT_STEPS", "sc2v_commit_t", [("by", "s"), ("op", "s"), ("rec", "s"), ("sig", "s"), ("ik", "s"),
                                                 ("id", "s"), ("count", "i"), ("roster_hash", "s")], cm["steps"])
    table("SC2V_COMMIT_BAD", "sc2v_commit_bad_t", [("note", "s"), ("at", "i"), ("rec", "s"), ("sig", "s"),
                                                   ("ik", "s"), ("fail", "s")], cm["bad"])
    md = v["mod"]
    L.append("#define SC2V_MOD_RELAY_SEED %s" % cstr(md["relay_seed"]))
    L.append("#define SC2V_MOD_RELAY_PUB %s" % cstr(md["relay_pub"]))
    L.append("#define SC2V_MOD_TAMPERED %s" % cstr(md["tampered"]))
    L.append("static const sc2v_mod_t SC2V_MOD[] = {")
    for c in md["cases"]:
        L.append("    {%s, %s, %dull, %dull, %s, %s, %dull, %s, %s, %s}," % (
            cstr(c["action"]), cstr(c["room"]), c["seq"], c["ts"], cstr(c["ref"]), cstr(c["device"]), c["until"],
            cstr(c["rec"]), cstr(c["sig"]), cstr(c["frame"])))
    L.append("};")
    L.append("")
    table("SC2V_MOD_BAD", "sc2v_mod_bad_t", [("note", "s"), ("frame", "s")], md["bad_frames"])
    iv = v["invite"]
    for k in ("room", "secret", "joiner", "key", "proof", "admin_seed", "admin_ik", "admin_id", "url", "code",
              "text"):
        L.append("#define SC2V_INVITE_%s %s" % (k.upper(), cstr(iv[k])))
    L.append("")
    table("SC2V_INVITE_BAD", "sc2v_invite_bad_t", [("note", "s"), ("code", "s")], iv["bad"])
    L.append("#define SC2V_COUNT(a) (sizeof(a) / sizeof((a)[0]))")
    return "\n".join(L) + "\n"


def build():
    sign, verify = gen_ed25519()
    x, xbad, iters = gen_x25519()
    good, bad = gen_b64()
    ratchet = gen_ratchet()
    room = gen_room()
    v = {
        "suite": "SC2-1",
        "generator": "protocol/sc2/tools/gen_vectors.py",
        "libsodium": "PyNaCl %s (bundled libsodium)" % __import__("nacl").__version__ +
                     ("; system %s" % SYS.sodium_version_string().decode() if SYS else ""),
        "sha256": gen_sha256(), "hmac": gen_hmac(), "hkdf": gen_hkdf(),
        "ed25519_sign": sign, "ed25519_verify": verify,
        "x25519": x, "x25519_reject": xbad, "x25519_iter": {str(k): s for k, s in iters.items()},
        "aead": gen_aead(), "b64": good, "b64_bad": bad,
        "identity": gen_identity(), "x3dh": gen_x3dh(), "ratchet": ratchet, "room": room,
        "safety": gen_safety(), "frames": gen_frames(ratchet, room),
    }
    v["commit"], v["mod"], v["invite"] = gen_commit(), gen_mod(), gen_invite()
    v["frames"].update(gen_frames_admin(v["commit"], v["mod"], v["invite"]))
    js = json.dumps(v, indent=1, ensure_ascii=False, sort_keys=False) + "\n"
    return js, emit_c(v)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="fail if the committed vectors differ")
    args = ap.parse_args()
    js, inc = build()
    jpath, cpath = os.path.join(OUT_DIR, "sc2_vectors.json"), os.path.join(OUT_DIR, "sc2_vectors.inc")
    if args.check:
        ok = True
        for path, data in ((jpath, js), (cpath, inc)):
            with open(path, encoding="utf-8") as f:
                stored = f.read()
                if path == jpath:
                    # Generator provenance varies by environment; protocol
                    # values must match regardless of the installed PyNaCl.
                    expected, actual = json.loads(stored), json.loads(data)
                    expected.pop("libsodium", None)
                    actual.pop("libsodium", None)
                    matches = expected == actual
                else:
                    matches = stored == data
                if not matches:
                    print("differs:", path)
                    ok = False
        print("vectors up to date" if ok else "vectors out of date")
        return 0 if ok else 1
    os.makedirs(OUT_DIR, exist_ok=True)
    with open(jpath, "w", encoding="utf-8") as f:
        f.write(js)
    with open(cpath, "w", encoding="utf-8") as f:
        f.write(inc)
    print("wrote", os.path.relpath(jpath), "and", os.path.relpath(cpath))
    return 0


if __name__ == "__main__":
    sys.exit(main())
