//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

// A simulated flash device for the POSIX host, modelled on the one the win32
// virtual device carries.
//
// A host has no storage, but a debugger asks for the flash sector map before it
// will ask for anything else, and abandons the connection when that map comes
// back empty. Describing a plausible flash, backed by ordinary memory, is what
// lets the rest of the conversation happen - and it is also where deployed
// assemblies land.
//
// NANOCLR_FLASH_IMAGE names a file to keep that memory in. With it a deployment
// outlives the process, the way a board keeps one across a power cycle; without
// it the flash is blank at every start.
//
// Only built when the debugger stack is; without it the target keeps the stubs
// in Target_BlockStorage.cpp.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <nanoHAL_Types.h>
#include <nanoPAL_BlockStorage.h>

namespace
{
constexpr uint32_t c_BlockCount = 512;
constexpr uint32_t c_BytesPerBlock = 0x1000;
constexpr uint32_t c_BytesPerSector = 1;

constexpr uint8_t c_ErasedByte = 0xFF;

// the backing memory, allocated when the device is initialised
std::vector<uint8_t> *s_storage = nullptr;

// the file the memory is kept in, when one was asked for
FILE *s_image = nullptr;

const BlockRange c_BlockRanges[] = {
    {BlockRange_BLOCKTYPE_DEPLOYMENT, 0, 509},
    {BlockRange_BLOCKTYPE_CONFIG, 510, 511},
};

const BlockRegionInfo c_BlockRegions[] = {
    {
        BlockRegionAttribute(0),
        // start address
        0,
        c_BlockCount,
        c_BytesPerBlock,
        ARRAYSIZE_CONST_EXPR(c_BlockRanges),
        c_BlockRanges,
    },
};

const DeviceBlockInfo c_DeviceBlockInfo = {
    MediaAttribute(0),
    c_BytesPerSector,
    ARRAYSIZE_CONST_EXPR(c_BlockRegions),
    (BlockRegionInfo *)c_BlockRegions,
};

// Put a range of the image back into the file it came from.
//
// The whole flash is half a megabyte, so writing only what changed keeps a
// deployment - which arrives a kilobyte at a time - from rewriting all of it
// per packet. Flushed on every call: a soak run that is killed rather than shut
// down should still find its deployment there afterwards.
void WriteThrough(size_t offset, size_t numBytes)
{
    if (s_image == nullptr)
    {
        return;
    }

    if (std::fseek(s_image, (long)offset, SEEK_SET) != 0)
    {
        return;
    }

    std::fwrite(s_storage->data() + offset, 1, numBytes, s_image);
    std::fflush(s_image);
}

// Load the named image into the freshly allocated memory and keep it open.
//
// The file holds the whole flash, so its size is the geometry: one that does
// not match was written by a build with a different layout, and reading it
// would put blocks at the wrong addresses. Such a file is reported and
// overwritten rather than trusted.
void OpenImage(std::vector<uint8_t> &storage)
{
    const char *path = std::getenv("NANOCLR_FLASH_IMAGE");

    if (path == nullptr || *path == '\0')
    {
        return;
    }

    if (FILE *existing = std::fopen(path, "rb"))
    {
        std::fseek(existing, 0, SEEK_END);
        const long size = std::ftell(existing);
        std::rewind(existing);

        if (size != (long)storage.size())
        {
            std::fprintf(
                stderr,
                "flash image '%s' holds %ld bytes, this build has %zu; starting blank\n",
                path,
                size,
                storage.size());
        }
        else if (std::fread(storage.data(), 1, storage.size(), existing) != storage.size())
        {
            std::fprintf(stderr, "flash image '%s' could not be read; starting blank\n", path);
            std::fill(storage.begin(), storage.end(), c_ErasedByte);
        }

        std::fclose(existing);
    }

    // Recreate the file from what is now in memory. That gives it the full size
    // up front, so every later write lands at the offset it belongs to instead
    // of extending a short file.
    s_image = std::fopen(path, "w+b");

    if (s_image == nullptr)
    {
        std::fprintf(stderr, "cannot write flash image '%s'; the deployment will not be kept\n", path);
        return;
    }

    std::fwrite(storage.data(), 1, storage.size(), s_image);
    std::fflush(s_image);
}
} // namespace

MEMORY_MAPPED_NOR_BLOCK_CONFIG Device_BlockStorageConfig = {
    {
        // BLOCK_CONFIG
        {
            0,     // GPIO_PIN Pin
            false, // BOOL ActiveState
        },
        (DeviceBlockInfo *)&c_DeviceBlockInfo,
    },
    {
        0,                              // ChipSelect
        true,                           // ReadOnly
        0,                              // WaitStates
        0,                              // ReleaseCounts
        16,                             // BitWidth
        0x00000000,                     // BaseAddress
        c_BlockCount * c_BytesPerBlock, // SizeInBytes
        0,                              // XREADYEnable
        0,                              // ByteSignalsForRead
        0,                              // ExternalBufferEnable
    },
    0, // ChipProtection
    0, // ManufacturerCode
    0, // DeviceCode
};

