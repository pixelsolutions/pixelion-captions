// Feature: macos-platform-support
//
// Host-runnable, dependency-free reimplementations of the pure logic that the
// macOS port reuses or must preserve. Each piece mirrors the algorithm used in
// the shared/platform adapter code exactly so the property/unit tests validate
// the real contract without linking OBS or Qt.
//
//   * AudioRingBuffer   -> src/platform/{windows,macos} AudioQueue (Prop 6)
//   * sha256 / base64Url -> PKCE S256 + state generation (Prop 5)
//   * MockKeychain       -> Keychain / Credential adapter semantics (Props 1-3, 10)
//   * buildRealtimeTokenBody / buildSessionConfig -> request payloads (Prop 9)
//   * redactLine         -> Debug_Log data-minimization (Props 7-8)

#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace purelogic {

// ---------------------------------------------------------------------------
// Audio ring buffer (mirror of AudioQueue in the live-session adapters).
// Fixed capacity, drop-on-full, no allocation on overflow.
// ---------------------------------------------------------------------------
class AudioRingBuffer {
public:
	static constexpr size_t kSlots = 64;
	static constexpr size_t kBytesPerSlot = 4096;

	// Returns true if the frame was stored, false if dropped (full or too big).
	// Mirrors audioCallback: a frame larger than a slot is dropped; when the
	// next write index would collide with read, the frame is dropped.
	bool push(const uint8_t *data, size_t size)
	{
		if (size > kBytesPerSlot)
			return false;
		const size_t next = (write_ + 1) % kSlots;
		if (next == read_)
			return false; // full -> drop, no allocation
		std::memcpy(bytes_[write_].data(), data, size);
		sizes_[write_] = static_cast<uint32_t>(size);
		write_ = next;
		return true;
	}

	bool pop(std::array<uint8_t, kBytesPerSlot> &out, uint32_t &size)
	{
		if (read_ == write_)
			return false;
		size = sizes_[read_];
		out = bytes_[read_];
		read_ = (read_ + 1) % kSlots;
		return true;
	}

	// Number of currently buffered slots.
	size_t buffered() const { return (write_ + kSlots - read_) % kSlots; }

private:
	std::array<std::array<uint8_t, kBytesPerSlot>, kSlots> bytes_{};
	std::array<uint32_t, kSlots> sizes_{};
	size_t read_ = 0;
	size_t write_ = 0;
};

// ---------------------------------------------------------------------------
// SHA-256 (public-domain style compact implementation) for the PKCE S256 check.
// ---------------------------------------------------------------------------
inline std::array<uint8_t, 32> sha256(const std::string &input)
{
	auto rotr = [](uint32_t x, uint32_t n) {
		return (x >> n) | (x << (32 - n));
	};
	static const uint32_t k[64] = {
		0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
		0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
		0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
		0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
		0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
		0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
		0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
		0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
	uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
			 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

	std::vector<uint8_t> msg(input.begin(), input.end());
	const uint64_t bitLen = static_cast<uint64_t>(msg.size()) * 8;
	msg.push_back(0x80);
	while (msg.size() % 64 != 56)
		msg.push_back(0x00);
	for (int i = 7; i >= 0; --i)
		msg.push_back(static_cast<uint8_t>((bitLen >> (i * 8)) & 0xff));

	for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
		uint32_t w[64];
		for (int i = 0; i < 16; ++i)
			w[i] = (uint32_t(msg[chunk + i * 4]) << 24) | (uint32_t(msg[chunk + i * 4 + 1]) << 16) |
			       (uint32_t(msg[chunk + i * 4 + 2]) << 8) | uint32_t(msg[chunk + i * 4 + 3]);
		for (int i = 16; i < 64; ++i) {
			const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
			const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
			w[i] = w[i - 16] + s0 + w[i - 7] + s1;
		}
		uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
		for (int i = 0; i < 64; ++i) {
			const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
			const uint32_t ch = (e & f) ^ ((~e) & g);
			const uint32_t t1 = hh + S1 + ch + k[i] + w[i];
			const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
			const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
			const uint32_t t2 = S0 + maj;
			hh = g;
			g = f;
			f = e;
			e = d + t1;
			d = c;
			c = b;
			b = a;
			a = t1 + t2;
		}
		h[0] += a;
		h[1] += b;
		h[2] += c;
		h[3] += d;
		h[4] += e;
		h[5] += f;
		h[6] += g;
		h[7] += hh;
	}
	std::array<uint8_t, 32> out{};
	for (int i = 0; i < 8; ++i) {
		out[i * 4] = static_cast<uint8_t>((h[i] >> 24) & 0xff);
		out[i * 4 + 1] = static_cast<uint8_t>((h[i] >> 16) & 0xff);
		out[i * 4 + 2] = static_cast<uint8_t>((h[i] >> 8) & 0xff);
		out[i * 4 + 3] = static_cast<uint8_t>(h[i] & 0xff);
	}
	return out;
}

