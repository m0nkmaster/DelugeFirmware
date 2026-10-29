#include "CppUTest/TestHarness.h"

#include "drivers/usb/hosted_audio.h"
#include "fixtures/tonex_usb_descriptors.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

using namespace deluge::hosted_audio;

TEST_GROUP(HostedAudio){};

TEST(HostedAudio, RealTonexOneAndPlugDescriptorsSelectHighSpeed44100Capture) {
	for (const auto& descriptor : {usb_audio_fixtures::tonexOne, usb_audio_fixtures::tonexPlug}) {
		CHECK_FALSE(findInputStream(descriptor, 44100, 2, BusSpeed::FULL).has_value());
		auto stream = findInputStream(descriptor, 44100, 2, BusSpeed::HIGH);
		CHECK(stream.has_value());
		CHECK(stream->version == AudioClassVersion::UAC2);
		CHECK_EQUAL(3, stream->interfaceNumber);
		CHECK_EQUAL(1, stream->alternateSetting);
		CHECK_EQUAL(0x84, stream->endpointAddress);
		CHECK_EQUAL(56, stream->maxPacketBytes);
		CHECK_EQUAL(2, stream->channels);
		CHECK_EQUAL(4, stream->subslotBytes);
		CHECK_EQUAL(24, stream->bitResolution);
		CHECK_EQUAL(2, stream->controlInterfaceNumber);
		CHECK_EQUAL(7, stream->clockSourceId);
		CHECK_EQUAL(3, stream->clockFrequencyControls);
	}
}

TEST(HostedAudio, HighSpeedBandwidthUsesMicroframesAndRejectsExtraTransactions) {
	auto descriptor = usb_audio_fixtures::tonexOne;
	descriptor[254] = 48; // ceil(44100/8000) * two channels * four bytes
	CHECK(findInputStream(descriptor, 44100, 2, BusSpeed::HIGH).has_value());
	descriptor[254] = 47;
	CHECK_FALSE(findInputStream(descriptor, 44100, 2, BusSpeed::HIGH).has_value());
	descriptor[254] = 0;
	descriptor[255] = 4; // Maximum single-transaction high-speed payload, 1024 bytes.
	CHECK(findInputStream(descriptor, 44100, 2, BusSpeed::HIGH).has_value());
	CHECK_FALSE(findInputStream(descriptor, 44100, 2, BusSpeed::FULL).has_value());
	descriptor[254] = 56;
	descriptor[255] = 8; // An additional transaction per microframe is not implemented.
	CHECK_FALSE(findInputStream(descriptor, 44100, 2, BusSpeed::HIGH).has_value());
	descriptor[255] = 0;
	descriptor[256] = 2;
	CHECK_FALSE(findInputStream(descriptor, 44100, 2, BusSpeed::HIGH).has_value());
}

TEST(HostedAudio, SelectsUac1Stereo24Bit44100Input) {
	constexpr std::array<uint8_t, 52> descriptor{
	    9,  2,    52,   0,    2,    1,    0,  0x80, 50,            // Configuration
	    9,  4,    1,    0,    0,    1,    2,  0,    0,             // AS interface, idle alternate
	    9,  4,    1,    1,    1,    1,    2,  0,    0,             // AS interface, streaming alternate
	    7,  0x24, 1,    1,    1,    1,    0,                       // UAC1 AS general, PCM
	    11, 0x24, 2,    1,    2,    3,    24, 1,    0x44, 0xac, 0, // Type I, stereo, packed 24-bit, 44100
	    7,  5,    0x81, 0x05, 0x20, 0x01, 1,                       // Isochronous IN, 288-byte packet
	};

	auto stream = findInputStream(descriptor);
	CHECK_TRUE(stream.has_value());
	CHECK_TRUE(stream->version == AudioClassVersion::UAC1);
	CHECK_EQUAL(1, stream->interfaceNumber);
	CHECK_EQUAL(1, stream->alternateSetting);
	CHECK_EQUAL(0x81, stream->endpointAddress);
	CHECK_EQUAL(288, stream->maxPacketBytes);
	CHECK_EQUAL(2, stream->channels);
	CHECK_EQUAL(3, stream->subslotBytes);
	CHECK_EQUAL(24, stream->bitResolution);
	CHECK_EQUAL(44100, stream->sampleRate);
	CHECK_FALSE(stream->sampleRateNeedsControlRequest);
}

