#include "CppUTest/TestHarness.h"

extern "C" {
#include "RZA1/usb/r_usb_basic/r_usb_basic_if.h"
#include "RZA1/usb/r_usb_basic/src/driver/inc/r_usb_extern.h"
#include "RZA1/usb/r_usb_basic/src/hw/inc/r_usb_bitdefine.h"
#include "RZA1/usb/r_usb_haudio/r_usb_haudio_if.h"
uint16_t g_usb_hstd_use_pipe[USB_NUM_USBIP]{};
uint16_t fifoSels[5]{};
extern volatile uint32_t usbLock;
uint16_t g_usb_usbmode = USB_PERI;
uint8_t usbCurrentlyInitialized = 1;
}

#include "drivers/usb/usb_mode.h"
#include "fixtures/tonex_usb_descriptors.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <type_traits>
#include <vector>

namespace {
usb_hcdreg_t registeredDriver{};
usb_utr_t context{};
std::remove_volatile_t<std::remove_pointer_t<usb_regadr_t>> registerBank{};
std::vector<std::vector<uint8_t>> fifoPackets;
size_t nextFifoPacket = 0;
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

uint16_t checkDevice(std::span<const uint8_t> configuration, uint16_t speed = USB_FSCONNECT, size_t controlOffset = 9) {
	uint16_t result = USB_ERROR;
	uint16_t address = 1;
	uint16_t* table[9]{};
	table[1] = reinterpret_cast<uint16_t*>(const_cast<uint8_t*>(configuration.data()));
	table[2] = reinterpret_cast<uint16_t*>(const_cast<uint8_t*>(configuration.data() + controlOffset));
	table[3] = &result;
	table[6] = &speed;
	table[7] = &address;
	registeredDriver.classcheck(&context, table);
	return result;
}

void configure(std::span<const uint8_t> configuration, uint16_t speed = USB_FSCONNECT, size_t controlOffset = 9) {
	CHECK_EQUAL(USB_OK, checkDevice(configuration, speed, controlOffset));
	registeredDriver.devconfig(&context, 1, 0);
}

void queueFifoPackets(std::initializer_list<std::vector<uint8_t>> packets) {
	fifoPackets = packets;
	nextFifoPacket = 0;
	registerBank.D1FIFOCTR = USB_FRDY | fifoPackets.front().size();
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
void usb_cstd_clr_transaction_counter(usb_utr_t*, uint16_t pipe) {
	CHECK_EQUAL(USB_PIPE2, pipe);
}
uint8_t* usb_hstd_read_fifo(usb_utr_t* ptr, uint16_t count, uint16_t port, uint8_t* destination) {
	CHECK_EQUAL(USB_D1USE, port);
	CHECK(nextFifoPacket < fifoPackets.size());
	const auto& packet = fifoPackets[nextFifoPacket++];
	CHECK_EQUAL(packet.size(), count);
	std::copy(packet.begin(), packet.end(), destination);
	ptr->ipp->D1FIFOCTR = nextFifoPacket < fifoPackets.size() ? USB_FRDY | fifoPackets[nextFifoPacket].size() : 0;
	return destination + count;
}
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
	table[1] = (table[1] & USB_DBLBFIELD) | USB_TYPFIELD_ISO | USB_DIR_H_IN | (endpoint[2] & 0x0f);
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
std::memset(&registerBank, 0, sizeof(registerBank));
context.ipp = &registerBank;
fifoPackets.clear();
nextFifoPacket = 0;
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
deluge::usb::setHostAudioEnabled(true);
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
	std::vector<std::vector<uint8_t>>().swap(fifoPackets);
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

TEST(UsbHostDriver, DisabledHostAudioDoesNotRegisterAClassOrClaimCompositeDevices) {
	deluge::usb::setHostAudioEnabled(false);
	deluge::usb::processPendingModeChange();
	registeredDriver = {};
	usb_hstd_audio_register(&context);
	CHECK(registeredDriver.classcheck == nullptr);
	CHECK_EQUAL(0, pipeRegistrations);
	CHECK(pendingControl == nullptr);
	deluge::usb::setHostAudioEnabled(true);
	deluge::usb::processPendingModeChange();
	usb_hstd_audio_register(&context);
	CHECK(registeredDriver.classcheck != nullptr);
	CHECK_EQUAL(USB_OK, checkDevice(uac1));
}

TEST(UsbHostDriver, HostAudioPreferenceDoesNotInterruptComputerConnection) {
	externalVbus = true;
	peripheralState = USB_DS_CNFG;
	deluge::usb::setHostAudioEnabled(false);
	deluge::usb::processPendingModeChange();
	deluge::usb::setHostAudioEnabled(true);
	deluge::usb::processPendingModeChange();
	CHECK(roleChanges.empty());
	CHECK_FALSE(deluge::usb::hostMode());
}

TEST(UsbHostDriver, HostAudioPreferenceReconnectsHostAfterUsbLockIsReleased) {
	CHECK(deluge::usb::setHostMode(true) == deluge::usb::ModeChangeResult::Success);
	roleChanges.clear();
	usbLock = 1;
	deluge::usb::setHostAudioEnabled(false);
	deluge::usb::processPendingModeChange();
	CHECK(roleChanges.empty());
	CHECK(deluge::usb::requestedHostMode());
	usbLock = 0;
	deluge::usb::processPendingModeChange();
	CHECK_EQUAL(2, roleChanges.size());
	CHECK_EQUAL(3, roleChanges[0]); // Close host before reopening with the new class set.
	CHECK_EQUAL(2, roleChanges[1]);
	CHECK(deluge::usb::hostMode());
	CHECK_FALSE(deluge::usb::hostAudioEnabled());
	roleChanges.clear();
	deluge::usb::setHostAudioEnabled(false); // An unchanged preference does not reconnect again.
	deluge::usb::processPendingModeChange();
	CHECK(roleChanges.empty());
}

TEST(UsbHostDriver, HostAudioPreferencePreservesPendingComputerRoleRequest) {
	CHECK(deluge::usb::setHostMode(true) == deluge::usb::ModeChangeResult::Success);
	roleChanges.clear();
	deluge::usb::requestHostMode(false, roleChanged);
	deluge::usb::setHostAudioEnabled(false);
	deluge::usb::processPendingModeChange();
	CHECK_FALSE(deluge::usb::hostMode());
	CHECK_EQUAL(2, roleChanges.size());
	CHECK_EQUAL(3, roleChanges[0]);
	CHECK_EQUAL(4, roleChanges[1]); // Open peripheral, never reopen host.
	CHECK_EQUAL(1, roleCallbacks);
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

TEST(UsbHostDriver, TonexHighSpeedCapturesBothFifoPlanesWithoutTaskTransfers) {
	configure(usb_audio_fixtures::tonexOne, USB_HSCONNECT, 83);
	CHECK_EQUAL(0x0702, pendingControl->p_setup[2]); // Clock 7 on AudioControl interface 2.
	completeControl();                               // SET_CUR
	completeControl();                               // GET_CUR
	CHECK_EQUAL(3, pendingControl->p_setup[2]);      // Select capture interface, not CDC or playback.
	CHECK_EQUAL(1, pendingControl->p_setup[1]);
	completeControl(); // SET_INTERFACE
	CHECK(inputRunning);
	CHECK(pendingPacket == nullptr); // No 125us audio requests go through the cooperative message queue.
	CHECK_EQUAL(USB_PID_BUF, registerBank.PIPE2CTR & USB_PID);
	CHECK(registerBank.BRDYENB & USB_BITSET(USB_PIPE2));
	registerBank.CFIFOSEL = 5;
	registerBank.PIPESEL = 4;
	std::vector<uint8_t> sixFrames(48);
	std::vector<uint8_t> fiveFrames(40);
	for (size_t i = 0; i < sixFrames.size(); i += 8) {
		sixFrames[i + 3] = 0x40;
		sixFrames[i + 7] = 0xc0;
	}
	queueFifoPackets({sixFrames, fiveFrames});
	CHECK_EQUAL(USB_BITSET(USB_PIPE2), usb_hstd_audio_handle_brdy(&context, USB_BITSET(USB_PIPE2) | USB_BRDY0));
	CHECK_EQUAL(2, usb_hstd_audio_packet_count());
	CHECK_EQUAL(22, capturedSamples.size());
	CHECK_EQUAL(0x40000000, capturedSamples[0]);
	CHECK_EQUAL(static_cast<int32_t>(0xc0000000u), capturedSamples[1]);
	CHECK_EQUAL(USB_HOST_AUDIO_RECEIVING, usb_hstd_audio_status());
	CHECK_EQUAL(5, registerBank.CFIFOSEL);
	CHECK_EQUAL(4, registerBank.PIPESEL);
	CHECK_EQUAL(USB_PID_BUF, registerBank.PIPE2CTR & USB_PID); // Still armed, including after short packets.
	CHECK(pendingPacket == nullptr);
}

TEST(UsbHostDriver, TonexPlugAlsoStartsAtHighSpeedAndStopsOnDetach) {
	configure(usb_audio_fixtures::tonexPlug, USB_HSCONNECT, 83);
	completeControl();
	completeControl();
	completeControl();
	CHECK_EQUAL(USB_HOST_AUDIO_WAITING_PACKETS, usb_hstd_audio_status());
	usb_hstd_audio_detach_device(2); // Disconnecting another device must not stop this stream.
	CHECK(inputRunning);
	usb_hstd_audio_detach_device(1); // Called before HCD deconfigures PIPE2.
	CHECK_FALSE(inputRunning);
	CHECK_EQUAL(0, registerBank.BRDYENB & USB_BITSET(USB_PIPE2));
	CHECK_EQUAL(USB_PID_NAK, registerBank.PIPE2CTR & USB_PID);
	CHECK_EQUAL(0, registerBank.D1FIFOSEL & USB_CURPIPE);
	queueFifoPackets({std::vector<uint8_t>(48)});
	CHECK_EQUAL(0, usb_hstd_audio_handle_brdy(&context, USB_BITSET(USB_PIPE2)));
	CHECK(capturedSamples.empty());
}

TEST(UsbHostDriver, HighSpeedDiscardsPartialFramesAndStopsOnOversizedPackets) {
	configure(usb_audio_fixtures::tonexOne, USB_HSCONNECT, 83);
	completeControl();
	completeControl();
	completeControl();
	queueFifoPackets({std::vector<uint8_t>(7)});
	usb_hstd_audio_handle_brdy(&context, USB_BITSET(USB_PIPE2));
	CHECK(capturedSamples.empty());
	CHECK_EQUAL(0, usb_hstd_audio_packet_count());
	CHECK(inputRunning);
	queueFifoPackets({std::vector<uint8_t>(64)}); // The selected endpoint allows at most 56.
	usb_hstd_audio_handle_brdy(&context, USB_BITSET(USB_PIPE2));
	CHECK_EQUAL(USB_HOST_AUDIO_TRANSFER_ERROR, usb_hstd_audio_status());
	CHECK_FALSE(inputRunning);
	CHECK_EQUAL(0, registerBank.BRDYENB & USB_BITSET(USB_PIPE2));
	CHECK(capturedSamples.empty());
}

TEST(UsbHostDriver, HighSpeedIrqWorkIsBoundedAndIgnoresUnrelatedBits) {
	configure(usb_audio_fixtures::tonexOne, USB_HSCONNECT, 83);
	completeControl();
	completeControl();
	completeControl();
	queueFifoPackets({std::vector<uint8_t>(48), std::vector<uint8_t>(48), std::vector<uint8_t>(48)});
	CHECK_EQUAL(0, usb_hstd_audio_handle_brdy(&context, USB_BRDY0));
	CHECK_EQUAL(0, nextFifoPacket);
	usb_hstd_audio_handle_brdy(&context, USB_BITSET(USB_PIPE2));
	CHECK_EQUAL(2, nextFifoPacket);
	usb_hstd_audio_handle_brdy(&context, USB_BITSET(USB_PIPE2));
	CHECK_EQUAL(3, usb_hstd_audio_packet_count());
}

TEST(UsbHostDriver, IsochronousAudioRejectsLowSpeed) {
	CHECK_EQUAL(USB_ERROR, checkDevice(uac1, USB_LSCONNECT));
	CHECK_EQUAL(0, enumerationReplies);
}
