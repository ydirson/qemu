/*
 * graphics passthrough
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "xen_pt.h"
#include "xen-host-pci-device.h"
#include "hw/xen/xen-legacy-backend.h"

#include "amd_renoir_vbios.h"

static unsigned long igd_guest_opregion;
static unsigned long igd_host_opregion;

#define XEN_PCI_INTEL_OPREGION_MASK 0xfff

typedef struct VGARegion {
    int type;           /* Memory or port I/O */
    uint64_t guest_base_addr;
    uint64_t machine_base_addr;
    uint64_t size;    /* size of the region */
    int rc;
} VGARegion;

#define IORESOURCE_IO           0x00000100
#define IORESOURCE_MEM          0x00000200

static struct VGARegion vga_args[] = {
    {
        .type = IORESOURCE_IO,
        .guest_base_addr = 0x3B0,
        .machine_base_addr = 0x3B0,
        .size = 0xC,
        .rc = -1,
    },
    {
        .type = IORESOURCE_IO,
        .guest_base_addr = 0x3C0,
        .machine_base_addr = 0x3C0,
        .size = 0x20,
        .rc = -1,
    },
    {
        .type = IORESOURCE_MEM,
        .guest_base_addr = 0xa0000 >> XC_PAGE_SHIFT,
        .machine_base_addr = 0xa0000 >> XC_PAGE_SHIFT,
        .size = 0x20,
        .rc = -1,
    },
};

/*
 * register VGA resources for the domain with assigned gfx
 */
int xen_pt_register_vga_regions(XenHostPCIDevice *dev)
{
    int i = 0;

    if (!is_igd_vga_passthrough(dev)) {
        return 0;
    }

    for (i = 0 ; i < ARRAY_SIZE(vga_args); i++) {
        if (vga_args[i].type == IORESOURCE_IO) {
            vga_args[i].rc = xc_domain_ioport_mapping(xen_xc, xen_domid,
                            vga_args[i].guest_base_addr,
                            vga_args[i].machine_base_addr,
                            vga_args[i].size, DPCI_ADD_MAPPING);
        } else {
            vga_args[i].rc = xc_domain_memory_mapping(xen_xc, xen_domid,
                            vga_args[i].guest_base_addr,
                            vga_args[i].machine_base_addr,
                            vga_args[i].size, DPCI_ADD_MAPPING);
        }

        if (vga_args[i].rc) {
            XEN_PT_ERR(NULL, "VGA %s mapping failed! (rc: %i)\n",
                    vga_args[i].type == IORESOURCE_IO ? "ioport" : "memory",
                    vga_args[i].rc);
            return vga_args[i].rc;
        }
    }

    return 0;
}

/*
 * unregister VGA resources for the domain with assigned gfx
 */
int xen_pt_unregister_vga_regions(XenHostPCIDevice *dev)
{
    int i = 0;
    int ret = 0;

    if (!is_igd_vga_passthrough(dev)) {
        return 0;
    }

    for (i = 0 ; i < ARRAY_SIZE(vga_args); i++) {
        if (vga_args[i].type == IORESOURCE_IO) {
            vga_args[i].rc = xc_domain_ioport_mapping(xen_xc, xen_domid,
                            vga_args[i].guest_base_addr,
                            vga_args[i].machine_base_addr,
                            vga_args[i].size, DPCI_REMOVE_MAPPING);
        } else {
            vga_args[i].rc = xc_domain_memory_mapping(xen_xc, xen_domid,
                            vga_args[i].guest_base_addr,
                            vga_args[i].machine_base_addr,
                            vga_args[i].size, DPCI_REMOVE_MAPPING);
        }

        if (vga_args[i].rc) {
            XEN_PT_ERR(NULL, "VGA %s unmapping failed! (rc: %i)\n",
                    vga_args[i].type == IORESOURCE_IO ? "ioport" : "memory",
                    vga_args[i].rc);
            return vga_args[i].rc;
        }
    }

    if (igd_guest_opregion) {
        ret = xc_domain_memory_mapping(xen_xc, xen_domid,
                (unsigned long)(igd_guest_opregion >> XC_PAGE_SHIFT),
                (unsigned long)(igd_host_opregion >> XC_PAGE_SHIFT),
                3,
                DPCI_REMOVE_MAPPING);
        if (ret) {
            return ret;
        }
    }

    return 0;
}

static void *get_sysfs_vgabios(XenPCIPassthroughState *s, int *size,
                       XenHostPCIDevice *dev)
{
    return pci_assign_dev_load_option_rom(&s->dev, size,
                                          dev->domain, dev->bus,
                                          dev->dev, dev->func);
}