// Base64url without padding, matching Qt's
// Base64UrlEncoding | OmitTrailingEquals used by the adapters.
inline std::string base64Url(const uint8_t *data, size_t size)
{
	static const char *tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
	std::string out;
	size_t i = 0;
	for (; i + 2 < size; i += 3) {
		const uint32_t n = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
		out.push_back(tbl[(n >> 18) & 63]);
		out.push_back(tbl[(n >> 12) & 63]);
		out.push_back(tbl[(n >> 6) & 63]);
		out.push_back(tbl[n & 63]);
	}
	if (i < size) {
		uint32_t n = uint32_t(data[i]) << 16;
		const bool two = (i + 1 < size);
		if (two)
			n |= uint32_t(data[i + 1]) << 8;
		out.push_back(tbl[(n >> 18) & 63]);
		out.push_back(tbl[(n >> 12) & 63]);
		if (two)
			out.push_back(tbl[(n >> 6) & 63]);
	}
	return out;
}

inline std::string base64Url(const std::string &bytes)
{
	return base64Url(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size());
}

// PKCE S256 challenge for a verifier, mirroring
// base64Url(Sha256(verifier.toUtf8())) in the adapters.
inline std::string pkceChallenge(const std::string &verifier)
{
	const auto digest = sha256(verifier);
	return base64Url(digest.data(), digest.size());
}

// ---------------------------------------------------------------------------
// Mock Keychain store: models the observable semantics of the Security.framework
// SecItem adapter (save overwrites, read returns stored-or-empty, delete clears).
// A readShouldFail flag models a Keychain read error (Prop 10 / Req 2.7).
// ---------------------------------------------------------------------------
class MockKeychain {
public:
	bool readShouldFail = false;

	// SecItemAdd + (on duplicate) SecItemUpdate: always leaves exactly one item.
	bool save(const std::string &credential)
	{
		value_ = credential;
		hasValue_ = true;
		return true;
	}

	// SecItemCopyMatching: returns the stored credential or empty on
	// not-found / error. A read failure is non-destructive (value_ untouched).
	std::string read() const
	{
		if (readShouldFail || !hasValue_)
			return {};
		return value_;
	}

	// SecItemDelete: no-op when nothing stored.
	void remove()
	{
		value_.clear();
		hasValue_ = false;
	}

	bool hasStored() const { return hasValue_; }

private:
	std::string value_;
	bool hasValue_ = false;
};

// ---------------------------------------------------------------------------
// Request-payload builders (Prop 9 parity). These emit the exact compact-JSON
// byte sequences the Windows and macOS adapters produce for the realtime-token
// request body and the realtime session.update config. Written by hand (not via
// a JSON library) so the test asserts the literal contract both adapters share.
// ---------------------------------------------------------------------------
inline std::string jsonEscape(const std::string &s)
{
	std::string out;
	for (char c : s) {
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		default:
			out.push_back(c);
			break;
		}
	}
	return out;
}

// POST /realtime-token body. Keys appear in the adapters' insertion order:
// mode, client, client_version, job_id, source_language[, target_language].
// NOTE: QJsonObject serializes keys in sorted order, so the golden reflects the
// sorted key order Qt emits; the builder below sorts to match.
struct RealtimeTokenInputs {
	std::string mode; // "transcribe" | "translate"
	std::string sourceLanguage;
	std::string targetLanguage;
	std::string jobId;
	std::string clientVersion = "0.1.0";
};

