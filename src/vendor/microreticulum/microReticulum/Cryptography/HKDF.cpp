#include "HKDF.h"

#include "HMAC.h"

#include <stdexcept>
#include <string.h>

using namespace RNS;

// RFC 5869 HKDF-SHA256, matching RNS.Cryptography.hkdf.
const Bytes RNS::Cryptography::hkdf(size_t length, const Bytes& derive_from, const Bytes& salt /*= {Bytes::NONE}*/, const Bytes& context /*= {Bytes::NONE}*/) {
	if (length == 0) {
		throw std::invalid_argument("Invalid output key length");
	}
	if (!derive_from) {
		throw std::invalid_argument("Cannot derive key from empty input material");
	}

	Bytes zero_salt;
	memset(zero_salt.writable(32), 0, 32);
	Bytes prk = HMAC::generate(salt ? salt : zero_salt, derive_from)->digest();

	Bytes derived;
	Bytes block;
	uint8_t counter = 0;
	while (derived.size() < length) {
		HMAC hmac(prk);
		hmac.update(block);
		if (context) {
			hmac.update(context);
		}
		++counter;
		hmac.update(Bytes(&counter, 1));
		block = hmac.digest();
		derived += block;
	}
	return derived.left(length);
}
