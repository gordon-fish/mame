// license:BSD-3-Clause
// copyright-holders:Ted Green
#include "emu.h"
#include "vrc4373.h"

#define LOG_NILE            (0)
#define LOG_NILE_MASTER     (0)
#define LOG_NILE_TARGET     (0)

const device_type VRC4373      = &device_creator<vrc4373_device>;

DEVICE_ADDRESS_MAP_START(config_map, 32, vrc4373_device)
	AM_RANGE(0x40, 0x43) AM_READWRITE  (pcictrl_r,  pcictrl_w)
	AM_INHERIT_FROM(pci_bridge_device::config_map)
ADDRESS_MAP_END

// cpu i/f map
DEVICE_ADDRESS_MAP_START(cpu_map, 32, vrc4373_device)
	AM_RANGE(0x00000000, 0x0000007b) AM_READWRITE(    cpu_if_r,          cpu_if_w)
ADDRESS_MAP_END

// Target Window 1 map
DEVICE_ADDRESS_MAP_START(target1_map, 32, vrc4373_device)
	AM_RANGE(0x00000000, 0xFFFFFFFF) AM_READWRITE(    target1_r,          target1_w)
ADDRESS_MAP_END

// Target Window 2 map
DEVICE_ADDRESS_MAP_START(target2_map, 32, vrc4373_device)
	AM_RANGE(0x00000000, 0xFFFFFFFF) AM_READWRITE(    target2_r,          target2_w)
ADDRESS_MAP_END

vrc4373_device::vrc4373_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock)
	: pci_host_device(mconfig, VRC4373, "NEC VRC4373 System Controller", tag, owner, clock, "vrc4373", __FILE__),
		m_cpu_space(nullptr), m_cpu(nullptr), cpu_tag(nullptr), m_irq_num(-1),
		m_mem_config("memory_space", ENDIANNESS_LITTLE, 32, 32),
		m_io_config("io_space", ENDIANNESS_LITTLE, 32, 32), m_pci1_laddr(0), m_pci2_laddr(0), m_pci_io_laddr(0), m_target1_laddr(0), m_target2_laddr(0),
		m_romRegion(*this, "rom")
{
}

const address_space_config *vrc4373_device::memory_space_config(address_spacenum spacenum) const
{
	return (spacenum == AS_PROGRAM) ? pci_bridge_device::memory_space_config(spacenum) : (spacenum == AS_DATA) ? &m_mem_config : (spacenum == AS_IO) ? &m_io_config : nullptr;
}

void vrc4373_device::device_start()
{
	pci_host_device::device_start();
	m_cpu = machine().device<mips3_device>(cpu_tag);
	m_cpu_space = &m_cpu->space(AS_PROGRAM);
	memory_space = &space(AS_DATA);
	io_space = &space(AS_IO);

	memset(m_cpu_regs, 0, sizeof(m_cpu_regs));

	memory_window_start = 0;
	memory_window_end   = 0xffffffff;
	memory_offset       = 0;
	io_window_start = 0;
	io_window_end   = 0xffffffff;
	io_offset       = 0x00000000;
	status = 0x0280;
	// Reserve 8M for ram
	m_ram.reserve(0x00800000 / 4);
	// Reserve 32M for simm[0]
	m_simm[0].reserve(0x02000000 / 4);

	// ROM
	uint32_t romSize = m_romRegion->bytes();
	m_cpu_space->install_rom(0x1fc00000, 0x1fc00000 + romSize - 1, m_romRegion->base());
	// Nile register mapppings
	m_cpu_space->install_device(0x0f000000, 0x0f0000ff, *static_cast<vrc4373_device *>(this), &vrc4373_device::cpu_map);
	// PCI Configuration also mapped at 0x0f000100
	m_cpu_space->install_device(0x0f000100, 0x0f0001ff, *static_cast<vrc4373_device *>(this), &vrc4373_device::config_map);

	// MIPS drc
	m_cpu->add_fastram(0x1fc00000, 0x1fcfffff, true, m_romRegion->base());

	// DMA timer
	m_dma_timer = machine().scheduler().timer_alloc(timer_expired_delegate(FUNC(vrc4373_device::dma_transfer), this));
	// Leave the timer disabled.
	m_dma_timer->adjust(attotime::never, 0, DMA_TIMER_PERIOD);
}

