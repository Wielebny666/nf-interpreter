//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

// Target capability queries, C linkage.
//
// targetHAL.h and targetHAL_Power.h already declare these as C++ inline
// functions for the CLR, but the Wire Protocol and PAL are C and cannot see
// them. This file provides the out-of-line C counterparts. Keep the answers in
// sync with the inline versions.
//
// Deliberately avoids including targetHAL.h: pulling the C++ inlines into a C
// translation unit would not compile.

#include <stdbool.h>

// A POSIX host is a plain process. There is no bootloader, no in-field update
// and no configuration block that would need erasing.
bool Target_HasNanoBooter(void)
{
    return false;
}

bool Target_HasProprietaryBooter(void)
{
    return false;
}

bool Target_IFUCapable(void)
{
    return false;
}

bool Target_ConfigUpdateRequiresErase(void)
{
    return true;
}

// The CLR can be restarted in place; the process itself is not rebooted.
bool CPU_IsSoftRebootSupported(void)
{
    return true;
}
