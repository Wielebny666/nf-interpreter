//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

// What System.Net.NetworkInformation sees of the network on a POSIX host.
//
// A device owns its interface: it runs DHCP, holds the address and can be told
// to change it. A host does not - the operating system owns the interface, so
// this side reports what the host has and refuses to change it. Interface 0 is
// the first host interface that is up and has an IPv4 address (loopback only if
// nothing else qualifies); NANOCLR_NETIF names another one.
//
// The configuration blocks the managed API reads and writes live in memory for
// the life of the process. There is no flash to keep them in, and a program that
// stores a setting has no host state to restore on the next run anyway.

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>

#if defined(__linux__)
#include <netpacket/packet.h>
#else
#include <net/if_dl.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <nanoCLR_Runtime.h>
#include <nanoHAL_ConfigurationManager.h>

#if !defined(NANOCLR_POSIX_SOCKETS)
#error "NetworkConfiguration_POSIX.cpp needs NANOCLR_POSIX_SOCKETS so that nanoPAL.h pulls in the socket declarations."
#endif

namespace
{
constexpr size_t c_maxNetworkInterfaces = 1;

// ─── Configuration blocks ───────────────────────────────────────────────────

HAL_Configuration_NetworkInterface s_networkConfig[c_maxNetworkInterfaces];

// The counted structs end in a flexible array, so they cannot be plain statics.
template <typename T> T *AllocateCounted(size_t count)
{
    T *block = static_cast<T *>(calloc(1, sizeof(T) + count * sizeof(void *)));

    block->Count = (uint8_t)count;
    return block;
}

struct ConfigurationInit
{
    ConfigurationInit()
    {
        g_TargetConfiguration.NetworkInterfaceConfigs =
            AllocateCounted<HAL_CONFIGURATION_NETWORK>(c_maxNetworkInterfaces);

        for (size_t i = 0; i < c_maxNetworkInterfaces; i++)
        {
            // a host interface is always addressed by the host's own DHCP or static setup
            s_networkConfig[i].InterfaceType = NetworkInterfaceType_Ethernet;
            s_networkConfig[i].StartupAddressMode = AddressMode_DHCP;
            s_networkConfig[i].AutomaticDNS = 1;

            g_TargetConfiguration.NetworkInterfaceConfigs->Configs[i] = &s_networkConfig[i];
        }

        // No wireless radio and no certificate store on a host. Counts of zero
        // are how the managed API is told so.
        g_TargetConfiguration.Wireless80211Configs = AllocateCounted<HAL_CONFIGURATION_NETWORK_WIRELESS80211>(0);
        g_TargetConfiguration.WirelessAPConfigs = AllocateCounted<HAL_CONFIGURATION_NETWORK_WIRELESSAP>(0);
        g_TargetConfiguration.CertificateStore = AllocateCounted<HAL_CONFIGURATION_X509_CERTIFICATE>(0);
        g_TargetConfiguration.DeviceCertificates = AllocateCounted<HAL_CONFIGURATION_X509_DEVICE_CERTIFICATE>(0);
    }
};

// Runs when the library is loaded, so the tables are there before any native
// method reads g_TargetConfiguration.
ConfigurationInit s_configurationInit;

// Everything after the marker: that is what a caller can change, and what tells
// two blocks apart.
constexpr size_t c_networkPayloadOffset = offsetof(HAL_Configuration_NetworkInterface, MacAddress);
constexpr size_t c_networkPayloadSize = sizeof(HAL_Configuration_NetworkInterface) - c_networkPayloadOffset;

// ─── The host adapter ───────────────────────────────────────────────────────

struct HostAdapter
{
    bool found = false;
    bool up = false;
    uint8_t mac[6] = {};
    uint32_t address = 0;
    uint32_t netmask = 0;
    uint32_t gateway = 0;
};

bool IsUsable(const ifaddrs *ifa)
{
    return ifa->ifa_addr != nullptr && ifa->ifa_addr->sa_family == AF_INET && (ifa->ifa_flags & IFF_UP) != 0;
}

std::string ChooseInterface(const ifaddrs *list)
{
    const char *wanted = getenv("NANOCLR_NETIF");

    if (wanted != nullptr && wanted[0] != '\0')
    {
        return wanted;
    }

    std::string loopback;

    for (const ifaddrs *ifa = list; ifa != nullptr; ifa = ifa->ifa_next)
    {
        if (!IsUsable(ifa))
        {
            continue;
        }

        if ((ifa->ifa_flags & IFF_LOOPBACK) != 0)
        {
            if (loopback.empty())
            {
                loopback = ifa->ifa_name;
            }
            continue;
        }

        if ((ifa->ifa_flags & IFF_RUNNING) != 0)
        {
            return ifa->ifa_name;
        }
    }

    return loopback;
}

// The default gateway of an interface. Linux keeps the routing table in /proc;
// there is no equivalent that does not mean talking to the routing socket on
// macOS, so it reports none there.
uint32_t FindGateway(const std::string &name)
{
#if defined(__linux__)
    FILE *routes = fopen("/proc/net/route", "r");

    if (routes == nullptr)
    {
        return 0;
    }

    char line[256];
    uint32_t gateway = 0;

    // header line first
    if (fgets(line, sizeof(line), routes) != nullptr)
    {
        while (fgets(line, sizeof(line), routes) != nullptr)
        {
            char iface[64];
            unsigned int destination;
            unsigned int via;
            unsigned int flags;

            if (sscanf(line, "%63s %x %x %x", iface, &destination, &via, &flags) == 4 && destination == 0 &&
                (flags & 0x2) != 0 && name == iface)
            {
                // the kernel prints the address as it holds it, which is the
                // network-order value s_addr carries on the host that wrote it
                gateway = via;
                break;
            }
        }
    }

    fclose(routes);
    return gateway;
#else
    (void)name;
    return 0;
#endif
}

HostAdapter QueryAdapter()
{
    HostAdapter adapter;
    ifaddrs *list = nullptr;

    if (getifaddrs(&list) != 0)
    {
        return adapter;
    }

    const std::string name = ChooseInterface(list);

    for (const ifaddrs *ifa = list; ifa != nullptr && !name.empty(); ifa = ifa->ifa_next)
    {
        if (name != ifa->ifa_name || ifa->ifa_addr == nullptr)
        {
            continue;
        }

        if (ifa->ifa_addr->sa_family == AF_INET)
        {
            adapter.found = true;
            adapter.up = (ifa->ifa_flags & IFF_UP) != 0 && (ifa->ifa_flags & IFF_RUNNING) != 0;
            adapter.address = reinterpret_cast<const sockaddr_in *>(ifa->ifa_addr)->sin_addr.s_addr;

            if (ifa->ifa_netmask != nullptr)
            {
                adapter.netmask = reinterpret_cast<const sockaddr_in *>(ifa->ifa_netmask)->sin_addr.s_addr;
            }
        }
#if defined(__linux__)
        else if (ifa->ifa_addr->sa_family == AF_PACKET)
        {
            const sockaddr_ll *link = reinterpret_cast<const sockaddr_ll *>(ifa->ifa_addr);

            if (link->sll_halen == sizeof(adapter.mac))
            {
                memcpy(adapter.mac, link->sll_addr, sizeof(adapter.mac));
            }
        }
#else
        else if (ifa->ifa_addr->sa_family == AF_LINK)
        {
            const sockaddr_dl *link = reinterpret_cast<const sockaddr_dl *>(ifa->ifa_addr);

            if (link->sdl_alen == sizeof(adapter.mac))
            {
                memcpy(adapter.mac, LLADDR(link), sizeof(adapter.mac));
            }
        }
#endif
    }

    freeifaddrs(list);

    if (adapter.found)
    {
        adapter.gateway = FindGateway(name);
    }

    return adapter;
}

// Up to two IPv4 name servers from resolv.conf.
void ReadNameServers(uint32_t &first, uint32_t &second)
{
    first = 0;
    second = 0;

    FILE *conf = fopen("/etc/resolv.conf", "r");

    if (conf == nullptr)
    {
        return;
    }

    char line[256];

    while (fgets(line, sizeof(line), conf) != nullptr && second == 0)
    {
        char address[64];
        in_addr parsed;

        if (sscanf(line, "nameserver %63s", address) == 1 && inet_pton(AF_INET, address, &parsed) == 1)
        {
            (first == 0 ? first : second) = parsed.s_addr;
        }
    }

    fclose(conf);
}
} // namespace