void vrc4373_device::device_reset()
{
	pci_device::device_reset();
	memset(m_cpu_regs, 0, sizeof(m_cpu_regs));
	regenerate_config_mapping();
	m_dma_timer->adjust(attotime::never);
}

void vrc4373_device::map_cpu_space()
{
	uint32_t winStart, winEnd, winSize;
	uint32_t regConfig;

	// VRC4373 is at 0x0f000000 to 0x0f0001ff
	// ROM region starts at 0x1f000000
	m_cpu_space->unmap_readwrite(0x00000000, 0x0effffff);
	m_cpu_space->unmap_readwrite(0x0f000200, 0x1effffff);

	// Clear fastram regions in cpu after rom
	m_cpu->clear_fastram(1);

	regConfig = m_cpu_regs[NREG_BMCR];
	if (regConfig & 0x8) {
		winSize = 1 << 22;  // 4MB
		for (int i = 14; i <= 15; i++) {
			if (!((regConfig >> i) & 0x1)) winSize <<= 1;
			else break;
		}
		winStart = (regConfig & 0x0fc00000);
		winEnd = winStart + winSize - 1;

		m_ram.resize(winSize / 4);
		m_cpu_space->install_ram(winStart, winEnd, m_ram.data());
		m_cpu->add_fastram(winStart, winEnd, false, m_ram.data());
		if (LOG_NILE)
			logerror("map_cpu_space ram_size=%08X ram_base=%08X\n", winSize, winStart);
	}

	// Map SIMMs
	for (int simIndex = 0; simIndex < 4; simIndex++) {
		regConfig = m_cpu_regs[NREG_SIMM1 + simIndex];
		if (regConfig & 0x8) {
			winSize = 1 << 21;  // 2MB
			for (int i = 13; i <= 17; i++) {
				if (!((regConfig >> i) & 0x1)) winSize <<= 1;
				else break;
			}
			winStart = (regConfig & 0x0fe00000);
			winEnd = winStart + winSize - 1;

			m_simm[simIndex].resize(winSize / 4);
			m_cpu_space->install_ram(winStart, winEnd, m_simm[simIndex].data());
			m_cpu->add_fastram(winStart, winEnd, false, m_simm[simIndex].data());
			if (LOG_NILE)
				logerror("map_cpu_space simm_size[%i]=%08X simm_base=%08X\n", simIndex, winSize, winStart);
		}
	}

	// PCI Master Window 1
	if (m_cpu_regs[NREG_PCIMW1]&0x1000) {
		winStart = m_cpu_regs[NREG_PCIMW1]&0xff000000;
		winEnd = winStart | (~(0x80000000 | (((m_cpu_regs[NREG_PCIMW1]>>13)&0x7f)<<24)));
		winSize = winEnd - winStart + 1;
		m_cpu_space->install_read_handler(winStart, winEnd, read32_delegate(FUNC(vrc4373_device::master1_r), this));
		m_cpu_space->install_write_handler(winStart, winEnd, write32_delegate(FUNC(vrc4373_device::master1_w), this));
		if (LOG_NILE)
			logerror("%s: map_cpu_space Master Window 1 start=%08X end=%08X size=%08X laddr=%08X\n", tag(), winStart, winEnd, winSize,  m_pci1_laddr);
	}
	// PCI Master Window 2
	if (m_cpu_regs[NREG_PCIMW2]&0x1000) {
		winStart = m_cpu_regs[NREG_PCIMW2]&0xff000000;
		winEnd = winStart | (~(0x80000000 | (((m_cpu_regs[NREG_PCIMW2]>>13)&0x7f)<<24)));
		winSize = winEnd - winStart + 1;
		m_cpu_space->install_read_handler(winStart, winEnd, read32_delegate(FUNC(vrc4373_device::master2_r), this));
		m_cpu_space->install_write_handler(winStart, winEnd, write32_delegate(FUNC(vrc4373_device::master2_w), this));
		if (LOG_NILE)
			logerror("%s: map_cpu_space Master Window 2 start=%08X end=%08X size=%08X laddr=%08X\n", tag(), winStart, winEnd, winSize,  m_pci2_laddr);
	}
	// PCI IO Window
	if (m_cpu_regs[NREG_PCIMIOW]&0x1000) {
		winStart = m_cpu_regs[NREG_PCIMIOW]&0xff000000;
		winEnd = winStart | (~(0x80000000 | (((m_cpu_regs[NREG_PCIMIOW]>>13)&0x7f)<<24)));
		winSize = winEnd - winStart + 1;
		m_cpu_space->install_read_handler(winStart, winEnd, read32_delegate(FUNC(vrc4373_device::master_io_r), this));
		m_cpu_space->install_write_handler(winStart, winEnd, write32_delegate(FUNC(vrc4373_device::master_io_w), this));
		if (LOG_NILE)
			logerror("%s: map_cpu_space IO Window start=%08X end=%08X size=%08X laddr=%08X\n", tag(), winStart, winEnd, winSize,  m_pci_io_laddr);
	}
}

