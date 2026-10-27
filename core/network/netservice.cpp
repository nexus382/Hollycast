/*
	Copyright 2025 flyinghead
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
#include "netservice.h"
#include "picoppp.h"
#include "dcnet.h"
#include "emulator.h"
#include "cfg/option.h"
#include "rawmodem.h"

namespace net::modbba
{

static Service *service;
// Special service installed by CapComDirect, which manages its lifecycle and starting/stopping
static Service *customService;

bool start()
{
	if (customService == nullptr)
	{
		delete service;
		if (settings.content.gameId == "HDR0010")	// Sega Rally 2 (JP)
			service = new RawModemService();
		else if (config::UseDCNet)
			service = new DCNetService();
		else
			service = new PicoTcpService();
	}
	return service->start();
}

void stop()
{
	if (service != nullptr)
		service->stop();
}


void setCustomService(Service *service)
{
	if (service == nullptr && net::modbba::service == customService)
		net::modbba::service = nullptr;
	else if (service != nullptr)
		net::modbba::service = service;
	customService = service;
}

void writeModem(u8 b) {
	if (service == nullptr)
		start();
	verify(service != nullptr);
	service->writeModem(b);
}
int readModem() {
	if (service == nullptr)
		start();
	verify(service != nullptr);
	return service->readModem();
}
int modemAvailable() {
	if (service == nullptr)
		start();
	verify(service != nullptr);
	return service->modemAvailable();
}

void receiveEthFrame(const u8 *frame, u32 size)
{
	if (service == nullptr || !settings.network.online)
		start();
	if (service == nullptr)
		return;
	service->receiveEthFrame(frame, size);
}
u32 sendEthFrame(u8 *frame)
{
	if (service == nullptr || !settings.network.online)
		start();
	if (service == nullptr)
		return 0;
	return service->sendEthFrame(frame);
}

}