BlockStorageDevice Device_BlockStorage;

bool SimulatedFlash_InitializeDevice(void *context)
{
    MEMORY_MAPPED_NOR_BLOCK_CONFIG *config = (MEMORY_MAPPED_NOR_BLOCK_CONFIG *)context;

    if (s_storage == nullptr)
    {
        s_storage = new std::vector<uint8_t>(config->Memory.SizeInBytes, c_ErasedByte);

        OpenImage(*s_storage);
    }

    return true;
}

bool SimulatedFlash_UninitializeDevice(void *context)
{
    (void)context;

    if (s_image != nullptr)
    {
        std::fclose(s_image);
        s_image = nullptr;
    }

    delete s_storage;
    s_storage = nullptr;

    return true;
}

DeviceBlockInfo *SimulatedFlash_GetDeviceInfo(void *context)
{
    MEMORY_MAPPED_NOR_BLOCK_CONFIG *config = (MEMORY_MAPPED_NOR_BLOCK_CONFIG *)context;

    return config->BlockConfig.BlockDeviceInformation;
}

bool SimulatedFlash_Read(void *context, ByteAddress startAddress, unsigned int numBytes, unsigned char *buffer)
{
    MEMORY_MAPPED_NOR_BLOCK_CONFIG *config = (MEMORY_MAPPED_NOR_BLOCK_CONFIG *)context;

    if (s_storage == nullptr)
    {
        return false;
    }

    const size_t offset = startAddress - config->Memory.BaseAddress;

    if (offset + numBytes > s_storage->size())
    {
        return false;
    }

    std::memcpy(buffer, s_storage->data() + offset, numBytes);

    return true;
}

bool SimulatedFlash_Write(
    void *context,
    ByteAddress startAddress,
    unsigned int numBytes,
    unsigned char *buffer,
    bool readModifyWrite)
{
    (void)readModifyWrite;

    MEMORY_MAPPED_NOR_BLOCK_CONFIG *config = (MEMORY_MAPPED_NOR_BLOCK_CONFIG *)context;

    if (s_storage == nullptr)
    {
        return false;
    }

    const size_t offset = startAddress - config->Memory.BaseAddress;

    if (offset + numBytes > s_storage->size())
    {
        return false;
    }

    std::memcpy(s_storage->data() + offset, buffer, numBytes);

    WriteThrough(offset, numBytes);

    return true;
}

bool SimulatedFlash_IsBlockErased(void *context, ByteAddress blockAddress, unsigned int length)
{
    MEMORY_MAPPED_NOR_BLOCK_CONFIG *config = (MEMORY_MAPPED_NOR_BLOCK_CONFIG *)context;

    if (s_storage == nullptr)
    {
        return false;
    }

    const size_t offset = blockAddress - config->Memory.BaseAddress;

    if (offset + length > s_storage->size())
    {
        return false;
    }

    // Answering honestly is what makes a second deployment replace the first
    // rather than land on top of it: the debugger only erases a block this
    // reports as dirty, and a tail left over from a larger deployment would be
    // read back as another assembly.
    const uint8_t *begin = s_storage->data() + offset;

    return std::all_of(begin, begin + length, [](uint8_t value) { return value == c_ErasedByte; });
}

bool SimulatedFlash_EraseBlock(void *context, ByteAddress address)
{
    MEMORY_MAPPED_NOR_BLOCK_CONFIG *config = (MEMORY_MAPPED_NOR_BLOCK_CONFIG *)context;

    if (s_storage == nullptr)
    {
        return false;
    }

    // a real part erases whole blocks, whatever address inside one it is given
    const size_t offset = ((address - config->Memory.BaseAddress) / c_BytesPerBlock) * c_BytesPerBlock;

    if (offset + c_BytesPerBlock > s_storage->size())
    {
        return false;
    }

    std::memset(s_storage->data() + offset, c_ErasedByte, c_BytesPerBlock);

    WriteThrough(offset, c_BytesPerBlock);

    return true;
}

IBlockStorageDevice SimulatedFlash_BlockStorageInterface = {
    &SimulatedFlash_InitializeDevice,
    &SimulatedFlash_UninitializeDevice,
    &SimulatedFlash_GetDeviceInfo,
    &SimulatedFlash_Read,
    &SimulatedFlash_Write,
    nullptr,
    &SimulatedFlash_IsBlockErased,
    &SimulatedFlash_EraseBlock,
    nullptr,
    nullptr,
};

void BlockStorage_AddDevices()
{
    BlockStorageList_AddDevice(
        (BlockStorageDevice *)&Device_BlockStorage,
        &SimulatedFlash_BlockStorageInterface,
        &Device_BlockStorageConfig,
        true);
}