// ─── Target configuration ───────────────────────────────────────────────────

HAL_TARGET_CONFIGURATION g_TargetConfiguration;

extern "C" bool ConfigurationManager_GetConfigurationBlock(
    void *configurationBlock,
    DeviceConfigurationOption configuration,
    uint32_t configurationIndex)
{
    if (configuration == DeviceConfigurationOption_Network && configurationIndex < c_maxNetworkInterfaces)
    {
        memcpy(configurationBlock, &s_networkConfig[configurationIndex], sizeof(HAL_Configuration_NetworkInterface));
        return true;
    }

    // Wireless and certificate blocks do not exist on a host, so there is
    // nothing to hand out.
    return false;
}

extern "C" UpdateConfigurationResult ConfigurationManager_UpdateConfigurationBlock(
    void *configurationBlock,
    DeviceConfigurationOption configuration,
    uint32_t configurationIndex)
{
    if (configuration != DeviceConfigurationOption_Network || configurationIndex >= c_maxNetworkInterfaces)
    {
        return UpdateConfigurationResult_Failed;
    }

    HAL_Configuration_NetworkInterface &stored = s_networkConfig[configurationIndex];
    const HAL_Configuration_NetworkInterface *incoming =
        static_cast<const HAL_Configuration_NetworkInterface *>(configurationBlock);

    if (memcmp(
            reinterpret_cast<const uint8_t *>(&stored) + c_networkPayloadOffset,
            reinterpret_cast<const uint8_t *>(incoming) + c_networkPayloadOffset,
            c_networkPayloadSize) == 0)
    {
        return UpdateConfigurationResult_NoChanges;
    }

    memcpy(
        reinterpret_cast<uint8_t *>(&stored) + c_networkPayloadOffset,
        reinterpret_cast<const uint8_t *>(incoming) + c_networkPayloadOffset,
        c_networkPayloadSize);

    return UpdateConfigurationResult_Success;
}