TEST(HostedAudio, RejectsUac1StreamWithout44100) {
	constexpr std::array<uint8_t, 43> descriptor{
	    9,  2,    43,   0,    1,    1,    0,  0x80, 50,            // Configuration
	    9,  4,    2,    1,    1,    1,    2,  0,    0,             // AS interface
	    7,  0x24, 1,    1,    1,    1,    0,                       // PCM
	    11, 0x24, 2,    1,    2,    3,    24, 1,    0x80, 0xbb, 0, // 48000 only
	    7,  5,    0x82, 0x05, 0x20, 0x01, 1,
	};

	CHECK_FALSE(findInputStream(descriptor).has_value());
}

TEST(HostedAudio, SelectsUac2PcmInputForClockNegotiation) {
	constexpr std::array<uint8_t, 76> descriptor{
	    9,  2,    76,   0,    2,    1,    0, 0x80, 50,                      // Configuration
	    9,  4,    0,    0,    0,    1,    1, 0x20, 0,                       // UAC2 AC interface
	    8,  0x24, 0x0a, 10,   1,    3,    0, 0,                             // Writable clock source 10
	    12, 0x24, 3,    1,    1,    1,    0, 2,    10, 0, 0, 0,             // Output terminal 1, clock source 10
	    9,  4,    3,    1,    1,    1,    2, 0x20, 0,                       // UAC2 AS interface
	    16, 0x24, 1,    1,    0,    1,    1, 0,    0,  0, 2, 3, 0, 0, 0, 0, // AS general: PCM, stereo
	    6,  0x24, 2,    1,    4,    24,                                     // Type I, 4-byte subslot, 24 valid bits
	    7,  5,    0x83, 0x05, 0x80, 0x01, 1,                                // Isochronous IN
	};

	auto stream = findInputStream(descriptor);
	CHECK_TRUE(stream.has_value());
	CHECK_TRUE(stream->version == AudioClassVersion::UAC2);
	CHECK_EQUAL(3, stream->interfaceNumber);
	CHECK_EQUAL(2, stream->channels);
	CHECK_EQUAL(4, stream->subslotBytes);
	CHECK_TRUE(stream->sampleRateNeedsControlRequest);
	CHECK_EQUAL(0, stream->controlInterfaceNumber);
	CHECK_EQUAL(10, stream->clockSourceId);
}

TEST(HostedAudio, PrefersStereoOverMono) {
	constexpr std::array<uint8_t, 77> descriptor{
	    9,    2, 77, 0, 1, 1,  0,    0x80, 50,   9, 4, 1,  1,    1,    1,    2, 0, 0, 7,    0x24,
	    1,    1, 1,  1, 0, 11, 0x24, 2,    1,    1, 3, 24, 1,    0x44, 0xac, 0, 7, 5, 0x81, 0x05,
	    0x90, 0, 1,  9, 4, 1,  2,    1,    1,    2, 0, 0,  7,    0x24, 1,    1, 1, 1, 0,    11,
	    0x24, 2, 1,  2, 2, 16, 1,    0x44, 0xac, 0, 7, 5,  0x82, 0x05, 0xb4, 0, 1,
	};

	auto stream = findInputStream(descriptor);
	CHECK_TRUE(stream.has_value());
	CHECK_EQUAL(2, stream->alternateSetting);
	CHECK_EQUAL(2, stream->channels);
}

TEST(HostedAudio, DecodesPacked24BitStereoToQ31) {
	InputStream stream{AudioClassVersion::UAC1, 1, 1, 0x81, 288, 2, 3, 24, 44100, false};
	constexpr std::array<uint8_t, 12> packet{
	    0x00, 0x00, 0x40, // +0.5 left
	    0x00, 0x00, 0xc0, // -0.5 right
	    0xff, 0xff, 0x7f, // maximum positive left
	    0x00, 0x00, 0x80, // minimum negative right
	};
	std::array<int32_t, 4> output{};

	CHECK_EQUAL(2, decodePcm(packet, stream, output));
	CHECK_EQUAL(0x40000000, output[0]);
	CHECK_EQUAL(static_cast<int32_t>(0xc0000000u), output[1]);
	CHECK_EQUAL(0x7fffff00, output[2]);
	CHECK_EQUAL(static_cast<int32_t>(0x80000000u), output[3]);
}

