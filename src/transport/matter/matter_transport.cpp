/*
 * SPDX-License-Identifier: MIT
 *
 * Matter over Thread transport.
 *
 * Data model (src/default_zap/battery_switch.zap):
 *   Endpoint 0     Root Node + Power Source (battery)
 *   Endpoint 1..6  Generic Switch: Identify, Descriptor, Switch
 *
 * Each switch endpoint also has a Mode Select cluster to change the switch
 * type from the smart home app (e.g. Home Assistant shows it as a dropdown):
 *   mode 0 momentary, mode 2 latching (enum switch_type; 1 was removed).
 *
 * Endpoints without a switch in devicetree are disabled at runtime. The
 * Switch cluster feature map is set per endpoint at runtime:
 *   momentary: MS | MSR | MSL | MSM  (press/release/long press/multi press)
 *   latching:  MS | MSR | MSM        (every change of position is a short press)
 *
 * The device is a Thread sleepy end device and a Matter ICD; switch events
 * are sent to subscribed controllers as soon as they happen.
 *
 * Pairing diagnostics (core/diag.h): each commissioning step the controller
 * takes is reported, and when pairing fails, the furthest step reached says
 * what probably went wrong (wrong code, certificate rejected, no Thread
 * credentials, Thread network unreachable, ...).
 */

#include "transport/transport.h"

#include "core/app_loop.h"
#include "core/diag.h"
#include "core/status_led.h"

#include "app/matter_init.h"
#ifdef CONFIG_CHIP_FACTORY_DATA
#include "self_provision.h"
#endif

#include <app-common/zap-generated/attributes/Accessors.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app-common/zap-generated/ids/Commands.h>
#include <app/server/AppDelegate.h>
#include <app/clusters/identify-server/identify-server.h>
#include <app/clusters/mode-select-server/supported-modes-manager.h>
#include <app/clusters/switch-server/switch-server.h>
#include <app/server/Server.h>
#include <app/util/attribute-storage.h>
#include <app/util/endpoint-config-api.h>
#include <app/util/MatterCallbacks.h>
#include <app/util/generic-callbacks.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/ThreadStackManager.h>
#include <platform/nrfconnect/FactoryDataProvider.h>
#include <setup_payload/OnboardingCodesUtil.h>

#include <openthread/dataset.h>
#include <openthread/link.h>
#include <openthread/srp_client.h>
#include <openthread/thread.h>

#include <zephyr/logging/log.h>

/* The nRF Matter sample commons log to the "app" module. */
LOG_MODULE_REGISTER(app, CONFIG_CHIP_APP_LOG_LEVEL);

using namespace ::chip;
using namespace ::chip::DeviceLayer;

namespace Clusters = ::chip::app::Clusters;

