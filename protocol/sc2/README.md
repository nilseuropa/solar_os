# SC2 portable crypto and session core

`protocol/sc2` is the SolarOS Chat v2 (SC2) end-to-end encryption core: plain
C11, no SDK, no heap, no global state, no clock and no RNG of its own. The
caller injects randomness (`sc2_rng_t`) and the time (`now`, seconds). The
same code runs in the host tests, in the firmware (`service_sc2`) and in the C
relay subset inside `chatd`.

It implements suite **SC2-1** of the SolarOS Chat v2 protocol. This README
covers the C API and the byte layouts that the code fixes.

## Contents

| Header | What |
| --- | --- |
| `sc2_common.h` | error codes, `sc2_rng_t`, `sc2_part_t`, big-endian helpers, constant-time compare, CRC-32 |
| `sc2_hash.h` | SHA-256, HMAC-SHA-256 and HKDF (RFC 5869). Portable by default; `SC2_USE_MBEDTLS=1` uses mbedTLS SHA-256 and `mbedtls_md` HMAC (ESP-IDF builds `MBEDTLS_HKDF_C` off, so HKDF is always ours) |
| `sc2_crypto.h` | strict Ed25519 over the orlp sources (`src/vendor/meshcore/ed25519`), X25519 with all-zero rejection and XChaCha20-Poly1305 over the WireGuard refc code (`components/wireguard_lwip`) |
| `sc2_codec.h` | strict base64 (standard alphabet, padding, zero trailing bits), hex, padding buckets 256/512/1024/2048/4096/8192 (`0x80` then zeros, minimal bucket enforced) |
| `sc2_identity.h` | IK (Ed25519), DK (X25519, signed by IK), SPK, OPK, device ID, relay `auth` signature |
| `sc2_x3dh.h` | prekey bundles, X3DH initiate/respond |
| `sc2_ratchet.h` | double ratchet, DM header, skipped-key store, versioned state blob |
| `sc2_room.h` | sender-key room chains, room header, seal/open/verify |
| `sc2_safety.h` | safety numbers: 6×4 digits, 8 BIP-39 words, 60-digit full form, own fingerprint |
| `sc2_json.h` | bounded JSON reader (depth 8, strict UTF-8, duplicate keys rejected on lookup) and writer (byte-identical to Python `json.dumps(separators=(",", ":"), ensure_ascii=False)`) |
| `sc2_frame.h` | line assembler (16384-byte lines), `hello`/`welcome`/`auth`, `dm.send`/`dm.msg`, `room.send`/`room.msg`, public posts (`room.post`/`room.pub`) with signed bytes, private `room.create`, `room.commit` (both directions), `room.invite`, `room.join` with a proof, `room.mod` decode |
| `sc2_commit.h` | private-room commits (130-byte record, sign/verify, commit ID), the roster (`sc2_roster_t`: make, check, apply, roster hash), invite key/proof and the invite code (binary and `sc2i:` text) |
| `sc2_mod.h` | relay-signed moderation record (122 bytes: remove/ban/unban), encode/decode/sign/verify |
| `sc2_minirelay.h` | sans-IO relay subset for small devices (`chatd`): hello/auth, keys publish/claim, DM mailbox, one public room, fixed caps in one allocation, bounded connection slots (one session per device, idle timeout, eviction when full); the host does the sockets |
| `sc2.h` | umbrella header |

Every function returns `SC2_OK` (0) or a negative `SC2_ERR_*`. Secrets on the
stack are wiped with `sc2_memzero`.

## Strictness

- **Ed25519 verify** matches libsodium's `crypto_sign_verify_detached`: S < L,
  canonical A and R that decode and are not of small order, then the usual
  check (`[S]B = R + [k]A`, compared bytewise). orlp's own `ed25519_verify`
  only checks the top bits of S, so SC2 does not call it. The vectors include
  the malleable, non-canonical and small-order cases, each checked against
  libsodium.
- **X25519** rejects an all-zero shared secret (constant time) and returns
  `SC2_ERR_KEY`. The output is zeroed.
- **AEAD**: XChaCha20-Poly1305 (IETF construction, as libsodium
  `crypto_aead_xchacha20poly1305_ietf_*`). Works in place.

