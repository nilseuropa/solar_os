#include "Hashes.h"

#include <mbedtls/sha256.h>
#include <mbedtls/sha512.h>

#include <stdexcept>

using namespace RNS;

const Bytes RNS::Cryptography::sha256(const Bytes& data) {
	Bytes hash;
	if (mbedtls_sha256(data.data(), data.size(), hash.writable(32), 0) != 0) {
		throw std::runtime_error("SHA-256 failed");
	}
	return hash;
}

const Bytes RNS::Cryptography::sha512(const Bytes& data) {
	Bytes hash;
	if (mbedtls_sha512(data.data(), data.size(), hash.writable(64), 0) != 0) {
		throw std::runtime_error("SHA-512 failed");
	}
	return hash;
}
