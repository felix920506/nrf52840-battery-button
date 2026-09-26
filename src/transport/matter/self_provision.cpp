/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * First boot of a generic firmware image: create the device's own Matter
 * factory data.
 *
 * Release images contain only the application, so the same UF2 file can be
 * used for every device. When the factory data partition is still empty, a
 * random setup passcode, discriminator and SPAKE2+ salt are generated with
 * the hardware RNG, the SPAKE2+ verifier is computed, and everything is
 * written to the factory data partition in the same format that
 * tools/provision_device.py (the nRF Connect factory data generator)
 * produces. From then on the device looks exactly like a provisioned one.
 *
 * Differences to tools/provision_device.py:
 *  - The passcode is stored as well, so the device can show its pairing code
 *    on the USB serial port (there is no label).
 *  - The serial number is the nRF52840's factory-programmed device ID.
 *
 * The partition is outside the application, so flashing a new application
 * UF2 or a factory reset keeps the pairing code. Devices provisioned with
 * tools/provision_device.py already have factory data and are not touched.
 *
 * Invalid contents (e.g. a write interrupted by a power loss, or an image
 * that overlapped the page) are replaced as well: Matter can't start with
 * them, so the device would otherwise be dead.
 *
 * Device attestation uses the Matter development certificates for the test
 * vendor ID (same as tools/provision_device.py).
 */

#include "self_provision.h"

#include <credentials/examples/ExampleDACs.h>
#include <credentials/examples/ExamplePAI.h>
#include <crypto/CHIPCryptoPAL.h>
#include <lib/support/CHIPMem.h>
#include <lib/support/CodeUtils.h>
#include <platform/CHIPDeviceConfig.h>
#include <platform/nrfconnect/FactoryDataParser.h>
#include <setup_payload/SetupPayload.h>
#include <system/SystemError.h>

#include <hal/nrf_ficr.h>
#include <zcbor_encode.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>

#include <stdio.h>
#include <string.h>

LOG_MODULE_DECLARE(app, CONFIG_CHIP_APP_LOG_LEVEL);

using namespace ::chip;

namespace
{
#define FACTORY_DATA_PARTITION factory_data_partition

/* Large enough for the development DAC and PAI certificates and the rest. */
constexpr size_t kFactoryDataMaxLength = 1536;
constexpr size_t kSaltLength = 32;
constexpr size_t kMapEntries = 16;

bool PartitionIsEmpty(const uint8_t *data, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		if (data[i] != 0xff) {
			return false;
		}
	}
	return true;
}

CHIP_ERROR RandomPasscode(uint32_t &passcode)
{
	do {
		ReturnErrorOnFailure(Crypto::DRBG_get_bytes(reinterpret_cast<uint8_t *>(&passcode), sizeof(passcode)));
		passcode = passcode % kSetupPINCodeMaximumValue + 1;
	} while (!SetupPayload::IsValidSetupPIN(passcode));

	return CHIP_NO_ERROR;
}

bool PutString(zcbor_state_t *state, const char *key, const char *value)
{
	/* The nRF Connect factory data stores strings as byte strings. */
	return zcbor_tstr_encode_ptr(state, key, strlen(key)) && zcbor_bstr_encode_ptr(state, value, strlen(value));
}

bool PutBytes(zcbor_state_t *state, const char *key, ByteSpan value)
{
	return zcbor_tstr_encode_ptr(state, key, strlen(key)) &&
	       zcbor_bstr_encode_ptr(state, reinterpret_cast<const char *>(value.data()), value.size());
}

bool PutUint(zcbor_state_t *state, const char *key, uint32_t value)
{
	return zcbor_tstr_encode_ptr(state, key, strlen(key)) && zcbor_uint32_put(state, value);
}

