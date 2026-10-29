#include "CppUTest/TestHarness.h"
#include "io/midi/midi_device_manager.h"
#include "io/midi/midi_engine.h"
#include <utility>
extern "C" {
#include "RZA1/usb/r_usb_basic/r_usb_basic_if.h"
#include "RZA1/usb/r_usb_basic/src/driver/inc/r_usb_extern.h"
#include "RZA1/usb/userdef/r_usb_hmidi_config.h"
extern uint8_t usbCurrentlyInitialized;
extern volatile uint32_t usbLock;
uint16_t g_usb_peri_connected = 0;
uint8_t stopSendingAfterDeviceNum[USB_NUM_USBIP]{};
uint8_t usbDeviceNumBeingSentToNow[USB_NUM_USBIP]{};
uint8_t anyUSBSendingStillHappening[USB_NUM_USBIP]{};
uint8_t currentDeviceNumWithSendPipe[USB_NUM_USBIP][2]{};
uint8_t usbHostOutPipeInitializedForHub[2]{};
usb_utr_t g_usb_midi_recv_utr[USB_NUM_USBIP][MAX_NUM_USB_MIDI_DEVICES]{};
uint16_t g_usb_hmidi_tmp_ep_tbl[USB_NUM_USBIP][MAX_NUM_USB_MIDI_DEVICES][USB_EPL * 2 + 1]{};
usb_utr_t* g_p_usb_pipe[USB_MAX_PIPE_NO + 1]{};
uint32_t g_usb_data_cnt[USB_MAX_PIPE_NO + 1]{};
uint8_t* g_p_usb_data[USB_MAX_PIPE_NO + 1]{};
uint16_t g_usb_hstd_ignore_cnt[USB_NUM_USBIP][USB_MAX_PIPE_NO + 1]{};
void resetUsbMidiConnections();
}
uint32_t timeLastBRDY[USB_NUM_USBIP]{};
bool anythingInUSBOutputBuffer = false;
ConnectedUSBMIDIDevice connectedUSBMIDIDevices[USB_NUM_USBIP][MAX_NUM_USB_MIDI_DEVICES];

namespace {
unsigned messages = 0;
unsigned tasks = 0;
size_t packetBytes = 0;
class Cable final : public MIDICableUSB {
public:
	void writeToFlash(uint8_t*) override {}
	char const* getDisplayName() override { return "test"; }
	void writeReferenceAttributesToFile(Serializer&) override {}
};
} // namespace
MIDICable::MIDICable() : connectionFlags(0) {
}
bool MIDICable::wantsToOutputMIDIOnChannel(MIDIMessage, int32_t) const {
	return true;
}
bool MIDICableUSB::wantsToOutputMIDIOnChannel(MIDIMessage, int32_t) const {
	return true;
}
void MIDICableUSB::sendMessage(MIDIMessage) {
}
void MIDICableUSB::sendSysex(const uint8_t*, int32_t) {
}
size_t MIDICableUSB::sendBufferSpace() const {
	return 0;
}
MidiEngine::MidiEngine() {
}
LearnedMIDI::LearnedMIDI() : cable(nullptr), channelOrZone(MIDI_CHANNEL_NONE), noteOrCC(0) {
}
void MidiEngine::midiMessageReceived(MIDICable&, uint8_t status, uint8_t channel, uint8_t note, uint8_t velocity,
                                     uint32_t*) {
	CHECK_EQUAL(9, status);
	CHECK_EQUAL(0, channel);
	CHECK_EQUAL(60, note);
	CHECK_EQUAL(100, velocity);
	++messages;
}
void MidiEngine::checkIncomingUsbSysex(const uint8_t*, int32_t, int32_t, int32_t) {
	FAIL("unexpected sysex");
}
namespace MIDIDeviceManager {
void recountSmallestMPEZones() {
}
} // namespace MIDIDeviceManager
extern "C" {
void usb_cstd_usb_task() {
	++tasks;
}
void usb_receive_start_rohan_midi(uint16_t pipe) {
	auto* transfer = g_p_usb_pipe[pipe];
	g_usb_data_cnt[pipe] = transfer->tranlen;
	g_p_usb_data[pipe] = static_cast<uint8_t*>(transfer->p_tranadr);
}
uint16_t usb_read_data_fast_rohan(uint16_t pipe) {
	CHECK_COMPARE(packetBytes, <=, g_usb_data_cnt[pipe]);
	for (size_t i = 0; i < packetBytes; i += 4) {
		auto* p = g_p_usb_data[pipe] + i;
		p[0] = 9;
		p[1] = 0x90;
		p[2] = 60;
		p[3] = 100;
	}
	g_usb_data_cnt[pipe] -= packetBytes;
	g_p_usb_data[pipe] += packetBytes;
	return USB_READEND;
}
uint16_t usb_cstd_get_pipe_type(usb_utr_t*, uint16_t) {
	return USB_TYPFIELD_BULK;
}
void usb_hstd_brdy_pipe_process(usb_utr_t*, uint16_t) {
	FAIL("unexpected audio pipe");
}
void usb_hstd_data_end(usb_utr_t*, uint16_t, uint16_t) {
	FAIL("unexpected hub completion");
}
void usb_hstd_forced_termination(usb_utr_t*, uint16_t, uint16_t) {
	FAIL("unexpected termination");
}
}

