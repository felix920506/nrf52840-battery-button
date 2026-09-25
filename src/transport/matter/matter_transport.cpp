/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Matter over Thread transport.
 *
 * Data model (src/default_zap/battery_switch.zap):
 *   Endpoint 0     Root Node + Power Source (battery)
 *   Endpoint 1..6  Generic Switch: Identify, Descriptor, Switch
 *
 * Endpoints without a switch in devicetree are disabled at runtime. The
 * Switch cluster feature map is set per endpoint at runtime:
 *   momentary:         MS | MSR | MSL | MSM  (press/release/long press/multi press)
 *   latching:          LS                    (SwitchLatched)
 *   latching-as-press: MS | MSR | MSM        (every change is a short press)
 *
 * The device is a Thread sleepy end device and a Matter ICD; switch events
 * are sent to subscribed controllers as soon as they happen.
 */

#include "transport/transport.h"

#include "core/status_led.h"

#include "app/matter_init.h"

#include <app-common/zap-generated/attributes/Accessors.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/clusters/identify-server/identify-server.h>
#include <app/clusters/switch-server/switch-server.h>
#include <app/server/Server.h>
#include <app/util/attribute-storage.h>
#include <app/util/endpoint-config-api.h>
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

/* ---- Setup ---- */

void SetupSwitchEndpoints()
{
	namespace Attr = Clusters::Switch::Attributes;
	using Feature = Clusters::Switch::Feature;

	for (uint8_t i = sSwitches.count; i < SWITCH_INPUT_MAX; i++) {
		emberAfEndpointEnableDisable(SwitchEndpoint(i), false);
	}

	for (uint8_t i = 0; i < sSwitches.count; i++) {
		EndpointId ep = SwitchEndpoint(i);
		uint32_t features;
		uint8_t position = SWITCH_POSITION_OPEN;

		switch (sSwitches.type[i]) {
		case SWITCH_TYPE_LATCHING:
			features = static_cast<uint32_t>(Feature::kLatchingSwitch);
			position = sSwitches.active[i] ? SWITCH_POSITION_CLOSED : SWITCH_POSITION_OPEN;
			break;
		case SWITCH_TYPE_LATCHING_AS_PRESS:
			/* No long press: the "press" is over as soon as it starts. */
			features = static_cast<uint32_t>(Feature::kMomentarySwitch) |
				   static_cast<uint32_t>(Feature::kMomentarySwitchRelease) |
				   static_cast<uint32_t>(Feature::kMomentarySwitchMultiPress);
			Attr::MultiPressMax::Set(ep, CONFIG_APP_MULTI_PRESS_MAX);
			break;
		case SWITCH_TYPE_MOMENTARY:
		default:
			features = static_cast<uint32_t>(Feature::kMomentarySwitch) |
				   static_cast<uint32_t>(Feature::kMomentarySwitchRelease) |
				   static_cast<uint32_t>(Feature::kMomentarySwitchLongPress) |
				   static_cast<uint32_t>(Feature::kMomentarySwitchMultiPress);
			Attr::MultiPressMax::Set(ep, CONFIG_APP_MULTI_PRESS_MAX);
			break;
		}

		Attr::FeatureMap::Set(ep, features);
		Attr::NumberOfPositions::Set(ep, 2);
		Attr::CurrentPosition::Set(ep, position);

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

/* ---- Transport API ---- */

int transport_init(const struct transport_switch_config *switches, const struct battery_info *battery)
{
	sSwitches = *switches;
	sBattery = battery;

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
	case SWITCH_EVENT_LATCHED:
		Attr::CurrentPosition::Set(ep, evt->position);
		server.OnSwitchLatch(ep, evt->position);
		break;
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
