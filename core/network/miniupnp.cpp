/*
	Copyright 2020 flyinghead
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
#include "build.h"
#ifndef FEAT_NO_MINIUPNPC
#include "miniupnp.h"
#include <upnpcommands.h>
#include <algorithm>
#include "log/Log.h"

#ifndef UPNP_LOCAL_PORT_ANY
#define UPNP_LOCAL_PORT_ANY 0
#endif

bool MiniUPnP::Init()
{
	DEBUG_LOG(NETWORK, "MiniUPnP::Init");
	int error = 0;
#if MINIUPNPC_API_VERSION >= 14
	UPNPDev *devlist = upnpDiscover(2000, nullptr, nullptr, UPNP_LOCAL_PORT_ANY, 0, 2, &error);
#else
	UPNPDev *devlist = upnpDiscover(2000, nullptr, nullptr, UPNP_LOCAL_PORT_ANY, 0, &error);
#endif
	if (devlist == nullptr)
	{
		WARN_LOG(NETWORK, "UPnP discover failed: error %d", error);
		return false;
	}
#if MINIUPNPC_API_VERSION >= 18
	error = UPNP_GetValidIGD(devlist, &urls, &data, lanAddress, sizeof(lanAddress), nullptr, 0);
#else
	error = UPNP_GetValidIGD(devlist, &urls, &data, lanAddress, sizeof(lanAddress));
#endif
	freeUPNPDevlist(devlist);
	if (error != 1)
	{
		WARN_LOG(NETWORK, "Internet Gateway not found: error %d", error);
		FreeUPNPUrls(&urls);
		return false;
	}
	initialized = true;
	return true;
}

std::string MiniUPnP::GetExternalIPv4Address()
{
	// Use an independent gateway handle so discovery cannot affect active mappings.
	MiniUPnP gateway;
	if (!gateway.Init())
		return {};
	std::string address = gateway.externalIPv4Address();
	gateway.Term();
	return address;
}

std::string MiniUPnP::externalIPv4Address() const
{
	char address[16]{}; // MiniUPnPc requires room for an IPv4 address and terminator.
	const int result = UPNP_GetExternalIPAddress(urls.controlURL, data.first.servicetype, address);
	return result == UPNPCOMMAND_SUCCESS ? std::string(address) : std::string();
}

void MiniUPnP::Term()
{
	std::lock_guard<std::mutex> _(mutex);
	if (!initialized)
		return;
	DEBUG_LOG(NETWORK, "MiniUPnP::Term");
	for (const auto& port : mappedPorts)
		UPNP_DeletePortMapping(urls.controlURL, data.first.servicetype, port.first.c_str(),
							   port.second ? "TCP" : "UDP", nullptr);
	mappedPorts.clear();
	FreeUPNPUrls(&urls);
	initialized = false;
	DEBUG_LOG(NETWORK, "MiniUPnP: terminated");
}

bool MiniUPnP::AddPortMapping(int port, bool tcp, bool preserveExisting)
{
	std::string portStr(std::to_string(port));
	const char *protocol = tcp ? "TCP" : "UDP";
	std::lock_guard<std::mutex> _(mutex);
	if (preserveExisting)
	{
		char client[16]{}, internalPort[6]{}, enabled[4]{};
		const int result = UPNP_GetSpecificPortMappingEntry(urls.controlURL, data.first.servicetype,
				portStr.c_str(), protocol, nullptr, client, internalPort, nullptr, enabled, nullptr);
		if (result == UPNPCOMMAND_SUCCESS)
		{
			const bool usable = std::strcmp(client, lanAddress) == 0 && portStr == internalPort
					&& (enabled[0] == '1' || enabled[0] == 't');
			if (!usable)
				WARN_LOG(NETWORK, "UPnP returned an incompatible existing mapping for %s port %d", protocol, port);
			return usable;
		}
		// An existing rule may have been set up manually, even on this device.
		if (result != 714) // NoSuchEntryInArray
		{
			WARN_LOG(NETWORK, "UPnP could not check %s port %d: error %d", protocol, port, result);
			return false;
		}
	}
	int error = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype,
								portStr.c_str(), // WAN port
								portStr.c_str(), // LAN port
								lanAddress,
								"Flycast",
								protocol,
								nullptr,   // remote (peer) host address or nullptr for no restriction
								"86400");  // port map lease duration (in seconds) or zero for "as long as possible"
	if (error != 0)
	{
		WARN_LOG(NETWORK, "Port %d redirection failed: error %d", port, error);
		return false;
	}
	mappedPorts.emplace_back(portStr, tcp);
	DEBUG_LOG(NETWORK, "MiniUPnP: forwarding %s port %d", tcp ? "TCP" : "UDP", port);
	return true;
}

void MiniUPnP::RemovePortMapping(int port, bool tcp)
{
	std::lock_guard<std::mutex> _(mutex);
	const auto mapping = std::find(mappedPorts.begin(), mappedPorts.end(), std::make_pair(std::to_string(port), tcp));
	if (mapping != mappedPorts.end())
	{
		const int result = UPNP_DeletePortMapping(urls.controlURL, data.first.servicetype,
				mapping->first.c_str(), tcp ? "TCP" : "UDP", nullptr);
		if (result == UPNPCOMMAND_SUCCESS || result == 714)
			mappedPorts.erase(mapping);
	}
}
#endif
