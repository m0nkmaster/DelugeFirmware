/*
 * Copyright © 2026 Synthstrom Audible Limited
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

namespace deluge::usb {

/// True while the controller is in host mode. This does not report or control the hardware's USB power supply.
/// Host mode is deliberately not persisted: it must only
/// be enabled while the Deluge has external DC power and is not connected to another USB host.
bool hostMode();

/// Experimental capture capability, independent of the controller role. Starts disabled until preferences load.
bool hostAudioEnabled();
/// Apply the preference and queue a host reconnection so audio/MIDI class ownership is reconsidered.
/// Does not change the requested role or interrupt a computer connection.
void setHostAudioEnabled(bool enabled);

enum class ModeChangeResult { SUCCESS, BUSY, HOST_CONNECTED, START_FAILED };
/// Switch USB roles immediately, only from a context where the USB stack is not in use.
/// VBUS alone does not identify a computer; reject when peripheral enumeration is also detected.
ModeChangeResult setHostMode(bool enabled);

/// Queue a menu request until the USB task can safely apply it. The latest pending request wins.
using ModeChangeCallback = void (*)(ModeChangeResult);
void requestHostMode(bool enabled, ModeChangeCallback callback);
bool requestedHostMode();
void processPendingModeChange();

} // namespace deluge::usb