static void xen_pt_direct_vbios_copy(XenPCIPassthroughState *s, Error **errp)
{
    int fd = -1;
    void *guest_bios = NULL;
    void *host_vbios = NULL;
    /* This is always 32 pages in the real mode reserved region */
    int bios_size = 32 << XC_PAGE_SHIFT;
    int vbios_addr = 0xc0000;

    fd = open("/dev/mem", O_RDONLY);
    if (fd == -1) {
        error_setg(errp, "Can't open /dev/mem: %s", strerror(errno));
        return;
    }
    host_vbios = mmap(NULL, bios_size,
            PROT_READ, MAP_ANONYMOUS | MAP_PRIVATE, fd, vbios_addr);
    close(fd);

    if (host_vbios == MAP_FAILED) {
        error_setg(errp, "Failed to mmap host vbios: %s", strerror(errno));
        return;
    }

    {
        unsigned char* romdata = host_vbios;
        if (romdata[0] != 0x55 || romdata[1] != 0xaa) {
            XEN_PT_ERR(&s->dev, "host vbios in /dev/mem has bad magic %02x %02x",
                       romdata[0], romdata[1]);
            error_setg(errp, "host vbios in /dev/mem has bad magic %02x %02x",
                       romdata[0], romdata[1]);
            return;
        }
    }

    memory_region_init_ram(&s->dev.rom, OBJECT(&s->dev),
            "legacy_vbios.rom", bios_size, &error_abort);
    guest_bios = memory_region_get_ram_ptr(&s->dev.rom);
    memcpy(guest_bios, host_vbios, bios_size);

    if (munmap(host_vbios, bios_size) == -1) {
        XEN_PT_LOG(&s->dev, "Failed to unmap host vbios: %s\n", strerror(errno));
    }

    cpu_physical_memory_rw(vbios_addr, guest_bios, bios_size, 1);
    memory_region_set_address(&s->dev.rom, vbios_addr);
    pci_register_bar(&s->dev, PCI_ROM_SLOT, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->dev.rom);
    s->dev.has_rom = true;
    XEN_PT_LOG(&s->dev, "Legacy VBIOS registered\n");
}

/* Refer to Seabios. */
struct rom_header {
    uint16_t signature;
    uint8_t size;
    uint8_t initVector[4];
    uint8_t reserved[17];
    uint16_t pcioffset;
    uint16_t pnpoffset;
} __attribute__((packed));

struct pci_data {
    uint32_t signature;
    uint16_t vendor;
    uint16_t device;
    uint16_t vitaldata;
    uint16_t dlen;
    uint8_t drevision;
    uint8_t class_lo;
    uint16_t class_hi;
    uint16_t ilen;
    uint16_t irevision;
    uint8_t type;
    uint8_t indicator;
    uint16_t reserved;
} __attribute__((packed));