## Byte layouts (all integers big-endian)

| Item | Bytes |
| --- | --- |
| DK signature | Ed25519(IK, `"SC2-DK\x01" ‖ dk_pub`) |
| SPK signature | Ed25519(IK, `"SC2-SPK\x01" ‖ u32 spk_id ‖ spk_pub`) |
| relay auth | Ed25519(IK, `"SC2-AUTH\x01" ‖ u8 len ‖ relay_id ‖ challenge[32] ‖ ik_pub`) |
| device ID | first 16 bytes of SHA-256(ik_pub); 32 hex chars on the wire |
| X3DH | DH1 = DH(DK_A, SPK_B), DH2 = DH(EK_A, DK_B), DH3 = DH(EK_A, SPK_B), DH4 = DH(EK_A, OPK_B) optional; SK = HKDF(salt 0³², IKM 0xFF³² ‖ DH1..DH4, `"SC2-X3DH\x01"`, 32); AD = IK_A ‖ IK_B |
| root KDF | (rk, ck) = HKDF(salt rk, IKM dh, `"SC2-RK\x01"`, 64) |
| chain KDF | mk = HMAC(ck, 0x01), ck' = HMAC(ck, 0x02) |
| DM header Hd | `"SC2D" ‖ u8 suite ‖ from[16] ‖ to[16] ‖ dh[32] ‖ u32 pn ‖ u32 n ‖ u8 flags [‖ ek[32] ‖ u32 spk ‖ u32 opk]`: 78 bytes, 118 with X3DH (flag 0x01) |
| DM AEAD | key mk, random 24-byte nonce, AAD = AD ‖ Hd, plaintext padded |
| room chain | CK0 = HKDF(salt room[16] ‖ u32 epoch, IKM epoch_key, `"SC2-SENDER\x01" ‖ sender[16]`, 32) |
| room header H | `"SC2R" ‖ u8 suite ‖ room[16] ‖ u32 epoch ‖ from[16] ‖ u32 n` (45 bytes) |
| room message | ct = XChaCha(mk, nonce, pad(pt), AAD = H); sig = Ed25519(IK, `"SC2-ROOM\x01" ‖ H ‖ nonce ‖ ct`) |
| public post | Ed25519(IK, `"SC2-PUB\x01" ‖ u8 suite ‖ u8 len ‖ relay_id ‖ u8 len ‖ room ‖ ik[32] ‖ id[8] ‖ u64 ts ‖ u8 t (1 text, 2 del) ‖ u8 len ‖ name ‖ u16 len ‖ body ‖ ref[8]`), ref all-zero when absent |
| commit | `"SC2-COMMIT\x01" ‖ u8 suite ‖ room[16] ‖ u32 epoch ‖ prev[32] ‖ by[16] ‖ u8 op ‖ target[16] ‖ u8 count ‖ roster_hash[32]` (130 bytes); op 1 create, 2 add, 3 remove, 4 promote, 5 demote, 6 rotate; commit ID = SHA-256(record); sig = Ed25519(IK_by, record); roster_hash = SHA-256 over sorted `device[16] ‖ u8 role` (1 admin) |
| moderation | `"SC2-MOD\x01" ‖ u8 suite ‖ u8 action (1 remove, 2 ban, 3 unban) ‖ room[64] (UTF-8, zero padded) ‖ u64 seq ‖ u64 ts ‖ ref[8] ‖ device[16] ‖ u64 until` (122 bytes), signed by the relay key |
| invite | key = HMAC(secret[16], `"SC2-INVITE\x01" ‖ room[16]`); proof = HMAC(key, `"SC2-JOIN\x01" ‖ joiner[16]`); code = `"SC2I" ‖ u8 1 ‖ room[16] ‖ secret[16] ‖ admin_id[16] ‖ admin_ik[32] ‖ u8 url_len ‖ url` (86..286 bytes), text `sc2i:` + base64 |
| safety | h = SHA-256(`"SC2-SAFETY\x01"` ‖ min(IK_A, IK_B) ‖ max(IK_A, IK_B)); short = six groups of (u40 from h[5i..5i+4]) mod 10⁴; words = first 88 bits as eight 11-bit BIP-39 indices; full = HKDF-Expand(h, `"SC2-SAFETY-FULL\x01"`, 60) as twelve groups of u40 mod 10⁵; own fingerprint = short form of SHA-256(IK) |

