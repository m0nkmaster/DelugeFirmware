#pragma once
#include "definitions_cxx.hpp"
#include <cstring>

extern "C" {
#include "util/cfunctions.h"
}

char halfByteToHexChar(uint8_t thisHalfByte);
void intToHex(uint32_t number, char* output, int32_t numChars = 8);
uint32_t hexToInt(char const* string);
uint32_t hexToIntFixedLength(char const* __restrict__ hexChars, int32_t length);

/// A string buffer with utility functions to append and format contents.
/// does not handle allocation
class StringBuf {
	// Not templated to optimize binary size.
public:
	StringBuf(char* buf, size_t capacity) : capacity_(capacity), buf_(buf) { memset(buf_, '\0', capacity_); }
	void append(std::string_view str);
	void append(char c);
	void removeSpaces() {
		size_t removed = 0;
		// upto size, not below it -- we want the null as well
		for (size_t i = 0; i <= size(); i++) {
			if (isspace(buf_[i])) {
				removed++;
			}
			else {
				buf_[i - removed] = buf_[i];
			}
		}
	}
	void clear() { buf_[0] = 0; }
	void truncate(size_t newSize) {
		if (newSize < capacity_) {
			buf_[newSize] = '\0';
		}
	}

	void appendInt(int i, int minChars = 1) {
		if (spaceRemaining() == 0) {
			return;
		}
		char tmp[16];
		if (minChars < 1) {
			minChars = 1;
		}
		if (minChars > static_cast<int>(sizeof(tmp) - 1)) {
			minChars = static_cast<int>(sizeof(tmp) - 1);
		}
		intToString(i, tmp, minChars);
		append(tmp);
	}
	void appendHex(int i, int minChars = 1) {
		if (spaceRemaining() == 0) {
			return;
		}
		char tmp[16];
		if (minChars < 1) {
			minChars = 1;
		}
		if (minChars > static_cast<int>(sizeof(tmp) - 1)) {
			minChars = static_cast<int>(sizeof(tmp) - 1);
		}
		intToHex(static_cast<uint32_t>(i), tmp, minChars);
		append(tmp);
	}
	void appendFloat(float f, int32_t minDecimals, int32_t maxDecimals) {
		if (spaceRemaining() == 0) {
			return;
		}
		char tmp[64];
		if (maxDecimals < 0) {
			maxDecimals = 0;
		}
		if (maxDecimals > 40) {
			maxDecimals = 40;
		}
		if (minDecimals < 0) {
			minDecimals = 0;
		}
		if (minDecimals > maxDecimals) {
			minDecimals = maxDecimals;
		}
		floatToString(f, tmp, minDecimals, maxDecimals);
		append(tmp);
	}

	[[nodiscard]] char* data() { return buf_; }
	[[nodiscard]] const char* data() const { return buf_; }
	[[nodiscard]] const char* c_str() const { return buf_; }

	[[nodiscard]] size_t capacity() const { return capacity_; }
	[[nodiscard]] size_t size() const { return std::strlen(buf_); }
	[[nodiscard]] size_t length() const { return std::strlen(buf_); }

	[[nodiscard]] bool empty() const { return buf_[0] == '\0'; }

	bool operator==(const char* rhs) const { return std::strcmp(buf_, rhs) == 0; }
	bool operator==(const StringBuf& rhs) const { return std::strcmp(buf_, rhs.c_str()) == 0; }

	operator std::string_view() const { return std::string_view{buf_}; }

private:
	[[nodiscard]] size_t spaceRemaining() const {
		size_t s = size();
		return (s + 1 < capacity_) ? (capacity_ - s - 1) : 0;
	}

	size_t capacity_;
	char* buf_;
};

/// Define a `StringBuf` that uses an array placed on the stack.
#define DEF_STACK_STRING_BUF(name, capacity)                                                                           \
	char name##__buf[capacity] = {0};                                                                                  \
	StringBuf name = {name##__buf, capacity}
