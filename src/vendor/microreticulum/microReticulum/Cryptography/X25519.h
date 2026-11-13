#pragma once

#include "../Bytes.h"

extern "C" {
#include <crypto/refc/x25519.h>
}

#include <esp_random.h>

#include <memory>
#include <stdexcept>
#include <stdint.h>
#include <string.h>

namespace RNS { namespace Cryptography {

	class X25519PublicKey {

	public:
		using Ptr = std::shared_ptr<X25519PublicKey>;

	public:
		X25519PublicKey(const Bytes& publicKey) {
			_publicKey = publicKey;
		}
		~X25519PublicKey() {}

	public:
		static inline Ptr from_public_bytes(const Bytes& publicKey) {
			return Ptr(new X25519PublicKey(publicKey));
		}

		Bytes public_bytes() {
			return _publicKey;
		}

	private:
		Bytes _publicKey;

	};

	class X25519PrivateKey {

	public:
		using Ptr = std::shared_ptr<X25519PrivateKey>;

	public:
		X25519PrivateKey(const Bytes& privateKey) {
			if (privateKey) {
				if (privateKey.size() != 32) {
					throw std::invalid_argument("X25519 private key must be 32 bytes");
				}
				_privateKey = privateKey;
			}
			else {
				esp_fill_random(_privateKey.writable(32), 32);
			}
			if (x25519_base(_publicKey.writable(32), _privateKey.data(), 1) != 0) {
				throw std::runtime_error("X25519 public key derivation failed");
			}
		}
		~X25519PrivateKey() {}

	public:
		static inline Ptr generate() {
			return Ptr(new X25519PrivateKey(Bytes::NONE));
		}

		static inline Ptr from_private_bytes(const Bytes& privateKey) {
			return Ptr(new X25519PrivateKey(privateKey));
		}

		inline const Bytes& private_bytes() {
			return _privateKey;
		}

		inline X25519PublicKey::Ptr public_key() {
			return X25519PublicKey::from_public_bytes(_publicKey);
		}

		inline const Bytes exchange(const Bytes& peer_public_key) {
			if (peer_public_key.size() != 32) {
				throw std::invalid_argument("X25519 peer public key must be 32 bytes");
			}
			// RFC 7748 requires the high bit of the u-coordinate to be ignored.
			uint8_t peer[32];
			memcpy(peer, peer_public_key.data(), 32);
			peer[31] &= 0x7f;
			Bytes sharedKey;
			if (x25519(sharedKey.writable(32), _privateKey.data(), peer, 1) != 0) {
				throw std::runtime_error("Peer key is invalid");
			}
			return sharedKey;
		}

	private:
		Bytes _privateKey;
		Bytes _publicKey;

	};

} }