### Ratchet state blob (version 1)

`"SC2S" ‖ u8 ver ‖ u8 flags ‖ ad[64] ‖ self[16] ‖ peer[16] ‖ rk ‖ dhs_priv ‖
dhs_pub ‖ dhr ‖ cks ‖ ckr ‖ u32 ns ‖ u32 nr ‖ u32 pn ‖ ek[32] ‖ u32 spk ‖ u32 opk ‖
u8 count ‖ count × (dh[32] ‖ u32 n ‖ mk[32] ‖ u64 stored_at) ‖ u32 crc32`

That is 351 + 76 × count bytes, at most 2783 (`sc2_ratchet_blob_max()`).
CRC-32 is the zlib polynomial. Unknown versions, flags, lengths or a bad CRC
give `SC2_ERR_FORMAT`. The blob holds secrets: the caller stores it encrypted
or in protected storage (the firmware keeps it under `/.sc2/`).

### Ratchet rules

- The initiator's first sending ratchet key targets SPK_B; the responder uses
  its SPK as the initial ratchet key. The initiator attaches the X3DH header
  until its first successful decrypt.
- At most 32 skipped keys per gap (`SC2_ERR_SKIP` above that) and 32 stored
  in total (oldest evicted first). Stored keys expire after 7 days of `now`.
- `sc2_ratchet_decrypt` works on a caller-supplied `work` state (about 2.9 KB,
  1 KB in the low-RAM profile; keep it static or in PSRAM, not on a small task
  stack) and commits only on success. A failed decrypt changes nothing. A
  replay gives `SC2_ERR_REPLAY`. Decrypt may run in place (`pt == ct`): the
  tag is checked before anything is decrypted.
- RNG draws, in order: identity 64 (IK seed, DK), SPK 32, OPK 32, X3DH EK 32,
  initiator ratchet key 32, each encrypt 24 (nonce), each DH step on decrypt 32.

## Commit rules (`sc2_roster_check` / `sc2_roster_apply`)

One change per commit. A commit applies only if the room matches, `epoch` is
the roster's epoch + 1 and `prev` its head, `by` is an admin (for create: the
creator, on an empty roster), the op fits the roster (add a non-member;
remove, promote or demote a member; promote a non-admin, demote an admin;
never remove or demote the last admin) and `count`/`roster_hash` match the
result. Verify the signature first (`sc2_commit_verify` with the IK whose
device ID is `by`). On any error the roster is left untouched.
`sc2_roster_t` is about 600 bytes (32 entries); every function works on
fixed buffers with stack frames of at most 336 bytes (xtensa, `-O2`).

## Low-RAM profile (no PSRAM)

`SC2_LOW_RAM=1` (default 0) is set by the SolarOS firmware build when
`CONFIG_SPIRAM` is off (`src/CMakeLists.txt`); the core itself includes no SDK
headers. Other builds pass `-DSC2_LOW_RAM=1`.
It only changes local limits, never the wire:

| | default | low-RAM |
| --- | ---: | ---: |
| stored skipped keys per session (`SC2_RATCHET_MAX_STORED`) | 32 | 8 |
| `sc2_ratchet_t` | 2,904 B | 984 B |
| engine work memory, worst operation (`SC2E_WORK_BUDGET`) | 20.5 KiB | 16.5 KiB |

A state blob saved with more stored keys than the build allows loads and
keeps the newest ones.

## Device private-room trust

The relay's `room.event` and `room.state` are routing information. They do not
authorize membership or key installation. The device client uses two additional
inner DM records, which the relay transports as opaque ciphertext:

- `room_join`: `room` and base64 `proof`. The proof is HMAC-SHA-256 with the
  invite's 16-byte secret over `SC2-ADMIN-JOIN\x01 || room:16 || joiner:16`.
  The receiver checks that joiner is the authenticated DM sender. This key is
  distinct from the invite key registered with the relay.