void vrc4373_device::map_extra(uint64_t memory_window_start, uint64_t memory_window_end, uint64_t memory_offset, address_space *memory_space,
									uint64_t io_window_start, uint64_t io_window_end, uint64_t io_offset, address_space *io_space)
{
	uint32_t winStart, winEnd, winSize;

	// PCI Target Window 1
	if (m_cpu_regs[NREG_PCITW1]&0x1000) {
		winStart = m_cpu_regs[NREG_PCITW1]&0xffe00000;
		winEnd = winStart | (~(0xf0000000 | (((m_cpu_regs[NREG_PCITW1]>>13)&0x7f)<<21)));
		winSize = winEnd - winStart + 1;
		memory_space->install_read_handler(winStart, winEnd, read32_delegate(FUNC(vrc4373_device::target1_r), this));
		memory_space->install_write_handler(winStart, winEnd, write32_delegate(FUNC(vrc4373_device::target1_w), this));
		if (LOG_NILE)
			logerror("%s: map_extra Target Window 1 start=%08X end=%08X size=%08X laddr=%08X\n", tag(), winStart, winEnd, winSize,  m_target1_laddr);
	}
	// PCI Target Window 2
	if (m_cpu_regs[NREG_PCITW2]&0x1000) {
		winStart = m_cpu_regs[NREG_PCITW2]&0xffe00000;
		winEnd = winStart | (~(0xf0000000 | (((m_cpu_regs[NREG_PCITW2]>>13)&0x7f)<<21)));
		winSize = winEnd - winStart + 1;
		memory_space->install_read_handler(winStart, winEnd, read32_delegate(FUNC(vrc4373_device::target2_r), this));
		memory_space->install_write_handler(winStart, winEnd, write32_delegate(FUNC(vrc4373_device::target2_w), this));
		if (LOG_NILE)
			logerror("%s: map_extra Target Window 2 start=%08X end=%08X size=%08X laddr=%08X\n", tag(), winStart, winEnd, winSize,  m_target2_laddr);
	}
}

void vrc4373_device::reset_all_mappings()
{
	pci_device::reset_all_mappings();
}

