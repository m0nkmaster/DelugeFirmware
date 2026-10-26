#include "CppUTest/TestHarness.h"

extern "C" {
#include "RZA1/usb/r_usb_basic/r_usb_basic_if.h"
#include "RZA1/usb/r_usb_basic/src/driver/inc/r_usb_extern.h"
#include "RZA1/usb/r_usb_basic/src/hw/inc/r_usb_bitdefine.h"
#include "RZA1/usb/r_usb_haudio/r_usb_haudio_if.h"
uint16_t g_usb_hstd_use_pipe[USB_NUM_USBIP]{};
extern volatile uint32_t usbLock;
uint16_t g_usb_usbmode = USB_PERI;
uint8_t usbCurrentlyInitialized = 1;
}

#include "drivers/usb/usb_mode.h"
#include <algorithm>
#include <array>
#include <span>
#include <vector>

namespace {
usb_hcdreg_t registeredDriver{};
usb_utr_t context{};
usb_utr_t* pendingControl = nullptr;
usb_utr_t* pendingPacket = nullptr;
unsigned enumerationReplies = 0;
unsigned pipeRegistrations = 0;
bool inputRunning = false;
bool externalVbus = false;
uint16_t peripheralState = USB_DS_POWR;
bool openFails = false;
bool requestDeviceDuringOpen = false;
unsigned roleCallbacks = 0;
deluge::usb::ModeChangeResult lastRoleResult = deluge::usb::ModeChangeResult::Success;
void roleChanged(deluge::usb::ModeChangeResult result) {
	++roleCallbacks;
	lastRoleResult = result;
}
std::vector<int32_t> capturedSamples;
std::vector<int> roleChanges;

constexpr std::array<uint8_t, 52> uac1{
    9, 2, 52,   0, 2, 1, 0, 0x80, 50, 9,    4, 0, 0, 0, 1,  1, 0,    0,    9, 4, 1, 1,    1, 1,    2, 0,
    0, 7, 0x24, 1, 1, 1, 1, 0,    11, 0x24, 2, 1, 2, 3, 24, 1, 0x44, 0xac, 0, 7, 5, 0x81, 5, 0x20, 1, 1,
};

constexpr std::array<uint8_t, 76> uac2{
    9,  2,    76, 0, 2, 1, 0, 0x80, 50, 9, 4, 0, 0,    0, 1, 1, 0x20, 0, 8, 0x24, 0x0a, 10,   1,    3, 0, 0,
    12, 0x24, 3,  1, 1, 1, 0, 2,    10, 0, 0, 0, 9,    4, 3, 1, 1,    1, 2, 0x20, 0,    16,   0x24, 1, 1, 0,
    1,  1,    0,  0, 0, 2, 3, 0,    0,  0, 0, 6, 0x24, 2, 1, 4, 24,   7, 5, 0x83, 5,    0x80, 1,    1,
};

uint16_t checkDevice(std::span<const uint8_t> configuration) {
	uint16_t result = USB_ERROR;
	uint16_t speed = USB_FSCONNECT;
	uint16_t address = 1;
	uint16_t* table[9]{};
	table[1] = reinterpret_cast<uint16_t*>(const_cast<uint8_t*>(configuration.data()));
	table[2] = reinterpret_cast<uint16_t*>(const_cast<uint8_t*>(configuration.data() + 9));
	table[3] = &result;
	table[6] = &speed;
	table[7] = &address;
	registeredDriver.classcheck(&context, table);
	return result;
}

void configure(std::span<const uint8_t> configuration) {
	CHECK_EQUAL(USB_OK, checkDevice(configuration));
	registeredDriver.devconfig(&context, 1, 0);
}

void completeControl(uint16_t status = USB_CTRL_END, uint32_t rate = 44100, uint32_t remaining = 0) {
	CHECK(pendingControl != nullptr);
	auto* transfer = pendingControl;
	if (transfer->p_setup[0] & USB_DEV_TO_HOST) {
		auto* bytes = static_cast<uint8_t*>(transfer->p_tranadr);
		for (size_t i = 0; i < transfer->tranlen; ++i) {
			bytes[i] = rate >> (8 * i);
		}
	}
	// Mirrors the HCD contract: release PIPE0 before calling a class callback which may queue its next request.
	pendingControl = nullptr;
	transfer->status = status;
	transfer->tranlen = remaining;
	transfer->complete(transfer, 0, 0);
}
} // namespace

