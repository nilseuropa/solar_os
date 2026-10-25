#pragma once

#include "../Bytes.h"

#include <mbedtls/aes.h>

#include <stdexcept>
#include <string.h>

namespace RNS { namespace Cryptography {

	// AES-CBC without padding; callers apply PKCS7 themselves.
	template <size_t KeyBits>
	class AES_CBC {

	public:
		static inline const Bytes encrypt(const Bytes& plaintext, const Bytes& key, const Bytes& iv) {
			Bytes ciphertext;
			crypt(MBEDTLS_AES_ENCRYPT, ciphertext.writable(plaintext.size()), plaintext.data(), plaintext.size(), key, iv);
			return ciphertext;
		}

		static inline const Bytes decrypt(const Bytes& ciphertext, const Bytes& key, const Bytes& iv) {
			Bytes plaintext;
			crypt(MBEDTLS_AES_DECRYPT, plaintext.writable(ciphertext.size()), ciphertext.data(), ciphertext.size(), key, iv);
			return plaintext;
		}

		// Bytes shares its buffer between copies, so "in place" assigns a new
		// one rather than writing through data().
		static inline void inplace_encrypt(Bytes& plaintext, const Bytes& key, const Bytes& iv) {
			plaintext = encrypt(plaintext, key, iv);
		}

		static inline void inplace_decrypt(Bytes& ciphertext, const Bytes& key, const Bytes& iv) {
			ciphertext = decrypt(ciphertext, key, iv);
		}

	private:
		static inline void crypt(int mode, uint8_t* out, const uint8_t* in, size_t length, const Bytes& key, const Bytes& iv) {
			if (key.size() != KeyBits / 8 || iv.size() != 16 || length % 16 != 0) {
				throw std::invalid_argument("Invalid AES-CBC key, IV or length");
			}
			uint8_t running_iv[16];
			memcpy(running_iv, iv.data(), 16);
			mbedtls_aes_context ctx;
			mbedtls_aes_init(&ctx);
			int rc = (mode == MBEDTLS_AES_ENCRYPT)
				? mbedtls_aes_setkey_enc(&ctx, key.data(), KeyBits)
				: mbedtls_aes_setkey_dec(&ctx, key.data(), KeyBits);
			if (rc == 0) rc = mbedtls_aes_crypt_cbc(&ctx, mode, length, running_iv, in, out);
			mbedtls_aes_free(&ctx);
			if (rc != 0) {
				throw std::runtime_error("AES-CBC failed");
			}
		}
	};

	using AES_128_CBC = AES_CBC<128>;
	using AES_256_CBC = AES_CBC<256>;

} }