namespace
{
constexpr EndpointId kRootEndpoint = 0;
constexpr EndpointId kFirstSwitchEndpoint = 1;

/* Common Number semantic tag namespace: tag N means "N". */
constexpr uint8_t kNamespaceCommonNumber = 0x07;

/*
 * Factory data, except that the hardware version string is the board this
 * firmware was built for (devicetree model), not the value stored when the
 * device was provisioned.
 */
class BoardFactoryDataProvider : public FactoryDataProvider<InternalFlashFactoryData>
{
public:
	CHIP_ERROR GetHardwareVersionString(char *buf, size_t bufSize) override
	{
		static constexpr char kBoard[] = APP_BOARD_NAME;

		VerifyOrReturnError(bufSize >= sizeof(kBoard), CHIP_ERROR_BUFFER_TOO_SMALL);
		memcpy(buf, kBoard, sizeof(kBoard));
		return CHIP_NO_ERROR;
	}
};

BoardFactoryDataProvider sFactoryDataProvider;

transport_switch_config sSwitches;
const battery_info *sBattery;

Clusters::Descriptor::Structs::SemanticTagStruct::Type sSwitchTags[SWITCH_INPUT_MAX];

EndpointId SwitchEndpoint(uint8_t index)
{
	return kFirstSwitchEndpoint + index;
}

/* ---- Identify ---- */

void OnIdentifyStart(::Identify *)
{
	status_led_set_identify(true);
}

void OnIdentifyStop(::Identify *)
{
	status_led_set_identify(false);
}

void OnTriggerEffect(::Identify *identify)
{
	using Clusters::Identify::EffectIdentifierEnum;

	switch (identify->mCurrentEffectIdentifier) {
	case EffectIdentifierEnum::kBlink:
		status_led_flash(1);
		break;
	case EffectIdentifierEnum::kBreathe:
		status_led_flash(15);
		break;
	case EffectIdentifierEnum::kOkay:
		status_led_flash(2);
		break;
	case EffectIdentifierEnum::kChannelChange:
		status_led_flash(8);
		break;
	case EffectIdentifierEnum::kFinishEffect:
	case EffectIdentifierEnum::kStopEffect:
		status_led_flash(0);
		status_led_set_identify(false);
		break;
	default:
		break;
	}
}

#ifdef CONFIG_APP_STATUS_LED
constexpr auto kIdentifyType = Clusters::Identify::IdentifyTypeEnum::kVisibleIndicator;
#else
constexpr auto kIdentifyType = Clusters::Identify::IdentifyTypeEnum::kNone;
#endif

#define SWITCH_IDENTIFY(ep) ::Identify sIdentify##ep(ep, OnIdentifyStart, OnIdentifyStop, kIdentifyType, OnTriggerEffect)

SWITCH_IDENTIFY(1);
SWITCH_IDENTIFY(2);
SWITCH_IDENTIFY(3);
SWITCH_IDENTIFY(4);
SWITCH_IDENTIFY(5);
SWITCH_IDENTIFY(6);

/* ---- Switch type (Mode Select) ---- */

using ModeOption = Clusters::ModeSelect::Structs::ModeOptionStruct::Type;

/* The mode value is the enum switch_type value (0 and 2; 1 was removed). */
const ModeOption kSwitchModes[] = {
	{ CharSpan::fromCharString("Momentary (push button)"), SWITCH_TYPE_MOMENTARY, {} },
	{ CharSpan::fromCharString("Latching (rocker, toggle)"), SWITCH_TYPE_LATCHING_AS_PRESS, {} },
};

class SwitchModesManager : public Clusters::ModeSelect::SupportedModesManager {
public:
	ModeOptionsProvider getModeOptionsProvider(EndpointId endpoint) const override
	{
		if (!IsSwitchEndpoint(endpoint)) {
			return ModeOptionsProvider();
		}
		return ModeOptionsProvider(std::begin(kSwitchModes), std::end(kSwitchModes));
	}

	Protocols::InteractionModel::Status getModeOptionByMode(EndpointId endpoint, uint8_t mode,
								const ModeOption **dataPtr) const override
	{
		if (IsSwitchEndpoint(endpoint)) {
			for (const ModeOption &option : kSwitchModes) {
				if (option.mode == mode) {
					*dataPtr = &option;
					return Protocols::InteractionModel::Status::Success;
				}
			}
		}
		return Protocols::InteractionModel::Status::InvalidCommand;
	}