extern "C" {
void usb_hstd_driver_registration(usb_utr_t*, usb_hcdreg_t* driver) {
	registeredDriver = *driver;
}
void usb_hstd_dummy_function(usb_utr_t*, uint16_t, uint16_t) {
}
void usb_hstd_return_enu_mgr(usb_utr_t*, uint16_t result) {
	CHECK_EQUAL(USB_OK, result);
	++enumerationReplies;
}
usb_regadr_t usb_hstd_get_usb_ip_adr(uint16_t) {
	return nullptr;
}
uint16_t usb_hstd_chk_pipe_info(uint16_t, uint16_t* table, uint8_t* endpoint) {
	table[1] = USB_TYPFIELD_ISO | USB_DIR_H_IN | (endpoint[2] & 0x0f);
	table[3] = endpoint[4] | endpoint[5] << 8;
	return USB_DIR_H_IN;
}
usb_er_t usb_hstd_set_pipe_registration(usb_utr_t*, uint16_t* table, uint16_t) {
	CHECK_EQUAL(USB_PIPE2, table[0]);
	g_usb_hstd_use_pipe[0] |= USB_BITSET(table[0]);
	++pipeRegistrations;
	return USB_OK;
}
usb_er_t usb_hstd_transfer_start(usb_utr_t* transfer) {
	auto*& pending = transfer->keyword == USB_PIPE0 ? pendingControl : pendingPacket;
	if (pending) {
		return USB_QOVR;
	}
	pending = transfer;
	return USB_OK;
}
uint16_t hw_usb_read_intsts() {
	return (externalVbus ? USB_VBSTS : 0) | peripheralState;
}
void openUSBHost() {
	roleChanges.push_back(2);
	g_usb_usbmode = USB_HOST;
	usbCurrentlyInitialized = !openFails;
	if (requestDeviceDuringOpen) {
		requestDeviceDuringOpen = false;
		deluge::usb::requestHostMode(false, roleChanged);
	}
}
void closeUSBHost() {
	roleChanges.push_back(3);
	usbCurrentlyInitialized = 0;
}
void openUSBPeripheral() {
	roleChanges.push_back(4);
	g_usb_usbmode = USB_PERI;
	usbCurrentlyInitialized = !openFails;
}
void closeUSBPeripheral() {
	roleChanges.push_back(1);
	usbCurrentlyInitialized = 0;
}
}

namespace deluge::usb_audio {
void startHostedInput() {
	inputRunning = true;
}
void stopHostedInput() {
	inputRunning = false;
}
void receiveHostedInput(std::span<const int32_t> samples) {
	capturedSamples.insert(capturedSamples.end(), samples.begin(), samples.end());
}
} // namespace deluge::usb_audio

TEST_GROUP(UsbHostDriver){void setup() override{pendingControl = nullptr;
pendingPacket = nullptr;
enumerationReplies = 0;
pipeRegistrations = 0;
g_usb_hstd_use_pipe[0] = 0;
usbLock = 0;
usbCurrentlyInitialized = 1;
g_usb_usbmode = USB_PERI;
externalVbus = false;
peripheralState = USB_DS_POWR;
openFails = false;
requestDeviceDuringOpen = false;
roleCallbacks = 0;
deluge::usb::requestHostMode(false, nullptr);
deluge::usb::processPendingModeChange();
capturedSamples.clear();
roleChanges.clear();
usb_hstd_audio_register(&context);
}
void teardown() override {
	usb_hstd_audio_stop();
	usbLock = 0;
	std::vector<int32_t>().swap(capturedSamples);
	std::vector<int>().swap(roleChanges);
}
}
;