- `epoch_key`: the existing `room`, `epoch`, and `key`, plus base64 `head`
  (32-byte commit ID) and `roster` (sorted `device:16 || role:u8` entries).
  The invited admin authenticates this checkpoint through its encrypted DM.
  The roster must include the recipient and the invited admin; it must not
  roll back the trusted epoch or conflict with an existing head at that epoch.
  Optional `title` supplies a printable UTF-8 room name (up to 48 bytes),
  authenticated by the same admin DM. Local room records upgrade version 1
  without discarding membership or invite keys; version 2 appends the title.

Text inner JSON may include `name`, a printable UTF-8 sender label up to 64
bytes. The SolarOS client sends its `user@hostname` identity. Private-room and
DM labels are protected by the message encryption; public labels are signed
with the post. Missing or invalid labels fall back to the device ID.

The checkpoint bootstraps a new member without requiring room history from
before admission. It also recovers membership after retained commit history
expires. Subsequent `room.commit` records must match the outer room, identify
an admin in the trusted roster, verify under that admin's identity key, and
pass `sc2_roster_apply()` (epoch, previous head, operation and roster hash).
Members wait for the new epoch key after a commit. A valid signed removal of
this device erases its room keys. Unsigned removal notifications cannot do so.

`sc2e_dm_open()` returns room controls without applying them. The network
client authorizes them before `sc2e_dm_apply_control()` can change room keys.
The lower-level `sc2e_dm_decrypt()` API retains its engine-only control handling;
applications using that API must supply their own room-admission policy.
Both devices need the admission exchange above; legacy join notifications and
bare epoch keys do not provide a secure fallback.

## Build and test

Host tests live in `tests/host` (`sc2_crypto_test`, `sc2_session_test`,
`sc2_frame_test`, `sc2_admin_test`, plus `sc2_engine_test` and its
`sc2_engine_lowram_test` build) and use the vectors in
`vectors/sc2_vectors.inc`:

```sh
make -C tests/host sc2-test          # also part of `make -C tests/host test`
make -C tests/host sc2-mbedtls-test  # mbedTLS backend (uses the ESP-IDF mbedTLS that PlatformIO installs)
python3 -m unittest tests/test_sc2_protocol.py   # ASan/UBSan, strict warnings, C++ headers
cmake -S protocol/sc2 -B build/sc2 -DSC2_BUILD_TESTS=ON && cmake --build build/sc2 && ctest --test-dir build/sc2
```

The library links the vendor sources itself (`fe.c ge.c sc.c sha512.c` from
orlp, `x25519.c chacha20.c chacha20poly1305.c poly1305-donna.c` and `crypto.c`
from the WireGuard component). The firmware build (`service_sc2`) shares
MeshCore's copy of the orlp sources instead of compiling its own.

### Vectors

`tools/gen_vectors.py` regenerates `vectors/sc2_vectors.{json,inc}` with an
independent Python implementation (`tools/sc2ref.py`) on PyNaCl (libsodium).
It asserts the RFC 8032, RFC 7748 (including the 1 and 1000 iteration
values), RFC 4231, RFC 5869 and XChaCha20-Poly1305 (draft-irtf-cfrg-xchacha
§A.3.1) vectors, and checks Ed25519 and HMAC against the system libsodium as
well when it is installed.

```sh
python3 -m venv ~/sc2-venv && ~/sc2-venv/bin/pip install pynacl
~/sc2-venv/bin/python protocol/sc2/tools/gen_vectors.py --check   # or without --check to rewrite
```

The JSON file is language-neutral and is also used by the Python reference
relay, which is maintained in a separate repository,
[solar_os_sc2-relay](https://github.com/LowFlowIO/solar_os_sc2-relay). Its test
suite vendors a copy of `sc2_vectors.json` and `tools/sc2ref.py`, and its C cross
test builds this tree's `protocol/sc2` and `src/services/solar_os_sc2_*` sources;
it finds this checkout through the `SC2_CLIENT_DIR` environment variable. After
regenerating the vectors here, copy them into the relay repository's
`tests/vendor/` and `tests/data/`.

## Licence

MIT (see `LICENSE`). The BIP-39 word list is from trezor/python-mnemonic
(MIT). The vendor crypto keeps its own licence: orlp ed25519 (zlib) and the
WireGuard component (see `components/wireguard_lwip`).