	static bool IsSwitchEndpoint(EndpointId endpoint)
	{
		return endpoint >= kFirstSwitchEndpoint && endpoint < kFirstSwitchEndpoint + sSwitches.count;
	}
};

SwitchModesManager sSwitchModes;

/* Switch cluster attributes for a switch type. Call with the stack locked. */
void ApplySwitchType(uint8_t index, enum switch_type type, bool active)
{
	namespace Attr = Clusters::Switch::Attributes;
	using Feature = Clusters::Switch::Feature;

	EndpointId ep = SwitchEndpoint(index);

	(void)active; /* No position to report: both types report presses. */

	/* Both types support multi press; a latching switch has no long press. */
	uint32_t features = static_cast<uint32_t>(Feature::kMomentarySwitch) |
			    static_cast<uint32_t>(Feature::kMomentarySwitchRelease) |
			    static_cast<uint32_t>(Feature::kMomentarySwitchMultiPress);

	if (type == SWITCH_TYPE_MOMENTARY) {
		features |= static_cast<uint32_t>(Feature::kMomentarySwitchLongPress);
	}

	Attr::MultiPressMax::Set(ep, CONFIG_APP_MULTI_PRESS_MAX);
	Attr::FeatureMap::Set(ep, features);
	Attr::CurrentPosition::Set(ep, SWITCH_POSITION_OPEN);
	Clusters::ModeSelect::Attributes::CurrentMode::Set(ep, static_cast<uint8_t>(type));
}

void SetupSwitchEndpoints()
{
	for (uint8_t i = sSwitches.count; i < SWITCH_INPUT_MAX; i++) {
		emberAfEndpointEnableDisable(SwitchEndpoint(i), false);
	}

	for (uint8_t i = 0; i < sSwitches.count; i++) {
		EndpointId ep = SwitchEndpoint(i);

		Clusters::Switch::Attributes::NumberOfPositions::Set(ep, 2);
		ApplySwitchType(i, sSwitches.type[i], sSwitches.active[i]);

		/* Lets controllers name the endpoints "1", "2", ... instead of six identical switches. */
		sSwitchTags[i].namespaceID = kNamespaceCommonNumber;
		sSwitchTags[i].tag = static_cast<uint8_t>(i + 1);
		SetTagList(ep, Span<const Clusters::Descriptor::Structs::SemanticTagStruct::Type>(&sSwitchTags[i], 1));
	}
}

void SetupPowerSource()
{
	namespace Attr = Clusters::PowerSource::Attributes;
	using namespace Clusters::PowerSource;

	CharSpan description = CharSpan::fromCharString(sBattery->description);
	bool aaa = sBattery->type != BATTERY_TYPE_CR2032;

	Attr::Status::Set(kRootEndpoint, PowerSourceStatusEnum::kActive);
	Attr::Order::Set(kRootEndpoint, 0);
	Attr::Description::Set(kRootEndpoint, description);
	Attr::BatPresent::Set(kRootEndpoint, true);
	Attr::BatReplaceability::Set(kRootEndpoint, BatReplaceabilityEnum::kUserReplaceable);
	Attr::BatReplacementDescription::Set(kRootEndpoint, description);
	Attr::BatQuantity::Set(kRootEndpoint, sBattery->quantity);
	Attr::BatCommonDesignation::Set(kRootEndpoint, aaa ? BatCommonDesignationEnum::kAaa
							   : BatCommonDesignationEnum::kUnspecified);
	/* EndpointList stays empty: the battery powers the whole node. */
}

/* Runs in the Matter thread during server start, with the stack locked. */
CHIP_ERROR PostServerInit()
{
	SetupSwitchEndpoints();
	SetupPowerSource();
	return CHIP_NO_ERROR;
}

/* ---- Pairing diagnostics ---- */

/*
 * Commissioning steps in the order a controller takes them. The furthest
 * step of an attempt tells where it stopped. Only touched in the Matter
 * thread or with the stack locked.
 */
enum class Step : uint8_t {
	kNone,
	kPaseStarted,    /* Controller started the secure session with the pairing code. */
	kPaseDone,       /* Pairing code accepted. */
	kAttestation,    /* Controller is checking the device certificate. */
	kCsr,            /* Certificate accepted, operational key requested. */
	kNoc,            /* Controller's credentials installed. */
	kNetworkAdded,   /* Thread network credentials received. */
	kNetworkConnect, /* Joining the Thread network. */
	kCase,           /* Controller reached the device over the Thread network. */
	kComplete,
};

const char *StepName(Step step)
{
	switch (step) {
	case Step::kNone:
		return "none";
	case Step::kPaseStarted:
		return "checking the pairing code";
	case Step::kPaseDone:
		return "pairing code accepted";
	case Step::kAttestation:
		return "controller checking the device certificate";
	case Step::kCsr:
		return "device certificate accepted";
	case Step::kNoc:
		return "controller credentials installed";
	case Step::kNetworkAdded:
		return "Thread network credentials received";
	case Step::kNetworkConnect:
		return "joining the Thread network";
	case Step::kCase:
		return "controller reached the device over Thread";
	case Step::kComplete:
		return "complete";
	}
	return "?";
}

struct PairingAttempt {
	Step step = Step::kNone;
	bool active = false;
	/* Result of the last finished attempt, for the status command. */
	const char *result = nullptr;
};

PairingAttempt sAttempt;
otDeviceRole sLastRole = OT_DEVICE_ROLE_DISABLED;

void ReachStep(Step step)
{
	if (static_cast<uint8_t>(step) > static_cast<uint8_t>(sAttempt.step)) {
		sAttempt.step = step;
	}
}

const char *RoleName(otDeviceRole role)
{
	switch (role) {
	case OT_DEVICE_ROLE_DISABLED:
		return "off";
	case OT_DEVICE_ROLE_DETACHED:
		return "searching for the network (detached)";
	case OT_DEVICE_ROLE_CHILD:
		return "connected (child)";
	case OT_DEVICE_ROLE_ROUTER:
		return "connected (router)";
	case OT_DEVICE_ROLE_LEADER:
		return "connected (leader)";
	}
	return "?";
}

const char *SrpStateName(otSrpClientItemState state)
{
	switch (state) {
	case OT_SRP_CLIENT_ITEM_STATE_TO_ADD:
	case OT_SRP_CLIENT_ITEM_STATE_ADDING:
		return "registering";
	case OT_SRP_CLIENT_ITEM_STATE_TO_REFRESH:
	case OT_SRP_CLIENT_ITEM_STATE_REFRESHING:
		return "refreshing";
	case OT_SRP_CLIENT_ITEM_STATE_TO_REMOVE:
	case OT_SRP_CLIENT_ITEM_STATE_REMOVING:
		return "removing";
	case OT_SRP_CLIENT_ITEM_STATE_REGISTERED:
		return "registered";
	case OT_SRP_CLIENT_ITEM_STATE_REMOVED:
		return "removed";
	}
	return "?";
}

struct ThreadState {
	otDeviceRole role;
	bool commissioned;
	bool srpRunning;
	otSrpClientItemState srpState;
	uint8_t channel;
	otPanId panId;
	char networkName[OT_NETWORK_NAME_MAX_SIZE + 1];
};

void GetThreadState(ThreadState &state)
{
	ThreadStackMgr().LockThreadStack();

	otInstance *ot = ThreadStackMgrImpl().OTInstance();

	state.role = otThreadGetDeviceRole(ot);
	state.commissioned = otDatasetIsCommissioned(ot);
	state.srpRunning = otSrpClientIsRunning(ot);
	state.srpState = otSrpClientGetHostInfo(ot)->mState;
	state.channel = otLinkGetChannel(ot);
	state.panId = otLinkGetPanId(ot);
	strncpy(state.networkName, otThreadGetNetworkName(ot), sizeof(state.networkName) - 1);
	state.networkName[sizeof(state.networkName) - 1] = '\0';

	ThreadStackMgr().UnlockThreadStack();
}

/* Explain a failed attempt from the furthest step it reached. Matter thread. */
void ExplainFailure(CHIP_ERROR err)
{
	ThreadState thread;

	switch (sAttempt.step) {
	case Step::kNone:
	case Step::kPaseStarted:
		if (err == CHIP_ERROR_INVALID_PASE_PARAMETER) {
			diag_event("  -> Wrong pairing code: the controller used a different code than this");
			diag_event("     device's. Scan the QR code again or check the digits (press I).");
		} else if (err == CHIP_ERROR_TIMEOUT) {
			diag_event("  -> The controller stopped answering while checking the pairing code.");
			diag_event("     Keep the phone close and the app open, then try again.");
		} else {
			diag_event("  -> The secure session with the pairing code failed (error 0x%" PRIx32 ").",
				   err.AsInteger());
		}
		break;
	case Step::kPaseDone:
	case Step::kAttestation:
		diag_event("  -> The controller stopped after reading the device certificate. This is an");
		diag_event("     uncertified test device (Matter test certificates, vendor 0xFFF1). Some");
		diag_event("     ecosystems refuse it or need a developer/test setting; others ask you to");
		diag_event("     confirm adding an uncertified device. Home Assistant accepts it.");
		break;
	case Step::kCsr:
		diag_event("  -> The controller stopped before installing its credentials on the device.");
		diag_event("     Check the controller's own logs for the reason.");
		break;
	case Step::kNoc:
		diag_event("  -> The controller never sent Thread network credentials. It needs a Thread");
		diag_event("     border router, and the phone app needs its Thread credentials (Home");
		diag_event("     Assistant: Settings > Thread > sync the credentials to the phone).");
		break;
	case Step::kNetworkAdded:
	case Step::kNetworkConnect:
		GetThreadState(thread);
		if (thread.role == OT_DEVICE_ROLE_DISABLED || thread.role == OT_DEVICE_ROLE_DETACHED) {
			diag_event("  -> The device could not join the Thread network \"%s\" (channel %u).",
				   thread.networkName, thread.channel);
			diag_event("     Is the border router on and within range? Are the phone's Thread");
			diag_event("     credentials current (border router reset or replaced)?");
		} else {
			diag_event("  -> The device joined the Thread network but the controller never reached");
			diag_event("     it there. The controller must reach Thread through the border router:");
			diag_event("     same LAN, IPv6 enabled, no VLAN or firewall in between.");
			diag_event("     Service registration with the border router (SRP): %s.",
				   thread.srpRunning ? SrpStateName(thread.srpState) : "not started");
		}
		break;
	case Step::kCase:
		diag_event("  -> The controller reached the device over Thread but did not finish.");
		diag_event("     Check the controller's own logs for the reason.");
		break;
	case Step::kComplete:
		break;
	}
}

class PairingDiagnostics : public AppDelegate, public DataModelCallbacks {
public:
	void OnCommissioningSessionEstablishmentStarted() override
	{
		sAttempt.step = Step::kPaseStarted;
		sAttempt.active = true;
		diag_event("Controller is checking the pairing code (PASE)");
	}

