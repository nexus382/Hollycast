/*
	Copyright 2022 flyinghead
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
#pragma once
#include <string>
#include <memory>
#include "hw/hwreg.h"

namespace card_reader {

// Standalone transport; does not attach to the Naomi/SH-4 serial singleton.
std::unique_ptr<SerialPort::Pipe> createSanwaReader();

void initdInit();
void derbyInit();
void clubkInit();
void wccfInit();
void term();

void barcodeInit();
void barcodeTerm();
bool barcodeAvailable();
std::string barcodeGetCard();
void barcodeSetCard(const std::string& card);

void insertCard(int playerNum);
void ejectCard();
bool readerAvailable();

}
