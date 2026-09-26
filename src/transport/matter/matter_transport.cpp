/*
 * SPDX-License-Identifier: Apache-2.0
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
 */

#include "transport/transport.h"

#include "core/app_loop.h"
#include "core/status_led.h"

#include "app/matter_init.h"

#include <app-common/zap-generated/attributes/Accessors.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/clusters/identify-server/identify-server.h>
#include <app/clusters/mode-select-server/supported-modes-manager.h>
#include <app/clusters/switch-server/switch-server.h>
#include <app/server/Server.h>
#include <app/util/attribute-storage.h>
#include <app/util/endpoint-config-api.h>
#include <app/util/generic-callbacks.h>
#include <platform/CHIPDeviceLayer.h>

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

void MatterEventHandler(const ChipDeviceEvent *event, intptr_t /* arg */)
{
	if (event->Type == DeviceEventType::kCHIPoBLEAdvertisingChange) {
		status_led_set_pairing(event->CHIPoBLEAdvertisingChange.Result == kActivity_Started);
	}
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

void transport_factory_reset(void)
{
	StackLock lock;

	Server::GetInstance().ScheduleFactoryReset();
}