CHIP_ERROR Encode(uint8_t *buf, size_t bufLen, size_t &outLen)
{
	uint32_t passcode;
	uint16_t discriminator;
	uint8_t salt[kSaltLength];
	char serial[17];
	uint8_t verifierData[Crypto::kSpake2p_VerifierSerialized_Length];
	MutableByteSpan verifierSpan(verifierData);
	Crypto::Spake2pVerifier verifier;

	ReturnErrorOnFailure(RandomPasscode(passcode));
	ReturnErrorOnFailure(Crypto::DRBG_get_bytes(reinterpret_cast<uint8_t *>(&discriminator), sizeof(discriminator)));
	discriminator &= 0xfff; /* 12 bits */
	ReturnErrorOnFailure(Crypto::DRBG_get_bytes(salt, sizeof(salt)));
	snprintf(serial, sizeof(serial), "%08X%08X", nrf_ficr_deviceid_get(NRF_FICR, 1),
		 nrf_ficr_deviceid_get(NRF_FICR, 0));

	ReturnErrorOnFailure(verifier.Generate(CONFIG_CHIP_DEVICE_SPAKE2_IT, ByteSpan(salt), passcode));
	ReturnErrorOnFailure(verifier.Serialize(verifierSpan));

	ZCBOR_STATE_E(state, 0, buf, bufLen, 1);

	bool ok = zcbor_map_start_encode(state, kMapEntries) &&
		  PutUint(state, "version", CONFIG_CHIP_FACTORY_DATA_VERSION) &&
		  PutString(state, "sn", serial) &&
		  PutUint(state, "vendor_id", CHIP_DEVICE_CONFIG_DEVICE_VENDOR_ID) &&
		  PutUint(state, "product_id", CHIP_DEVICE_CONFIG_DEVICE_PRODUCT_ID) &&
		  PutString(state, "vendor_name", CHIP_DEVICE_CONFIG_DEVICE_VENDOR_NAME) &&
		  PutString(state, "product_name", CHIP_DEVICE_CONFIG_DEVICE_PRODUCT_NAME) &&
		  PutUint(state, "hw_ver", CHIP_DEVICE_CONFIG_DEFAULT_DEVICE_HARDWARE_VERSION) &&
		  PutString(state, "hw_ver_str", CHIP_DEVICE_CONFIG_DEFAULT_DEVICE_HARDWARE_VERSION_STRING) &&
		  PutBytes(state, "dac_cert", DevelopmentCerts::kDacCert) &&
		  PutBytes(state, "dac_key", DevelopmentCerts::kDacPrivateKey) &&
		  PutBytes(state, "pai_cert", DevelopmentCerts::kPaiCert) &&
		  PutUint(state, "passcode", passcode) &&
		  PutUint(state, "spake2_it", CONFIG_CHIP_DEVICE_SPAKE2_IT) &&
		  PutBytes(state, "spake2_salt", ByteSpan(salt)) &&
		  PutBytes(state, "spake2_verifier", verifierSpan) &&
		  PutUint(state, "discriminator", discriminator) &&
		  zcbor_map_end_encode(state, kMapEntries);

	Crypto::ClearSecretData(salt, sizeof(salt));
	passcode = 0;

	VerifyOrReturnError(ok, CHIP_ERROR_BUFFER_TOO_SMALL);
	outLen = state->payload - buf;
	return CHIP_NO_ERROR;
}

} /* namespace */

CHIP_ERROR SelfProvisionFactoryData()
{
	const struct flash_area *fa;
	int ret = flash_area_open(PARTITION_ID(FACTORY_DATA_PARTITION), &fa);

	VerifyOrReturnError(ret == 0, System::MapErrorZephyr(ret));

	/* The factory data partition is memory mapped. */
	uint8_t *existing = reinterpret_cast<uint8_t *>(PARTITION_ADDRESS(FACTORY_DATA_PARTITION));

	if (!PartitionIsEmpty(existing, 16)) {
		struct FactoryData parsed;

		if (ParseFactoryData(existing, fa->fa_size, &parsed) &&
		    parsed.version == CONFIG_CHIP_FACTORY_DATA_VERSION) {
			flash_area_close(fa);
			return CHIP_NO_ERROR;
		}
		LOG_WRN("Invalid factory data, replacing it");
	}

	LOG_INF("Creating this device's pairing credentials");

	uint8_t *buf = static_cast<uint8_t *>(Platform::MemoryAlloc(kFactoryDataMaxLength));
	size_t len = 0;
	CHIP_ERROR err = CHIP_ERROR_NO_MEMORY;

	if (buf) {
		memset(buf, 0xff, kFactoryDataMaxLength);
		err = Encode(buf, kFactoryDataMaxLength, len);
	}

	if (err == CHIP_NO_ERROR) {
		/* Writes must be a multiple of the flash write block size (4 bytes). */
		len = ROUND_UP(len, 4);
		ret = flash_area_erase(fa, 0, fa->fa_size);
		if (ret == 0) {
			ret = flash_area_write(fa, 0, buf, len);
		}
		err = System::MapErrorZephyr(ret);
	}

	if (buf) {
		Crypto::ClearSecretData(buf, kFactoryDataMaxLength);
		Platform::MemoryFree(buf);
	}
	flash_area_close(fa);

	if (err != CHIP_NO_ERROR) {
		LOG_ERR("Creating factory data failed: %" CHIP_ERROR_FORMAT, err.Format());
	}
	return err;
}
