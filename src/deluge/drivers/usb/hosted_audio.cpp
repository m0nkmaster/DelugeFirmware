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
	if (descriptor.size() < static_cast<size_t>(8 + count * 3)) {
		return false;
	}
	for (uint8_t i = 0; i < count; ++i) {
		if (readLe24(&descriptor[8 + i * 3]) == target) {
			needsControlRequest = count > 1;
			return true;
		}
	}
	return false;
}

int streamScore(const InputStream& stream) {
	int score = stream.channels == 2 ? 1000 : (stream.channels == 1 ? 500 : 100);
	score += stream.bitResolution == 24 ? 100 : 0;
	score += stream.subslotBytes == 4 ? 20 : 0;
	score -= static_cast<int>(stream.channels > 2 ? stream.channels - 2 : 0);
	return score;
}

int32_t decodeSample(const uint8_t* input, uint8_t subslotBytes, uint8_t bitResolution) {
	uint32_t raw;
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

std::optional<InputStream> findInputStream(std::span<const uint8_t> configuration, uint32_t targetSampleRate,
                                           uint8_t controlInterface) {
	if (configuration.size() < 9 || configuration[0] != 9 || configuration[1] != kDescriptorConfiguration
	    || targetSampleRate == 0) {
		return std::nullopt;
	}

	size_t declaredLength = readLe16(&configuration[2]);
	if (declaredLength < 9 || declaredLength > configuration.size()) {
		return std::nullopt; // Never select a stream from a truncated configuration.
	}
	size_t length = declaredLength;
	for (size_t offset = 0; offset < length;) {
		if (length - offset < 2 || configuration[offset] < 2 || configuration[offset] > length - offset) {
			return std::nullopt;
		}
		offset += configuration[offset];
	}
	std::optional<InputStream> best;
	int bestScore = std::numeric_limits<int>::min();
	uint8_t uac2ControlInterface = controlInterface;
	std::array<uint8_t, 256> terminalClockSources{};
	std::array<uint8_t, 256> clockFrequencyControls{};
	bool inUac2AudioControlInterface = false;

	// UAC2 puts the sample clock on a terminal in its AudioControl interface, while the stream refers to that terminal
	// by ID. Collect this small piece of topology before considering the streaming alternate settings.
	for (size_t offset = 0; offset + 2 <= length;) {
		uint8_t descriptorLength = configuration[offset];
		if (descriptorLength < 2 || offset + descriptorLength > length) {
			break;
		}
		auto descriptor = configuration.subspan(offset, descriptorLength);
		if (descriptor[1] == kDescriptorInterface) {
			inUac2AudioControlInterface = descriptorLength >= 9 && descriptor[5] == kClassAudio
			                              && descriptor[6] == kSubclassAudioControl && descriptor[7] == kProtocolUac2;
			if (inUac2AudioControlInterface && uac2ControlInterface == 0xff) {
				uac2ControlInterface = descriptor[2];
			}
			inUac2AudioControlInterface &= descriptorLength >= 9 && descriptor[2] == uac2ControlInterface;
		}
		else if (inUac2AudioControlInterface && descriptor[1] == kDescriptorCsInterface && descriptorLength >= 4) {
			if (descriptor[2] == kAcInputTerminal && descriptorLength >= 17) {
				terminalClockSources[descriptor[3]] = descriptor[7];
			}
			else if (descriptor[2] == kAcOutputTerminal && descriptorLength >= 12) {
				terminalClockSources[descriptor[3]] = descriptor[8];
			}
			else if (descriptor[2] == 0x0a && descriptorLength >= 8) { // CLOCK_SOURCE
				clockFrequencyControls[descriptor[3]] = descriptor[5] & 3;
			}
		}
		offset += descriptorLength;
	}

	bool inAudioStreamingInterface = false;
	bool isUac2 = false;
	bool pcm = false;
	uint8_t interfaceNumber = 0;
	uint8_t alternateSetting = 0;
	uint8_t channels = 0;
	uint8_t subslotBytes = 0;
	uint8_t bitResolution = 0;
	bool rateSupported = false;
	bool rateNeedsControlRequest = false;
	uint8_t terminalLink = 0;
	uint8_t currentControlInterface = 0xff;

	for (size_t offset = 0; offset + 2 <= length;) {
		uint8_t descriptorLength = configuration[offset];
		if (descriptorLength < 2 || offset + descriptorLength > length) {
			break;
		}
		auto descriptor = configuration.subspan(offset, descriptorLength);

		switch (descriptor[1]) {
		case kDescriptorInterface:
			if (descriptorLength >= 9 && descriptor[5] == kClassAudio && descriptor[6] == kSubclassAudioControl) {
				currentControlInterface = descriptor[2];
			}
			inAudioStreamingInterface = descriptorLength >= 9 && descriptor[5] == kClassAudio
			                            && descriptor[6] == kSubclassAudioStreaming && descriptor[3] != 0
			                            && (descriptor[7] == 0 || descriptor[7] == kProtocolUac2)
			                            && (controlInterface == 0xff || currentControlInterface == controlInterface);
			if (inAudioStreamingInterface) {
				interfaceNumber = descriptor[2];
				alternateSetting = descriptor[3];
				isUac2 = descriptor[7] == kProtocolUac2;
				pcm = false;
				channels = 0;
				subslotBytes = 0;
				bitResolution = 0;
				rateSupported = isUac2;
				rateNeedsControlRequest = isUac2;
				terminalLink = 0;
			}
			break;

		case kDescriptorCsInterface:
			if (!inAudioStreamingInterface || descriptorLength < 4) {
				break;
			}
			if (descriptor[2] == kAsGeneral) {
				if (isUac2) {
					if (descriptorLength >= 16 && currentControlInterface == uac2ControlInterface) {
						terminalLink = descriptor[3];
						pcm = descriptor[5] == kFormatTypeI && readLe32(&descriptor[6]) == kUac2Pcm;
						channels = descriptor[10];
					}
				}
				else if (descriptorLength >= 7) {
					pcm = readLe16(&descriptor[5]) == kUac1Pcm;
				}
			}
			else if (descriptor[2] == kFormatType && descriptor[3] == kFormatTypeI) {
				if (isUac2) {
					if (descriptorLength >= 6) {
						subslotBytes = descriptor[4];
						bitResolution = descriptor[5];
					}
				}
				else if (descriptorLength >= 8) {
					channels = descriptor[4];
					subslotBytes = descriptor[5];
					bitResolution = descriptor[6];
					rateSupported = uac1SupportsRate(descriptor, targetSampleRate, rateNeedsControlRequest);
				}
			}
			break;

		case kDescriptorEndpoint:
			if (!inAudioStreamingInterface || descriptorLength < 7 || !pcm || !rateSupported || channels == 0
			    || channels > 32 || subslotBytes == 0 || subslotBytes > 4 || bitResolution == 0
			    || bitResolution > subslotBytes * 8 || !(descriptor[2] & kEndpointIn) || (descriptor[2] & 0x0f) == 0
			    || (descriptor[3] & 0x30) == 0x10 || (descriptor[3] & kTransferTypeMask) != kTransferIsochronous
			    || descriptor[6] != 1) {
				break;
			}
			{
				uint16_t maxPacket = readLe16(&descriptor[4]);
				uint32_t requiredBytes = ((targetSampleRate + 999) / 1000) * channels * subslotBytes;
				if (maxPacket < requiredBytes || maxPacket > 1023) {
					break;
				}
				InputStream candidate{
				    .version = isUac2 ? AudioClassVersion::UAC2 : AudioClassVersion::UAC1,
				    .interfaceNumber = interfaceNumber,
				    .alternateSetting = alternateSetting,
				    .endpointAddress = descriptor[2],
				    .maxPacketBytes = maxPacket,
				    .channels = channels,
				    .subslotBytes = subslotBytes,
				    .bitResolution = bitResolution,
				    .sampleRate = targetSampleRate,
				    .sampleRateNeedsControlRequest = rateNeedsControlRequest,
				    .controlInterfaceNumber = uac2ControlInterface,
				    .clockSourceId = terminalClockSources[terminalLink],
				    .clockFrequencyControls = clockFrequencyControls[terminalClockSources[terminalLink]],
				};
				if (isUac2 && (candidate.clockSourceId == 0 || !(candidate.clockFrequencyControls & 1))) {
					break;
				}
				int score = streamScore(candidate);
				if (!best || score > bestScore) {
					best = candidate;
					bestScore = score;
				}
			}
			break;

		default:
			break;
		}

		offset += descriptorLength;
	}

	return best;
}

size_t decodePcm(std::span<const uint8_t> packet, const InputStream& stream, std::span<int32_t> stereoOutput) {
	if (stream.channels == 0 || stream.subslotBytes == 0 || stream.subslotBytes > 4 || stereoOutput.size() < 2) {
		return 0;
	}

	size_t inputFrameBytes = static_cast<size_t>(stream.channels) * stream.subslotBytes;
	size_t frames = std::min(packet.size() / inputFrameBytes, stereoOutput.size() / 2);
	for (size_t frame = 0; frame < frames; ++frame) {
		const uint8_t* input = &packet[frame * inputFrameBytes];
		int32_t left = decodeSample(input, stream.subslotBytes, stream.bitResolution);
		int32_t right = stream.channels == 1
		                    ? left
		                    : decodeSample(input + stream.subslotBytes, stream.subslotBytes, stream.bitResolution);
		stereoOutput[frame * 2] = left;
		stereoOutput[frame * 2 + 1] = right;
	}
	return frames;
}

} // namespace deluge::hosted_audio
