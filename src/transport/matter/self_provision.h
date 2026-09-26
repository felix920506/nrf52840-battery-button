/*
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <lib/core/CHIPError.h>

/*
 * Create this device's Matter factory data (unique pairing code) if the
 * factory data partition is empty. Must run before the factory data
 * provider is initialized.
 */
CHIP_ERROR SelfProvisionFactoryData();