void vrc4373_device::set_cpu_tag(const char *_cpu_tag)
{
	if (LOG_NILE)
		logerror("%s: set_cpu_tag\n", tag());
	cpu_tag = _cpu_tag;
}
// PCI bus control
READ32_MEMBER (vrc4373_device::pcictrl_r)
{
	uint32_t result = 0;
	if (LOG_NILE)
		logerror("%06X:nile pcictrl_r from offset %02X = %08X & %08X\n", space.device().safe_pc(), offset*4, result, mem_mask);
	return result;
}
WRITE32_MEMBER (vrc4373_device::pcictrl_w)
{
	if (LOG_NILE)
		logerror("%06X:nile pcictrl_w to offset %02X = %08X & %08X\n", space.device().safe_pc(), offset*4, data, mem_mask);
}
// PCI Master Window 1
READ32_MEMBER (vrc4373_device::master1_r)
{
	uint32_t result = this->space(AS_DATA).read_dword(m_pci1_laddr | (offset*4), mem_mask);
	if (LOG_NILE_MASTER)
		logerror("%06X:nile master1 read from offset %02X = %08X & %08X\n", space.device().safe_pc(), offset*4, result, mem_mask);
	return result;
}
WRITE32_MEMBER (vrc4373_device::master1_w)
{
	this->space(AS_DATA).write_dword(m_pci1_laddr | (offset*4), data, mem_mask);
	if (LOG_NILE_MASTER)
		logerror("%06X:nile master1 write to offset %02X = %08X & %08X\n", space.device().safe_pc(), offset*4, data, mem_mask);
}

// PCI Master Window 2
READ32_MEMBER (vrc4373_device::master2_r)
{
	uint32_t result = this->space(AS_DATA).read_dword(m_pci2_laddr | (offset*4), mem_mask);
	if (LOG_NILE_MASTER)
		logerror("%06X:nile master2 read from offset %02X = %08X & %08X\n", space.device().safe_pc(), offset*4, result, mem_mask);
	return result;
}
WRITE32_MEMBER (vrc4373_device::master2_w)
{
	this->space(AS_DATA).write_dword(m_pci2_laddr | (offset*4), data, mem_mask);
	if (LOG_NILE_MASTER)
		logerror("%06X:nile master2 write to offset %02X = %08X & %08X\n", space.device().safe_pc(), offset*4, data, mem_mask);
}

// PCI Master IO Window
READ32_MEMBER (vrc4373_device::master_io_r)
{
	uint32_t result = this->space(AS_IO).read_dword(m_pci_io_laddr | (offset*4), mem_mask);
	if (LOG_NILE_MASTER)
		logerror("%06X:nile master io read from offset %02X = %08X & %08X\n", space.device().safe_pc(), offset*4, result, mem_mask);
	return result;
}
WRITE32_MEMBER (vrc4373_device::master_io_w)
{
	this->space(AS_IO).write_dword(m_pci_io_laddr | (offset*4), data, mem_mask);
	if (LOG_NILE_MASTER)
		logerror("%06X:nile master io write to offset %02X = %08X & %08X\n", space.device().safe_pc(), offset*4, data, mem_mask);
}

// PCI Target Window 1
READ32_MEMBER (vrc4373_device::target1_r)
{
	uint32_t result = m_cpu->space(AS_PROGRAM).read_dword(m_target1_laddr | (offset*4), mem_mask);
	if (LOG_NILE_TARGET)
		logerror("%08X:nile target1 read from offset %02X = %08X & %08X\n", m_cpu->device_t::safe_pc(), offset*4, result, mem_mask);
	return result;
}
WRITE32_MEMBER (vrc4373_device::target1_w)
{
	m_cpu->space(AS_PROGRAM).write_dword(m_target1_laddr | (offset*4), data, mem_mask);
	if (LOG_NILE_TARGET)
		logerror("%08X:nile target1 write to offset %02X = %08X & %08X\n", m_cpu->device_t::safe_pc(), offset*4, data, mem_mask);
}

