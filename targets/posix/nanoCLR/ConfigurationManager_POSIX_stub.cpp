//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

// Configuration Manager stubs — the POSIX host has no OEM identity or serial
// number of its own, so these report fixed placeholder values.

#include <cstddef>

#include <nanoCLR_Runtime.h>
#include <nanoHAL_ConfigurationManager.h>

extern "C" void ConfigurationManager_GetOemModelSku(char *model, size_t modelSkuSize)
{
    if (model && modelSkuSize > 0)
        hal_strncpy_s(model, modelSkuSize, "POSIX", modelSkuSize - 1);
}

extern "C" void ConfigurationManager_GetModuleSerialNumber(char *serialNumber, size_t serialNumberSize)
{
    if (serialNumber && serialNumberSize > 0)
        hal_strncpy_s(serialNumber, serialNumberSize, "0000000000000000", serialNumberSize - 1);
}

extern "C" void ConfigurationManager_GetSystemSerialNumber(char *serialNumber, size_t serialNumberSize)
{
    if (serialNumber && serialNumberSize > 0)
        hal_strncpy_s(serialNumber, serialNumberSize, "0000000000000000", serialNumberSize - 1);
}
