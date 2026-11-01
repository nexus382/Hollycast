/* Modified for Hollycast on 2026-10-31; see valkyrie/PROVENANCE.md. */
/* 
 * Valkyrie
 * Copyright (C) 2011, 2012, Stefano Teso
 * 
 * Valkyrie is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Valkyrie is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Valkyrie.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "mach/hikaru/hikaru-aica.h"
#include "mach/hikaru/hikaru.h"
#include "cpu/sh/sh4.h"
#include "engine_internal.h"

/*
 * Sound Boards
 * ============
 *
 * Apparently SNDBD and SNDBD2 are identical, except that the latter is
 * optional.
 *
 * 700000-701FFF AICA Channels 0-63. Each slot is 128 bytes long.
 * 702000-7027FF AICA ESF (?)
 * 702800-7028BD AICA Global (?)
 * 702C00        ARM Reset
 * 702D00        AICA IRQ L
 * 702D04        AICA IRQ R
 * 703000-7031FF COEF
 * 703200-7033FF MADRS
 * 703400-703BFF MPRO; 3BFE --> AICA DSP start
 *
 * 710000 RTC Lo
 * 710004 RTC Hi
 * 710008 RTC Write Enable
 *
 * 800000-FFFFFF RAM
 *
 * Note: this documentation comes from the following sources: MAME,
 * lxdream, nullDC. Kudos to the original authors.
 */

typedef struct {
	vk_device_t base;
	vk_buffer_t *ram;
	vk_buffer_t *regs;
	uint32_t rtc[4];
	bool master;
} hikaru_aica_t;

static void
hikaru_aica_reset_cpu (hikaru_aica_t *aica)
{
	vk_device_t *dev = (vk_device_t *) aica;
	VK_MACH_LOG (dev->mach, "AICA/%c: resetting ARM cpu",
	             aica->master ? 'M' : 'S');
}

static int
hikaru_aica_get (vk_device_t *device, unsigned size, uint32_t addr, void *val)
{
	hikaru_aica_t *aica = (hikaru_aica_t *) device;
	uint32_t *val32 = (uint32_t *) val;
	uint32_t offs = addr & 0xFFFFFF;
	HikaruEngine *engine = device->mach->host;

	VK_MACH_LOG (device->mach, "AICA/%c R%u @%08X",
	             aica->master ? 'M' : 'S', 8*size, offs);

	if (engine && engine->sound_attached && offs >= 0x700000 && offs < 0x708000) {
		if (!is_size_valid(size) || size > 0x708000 - offs) return -1;
		uint64_t value = 0;
		if (engine->sound.read(engine->sound.opaque, aica->master ? 0 : 1,
		                       offs - 0x700000, size, &value)) {
			snprintf(engine->error, sizeof(engine->error), "Hikaru sound register read failed at %08x", addr);
			return -1;
		}
		return set_ptr(val, size, value);
	}
	switch (offs) {
	case 0x000000:
		/* XXX required for PHARRER; see PH:@0C0B2884 */
		set_ptr (val, size, 4);
		break;
	case 0x700000 ... 0x703BFF:
		set_ptr (val, size, vk_buffer_get (aica->regs, size, addr & 0x3FFF));
		break;
	case 0x710000:
	case 0x710004:
	case 0x710008:
		/* AICA RTC */
		VK_ASSERT (size == 4);
		*val32 = aica->rtc[(offs & 0xF) / 4];
		break;
	case 0x800000 ... 0xFFFFFF:
		/* RAM */
		/* The CPU-visible aperture has reached an actual sound RAM device. */
		sh4_charge_sound_ram_read(((hikaru_t *)device->mach)->sh_current,
		                          ((hikaru_t *)device->mach)->cpu_clock_mhz);
		set_ptr (val, size, vk_buffer_get (aica->ram, size, addr & 0x7FFFFF));
		if ((!engine || !engine->sound_attached) && offs == 0x80005C)
			/* XXX hack for AIRTRIX */
			set_ptr (val, size, 1);
		break;
	default:
		VK_MACH_ERROR (device->mach, "AICA unhandled R%u %08X", size*8, addr);
		return -1;
	}
	return 0;
}

static int
hikaru_aica_put (vk_device_t *device, unsigned size, uint32_t addr, uint64_t val)
{
	hikaru_aica_t *aica = (hikaru_aica_t *) device;
	uint32_t offs = addr & 0xFFFFFF;
	HikaruEngine *engine = device->mach->host;

	VK_MACH_LOG (device->mach, "AICA/%c W%u @%08X = %" PRIX64 "",
	             aica->master ? 'M' : 'S', 8*size, offs, val);

	if (engine && engine->sound_attached && offs >= 0x700000 && offs < 0x708000) {
		if (!is_size_valid(size) || size > 0x708000 - offs) return -1;
		if (engine->sound.write(engine->sound.opaque, aica->master ? 0 : 1,
		                        offs - 0x700000, size, val)) {
			snprintf(engine->error, sizeof(engine->error), "Hikaru sound register write failed at %08x", addr);
			return -1;
		}
		return 0;
	}
	switch (offs) {
	case 0x700000 ... 0x703BFF:
		vk_buffer_put (aica->regs, size, addr & 0x3FFF, val);
		if (offs == 0x702C00 && (val & 1))
			hikaru_aica_reset_cpu (aica);
		break;
	case 0x710000:
	case 0x710004:
	case 0x710008:
		/* AICA RTC */
		VK_ASSERT (size == 4);
		return -1;
	case 0x800000 ... 0xFFFFFF:
		/* RAM */
		vk_buffer_put (aica->ram, size, addr & 0x7FFFFF, val);
		break;
	default:
		VK_MACH_ERROR (device->mach, "AICA unhandled W%u %08X = %" PRIX64 "", size*8, addr, val);
		return -1;
	}
	return 0;
}

static void
hikaru_aica_reset (vk_device_t *dev, vk_reset_type_t type)
{
	hikaru_aica_t *aica = (hikaru_aica_t *) dev;
	aica->rtc[0] = 0x5BFC;
	aica->rtc[1] = 0x8900;
	aica->rtc[2] = 0;
}

vk_device_t *
hikaru_aica_new (vk_machine_t *mach, vk_buffer_t *ram, bool master)
{
	hikaru_aica_t *aica;
	vk_device_t *dev;

	VK_DEVICE_ALLOC (aica, mach);
	dev = (vk_device_t *) aica;
	if (!aica)
		return NULL;

	dev->destroy	= NULL;
	dev->reset	= hikaru_aica_reset;
	dev->exec	= NULL;
	dev->get	= hikaru_aica_get;
	dev->put	= hikaru_aica_put;
	dev->save_state	= NULL;
	dev->load_state	= NULL;

	aica->ram	= ram;
	aica->master	= master;

	aica->regs	= vk_buffer_le32_new (0x3C00, 0);
	if (!aica->regs)
		goto fail;

	vk_machine_register_buffer (mach, aica->regs);

	return dev;

fail:
	vk_device_destroy (&dev);
	return NULL;
}