// PCI Target Window 2
READ32_MEMBER (vrc4373_device::target2_r)
{
	uint32_t result = m_cpu->space(AS_PROGRAM).read_dword(m_target2_laddr | (offset*4), mem_mask);
	if (LOG_NILE_TARGET)
		logerror("%08X:nile target2 read from offset %02X = %08X & %08X\n", m_cpu->device_t::safe_pc(), offset*4, result, mem_mask);
	return result;
}
WRITE32_MEMBER (vrc4373_device::target2_w)
{
	m_cpu->space(AS_PROGRAM).write_dword(m_target2_laddr | (offset*4), data, mem_mask);
	if (LOG_NILE_TARGET)
		logerror("%08X:nile target2 write to offset %02X = %08X & %08X\n", m_cpu->device_t::safe_pc(), offset*4, data, mem_mask);
}

// DMA Transfer
TIMER_CALLBACK_MEMBER (vrc4373_device::dma_transfer)
{
	int which = param;

	// Check for dma suspension
	if (m_cpu_regs[NREG_DMACR1 + which * 0xc] & DMA_SUS) {
		if (LOG_NILE)
			logerror("%08X:nile DMA Suspended PCI: %08X MEM: %08X Words: %X\n", m_cpu->space(AS_PROGRAM).device().safe_pc(), m_cpu_regs[NREG_DMA_CPAR], m_cpu_regs[NREG_DMA_CMAR], m_cpu_regs[NREG_DMA_REM]);
		return;
	}

	int pciSel = (m_cpu_regs[NREG_DMACR1+which*0xC] & DMA_MIO) ? AS_DATA : AS_IO;
	address_space *src, *dst;
	uint32_t srcAddr, dstAddr;

	if (m_cpu_regs[NREG_DMACR1+which*0xC]&DMA_RW) {
		// Read data from PCI and write to cpu
		src = &this->space(pciSel);
		dst = &m_cpu->space(AS_PROGRAM);
		srcAddr = m_cpu_regs[NREG_DMA_CPAR];
		dstAddr = m_cpu_regs[NREG_DMA_CMAR];
	} else {
		// Read data from cpu and write to PCI
		src = &m_cpu->space(AS_PROGRAM);
		dst = &this->space(pciSel);
		srcAddr = m_cpu_regs[NREG_DMA_CMAR];
		dstAddr = m_cpu_regs[NREG_DMA_CPAR];
	}
	int dataCount = m_cpu_regs[NREG_DMA_REM];
	int burstCount = DMA_BURST_SIZE;
	while (dataCount>0 && burstCount>0) {
		dst->write_dword(dstAddr, src->read_dword(srcAddr));
		dstAddr += 0x4;
		srcAddr += 0x4;
		--dataCount;
		--burstCount;
	}
	if (m_cpu_regs[NREG_DMACR1+which*0xC]&DMA_RW) {
		m_cpu_regs[NREG_DMA_CPAR] = srcAddr;
		m_cpu_regs[NREG_DMA_CMAR] = dstAddr;
	} else {
		m_cpu_regs[NREG_DMA_CMAR] = srcAddr;
		m_cpu_regs[NREG_DMA_CPAR] = dstAddr;
	}
	m_cpu_regs[NREG_DMA_REM] = dataCount;
	// Check for end of DMA
	if (dataCount == 0) {
		// Clear the busy and go flags
		m_cpu_regs[NREG_DMACR1 + which * 0xc] &= ~DMA_BUSY;
		m_cpu_regs[NREG_DMACR1 + which * 0xc] &= ~DMA_GO;
		// Set the interrupt
		if (m_cpu_regs[NREG_DMACR1 + which * 0xc] & DMA_INT_EN) {
			if (m_irq_num != -1) {
				m_cpu->set_input_line(m_irq_num, ASSERT_LINE);
			} else {
				logerror("vrc4373_device::dma_transfer Error: DMA configured to trigger interrupt but no interrupt line configured\n");
			}
		}
		// Turn off the timer
		m_dma_timer->adjust(attotime::never);
	}
}

