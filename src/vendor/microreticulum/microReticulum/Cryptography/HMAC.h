#pragma once

#include "../Bytes.h"

#include <mbedtls/md.h>

#include <memory>
#include <stdexcept>

namespace RNS { namespace Cryptography {

	class HMAC {

	public:
		enum Digest {
			DIGEST_NONE,
			DIGEST_SHA256,
			DIGEST_SHA512,
		};
		using Ptr = std::shared_ptr<HMAC>;

	public:
		HMAC(const Bytes& key, const Bytes& msg = {Bytes::NONE}, Digest digest = DIGEST_SHA256) {
			mbedtls_md_type_t type;
			switch (digest) {
			case DIGEST_SHA256:
				type = MBEDTLS_MD_SHA256;
				break;
			case DIGEST_SHA512:
				type = MBEDTLS_MD_SHA512;
				break;
			default:
				throw std::invalid_argument("Unknown or unsupported digest");
			}
			mbedtls_md_init(&_ctx);
			const mbedtls_md_info_t* info = mbedtls_md_info_from_type(type);
			if (mbedtls_md_setup(&_ctx, info, 1) != 0 ||
				mbedtls_md_hmac_starts(&_ctx, key.data(), key.size()) != 0) {
				mbedtls_md_free(&_ctx);
				throw std::runtime_error("HMAC setup failed");
			}
			_size = mbedtls_md_get_size(info);
			if (msg) {
				// The destructor does not run if the constructor throws.
				try {
					update(msg);
				} catch (...) {
					mbedtls_md_free(&_ctx);
					throw;
				}
			}
		}

		HMAC(const HMAC&) = delete;
		HMAC& operator=(const HMAC&) = delete;

		~HMAC() {
			mbedtls_md_free(&_ctx);
		}

		void update(const Bytes& msg) {
			if (mbedtls_md_hmac_update(&_ctx, msg.data(), msg.size()) != 0) {
				throw std::runtime_error("HMAC update failed");
			}
		}

		// Returns the MAC of everything fed so far and resets for another message.
		// Don't finish a clone: mbedtls_md_clone() doesn't copy the HMAC key schedule.
		Bytes digest() {
			Bytes result;
			int rc = mbedtls_md_hmac_finish(&_ctx, result.writable(_size));
			if (rc == 0) rc = mbedtls_md_hmac_reset(&_ctx);
			if (rc != 0) {
				throw std::runtime_error("HMAC finish failed");
			}
			return result;
		}

		static inline Ptr generate(const Bytes& key, const Bytes& msg = {Bytes::NONE}, Digest digest = DIGEST_SHA256) {
			return Ptr(new HMAC(key, msg, digest));
		}

	private:
		mbedtls_md_context_t _ctx;
		size_t _size = 0;
	};

	inline const Bytes digest(const Bytes& key, const Bytes& msg, HMAC::Digest digest = HMAC::DIGEST_SHA256) {
		HMAC hmac(key, msg, digest);
		return hmac.digest();
	}

} }