extern "C" bool ConfigurationManager_StoreConfigurationBlock(
    void *configurationBlock,
    DeviceConfigurationOption configuration,
    uint32_t configurationIndex,
    uint32_t blockSize,
    uint32_t offset,
    bool done)
{
    (void)blockSize;
    (void)offset;
    (void)done;

    // A whole network block arrives in one piece; anything else has no home here.
    return ConfigurationManager_UpdateConfigurationBlock(configurationBlock, configuration, configurationIndex) !=
           UpdateConfigurationResult_Failed;
}

// ─── Adapter ────────────────────────────────────────────────────────────────

HRESULT SOCK_CONFIGURATION_LoadAdapterConfiguration(HAL_Configuration_NetworkInterface *config, uint32_t interfaceIndex)
{
    if (interfaceIndex >= c_maxNetworkInterfaces)
    {
        return CLR_E_INVALID_PARAMETER;
    }

    const HostAdapter adapter = QueryAdapter();

    // What the host reports is what is true, whatever the stored block says.
    memcpy(config->MacAddress, adapter.mac, sizeof(config->MacAddress));
    config->IPv4Address = adapter.address;
    config->IPv4NetMask = adapter.netmask;
    config->IPv4GatewayAddress = adapter.gateway;

    // the config struct is packed, so the name servers cannot be filled by reference
    uint32_t firstDns;
    uint32_t secondDns;

    ReadNameServers(firstDns, secondDns);
    config->IPv4DNSAddress1 = firstDns;
    config->IPv4DNSAddress2 = secondDns;

    return S_OK;
}

HRESULT SOCK_CONFIGURATION_LoadConfiguration(HAL_Configuration_NetworkInterface *config, uint32_t interfaceIndex)
{
    return SOCK_CONFIGURATION_LoadAdapterConfiguration(config, interfaceIndex);
}

HRESULT SOCK_CONFIGURATION_UpdateAdapterConfiguration(
    HAL_Configuration_NetworkInterface *config,
    uint32_t interfaceIndex,
    uint32_t updateFlags)
{
    if (interfaceIndex >= c_maxNetworkInterfaces)
    {
        return CLR_E_INVALID_PARAMETER;
    }

    // Asking for DHCP changes nothing the host does not already do on its own.
    // Anything else - an address, a name server, a MAC, a renewal - would be a
    // change to the host's network that this process has no business making.
    const bool onlyDhcp = (updateFlags & ~(uint32_t)NetworkInterface_UpdateOperation_Dhcp) == 0;

    if (onlyDhcp && config->StartupAddressMode == AddressMode_DHCP)
    {
        return S_OK;
    }

    return CLR_E_NOT_SUPPORTED;
}

HRESULT SOCK_CONFIGURATION_LinkStatus(uint32_t interfaceIndex, bool *status)
{
    if (interfaceIndex >= c_maxNetworkInterfaces)
    {
        return CLR_E_INVALID_PARAMETER;
    }

    const HostAdapter adapter = QueryAdapter();

    *status = adapter.found && adapter.up;
    return S_OK;
}

HRESULT SOCK_IPV4AddressFromString(const char *ipString, uint64_t *address)
{
    in_addr parsed;

    if (ipString == nullptr || inet_pton(AF_INET, ipString, &parsed) != 1)
    {
        return CLR_E_INVALID_PARAMETER;
    }

    *address = parsed.s_addr;
    return S_OK;
}

const char *SOCK_IPV4AddressToString(uint32_t address)
{
    // The managed side copies the text out before it calls again.
    static thread_local char text[INET_ADDRSTRLEN];
    in_addr in;

    in.s_addr = address;
    inet_ntop(AF_INET, &in, text, sizeof(text));

    return text;
}