void xen_pt_setup_vga(XenPCIPassthroughState *s, XenHostPCIDevice *dev,
                     Error **errp)
{
    unsigned char *bios = NULL;
    struct rom_header *rom;
    int bios_size;
    char *c = NULL;
    char checksum = 0;
    uint32_t len = 0;
    struct pci_data *pd = NULL;

    if (!is_igd_vga_passthrough(dev)) {
        XEN_PT_LOG(&s->dev, "VGA: igd-passthrough not enabled\n");
        error_setg(errp, "Need to enable igd-passthrough");
        return;
    }

    if (1) {
        void *guest_bios = NULL;
        /* This is always 32 pages in the real mode reserved region */
        int bios_size = 32 << XC_PAGE_SHIFT;
        int vbios_addr = 0xc0000;

        bios = &VBIOS_RENOIR;
        if (bios[0] != 0x55 || bios[1] != 0xaa) {
            XEN_PT_ERR(&s->dev, "vbios file has bad magic %02x %02x",
                       bios[0], bios[1]);
            return;
        }

        memory_region_init_ram(&s->dev.rom, OBJECT(&s->dev),
                               "legacy_vbios.rom", bios_size, &error_abort);
        guest_bios = memory_region_get_ram_ptr(&s->dev.rom);
        memset(guest_bios, 0, bios_size);
        memcpy(guest_bios, bios, sizeof(VBIOS_RENOIR));
        {
            unsigned char* romdata = guest_bios;
            if (romdata[0] != 0x55 || romdata[1] != 0xaa) {
                XEN_PT_ERR(&s->dev, "copied vbios data has bad magic %02x %02x",
                           romdata[0], romdata[1]);
                return;
            }
        }

        cpu_physical_memory_rw(vbios_addr, guest_bios, bios_size, 1);
        memory_region_set_address(&s->dev.rom, vbios_addr);
        pci_register_bar(&s->dev, PCI_ROM_SLOT, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->dev.rom);
        s->dev.has_rom = true;
        XEN_PT_LOG(&s->dev, "Legacy VBIOS imported\n");
        return;
    }
    bios = get_sysfs_vgabios(s, &bios_size, dev);
    if (!bios) {
        XEN_PT_LOG(&s->dev, "Unable to get host VBIOS from sysfs - "
                            "falling back to a direct copy of memory ranges\n");
        xen_pt_direct_vbios_copy(s, errp);
        return;
    }

    if (bios_size < sizeof(struct rom_header)) {
        XEN_PT_LOG(&s->dev, "VGA: VBIOS image corrupt (too small)\n");
        error_setg(errp, "VGA: VBIOS image corrupt (too small)");
        return;
    }

    /* Currently we fixed this address as a primary. */
    rom = (struct rom_header *)bios;

    if (rom->pcioffset + sizeof(struct pci_data) > bios_size) {
        XEN_PT_LOG(&s->dev, "VGA: VBIOS image corrupt (bad pcioffset field)\n");
        error_setg(errp, "VGA: VBIOS image corrupt (bad pcioffset field)");
        return;
    }

    pd = (void *)(bios + (unsigned char)rom->pcioffset);

    /* We may need to fixup Device Identification. */
    if (pd->device != s->real_device.device_id) {
        pd->device = s->real_device.device_id;

        len = rom->size * 512;
        if (len > bios_size) {
            XEN_PT_LOG(&s->dev, "VGA: VBIOS image corrupt (bad size field)\n");
            error_setg(errp, "VGA: VBIOS image corrupt (bad size field)");
            return;
        }

        /* Then adjust the bios checksum */
        for (c = (char *)bios; c < ((char *)bios + len); c++) {
            checksum += *c;
        }
        if (checksum) {
            bios[len - 1] -= checksum;
            XEN_PT_LOG(&s->dev, "vga bios checksum is adjusted %x!\n",
                       checksum);
        }
    }

    /* Currently we fixed this address as a primary for legacy BIOS. */
    cpu_physical_memory_rw(0xc0000, bios, bios_size, 1);
    XEN_PT_LOG(&s->dev, "Legacy VBIOS registered\n");
}

uint32_t igd_read_opregion(XenPCIPassthroughState *s)
{
    uint32_t val = 0;

    if (!igd_guest_opregion) {
        return val;
    }

    val = igd_guest_opregion;

    XEN_PT_LOG(&s->dev, "Read opregion val=%x\n", val);
    return val;
}

#define XEN_PCI_INTEL_OPREGION_PAGES 0x3
#define XEN_PCI_INTEL_OPREGION_ENABLE_ACCESSED 0x1
void igd_write_opregion(XenPCIPassthroughState *s, uint32_t val)
{
    int ret;

    if (igd_guest_opregion) {
        XEN_PT_LOG(&s->dev, "opregion register already been set, ignoring %x\n",
                   val);
        return;
    }

    /* We just work with LE. */
    xen_host_pci_get_block(&s->real_device, XEN_PCI_INTEL_OPREGION,
            (uint8_t *)&igd_host_opregion, 4);
    igd_guest_opregion = (unsigned long)(val & ~XEN_PCI_INTEL_OPREGION_MASK)
                            | (igd_host_opregion & XEN_PCI_INTEL_OPREGION_MASK);

    ret = xc_domain_memory_mapping(xen_xc, xen_domid,
            (unsigned long)(igd_guest_opregion >> XC_PAGE_SHIFT),
            (unsigned long)(igd_host_opregion >> XC_PAGE_SHIFT),
            XEN_PCI_INTEL_OPREGION_PAGES,
            DPCI_ADD_MAPPING);

    if (ret) {
        XEN_PT_ERR(&s->dev, "[%d]:Can't map IGD host opregion:0x%lx to"
                    " guest opregion:0x%lx.\n", ret,
                    (unsigned long)(igd_host_opregion >> XC_PAGE_SHIFT),
                    (unsigned long)(igd_guest_opregion >> XC_PAGE_SHIFT));
        igd_guest_opregion = 0;
        return;
    }

    XEN_PT_LOG(&s->dev, "Map OpRegion: 0x%lx -> 0x%lx\n",
                    (unsigned long)(igd_host_opregion >> XC_PAGE_SHIFT),
                    (unsigned long)(igd_guest_opregion >> XC_PAGE_SHIFT));
}