TEST_GROUP(UsbMidiIntegration) {
	Cable cable;
	void setup() override {
		usbLock = 0;
		resetUsbMidiConnections();
		messages = 0;
		tasks = 0;
		usbCurrentlyInitialized = 1;
		g_usb_usbmode = USB_HOST;
		g_usb_peri_connected = 0;
	}
	void teardown() override {
		resetUsbMidiConnections();
	}
};

TEST(UsbMidiIntegration, RealSetupCompletionAndPollingHandleFullAndHighSpeedPackets) {
	auto& device = connectedUSBMIDIDevices[0][0];
	device.cable[0] = &cable;
	constexpr uint16_t pipe = USB_PIPE2;
	auto& table = g_usb_hmidi_tmp_ep_tbl[0][0];
	table[USB_EPL] = pipe;
	g_usb_midi_recv_utr[0][0].p_tranadr = device.receiveData;
	MidiEngine engine;
	for (auto [capacity, length] : {std::pair<uint16_t, uint16_t>{64, 64}, {512, 512}, {512, 12}, {64, 0}}) {
		messages = 0;
		table[USB_EPL + 3] = capacity;
		engine.checkIncomingUsbMidi();
		CHECK_EQUAL(capacity, g_p_usb_pipe[pipe]->tranlen);
		CHECK(device.currentlyWaitingToReceive);
		packetBytes = length;
		usb_utr_t context{};
		usb_hstd_brdy_pipe_process_rohan_midi_and_hub(&context, USB_BITSET(pipe));
		CHECK_FALSE(device.currentlyWaitingToReceive);
		CHECK_EQUAL(length, device.numBytesReceived);
		engine.checkIncomingUsbMidi();
		CHECK_EQUAL(length / 4, messages);
		CHECK(device.currentlyWaitingToReceive);
		device.currentlyWaitingToReceive = 0;
	}
	device.cable[0] = nullptr;
}

TEST(UsbMidiIntegration, InterruptedMultiDeviceSendDoesNotBlockInputAfterReset) {
	usbDeviceNumBeingSentToNow[0] = 1;
	stopSendingAfterDeviceNum[0] = 3;
	anyUSBSendingStillHappening[0] = 1;
	resetUsbMidiConnections();
	CHECK_EQUAL(usbDeviceNumBeingSentToNow[0], stopSendingAfterDeviceNum[0]);
	CHECK_EQUAL(0, anyUSBSendingStillHappening[0]);
	auto& device = connectedUSBMIDIDevices[0][0];
	device.cable[0] = &cable;
	g_usb_hmidi_tmp_ep_tbl[0][0][USB_EPL] = USB_PIPE2;
	g_usb_hmidi_tmp_ep_tbl[0][0][USB_EPL + 3] = 64;
	g_usb_midi_recv_utr[0][0].p_tranadr = device.receiveData;
	MidiEngine engine;
	engine.checkIncomingUsbMidi();
	CHECK(device.currentlyWaitingToReceive);
	device.cable[0] = nullptr;
}