	void OnCommissioningSessionStarted() override
	{
		ReachStep(Step::kPaseDone);
		diag_event("Pairing code accepted, secure session established");
	}

	/* PASE failed, or it succeeded and the fail-safe expired later. */
	void OnCommissioningSessionEstablishmentError(CHIP_ERROR err) override
	{
		diag_event("Pairing FAILED at step: %s", StepName(sAttempt.step));
		ExplainFailure(err);
		sAttempt.result = "failed";
		sAttempt.active = false;
	}

	void OnCommissioningSessionStopped() override
	{
		diag_event("Too many failed attempts, pairing mode stopped. Press P or a switch to retry.");
	}

	void OnCommissioningWindowOpened() override
	{
		diag_event("Pairing mode on: Bluetooth advertising for up to %d minutes",
			   CONFIG_CHIP_BLE_ADVERTISING_DURATION);
	}

	void OnCommissioningWindowClosed() override
	{
		if (Server::GetInstance().GetFabricTable().FabricCount() != 0) {
			diag_event("Pairing mode off");
		} else {
			diag_event("Pairing mode off. Press P or a switch to start it again.");
		}
	}

	/* Called for every command; only commissioning commands are reported. */
	CHIP_ERROR PreCommandReceived(const app::ConcreteCommandPath &path,
				      const Access::SubjectDescriptor &subject) override
	{
		if (subject.authMode != Access::AuthMode::kPase &&
		    !Server::GetInstance().GetFailSafeContext().IsFailSafeArmed()) {
			return CHIP_NO_ERROR;
		}

		const char *name = CommissioningStep(path);

		if (name) {
			diag_event("Controller: %s", name);
		} else {
			diag_event("Controller: command 0x%04" PRIx32 "/0x%02" PRIx32 " on endpoint %u",
				   path.mClusterId, path.mCommandId, path.mEndpointId);
		}
		return CHIP_NO_ERROR;
	}

private:
	const char *CommissioningStep(const app::ConcreteCommandPath &path)
	{
		namespace GC = Clusters::GeneralCommissioning::Commands;
		namespace OC = Clusters::OperationalCredentials::Commands;
		namespace NC = Clusters::NetworkCommissioning::Commands;

		switch (path.mClusterId) {
		case Clusters::GeneralCommissioning::Id:
			switch (path.mCommandId) {
			case GC::ArmFailSafe::Id:
				return "arm fail-safe (start or extend, or cancel commissioning)";
			case GC::SetRegulatoryConfig::Id:
				return "set regulatory config";
			case GC::CommissioningComplete::Id:
				return "commissioning complete";
			}
			break;
		case Clusters::OperationalCredentials::Id:
			switch (path.mCommandId) {
			case OC::CertificateChainRequest::Id:
				ReachStep(Step::kAttestation);
				return "read device certificate";
			case OC::AttestationRequest::Id:
				ReachStep(Step::kAttestation);
				return "check device attestation";
			case OC::CSRRequest::Id:
				ReachStep(Step::kCsr);
				return "request operational key (certificate accepted)";
			case OC::AddTrustedRootCertificate::Id:
				return "install controller root certificate";
			case OC::AddNOC::Id:
				ReachStep(Step::kNoc);
				return "install operational certificate (join fabric)";
			}
			break;
		case Clusters::NetworkCommissioning::Id:
			switch (path.mCommandId) {
			case NC::ScanNetworks::Id:
				return "scan for Thread networks";
			case NC::AddOrUpdateThreadNetwork::Id:
				ReachStep(Step::kNetworkAdded);
				return "set Thread network credentials";
			case NC::ConnectNetwork::Id:
				ReachStep(Step::kNetworkConnect);
				return "join the Thread network";
			}
			break;
		case Clusters::IcdManagement::Id:
			return "register for check-ins (ICD)";
		case Clusters::TimeSynchronization::Id:
			return "set time";
		}
		return nullptr;
	}
};

PairingDiagnostics sPairingDiagnostics;
CommonCaseDeviceServerInitParams sServerInitParams;

void DiagEvent(const ChipDeviceEvent *event)
{
	switch (event->Type) {
	case DeviceEventType::kCHIPoBLEConnectionEstablished:
		diag_event("Bluetooth: controller connected");
		break;
	case DeviceEventType::kCHIPoBLEConnectionClosed:
		diag_event("Bluetooth: disconnected");
		break;
	case DeviceEventType::kFailSafeTimerExpired:
		diag_event("Fail-safe expired: the controller cancelled pairing or a step timed out");
		break;
	case DeviceEventType::kSecureSessionEstablished:
		if (event->SecureSessionEstablished.SecureSessionType ==
		    static_cast<uint8_t>(Transport::SecureSession::Type::kCASE)) {
			if (sAttempt.active) {
				ReachStep(Step::kCase);
			}
			diag_event("Controller (fabric %u) connected over the Thread network",
				   event->SecureSessionEstablished.FabricIndex);
		}
		break;
	case DeviceEventType::kCommissioningComplete: {
		const FabricInfo *fabric =
			Server::GetInstance().GetFabricTable().FindFabricWithIndex(event->CommissioningComplete.fabricIndex);

		ReachStep(Step::kComplete);
		sAttempt.result = "succeeded";
		sAttempt.active = false;
		diag_event("Pairing complete: added to fabric %u (controller vendor 0x%04x)",
			   event->CommissioningComplete.fabricIndex,
			   fabric ? static_cast<unsigned int>(fabric->GetVendorId()) : 0U);
		break;
	}
	case DeviceEventType::kThreadStateChange:
		if (event->ThreadStateChange.RoleChanged) {
			ThreadState thread;

			GetThreadState(thread);
			if (thread.role != sLastRole) {
				sLastRole = thread.role;
				diag_event("Thread: %s", RoleName(thread.role));
			}
		}
		break;
	default:
		break;
	}
}

void MatterEventHandler(const ChipDeviceEvent *event, intptr_t /* arg */)
{
	if (event->Type == DeviceEventType::kCHIPoBLEAdvertisingChange) {
		status_led_set_pairing(event->CHIPoBLEAdvertisingChange.Result == kActivity_Started);
	}
	DiagEvent(event);
}

} /* namespace */

