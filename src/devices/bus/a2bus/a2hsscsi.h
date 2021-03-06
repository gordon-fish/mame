// license:BSD-3-Clause
// copyright-holders:R. Belmont
/*********************************************************************

    a2hsscsi.h

    Implementation of the Apple II High Speed SCSI Card

*********************************************************************/

#ifndef __A2BUS_HSSCSI__
#define __A2BUS_HSSCSI__

#include "a2bus.h"
#include "machine/ncr5380n.h"

//**************************************************************************
//  TYPE DEFINITIONS
//**************************************************************************

class a2bus_hsscsi_device:
	public device_t,
	public device_a2bus_card_interface
{
public:
	// construction/destruction
	a2bus_hsscsi_device(const machine_config &mconfig, device_type type, const char *name, const char *tag, device_t *owner, uint32_t clock, const char *shortname, const char *source);
	a2bus_hsscsi_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock);

	// optional information overrides
	virtual machine_config_constructor device_mconfig_additions() const override;
	virtual const tiny_rom_entry *device_rom_region() const override;

	required_device<ncr5380n_device> m_ncr5380;
	required_device<nscsi_bus_device> m_scsibus;

	DECLARE_WRITE_LINE_MEMBER( drq_w );

protected:
	virtual void device_start() override;
	virtual void device_reset() override;

	// overrides of standard a2bus slot functions
	virtual uint8_t read_c0nx(address_space &space, uint8_t offset) override;
	virtual void write_c0nx(address_space &space, uint8_t offset, uint8_t data) override;
	virtual uint8_t read_cnxx(address_space &space, uint8_t offset) override;
	virtual void write_cnxx(address_space &space, uint8_t offset, uint8_t data) override;
	virtual uint8_t read_c800(address_space &space, uint16_t offset) override;
	virtual void write_c800(address_space &space, uint16_t offset, uint8_t data) override;

private:
	uint8_t *m_rom;
	uint8_t m_ram[8192];  // 8 banks of 1024 bytes
	int m_rambank, m_rombank;
	uint8_t m_drq;
	uint8_t m_bank;
	bool m_816block;
	uint8_t m_c0ne, m_c0nf;
};

// device type definition
extern const device_type A2BUS_HSSCSI;

#endif /* __A2BUS_HSSCSI__ */
