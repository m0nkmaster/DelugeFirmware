/*
 * Copyright © 2026 Synthstrom Audible Limited
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

namespace deluge::usb {

/// True while the Deluge owns the bus and supplies USB VBUS. Host mode is deliberately not persisted: it must only
/// be enabled while the Deluge has external DC power and is not connected to another USB host.
bool hostMode();

/// Switch USB roles immediately. In host mode the controller remains active across device detach/attach, so a class
/// compliant audio source can be hot-plugged without restarting the Deluge.
/// Returns false if USB is busy, another host is supplying VBUS, or opening the new role fails.
bool setHostMode(bool enabled);

} // namespace deluge::usb