/*
 * Attribute changes from the data model. A ChangeToMode command from a
 * controller writes Mode Select CurrentMode; hand the request to the
 * application loop, which applies and stores the new switch type. Our own
 * writes of CurrentMode come through here too and are ignored there because
 * the type doesn't change.
 */
void MatterPostAttributeChangeCallback(const chip::app::ConcreteAttributePath &path, uint8_t type, uint16_t size,
				       uint8_t *value)
{
	if (path.mClusterId == Clusters::ModeSelect::Id &&
	    path.mAttributeId == Clusters::ModeSelect::Attributes::CurrentMode::Id &&
	    SwitchModesManager::IsSwitchEndpoint(path.mEndpointId) && size == 1) {
		app_loop_post(APP_EVT_SWITCH_TYPE, path.mEndpointId - kFirstSwitchEndpoint, *value);
	}
}

/* ---- Transport API ---- */

int transport_init(const struct transport_switch_config *switches, const struct battery_info *battery)
{
	sSwitches = *switches;
	sBattery = battery;

	Clusters::ModeSelect::setSupportedModesManager(&sSwitchModes);

	Nrf::Matter::InitData initData;
	initData.mFactoryDataProvider = &sFactoryDataProvider;
	sServerInitParams.appDelegate = &sPairingDiagnostics;
	initData.mServerInitParams = &sServerInitParams;
	DataModelCallbacks::SetInstance(&sPairingDiagnostics);
#ifdef CONFIG_CHIP_FACTORY_DATA
	/* Generic firmware image: the first boot creates the pairing credentials. */
	initData.mPreServerInitClbk = SelfProvisionFactoryData;
#endif
	initData.mPostServerInitClbk = PostServerInit;

	CHIP_ERROR err = Nrf::Matter::PrepareServer(initData);
	if (err != CHIP_NO_ERROR) {
		LOG_ERR("PrepareServer failed: %" CHIP_ERROR_FORMAT, err.Format());
		return -EIO;
	}

	err = Nrf::Matter::RegisterEventHandler(MatterEventHandler, 0);
	if (err != CHIP_NO_ERROR) {
		LOG_ERR("RegisterEventHandler failed: %" CHIP_ERROR_FORMAT, err.Format());
		return -EIO;
	}

	err = Nrf::Matter::StartServer();
	if (err != CHIP_NO_ERROR) {
		LOG_ERR("StartServer failed: %" CHIP_ERROR_FORMAT, err.Format());
		return -EIO;
	}

	return 0;
}