TEST(HostedAudio, DuplicatesMonoAndIgnoresTrailingBytes) {
	InputStream stream{AudioClassVersion::UAC1, 1, 1, 0x81, 96, 1, 2, 16, 44100, false};
	constexpr std::array<uint8_t, 5> packet{0x00, 0x40, 0x00, 0xc0, 0xaa};
	std::array<int32_t, 6> output{};

	CHECK_EQUAL(2, decodePcm(packet, stream, output));
	CHECK_EQUAL(0x40000000, output[0]);
	CHECK_EQUAL(output[0], output[1]);
	CHECK_EQUAL(static_cast<int32_t>(0xc0000000u), output[2]);
	CHECK_EQUAL(output[2], output[3]);
}

TEST(HostedAudio, Decodes24ValidBitsLeftJustifiedIn32BitSubslots) {
	InputStream stream{AudioClassVersion::UAC2, 1, 1, 0x81, 384, 2, 4, 24, 44100, true};
	constexpr std::array<uint8_t, 16> packet{0, 0, 0, 0x40, 0, 0, 0, 0xc0, 0, 0xff, 0xff, 0x7f, 0, 0, 0, 0x80};
	std::array<int32_t, 4> output{};
	CHECK_EQUAL(2, decodePcm(packet, stream, output));
	CHECK_EQUAL(0x40000000, output[0]);
	CHECK_EQUAL(static_cast<int32_t>(0xc0000000u), output[1]);
	CHECK_EQUAL(0x7fffff00, output[2]);
	CHECK_EQUAL(static_cast<int32_t>(0x80000000u), output[3]);
}

namespace {
constexpr std::array<uint8_t, 43> validUac1{
    9, 2, 43, 0,  1,    1, 0, 0x80, 50, 9,  4, 1,    1,    1, 1, 2, 0,    0, 7,    0x24, 1, 1,
    1, 1, 0,  11, 0x24, 2, 1, 2,    3,  24, 1, 0x44, 0xac, 0, 7, 5, 0x81, 5, 0x20, 1,    1,
};
}

TEST(HostedAudio, RejectsEveryTruncationOfConfiguration) {
	CHECK(findInputStream(validUac1).has_value());
	for (size_t length = 0; length < validUac1.size(); ++length) {
		CHECK_FALSE(findInputStream(std::span{validUac1}.first(length)).has_value());
	}
}

TEST(HostedAudio, RejectsMalformedTrailingDescriptorInsteadOfReturningEarlierStream) {
	std::array<uint8_t, 45> descriptor{};
	std::copy(validUac1.begin(), validUac1.end(), descriptor.begin());
	descriptor[2] = descriptor.size();
	descriptor[43] = 0;
	CHECK_FALSE(findInputStream(descriptor).has_value());
	descriptor[43] = 3;
	CHECK_FALSE(findInputStream(descriptor).has_value());
}

TEST(HostedAudio, RejectsFeedbackEndpointAndUnknownAudioProtocol) {
	auto descriptor = validUac1;
	descriptor[39] = 0x11; // Explicit feedback is not PCM sample data.
	CHECK_FALSE(findInputStream(descriptor).has_value());
	descriptor = validUac1;
	descriptor[16] = 0x30; // UAC3 must not be interpreted as UAC1.
	CHECK_FALSE(findInputStream(descriptor).has_value());
}

TEST(HostedAudio, RejectsInsufficientBandwidthAndUnsupportedInterval) {
	auto descriptor = validUac1;
	descriptor[40] = 64;
	descriptor[41] = 0;
	CHECK_FALSE(findInputStream(descriptor).has_value());
	descriptor = validUac1;
	descriptor[42] = 2;
	CHECK_FALSE(findInputStream(descriptor).has_value());
}
