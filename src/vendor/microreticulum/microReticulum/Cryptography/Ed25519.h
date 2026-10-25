#pragma once

#include "../Bytes.h"

#include "../../../meshcore/ed25519/ed_25519.h"

#include <esp_random.h>

#include <memory>
#include <stdexcept>

namespace RNS { namespace Cryptography {

	class Ed25519PublicKey {

	public:
		using Ptr = std::shared_ptr<Ed25519PublicKey>;

	public:
		Ed25519PublicKey(const Bytes& publicKey) {
			_publicKey = publicKey;
		}
		~Ed25519PublicKey() {}

	public:
		static inline Ptr from_public_bytes(const Bytes& publicKey) {
			return Ptr(new Ed25519PublicKey(publicKey));
		}

		inline const Bytes& public_bytes() {
			return _publicKey;
		}

		inline bool verify(const Bytes& signature, const Bytes& message) {
			if (signature.size() != 64 || _publicKey.size() != 32) {
				return false;
			}
			return ed25519_verify(signature.data(), message.data(), message.size(), _publicKey.data()) == 1;
		}

	private:
		Bytes _publicKey;

	};

	// The private key is the 32-byte RFC 8032 seed.
	class Ed25519PrivateKey {

	public:
		using Ptr = std::shared_ptr<Ed25519PrivateKey>;

	public:
		Ed25519PrivateKey(const Bytes& privateKey) {
			if (privateKey) {
				if (privateKey.size() != 32) {
					throw std::invalid_argument("Ed25519 private key must be 32 bytes");
				}
				_privateKey = privateKey;
			}
			else {
				esp_fill_random(_privateKey.writable(32), 32);
			}
			ed25519_create_keypair(_publicKey.writable(32), _expandedKey.writable(64), _privateKey.data());
		}
		~Ed25519PrivateKey() {}

	public:
		static inline Ptr generate() {
			return Ptr(new Ed25519PrivateKey(Bytes::NONE));
		}

		static inline Ptr from_private_bytes(const Bytes& privateKey) {
			return Ptr(new Ed25519PrivateKey(privateKey));
		}

		inline const Bytes& private_bytes() {
			return _privateKey;
		}

		inline Ed25519PublicKey::Ptr public_key() {
			return Ed25519PublicKey::from_public_bytes(_publicKey);
		}

		inline const Bytes sign(const Bytes& message) {
			Bytes signature;
			ed25519_sign(signature.writable(64), message.data(), message.size(), _publicKey.data(), _expandedKey.data());
			return signature;
		}

	private:
		Bytes _privateKey;
		Bytes _publicKey;
		Bytes _expandedKey;

	};

} }