// CPU I/F
READ32_MEMBER (vrc4373_device::cpu_if_r)
{
	uint32_t result = m_cpu_regs[offset];
	switch (offset) {
		case NREG_PCICAR:
			result = config_address_r(space, offset);
			break;
		case NREG_PCICDR:
			result = config_data_r(space, offset);
			break;
		default:
			break;
	}
	if (LOG_NILE)
		logerror("%06X:nile read from offset %02X = %08X & %08X\n", space.device().safe_pc(), offset*4, result, mem_mask);
	return result;
}

WRITE32_MEMBER(vrc4373_device::cpu_if_w)
{
	if (LOG_NILE)
		logerror("%06X:nile write to offset %02X = %08X & %08X\n", space.device().safe_pc(), offset*4, data, mem_mask);

	uint32_t modData, oldData;
	oldData = m_cpu_regs[offset];
	COMBINE_DATA(&m_cpu_regs[offset]);
	switch (offset) {
		case NREG_PCIMW1:
				m_pci1_laddr = (data&0xff)<<24;
				map_cpu_space();
			break;
		case NREG_PCIMW2:
				m_pci2_laddr = (data&0xff)<<24;
				map_cpu_space();
			break;
		case NREG_PCIMIOW:
				m_pci_io_laddr = (data&0xff)<<24;
				map_cpu_space();
			break;
		case NREG_PCITW1:
				m_target1_laddr = 0x00000000 | ((data&0x7FF)<<21);
				remap_cb();
			break;
		case NREG_PCITW2:
				m_target2_laddr = 0x00000000 | ((data&0x7FF)<<21);
				remap_cb();
			break;
		case NREG_PCICAR:
			// Bits in reserved area are used for device selection of type 0 config transactions
			// Assuming 23:11 get mapped into device number for configuration
			if ((data&0x3) == 0x0) {
				// Type 0 transaction
				modData = 0;
				// Select the device based on one hot bit
				for (int i=11; i<24; i++) {
					if ((data>>i)&0x1) {
						// One hot encoding, bit 11 will mean device 1
						modData = i-10;
						break;
					}
				}
				// Re-organize into Type 1 transaction for bus 0 (local bus)
				modData = (modData<<11) | (data&0x7ff) | (0x80000000);
			} else {
				// Type 1 transaction, no modification needed
				modData = data;
			}
			pci_host_device::config_address_w(space, offset, modData);
			break;
		case NREG_PCICDR:
			pci_host_device::config_data_w(space, offset, data);
			break;
		case NREG_DMACR1:
		case NREG_DMACR2:
			// Start when DMA_GO bit is set
			if (!(oldData & DMA_GO) && (data & DMA_GO)) {
				int which = (offset - NREG_DMACR1) >> 3;
				// Set counts and address
				m_cpu_regs[NREG_DMA_CPAR] = m_cpu_regs[NREG_DMAPCI1 + which * 0xC];
				m_cpu_regs[NREG_DMA_CMAR] = m_cpu_regs[NREG_DMAMAR1 + which * 0xC];
				// Set number of words remaining
				m_cpu_regs[NREG_DMA_REM] = (data & DMA_BLK_SIZE) >> 2;
				// Set busy flag
				m_cpu_regs[NREG_DMACR1 + which * 0xc] |= DMA_BUSY;
				// Start the transfer
				m_dma_timer->set_param(which);
				m_dma_timer->adjust(attotime::zero, 0, DMA_TIMER_PERIOD);
				if (LOG_NILE)
					logerror("%08X:nile Start DMA Lane %i PCI: %08X MEM: %08X Words: %X\n", m_cpu->space(AS_PROGRAM).device().safe_pc(), which, m_cpu_regs[NREG_DMA_CPAR], m_cpu_regs[NREG_DMA_CMAR], m_cpu_regs[NREG_DMA_REM]);
			}
			break;
		case NREG_BMCR:
		case NREG_SIMM1:
		case NREG_SIMM2:
		case NREG_SIMM3:
		case NREG_SIMM4:
			map_cpu_space();
			break;
		default:
			break;
	}

}
