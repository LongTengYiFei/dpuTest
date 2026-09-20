#include <stdint.h>
#include <stdio.h>

#include <doca_dev.h>
#include <doca_error.h>
#include <doca_sta.h>

int
main(void)
{
	struct doca_devinfo **devices = NULL;
	uint32_t count = 0;
	doca_error_t result;
	uint32_t i;
	int supported = 0;

	result = doca_devinfo_create_list(&devices, &count);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Cannot enumerate DOCA devices: %s\n",
			doca_error_get_descr(result));
		return 2;
	}

	for (i = 0; i < count; ++i) {
		char pci_addr[DOCA_DEVINFO_PCI_ADDR_SIZE] = "unknown";
		doca_error_t addr_result;

		addr_result = doca_devinfo_get_pci_addr_str(devices[i], pci_addr);
		if (addr_result != DOCA_SUCCESS)
			snprintf(pci_addr, sizeof(pci_addr), "device-%u", i);

		result = doca_sta_cap_is_supported(devices[i]);
		printf("%s: STA %s (%s)\n", pci_addr,
		       result == DOCA_SUCCESS ? "SUPPORTED" : "NOT SUPPORTED",
		       doca_error_get_descr(result));
		if (result == DOCA_SUCCESS)
			supported = 1;
	}

	result = doca_devinfo_destroy_list(devices);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Cannot destroy DOCA device list: %s\n",
			doca_error_get_descr(result));
		return 2;
	}

	return supported ? 0 : 1;
}