inline std::string buildRealtimeTokenBody(const RealtimeTokenInputs &in)
{
	// Qt's QJsonDocument::toJson(Compact) emits object keys in ascending
	// lexicographic order. Keys present: client, client_version, job_id, mode,
	// source_language, and (translate only) target_language.
	std::string out = "{";
	out += "\"client\":\"obs_plugin\",";
	out += "\"client_version\":\"" + jsonEscape(in.clientVersion) + "\",";
	out += "\"job_id\":\"" + jsonEscape(in.jobId) + "\",";
	out += "\"mode\":\"" + jsonEscape(in.mode) + "\",";
	out += "\"source_language\":\"" + jsonEscape(in.sourceLanguage) + "\"";
	if (in.mode == "translate")
		out += ",\"target_language\":\"" + jsonEscape(in.targetLanguage) + "\"";
	out += "}";
	return out;
}

// session.update config message sent once the WSS connects. Mirrors the nested
// object both adapters build (transcribe vs. translate). Qt emits keys sorted
// lexicographically at every nesting level; this builder reproduces that order.
inline std::string buildSessionConfig(const RealtimeTokenInputs &in)
{
	if (in.mode == "translate") {
		// { session:{ audio:{ input:{ transcription:{ model } },
		//                      output:{ language } } }, type }
		std::string out = "{";
		out += "\"session\":{";
		out += "\"audio\":{";
		out += "\"input\":{\"transcription\":{\"model\":\"gpt-4o-mini-transcribe\"}},";
		out += "\"output\":{\"language\":\"" + jsonEscape(in.targetLanguage) + "\"}";
		out += "}";  // audio
		out += "},"; // session
		out += "\"type\":\"session.update\"";
		out += "}";
		return out;
	}
	// transcribe: transcription has model + delay, plus languages when the
	// source language is set and not "auto".
	const bool withLanguages = !in.sourceLanguage.empty() && in.sourceLanguage != "auto";
	std::string transcription = "{";
	transcription += "\"delay\":\"low\",";
	if (withLanguages)
		transcription += "\"languages\":[\"" + jsonEscape(in.sourceLanguage) + "\"],";
	transcription += "\"model\":\"gpt-live-transcribe\"";
	transcription += "}";
	// { session:{ audio:{ input:{ format:{ rate, type }, transcription,
	//                              turn_detection:null } }, type }, type }
	std::string out = "{";
	out += "\"session\":{";
	out += "\"audio\":{";
	out += "\"input\":{";
	out += "\"format\":{\"rate\":24000,\"type\":\"audio/pcm\"},";
	out += "\"transcription\":" + transcription + ",";
	out += "\"turn_detection\":null";
	out += "}";  // input
	out += "},"; // audio
	out += "\"type\":\"transcription\"";
	out += "},"; // session
	out += "\"type\":\"session.update\"";
	out += "}";
	return out;
}

// ---------------------------------------------------------------------------
// Debug_Log data minimization (Props 7-8). The shared logger never performs
// substitution; the design keeps secrets out of the log by never passing them
// (redact-or-omit). This helper models the redact-or-omit contract a caller
// must satisfy: given a candidate line and the set of sensitive values in
// scope, it returns the line with each sensitive value replaced by the fixed
// redaction marker, or std::nullopt (omit the entry) if redaction is not
// possible. A compliant log line contains none of the sensitive values.
inline constexpr const char *kRedactionMarker = "[redacted]";

inline std::optional<std::string> redactLine(const std::string &line, const std::vector<std::string> &sensitiveValues)
{
	std::string out = line;
	for (const auto &secret : sensitiveValues) {
		if (secret.empty())
			continue;
		size_t pos = 0;
		while ((pos = out.find(secret, pos)) != std::string::npos) {
			out.replace(pos, secret.size(), kRedactionMarker);
			pos += std::strlen(kRedactionMarker);
		}
	}
	// If, after substitution, any secret still appears as a substring (e.g. a
	// secret equal to the marker), omit the entry entirely (Req 8.5).
	for (const auto &secret : sensitiveValues) {
		if (!secret.empty() && out.find(secret) != std::string::npos)
			return std::nullopt;
	}
	return out;
}

// ---------------------------------------------------------------------------
// Debug_Log rotation threshold (Req 5.5 / Task 8.7). Mirrors rotateIfNeeded in
// src/shared/plugin-debug-log.cpp: rotate when the existing file size reaches
// the 2 MB cap, keeping exactly 2 files (current + ".1"). This is the pure
// decision the shared logger makes; the file I/O itself is not host-testable.
inline constexpr long long kMaxLogBytes = 2 * 1024 * 1024; // 2,097,152

inline bool shouldRotateLog(long long currentSize)
{
	return currentSize >= kMaxLogBytes;
}

} // namespace purelogic