void transport_switch_event(uint8_t index, const struct switch_event *evt)
{
	namespace Attr = Clusters::Switch::Attributes;

	if (index >= sSwitches.count) {
		return;
	}

	EndpointId ep = SwitchEndpoint(index);
	auto &server = Clusters::SwitchServer::Instance();

	StackLock lock;

	switch (evt->type) {
	case SWITCH_EVENT_INITIAL_PRESS:
		Attr::CurrentPosition::Set(ep, evt->position);
		server.OnInitialPress(ep, evt->position);
		break;
	case SWITCH_EVENT_LONG_PRESS:
		server.OnLongPress(ep, evt->position);
		break;
	case SWITCH_EVENT_SHORT_RELEASE:
		Attr::CurrentPosition::Set(ep, SWITCH_POSITION_OPEN);
		server.OnShortRelease(ep, evt->position);
		break;
	case SWITCH_EVENT_LONG_RELEASE:
		Attr::CurrentPosition::Set(ep, SWITCH_POSITION_OPEN);
		server.OnLongRelease(ep, evt->position);
		break;
	case SWITCH_EVENT_MULTI_PRESS_ONGOING:
		server.OnMultiPressOngoing(ep, evt->position, evt->count);
		break;
	case SWITCH_EVENT_MULTI_PRESS_COMPLETE:
		server.OnMultiPressComplete(ep, evt->position, evt->count);
		break;
	}

#ifdef CONFIG_CHIP_ICD_UAT_SUPPORT
	/* User activity: stay in active mode briefly so acknowledgements arrive quickly. */
	Server::GetInstance().GetICDManager().OnNetworkActivity();
#endif
}

