/*
 * Copyright © 2026 Synthstrom Audible Limited
 *
 * This file is part of the Deluge Firmware.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace deluge::hosted_audio {

constexpr uint32_t kDelugeSampleRate = 44100;

enum class AudioClassVersion : uint8_t {
	UAC1,
	UAC2,
};

enum class BusSpeed : uint8_t {
	FULL,
	HIGH,
};

/// A class-compliant USB AudioStreaming alternate setting which can supply PCM audio to the Deluge.
struct InputStream {
	AudioClassVersion version{};
	uint8_t interfaceNumber{};
	uint8_t alternateSetting{};
	uint8_t endpointAddress{};
	uint16_t maxPacketBytes{};
	uint8_t channels{};
	uint8_t subslotBytes{};
	uint8_t bitResolution{};
	uint32_t sampleRate{};
	bool sampleRateNeedsControlRequest{};
	uint8_t controlInterfaceNumber{};
	uint8_t clockSourceId{};
	uint8_t clockFrequencyControls = 0; // UAC2: 1 = read-only, 3 = read/write
};

/// Select the best PCM isochronous IN stream from a complete USB configuration descriptor.
///
/// UAC1 advertises its rates in the format descriptor, so only an alternate setting supporting targetSampleRate is
/// returned. UAC2 advertises rates through its clock controls; the selected stream is marked as needing a control
/// request before it can be used.
/// Only bInterval=1 and one transaction per service interval are supported. High-speed intervals are 125us,
/// full-speed intervals are 1ms; use the negotiated bus speed, never infer it from the packet size.
std::optional<InputStream> findInputStream(std::span<const uint8_t> configuration,
                                           uint32_t targetSampleRate = kDelugeSampleRate,
                                           uint8_t controlInterface = 0xff, BusSpeed speed = BusSpeed::FULL);

/// Decode interleaved, signed little-endian PCM into the Deluge's Q31-shaped stereo representation. Mono is copied to
/// both sides; for devices with more than two channels, the first two are used. Returns the number of complete frames
/// written.
size_t decodePcm(std::span<const uint8_t> packet, const InputStream& stream, std::span<int32_t> stereoOutput);

} // namespace deluge::hosted_audio
