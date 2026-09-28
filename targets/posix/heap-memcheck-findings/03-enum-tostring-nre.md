# 3. `Enum.ToString()` throws `NullReferenceException`

## Summary

Calling `ToString()` on an enum value throws `NullReferenceException` on this CLR. The root cause has not been found.

## Where

`System.Enum.ToString()` in nanoFramework.CoreLibrary 1.17.11 (managed code, IL offset `0x12`):

```
IL_0000: ldarg.0
IL_0001: call      System.Object::GetType()
IL_0006: stloc.0
IL_0007: ldloc.0
IL_0008: ldstr     "value__"
IL_000d: callvirt  System.Type::GetField(string)
IL_0012: stloc.1
IL_0013: ldloc.1
IL_0014: ldarg.0
IL_0015: callvirt  System.Reflection.FieldInfo::GetValue(object)   <- FieldInfo is null
IL_001a: stloc.2
IL_001b: ldloc.2
IL_001c: callvirt  System.Object::ToString()
```

`GetType().GetField("value__")` returns null, so the following `GetValue` dereferences null. (This implementation formats the underlying number, not the name; the check in HeapStress accepts either.)

## How it was found

1. HeapStress, natively, no stress: the Array module failed with an unexpected `NullReferenceException`.
2. The CLR's exception dump pointed at `System.Enum::ToString [IP: 0012]` called from the module.
3. `monodis` on the CoreLibrary reference assembly gave the IL above.

Not investigated further: whether `GetType()` on a boxed enum returns the enum type or its underlying type on this CLR, and whether the metadata processor keeps the `value__` field of enum types. Either would explain it. The behaviour does not depend on GC stress and produced no memcheck report.

## Evidence

```
    ++++ Exception System.NullReferenceException - CLR_E_NULL_REFERENCE (1) ++++
    ++++ Message:
    ++++ System.Enum::ToString [IP: 0012] ++++
    ++++ HeapStress.ArrayModule::Run [IP: 0202] ++++
    ++++ HeapStress.Program::Main [IP: 00d8] ++++
```

HeapStress reports it as `HEAPSTRESS KNOWN Array round <n>: Enum.ToString() throws NullReferenceException` and prints `KNOWN ISSUE NOW PASSES` once it is fixed.

## Patch

None.