void transport_switch_type_changed(uint8_t index, enum switch_type type, bool active)
{
	if (index >= sSwitches.count) {
		return;
	}

	StackLock lock;

	sSwitches.type[index] = type;
	ApplySwitchType(index, type, active);
}

void transport_battery_update(const struct battery_state *state)
{
	namespace Attr = Clusters::PowerSource::Attributes;
	using Clusters::PowerSource::BatChargeLevelEnum;

	BatChargeLevelEnum level = BatChargeLevelEnum::kOk;

	if (state->level == BATTERY_LEVEL_CRITICAL) {
		level = BatChargeLevelEnum::kCritical;
	} else if (state->level == BATTERY_LEVEL_WARNING) {
		level = BatChargeLevelEnum::kWarning;
	}

	StackLock lock;

	Attr::BatVoltage::Set(kRootEndpoint, state->voltage_mv);
	/* BatPercentRemaining is in half percent units. */
	Attr::BatPercentRemaining::Set(kRootEndpoint, static_cast<uint8_t>(state->percent * 2));
	Attr::BatChargeLevel::Set(kRootEndpoint, level);
	Attr::BatReplacementNeeded::Set(kRootEndpoint, state->level == BATTERY_LEVEL_CRITICAL);
}

bool transport_is_provisioned(void)
{
	StackLock lock;

	return Server::GetInstance().GetFabricTable().FabricCount() != 0;
}