TEST(UsbHostDriver, FixedUac1StartsAfterSelectingInterfaceAndDecodesPackets) {
	configure(uac1);
	CHECK_EQUAL(1, enumerationReplies);
	CHECK_EQUAL(1, pipeRegistrations);
	CHECK_EQUAL(USB_SET_INTERFACE | USB_INTERFACE, pendingControl->p_setup[0]);
	CHECK_FALSE(inputRunning);
	completeControl();
	CHECK(inputRunning);
	CHECK(pendingPacket != nullptr);
	const std::array<uint8_t, 6> packet{0, 0, 0x40, 0, 0, 0xc0};
	auto* transfer = pendingPacket;
	std::copy(packet.begin(), packet.end(), static_cast<uint8_t*>(transfer->p_tranadr));
	transfer->tranlen -= packet.size();
	transfer->status = USB_DATA_READING;
	pendingPacket = nullptr;
	transfer->complete(transfer, 0, 0);
	CHECK_EQUAL(2, capturedSamples.size());
	CHECK_EQUAL(0x40000000, capturedSamples[0]);
	CHECK_EQUAL(static_cast<int32_t>(0xc0000000u), capturedSamples[1]);
	CHECK(pendingPacket != nullptr);
}

TEST(UsbHostDriver, SynchronousRejectionDoesNotPostAnEnumerationReply) {
	auto descriptor = uac1;
	descriptor[42] = 0x80;
	descriptor[43] = 0xbb; // 48 kHz only
	CHECK_EQUAL(USB_ERROR, checkDevice(descriptor));
	CHECK_EQUAL(0, enumerationReplies);
	CHECK(pendingControl == nullptr);
}

TEST(UsbHostDriver, MultirateUac1SetsAndReadsEndpointClockAfterSelectingInterface) {
	std::vector<uint8_t> descriptor(uac1.begin(), uac1.end());
	descriptor.insert(descriptor.begin() + 45, {0x80, 0xbb, 0}); // Add 48 kHz alongside 44.1 kHz.
	descriptor[2] = descriptor.size();
	descriptor[34] = 14;
	descriptor[41] = 2;
	configure(descriptor);
	completeControl(); // SET_INTERFACE
	CHECK_EQUAL(0x0122, pendingControl->p_setup[0]);
	CHECK_EQUAL(0x81, pendingControl->p_setup[2]);
	CHECK_EQUAL(3, pendingControl->tranlen);
	completeControl(); // SET_CUR
	CHECK_EQUAL(0x81a2, pendingControl->p_setup[0]);
	CHECK_FALSE(inputRunning);
	completeControl(); // GET_CUR
	CHECK(inputRunning);
}

TEST(UsbHostDriver, DoesNotStealMidiPipe) {
	g_usb_hstd_use_pipe[0] = USB_BITSET(USB_PIPE2);
	CHECK_EQUAL(USB_ERROR, checkDevice(uac1));
	CHECK_EQUAL(0, enumerationReplies);
	CHECK_EQUAL(0, pipeRegistrations);
	CHECK_EQUAL(USB_HOST_AUDIO_PIPE_IN_USE, usb_hstd_audio_status());
}

TEST(UsbHostDriver, WritableUac2SetsThenVerifiesClockBeforeSelectingInterface) {
	configure(uac2);
	CHECK_EQUAL(0x0121, pendingControl->p_setup[0]);
	CHECK_EQUAL(10 << 8, pendingControl->p_setup[2]);
	CHECK_EQUAL(4, pendingControl->tranlen);
	const auto* bytes = static_cast<uint8_t*>(pendingControl->p_tranadr);
	CHECK_EQUAL(0x44, bytes[0]);
	CHECK_EQUAL(0xac, bytes[1]);
	completeControl();
	CHECK_EQUAL(0x01a1, pendingControl->p_setup[0]);
	completeControl();
	CHECK_EQUAL(USB_SET_INTERFACE | USB_INTERFACE, pendingControl->p_setup[0]);
	completeControl();
	CHECK(inputRunning);
}

TEST(UsbHostDriver, ReadOnlyUac2ClockDoesNotNeedASetRequest) {
	auto descriptor = uac2;
	descriptor[23] = 1;
	configure(descriptor);
	CHECK_EQUAL(0x01a1, pendingControl->p_setup[0]);
	completeControl();
	completeControl();
	CHECK(inputRunning);
}

TEST(UsbHostDriver, ClockMismatchDoesNotStartAudio) {
	configure(uac2);
	completeControl();
	completeControl(USB_CTRL_END, 48000);
	CHECK_FALSE(inputRunning);
	CHECK_EQUAL(USB_HOST_AUDIO_CLOCK_ERROR, usb_hstd_audio_status());
	CHECK(pendingControl == nullptr);
	CHECK(pendingPacket == nullptr);
}

