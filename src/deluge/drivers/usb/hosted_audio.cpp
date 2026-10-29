/*
 * Copyright © 2026 Synthstrom Audible Limited
 *
 * This file is part of the Deluge Firmware.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "drivers/usb/hosted_audio.h"

#include <algorithm>
#include <array>
#include <limits>

namespace deluge::hosted_audio {
namespace {

constexpr uint8_t kDescriptorConfiguration = 0x02;
constexpr uint8_t kDescriptorInterface = 0x04;
constexpr uint8_t kDescriptorEndpoint = 0x05;
constexpr uint8_t kDescriptorCsInterface = 0x24;
constexpr uint8_t kClassAudio = 0x01;
constexpr uint8_t kSubclassAudioStreaming = 0x02;
constexpr uint8_t kSubclassAudioControl = 0x01;
constexpr uint8_t kProtocolUac2 = 0x20;
constexpr uint8_t kAsGeneral = 0x01;
constexpr uint8_t kFormatType = 0x02;
constexpr uint8_t kFormatTypeI = 0x01;
constexpr uint8_t kEndpointIn = 0x80;
constexpr uint8_t kTransferTypeMask = 0x03;
constexpr uint8_t kTransferIsochronous = 0x01;
constexpr uint16_t kUac1Pcm = 0x0001;
constexpr uint32_t kUac2Pcm = 1u << 0;
constexpr uint8_t kAcInputTerminal = 0x02;
constexpr uint8_t kAcOutputTerminal = 0x03;

uint16_t readLe16(const uint8_t* bytes) {
	return static_cast<uint16_t>(bytes[0]) | static_cast<uint16_t>(bytes[1]) << 8;
}

uint32_t readLe24(const uint8_t* bytes) {
	return static_cast<uint32_t>(bytes[0]) | static_cast<uint32_t>(bytes[1]) << 8
	       | static_cast<uint32_t>(bytes[2]) << 16;
}

uint32_t readLe32(const uint8_t* bytes) {
	return static_cast<uint32_t>(bytes[0]) | static_cast<uint32_t>(bytes[1]) << 8
	       | static_cast<uint32_t>(bytes[2]) << 16 | static_cast<uint32_t>(bytes[3]) << 24;
}

bool uac1SupportsRate(std::span<const uint8_t> descriptor, uint32_t target, bool& needsControlRequest) {
	if (descriptor.size() < 11) {
		return false;
	}

	uint8_t count = descriptor[7];
	if (count == 0) {
		needsControlRequest = true;
		return descriptor.size() >= 14 && target >= readLe24(&descriptor[8]) && target <= readLe24(&descriptor[11]);
	}
	if (descriptor.size() < static_cast<size_t>(8 + (count * 3))) {
		return false;
	}
	for (uint8_t i = 0; i < count; ++i) {
		if (readLe24(&descriptor[8 + (i * 3)]) == target) {
			needsControlRequest = count > 1;
			return true;
		}
	}
	return false;
}

int streamScore(const InputStream& stream) {
	int score = 100;
	if (stream.channels == 2) {
		score = 1000;
	}
	else if (stream.channels == 1) {
		score = 500;
	}
	score += stream.bitResolution == 24 ? 100 : 0;
	score += stream.subslotBytes == 4 ? 20 : 0;
	score -= (stream.channels > 2 ? stream.channels - 2 : 0);
	return score;
}

int32_t decodeSample(const uint8_t* input, uint8_t subslotBytes, uint8_t bitResolution) {
	uint32_t raw = 0;
	switch (subslotBytes) {
	case 1:
		raw = input[0];
		break;
	case 2:
		raw = readLe16(input);
		break;
	case 3:
		raw = readLe24(input);
		break;
	case 4:
		raw = readLe32(input);
		break;
	default:
		return 0;
	}

	if (bitResolution == 0 || bitResolution > subslotBytes * 8) {
		return 0;
	}
	// USB PCM is left-justified within the subslot (USB Audio Data Formats 1.0, 2.2.6.1).
	// In particular, 24 valid bits in a 32-bit subslot already have their sign bit at bit 31.
	return static_cast<int32_t>(raw << (32 - subslotBytes * 8));
}

} // namespace

// Keep descriptor validation in wire order with per-interface state.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
std::optional<InputStream> findInputStream(std::span<const uint8_t> configuration, uint32_t targetSampleRate,
                                           uint8_t controlInterface, BusSpeed speed) {
	if (configuration.size() < 9 || configuration[0] != 9 || configuration[1] != kDescriptorConfiguration
	    || targetSampleRate == 0) {
		return std::nullopt;
	}

	size_t declared_length = readLe16(&configuration[2]);
	if (declared_length < 9 || declared_length > configuration.size()) {
		return std::nullopt; // Never select a stream from a truncated configuration.
	}
	size_t length = declared_length;
	for (size_t offset = 0; offset < length;) {
		if (length - offset < 2 || configuration[offset] < 2 || configuration[offset] > length - offset) {
			return std::nullopt;
		}
		offset += configuration[offset];
	}
	std::optional<InputStream> best;
	int best_score = std::numeric_limits<int>::min();
	uint8_t uac2_control_interface = controlInterface;
	std::array<uint8_t, 256> terminal_clock_sources{};
	std::array<uint8_t, 256> clock_frequency_controls{};
	bool in_uac2_audio_control_interface = false;

	// UAC2 puts the sample clock on a terminal in its AudioControl interface, while the stream refers to that terminal
	// by ID. Collect this small piece of topology before considering the streaming alternate settings.
	for (size_t offset = 0; offset + 2 <= length;) {
		uint8_t descriptor_length = configuration[offset];
		if (descriptor_length < 2 || offset + descriptor_length > length) {
			break;
		}
		auto descriptor = configuration.subspan(offset, descriptor_length);
		if (descriptor[1] == kDescriptorInterface) {
			in_uac2_audio_control_interface = descriptor_length >= 9 && descriptor[5] == kClassAudio
			                                  && descriptor[6] == kSubclassAudioControl
			                                  && descriptor[7] == kProtocolUac2;
			if (in_uac2_audio_control_interface && uac2_control_interface == 0xff) {
				uac2_control_interface = descriptor[2];
			}
			in_uac2_audio_control_interface &= descriptor_length >= 9 && descriptor[2] == uac2_control_interface;
		}
		else if (in_uac2_audio_control_interface && descriptor[1] == kDescriptorCsInterface && descriptor_length >= 4) {
			if (descriptor[2] == kAcInputTerminal && descriptor_length >= 17) {
				terminal_clock_sources[descriptor[3]] = descriptor[7];
			}
			else if (descriptor[2] == kAcOutputTerminal && descriptor_length >= 12) {
				terminal_clock_sources[descriptor[3]] = descriptor[8];
			}
			else if (descriptor[2] == 0x0a && descriptor_length >= 8) { // CLOCK_SOURCE
				clock_frequency_controls[descriptor[3]] = descriptor[5] & 3;
			}
		}
		offset += descriptor_length;
	}

	bool in_audio_streaming_interface = false;
	bool is_uac2 = false;
	bool pcm = false;
	uint8_t interface_number = 0;
	uint8_t alternate_setting = 0;
	uint8_t channels = 0;
	uint8_t subslot_bytes = 0;
	uint8_t bit_resolution = 0;
	bool rate_supported = false;
	bool rate_needs_control_request = false;
	uint8_t terminal_link = 0;
	uint8_t current_control_interface = 0xff;

	for (size_t offset = 0; offset + 2 <= length;) {
		uint8_t descriptor_length = configuration[offset];
		if (descriptor_length < 2 || offset + descriptor_length > length) {
			break;
		}
		auto descriptor = configuration.subspan(offset, descriptor_length);

		switch (descriptor[1]) {
		case kDescriptorInterface:
			if (descriptor_length >= 9 && descriptor[5] == kClassAudio && descriptor[6] == kSubclassAudioControl) {
				current_control_interface = descriptor[2];
			}
			in_audio_streaming_interface =
			    descriptor_length >= 9 && descriptor[5] == kClassAudio && descriptor[6] == kSubclassAudioStreaming
			    && descriptor[3] != 0 && (descriptor[7] == 0 || descriptor[7] == kProtocolUac2)
			    && (controlInterface == 0xff || current_control_interface == controlInterface);
			if (in_audio_streaming_interface) {
				interface_number = descriptor[2];
				alternate_setting = descriptor[3];
				is_uac2 = descriptor[7] == kProtocolUac2;
				pcm = false;
				channels = 0;
				subslot_bytes = 0;
				bit_resolution = 0;
				rate_supported = is_uac2;
				rate_needs_control_request = is_uac2;
				terminal_link = 0;
			}
			break;

		case kDescriptorCsInterface:
			if (!in_audio_streaming_interface || descriptor_length < 4) {
				break;
			}
			if (descriptor[2] == kAsGeneral) {
				if (is_uac2) {
					if (descriptor_length >= 16 && current_control_interface == uac2_control_interface) {
						terminal_link = descriptor[3];
						pcm = descriptor[5] == kFormatTypeI && readLe32(&descriptor[6]) == kUac2Pcm;
						channels = descriptor[10];
					}
				}
				else if (descriptor_length >= 7) {
					pcm = readLe16(&descriptor[5]) == kUac1Pcm;
				}
			}
			else if (descriptor[2] == kFormatType && descriptor[3] == kFormatTypeI) {
				if (is_uac2) {
					if (descriptor_length >= 6) {
						subslot_bytes = descriptor[4];
						bit_resolution = descriptor[5];
					}
				}
				else if (descriptor_length >= 8) {
					channels = descriptor[4];
					subslot_bytes = descriptor[5];
					bit_resolution = descriptor[6];
					rate_supported = uac1SupportsRate(descriptor, targetSampleRate, rate_needs_control_request);
				}
			}
			break;

		case kDescriptorEndpoint:
			if (!in_audio_streaming_interface || descriptor_length < 7 || !pcm || !rate_supported || channels == 0
			    || channels > 32 || subslot_bytes == 0 || subslot_bytes > 4 || bit_resolution == 0
			    || bit_resolution > subslot_bytes * 8 || ((descriptor[2] & kEndpointIn) == 0)
			    || (descriptor[2] & 0x0f) == 0 || (descriptor[3] & 0x30) == 0x10
			    || (descriptor[3] & kTransferTypeMask) != kTransferIsochronous || descriptor[6] != 1) {
				break;
			}
			{
				uint16_t max_packet = readLe16(&descriptor[4]);
				uint32_t intervals_per_second = speed == BusSpeed::HIGH ? 8000 : 1000;
				uint64_t required_bytes =
				    ((static_cast<uint64_t>(targetSampleRate) + intervals_per_second - 1) / intervals_per_second)
				    * channels * subslot_bytes;
				// The upper bits encode extra HS transactions, which this receiver cannot service. Comparing the
				// entire field also rejects reserved bits, rather than accidentally treating them as payload bytes.
				if (max_packet < required_bytes || max_packet > (speed == BusSpeed::HIGH ? 1024 : 1023)) {
					break;
				}
				InputStream candidate{
				    .version = is_uac2 ? AudioClassVersion::UAC2 : AudioClassVersion::UAC1,
				    .interfaceNumber = interface_number,
				    .alternateSetting = alternate_setting,
				    .endpointAddress = descriptor[2],
				    .maxPacketBytes = max_packet,
				    .channels = channels,
				    .subslotBytes = subslot_bytes,
				    .bitResolution = bit_resolution,
				    .sampleRate = targetSampleRate,
				    .sampleRateNeedsControlRequest = rate_needs_control_request,
				    .controlInterfaceNumber = uac2_control_interface,
				    .clockSourceId = terminal_clock_sources[terminal_link],
				    .clockFrequencyControls = clock_frequency_controls[terminal_clock_sources[terminal_link]],
				};
				if (is_uac2 && (candidate.clockSourceId == 0 || ((candidate.clockFrequencyControls & 1) == 0))) {
					break;
				}
				int score = streamScore(candidate);
				if (!best || score > best_score) {
					best = candidate;
					best_score = score;
				}
			}
			break;

		default:
			break;
		}

		offset += descriptor_length;
	}

	return best;
}

size_t decodePcm(std::span<const uint8_t> packet, const InputStream& stream, std::span<int32_t> stereoOutput) {
	if (stream.channels == 0 || stream.subslotBytes == 0 || stream.subslotBytes > 4 || stereoOutput.size() < 2) {
		return 0;
	}

	size_t input_frame_bytes = static_cast<size_t>(stream.channels) * stream.subslotBytes;
	size_t frames = std::min(packet.size() / input_frame_bytes, stereoOutput.size() / 2);
	for (size_t frame = 0; frame < frames; ++frame) {
		const uint8_t* input = &packet[frame * input_frame_bytes];
		int32_t left = decodeSample(input, stream.subslotBytes, stream.bitResolution);
		int32_t right = stream.channels == 1
		                    ? left
		                    : decodeSample(input + stream.subslotBytes, stream.subslotBytes, stream.bitResolution);
		stereoOutput[frame * 2] = left;
		stereoOutput[(frame * 2) + 1] = right;
	}
	return frames;
}

} // namespace deluge::hosted_audio