void transport_start_pairing(void)
{
	StackLock lock;

	auto &windowManager = Server::GetInstance().GetCommissioningWindowManager();

	if (Server::GetInstance().GetFabricTable().FabricCount() != 0 ||
	    windowManager.IsCommissioningWindowOpen()) {
		return;
	}

	CHIP_ERROR err = windowManager.OpenBasicCommissioningWindow();
	if (err != CHIP_NO_ERROR) {
		LOG_ERR("OpenBasicCommissioningWindow failed: %" CHIP_ERROR_FORMAT, err.Format());
	}
}

void transport_stop_pairing(void)
{
	StackLock lock;

	Server::GetInstance().GetCommissioningWindowManager().CloseCommissioningWindow();
}

bool transport_is_pairing(void)
{
	StackLock lock;

	return Server::GetInstance().GetCommissioningWindowManager().IsCommissioningWindowOpen();
}

void transport_print_status(void)
{
	ThreadState thread;

	{
		StackLock lock;
		auto &fabrics = Server::GetInstance().GetFabricTable();

		if (fabrics.FabricCount() == 0) {
			diag_print("Paired: no");
		} else {
			diag_print("Paired: with %u controller(s)", fabrics.FabricCount());
			for (const FabricInfo &fabric : fabrics) {
				diag_print("  fabric %u: controller vendor 0x%04x, fabric ID 0x%" PRIx64
					   ", node ID 0x%" PRIx64,
					   fabric.GetFabricIndex(), static_cast<unsigned int>(fabric.GetVendorId()),
					   fabric.GetFabricId(), fabric.GetNodeId());
			}
		}

		diag_print("Pairing mode: %s",
			   Server::GetInstance().GetCommissioningWindowManager().IsCommissioningWindowOpen()
				   ? "on (Bluetooth advertising)"
				   : "off");
		if (sAttempt.active) {
			diag_print("Pairing attempt: in progress, at step: %s", StepName(sAttempt.step));
		} else if (sAttempt.result) {
			diag_print("Last pairing attempt: %s, furthest step: %s", sAttempt.result,
				   StepName(sAttempt.step));
		}
	}

	GetThreadState(thread);
	if (!thread.commissioned) {
		diag_print("Thread: no network credentials yet (the controller sends them while pairing)");
		return;
	}
	diag_print("Thread: %s, network \"%s\", channel %u, PAN ID 0x%04x", RoleName(thread.role),
		   thread.networkName, thread.channel, thread.panId);
	diag_print("Service registration with the border router (SRP): %s",
		   thread.srpRunning ? SrpStateName(thread.srpState) : "not started");
}

void transport_factory_reset(void)
{
	StackLock lock;

	Server::GetInstance().ScheduleFactoryReset();
}

void transport_get_pairing_info(struct transport_pairing_info *info)
{
	StackLock lock;
	RendezvousInformationFlags flags(RendezvousInformationFlag::kBLE);
	char manual[kManualSetupLongCodeCharLength + 1];
	MutableCharSpan manualSpan(manual);
	MutableCharSpan qrSpan(info->qr, sizeof(info->qr) - 1);

	info->provisioned = Server::GetInstance().GetFabricTable().FabricCount() != 0;
	(void)GetDeviceInstanceInfoProvider()->GetSerialNumber(info->serial, sizeof(info->serial));

	/*
	 * Devices from tools/provision_device.py don't store their passcode.
	 * Check first: the payload helpers would fall back to the public test
	 * passcode and show a wrong code.
	 */
	uint32_t passcode;

	if (GetCommissionableDataProvider()->GetSetupPasscode(passcode) != CHIP_NO_ERROR || passcode == 0 ||
	    GetQRCode(qrSpan, flags) != CHIP_NO_ERROR || GetManualPairingCode(manualSpan, flags) != CHIP_NO_ERROR ||
	    manualSpan.size() != 11) {
		info->qr[0] = '\0';
		strncpy(info->hint, "Pairing code: see the label that came with the device.", sizeof(info->hint) - 1);
		return;
	}

	/* 11 digits, shown as 1234-567-8901 like on Matter labels. */
	snprintf(info->code, sizeof(info->code), "%.4s-%.3s-%.4s", manual, manual + 4, manual + 7);
}