TEST(UsbHostDriver, FailedClockRequestDoesNotStartAudio) {
	configure(uac2);
	completeControl(USB_DATA_STALL);
	CHECK_FALSE(inputRunning);
	CHECK(pendingControl == nullptr);
	CHECK(pendingPacket == nullptr);
}

TEST(UsbHostDriver, ShortClockReplyDoesNotStartAudio) {
	configure(uac2);
	completeControl();
	completeControl(USB_CTRL_END, 44100, 1);
	CHECK_FALSE(inputRunning);
	CHECK(pendingControl == nullptr);
	CHECK(pendingPacket == nullptr);
}

TEST(UsbHostDriver, FailedInterfaceRequestDoesNotStartAudio) {
	configure(uac1);
	completeControl(USB_DATA_STALL);
	CHECK_FALSE(inputRunning);
	CHECK_EQUAL(USB_HOST_AUDIO_INTERFACE_ERROR, usb_hstd_audio_status());
	CHECK(pendingPacket == nullptr);
}

TEST(UsbHostDriver, IncompleteAudioFrameIsDiscardedAndCaptureContinues) {
	configure(uac1);
	completeControl();
	auto* transfer = pendingPacket;
	pendingPacket = nullptr;
	transfer->tranlen -= 5; // A stereo 24-bit frame needs six bytes.
	transfer->status = USB_DATA_READING;
	transfer->complete(transfer, 0, 0);
	CHECK(capturedSamples.empty());
	CHECK(inputRunning);
	CHECK(pendingPacket != nullptr);
}

TEST(UsbHostDriver, DetachTerminationDoesNotResubmitOrPublishOldAudio) {
	configure(uac1);
	completeControl();
	auto* transfer = pendingPacket;
	pendingPacket = nullptr;
	transfer->status = USB_DATA_STOP;
	transfer->tranlen = 0;
	transfer->complete(transfer, 0, 0);
	CHECK_FALSE(inputRunning);
	CHECK(pendingPacket == nullptr);
	CHECK(capturedSamples.empty());
	registeredDriver.devdetach(&context, 1, 0);
	g_usb_hstd_use_pipe[0] = 0;
	configure(uac1);
	completeControl();
	CHECK(inputRunning);
}

TEST(UsbHostDriver, ClosingSessionCancelsAnOutstandingControlSequence) {
	configure(uac2);
	usb_hstd_audio_stop();
	completeControl();
	CHECK_FALSE(inputRunning);
	CHECK(pendingControl == nullptr);
	CHECK(pendingPacket == nullptr);
}

TEST(UsbHostDriver, RoleSwitchClosesOldRoleBeforeOpeningNewRole) {
	CHECK(deluge::usb::setHostMode(true) == deluge::usb::ModeChangeResult::Success);
	CHECK(deluge::usb::hostMode());
	CHECK(deluge::usb::setHostMode(false) == deluge::usb::ModeChangeResult::Success);
	CHECK_FALSE(deluge::usb::hostMode());
	CHECK_EQUAL(4, roleChanges.size());
	for (size_t i = 0; i < roleChanges.size(); ++i) {
		CHECK_EQUAL(i + 1, roleChanges[i]);
	}
	CHECK_EQUAL(0, usbLock);
}

TEST(UsbHostDriver, EnumeratingHostAndBusyUsbPreventImmediateRoleSwitch) {
	externalVbus = true;
	peripheralState = USB_DS_DFLT;
	CHECK(deluge::usb::setHostMode(true) == deluge::usb::ModeChangeResult::HostConnected);
	CHECK(roleChanges.empty());
	CHECK_EQUAL(0, usbLock);
	externalVbus = false;
	usbLock = 1;
	CHECK(deluge::usb::setHostMode(true) == deluge::usb::ModeChangeResult::Busy);
	CHECK_EQUAL(1, usbLock);
	CHECK(roleChanges.empty());
}

TEST(UsbHostDriver, UsbVoltageAloneDoesNotIdentifyAnExternalHost) {
	externalVbus = true;
	peripheralState = USB_DS_SPD_POWR;
	CHECK(deluge::usb::setHostMode(true) == deluge::usb::ModeChangeResult::Success);
	CHECK(deluge::usb::hostMode());
}

TEST(UsbHostDriver, SuspendedConfiguredComputerPreventsHostMode) {
	externalVbus = true;
	peripheralState = USB_DS_SPD_CNFG;
	CHECK(deluge::usb::setHostMode(true) == deluge::usb::ModeChangeResult::HostConnected);
	CHECK(roleChanges.empty());
}

TEST(UsbHostDriver, QueuedRoleChangeWaitsForUsbLockAndReportsResult) {
	usbLock = 1;
	deluge::usb::requestHostMode(true, roleChanged);
	deluge::usb::processPendingModeChange();
	CHECK(roleChanges.empty());
	CHECK_EQUAL(0, roleCallbacks);
	CHECK(deluge::usb::requestedHostMode());
	usbLock = 0;
	deluge::usb::processPendingModeChange();
	CHECK(deluge::usb::hostMode());
	CHECK_EQUAL(1, roleCallbacks);
	CHECK(lastRoleResult == deluge::usb::ModeChangeResult::Success);
	deluge::usb::processPendingModeChange();
	CHECK_EQUAL(1, roleCallbacks);
}

TEST(UsbHostDriver, MostRecentQueuedRoleWins) {
	deluge::usb::requestHostMode(true, roleChanged);
	deluge::usb::requestHostMode(false, roleChanged);
	deluge::usb::processPendingModeChange();
	CHECK(roleChanges.empty());
	CHECK_FALSE(deluge::usb::requestedHostMode());
	CHECK_EQUAL(1, roleCallbacks);
}

TEST(UsbHostDriver, ControllerStartFailureHasItsOwnResult) {
	openFails = true;
	deluge::usb::requestHostMode(true, roleChanged);
	deluge::usb::processPendingModeChange();
	CHECK(lastRoleResult == deluge::usb::ModeChangeResult::StartFailed);
	CHECK_FALSE(deluge::usb::hostMode());
	CHECK_EQUAL(0, usbLock);
}

TEST(UsbHostDriver, RequestDuringControllerOpenIsPreserved) {
	requestDeviceDuringOpen = true;
	deluge::usb::requestHostMode(true, roleChanged);
	deluge::usb::processPendingModeChange();
	CHECK(deluge::usb::hostMode());
	CHECK_FALSE(deluge::usb::requestedHostMode());
	CHECK_EQUAL(0, roleCallbacks);
	deluge::usb::processPendingModeChange();
	CHECK_FALSE(deluge::usb::hostMode());
	CHECK_EQUAL(1, roleCallbacks);
}

TEST(UsbHostDriver, OversizedConfigurationHasADistinctStatus) {
	auto descriptor = uac1;
	descriptor[2] = 1;
	descriptor[3] = 4; // 1025 bytes exceeds the configuration buffer.
	CHECK_EQUAL(USB_ERROR, checkDevice(descriptor));
	CHECK_EQUAL(USB_HOST_AUDIO_CONFIG_TOO_LARGE, usb_hstd_audio_status());
	CHECK_EQUAL(0, enumerationReplies);
}

TEST(UsbHostDriver, AudioStatusDistinguishesRoleFromStreamProgress) {
	CHECK_EQUAL(USB_HOST_AUDIO_IDLE, usb_hstd_audio_status());
	configure(uac1);
	CHECK_EQUAL(USB_HOST_AUDIO_CONFIGURING, usb_hstd_audio_status());
	completeControl();
	CHECK_EQUAL(USB_HOST_AUDIO_WAITING_PACKETS, usb_hstd_audio_status());
	CHECK_EQUAL(0, usb_hstd_audio_packet_count());
	auto* transfer = pendingPacket;
	pendingPacket = nullptr;
	std::fill_n(static_cast<uint8_t*>(transfer->p_tranadr), 6, 0);
	transfer->tranlen -= 6;
	transfer->status = USB_DATA_READING;
	transfer->complete(transfer, 0, 0);
	CHECK_EQUAL(USB_HOST_AUDIO_RECEIVING, usb_hstd_audio_status());
	CHECK_EQUAL(1, usb_hstd_audio_packet_count());
	usb_hstd_audio_stop();
	CHECK_EQUAL(USB_HOST_AUDIO_IDLE, usb_hstd_audio_status());
	CHECK_EQUAL(0, usb_hstd_audio_packet_count());
}
