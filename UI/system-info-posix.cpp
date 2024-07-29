#include "system-info.hpp"
#include "util/dstr.hpp"
#include "util/platform.h"
#include <sys/utsname.h>
#include <unistd.h>
#include <string>
#include <fstream>

using namespace std;

extern "C" {
#include "pci/pci.h"
}

struct drm_card_info {
	uint16_t vendor_id;
	uint16_t device_id;
	uint16_t subsystem_vendor;
	uint16_t subsystem_device;
	/* The following 2 fields are easily
	 * obtained for AMD GPUs. Reporting
	 * for NVIDIA GPUs is not working at
	 * the moment.
	 */
	uint64_t dedicated_vram_total;
	uint64_t shared_system_memory_total;
	bool boot_vga;
	// PCI domain:bus:slot:function
	std::string dbsf;
};

const std::string WHITE_SPACE = " \f\n\r\t\v";

// trim_ws() will remove leading and trailing
// white space from the string.
static void trim_ws(std::string &s)
{
	// Trim leading whitespace
	size_t pos = s.find_first_not_of(WHITE_SPACE);
	if (pos != string::npos)
		s = s.substr(pos);

	// Trim trailing whitespace
	pos = s.find_last_not_of(WHITE_SPACE);
	if (pos != string::npos)
		s = s.substr(0, pos + 1);
}

static bool get_distribution_info(string &distro, string &version)
{
	ifstream file("/etc/os-release");
	string line;

	if (file.is_open()) {
		while (getline(file, line)) {
			if (line.compare(0, 4, "NAME") == 0) {
				size_t pos = line.find('=');
				if (pos != string::npos && line.at(pos + 1) != '\0') {
					distro = line.substr(pos + 1);

					// Remove the '"' characters from the string, if any.
					distro.erase(std::remove(distro.begin(), distro.end(), '"'), distro.end());

					trim_ws(distro);
					continue;
				}
			}

			if (line.compare(0, 10, "VERSION_ID") == 0) {
				size_t pos = line.find('=');
				if (pos != string::npos && line.at(pos + 1) != '\0') {
					version = line.substr(pos + 1);

					// Remove the '"' characters from the string, if any.
					version.erase(std::remove(version.begin(), version.end(), '"'), version.end());

					trim_ws(version);
					continue;
				}
			}
		}
		file.close();
	} else {
		blog(LOG_INFO, "Distribution: Missing /etc/os-release !");
		return false;
	}

	return true;
}

static bool get_cpu_name(optional<string> &proc_name)
{
	ifstream file("/proc/cpuinfo");
	string line;
	int physical_id = -1;
	bool found_name = false;

	/* Initialize processor name. Some ARM-based hosts do
	 * not output the "model name" field in /proc/cpuinfo.
	 */
	proc_name = "Unavailable";

	if (file.is_open()) {
		while (getline(file, line)) {
			if (line.compare(0, 10, "model name") == 0) {
				size_t pos = line.find(':');
				if (pos != string::npos && line.at(pos + 1) != '\0') {
					proc_name = line.substr(pos + 1);
					trim_ws((string &)proc_name);
					found_name = true;
					continue;
				}
			}

			if (line.compare(0, 11, "physical id") == 0) {
				size_t pos = line.find(':');
				if (pos != string::npos && line.at(pos + 1) != '\0') {
					physical_id = atoi(&line[pos + 1]);
					if (physical_id == 0 && found_name)
						break;
				}
			}
		}
		file.close();
	}
	return true;
}

static bool get_cpu_freq(uint32_t *cpu_freq)
{
	if (!cpu_freq)
		return false;

	ifstream freq_file;
	string line;

	/* Look for the sysfs tree "base_frequency" first.
	 * Intel exports "base_frequency, AMD does not.
	 * If not found, use "cpuinfo_max_freq".
	 */
	freq_file.open("/sys/devices/system/cpu/cpu0/cpufreq/base_frequency");
	if (!freq_file.is_open()) {
		freq_file.open("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq");
		if (!freq_file.is_open())
			return false;
	}

	if (getline(freq_file, line)) {
		trim_ws(line);
		// Convert the CPU frequency string to an integer in MHz
		*cpu_freq = atoi(line.c_str()) / 1000;
	} else {
		*cpu_freq = 0;
	}
	freq_file.close();

	return true;
}

/* get_drm_card_info() will update the card_idx pointer argument
 * if an entry was skipped due to discovering a software framebuffer
 * entry instead of hardware.
 */
static bool get_drm_card_info(uint8_t *card_idx, struct drm_card_info *dci)
{
	string drm_path;
	size_t base_length = 0;
	char *file_str = NULL;
	uint val;

	if (!dci)
		return false;

	drm_path = "/sys/class/drm/card";
	base_length = drm_path.length();

	// Walk the entries looking for the next non-framebuffer GPU
	char buf[256];
	int len = 0;
	bool found_card = false;
	while (!found_card) {
		drm_path += std::to_string(*card_idx) + "/device";

		/* Get the PCI D:B:S:F (domain:bus:slot:function) in string form
		 * by reading the device symlink.
		 */
		if ((len = readlink(drm_path.c_str(), buf, sizeof(buf) - 1)) != -1) {
			// readlink() doesn't null terminate strings, so do it explicitly
			buf[len] = '\0';
			// If the entry is a simple framebuffer, skip it
			if (strstr(buf, "framebuffer")) {
				(*card_idx)++;
				drm_path.resize(base_length);
				continue;
			} else {
				dci->dbsf = buf;
				/* The DBSF string is of the form: "../../../0000:65:00.0/".
				* Remove all the '/' characters, and
				* remove all the leading '.' characters.
				*/
				dci->dbsf.erase(std::remove(dci->dbsf.begin(), dci->dbsf.end(), '/'), dci->dbsf.end());
				dci->dbsf.erase(0, dci->dbsf.find_first_not_of("."));
				found_card = true;
			}
		} else {
			return false;
		}
	}
	base_length = drm_path.length();

	if (os_file_exists(drm_path.c_str())) {
		// Get the device_id
		drm_path += "/device";
		file_str = os_quick_read_utf8_file(drm_path.c_str());
		if (!file_str) {
			blog(LOG_ERROR, "Could not read from '%s'", drm_path.c_str());
			return false;
		}
		// Skip over the "0x" and convert
		sscanf(file_str + 2, "%x", &val);
		dci->device_id = val;
		bfree(file_str);

		// Get the vendor_id
		drm_path.resize(base_length);
		drm_path += "/vendor";
		file_str = os_quick_read_utf8_file(drm_path.c_str());
		if (!file_str) {
			blog(LOG_ERROR, "Could not read from '%s'", drm_path.c_str());
			return false;
		}
		// Skip over the "0x" and convert
		sscanf(file_str + 2, "%x", &val);
		dci->vendor_id = val;
		bfree(file_str);

		// Get the subsystem_vendor
		drm_path.resize(base_length);
		drm_path += "/subsystem_vendor";
		file_str = os_quick_read_utf8_file(drm_path.c_str());
		if (!file_str) {
			dci->subsystem_vendor = 0;
		} else {
			// Skip over the "0x" and convert
			sscanf(file_str + 2, "%x", &val);
			dci->subsystem_vendor = val;
			bfree(file_str);
		}

		// Get the subsystem_device
		drm_path.resize(base_length);
		drm_path += "/subsystem_device";
		file_str = os_quick_read_utf8_file(drm_path.c_str());
		if (!file_str) {
			dci->subsystem_device = 0;
		} else {
			// Skip over the "0x" and convert
			sscanf(file_str + 2, "%x", &val);
			dci->subsystem_device = val;
			bfree(file_str);
		}

		// Get the boot_vga flag
		dci->boot_vga = false;
		drm_path.resize(base_length);
		drm_path += "/boot_vga";
		file_str = os_quick_read_utf8_file(drm_path.c_str());
		// 'boot_vga' is not present for some adapters (iGPU)
		if (file_str) {
			sscanf(file_str, "%d", &val);
			if (val == 1)
				dci->boot_vga = true;
			bfree(file_str);
		}

		/* The amdgpu driver exports the GPU memory information
		 * via sysfs nodes. Sadly NVIDIA doesn't have the same
		 * information via sysfs. Read the amdgpu-based nodes
		 * if present and get the required fields.
		 */

		// Get the GPU total dedicated VRAM, if available
		drm_path.resize(base_length);
		drm_path += "/mem_info_vram_total";
		file_str = os_quick_read_utf8_file(drm_path.c_str());
		if (!file_str) {
			dci->dedicated_vram_total = 0;
		} else {
			sscanf(file_str, "%lu", &dci->dedicated_vram_total);
			bfree(file_str);
		}

		// Get the GPU total shared system memory, if available
		drm_path.resize(base_length);
		drm_path += "/mem_info_gtt_total";
		file_str = os_quick_read_utf8_file(drm_path.c_str());
		if (!file_str) {
			dci->shared_system_memory_total = 0;
		} else {
			sscanf(file_str, "%lu", &dci->shared_system_memory_total);
			bfree(file_str);
		}

		blog(LOG_DEBUG,
		     "%s: drm_adapter_info: PCI B:D:S:F: %s, device_id:0x%x,"
		     "vendor_id:0x%x, sub_device:0x%x, sub_vendor:0x%x,"
		     "boot_flag:%d, vram_total: %lu, sys_memory: %lu",
		     __FUNCTION__, dci->dbsf.c_str(), dci->device_id, dci->vendor_id, dci->subsystem_device,
		     dci->subsystem_vendor, dci->boot_vga, dci->dedicated_vram_total, dci->shared_system_memory_total);
	} else {
		return false;
	}

	return true;
}

static void adjust_gpu_model(std::string *model)
{
	/* Use the sub-string between the [] brackets. For example,
	 * the NVIDIA Quadro P4000 model string from PCI ID database
	 * is "GP104GL [Quadro P4000]", and we only want the "Quadro
	 * P4000" sub-string.
	 */
	size_t first = 0;
	size_t last = 0;
	first = model->find('[');
	last = model->find_last_of(']');
	if ((last - first) > 1) {
		std::string adjusted_model = model->substr(first + 1, last - first - 1);
		model->assign(adjusted_model);
	}
}

static std::optional<std::vector<GoLiveApi::Gpu>> system_gpu_data()
{
	std::vector<GoLiveApi::Gpu> adapter_info;

	struct pci_access *pacc;
	pacc = pci_alloc();
	pci_init(pacc);
	char slot[256];

	// Scan the PCI bus once
	pci_scan_bus(pacc);

	// Walk the DRM (Direct Rendering Management) nodes
	struct drm_card_info card = {0};
	uint8_t i = 0;
	while (get_drm_card_info(&i, &card)) {
		GoLiveApi::Gpu data;
		struct pci_dev *pdev;
		struct pci_filter pfilter;
		char namebuf[1024], *name;

		data.device_id = card.device_id;
		data.vendor_id = card.vendor_id;

		// Get around the const char* vs char*
		// type issue with pci_filter_parse_slot()
		strncpy(slot, card.dbsf.c_str(), sizeof(slot));

		pci_filter_init(pacc, &pfilter);
		if (pci_filter_parse_slot(&pfilter, slot)) {
			blog(LOG_DEBUG, "%s: pci_filter_parse_slot() failed", __FUNCTION__);
			continue;
		}

		for (pdev = pacc->devices; pdev; pdev = pdev->next) {
			if (pci_filter_match(&pfilter, pdev)) {
				pci_fill_info(pdev, PCI_FILL_IDENT);
				name = pci_lookup_name(pacc, namebuf, sizeof(namebuf),
						       PCI_LOOKUP_DEVICE | PCI_LOOKUP_NETWORK, pdev->vendor_id,
						       pdev->device_id);
				data.model = name;
				adjust_gpu_model(&data.model);
				break;
			}
		}

		/* By default OBS should be assigned the GPU with boot_vga=1.
		 * If this is not the case, the logic below will mixup the
		 * values when multiple GPU devices are installed. We need
		 * a better way of detecting at runtime exactly which GPU is
		 * being used by OBS, preferably the PCIe D:B:S:F (domain:bus:
		 * slot:function) information. Obtaining PCIe vendor_id/device_id
		 * from the graphics API would help, but would not solve the case
		 * of 2 or more GPUs of the exact same model.
		 */
		if (card.boot_vga) {
			// Obtain the GPU memory by querying via graphics API
			obs_enter_graphics();
			data.driver_version = gs_get_driver_version();
			data.dedicated_video_memory = gs_get_gpu_dmem() * 1024;
			data.shared_system_memory = gs_get_gpu_smem() * 1024;
			obs_leave_graphics();
			/* Insert the boot_vga adapter at the beginning. This
			 * will align it with composition_gpu_index=0, which
			 * is set to the ovi.adapter value (which is 0).
			 */
			adapter_info.insert(adapter_info.begin(), data);
		} else {
			/* The driver version for the non-boot-vga device
			 * is not accessible easily in a common location.
			 * It is not necessary at the moment so set to unknown.
			 */
			data.driver_version = "Unknown";

			/* Use the GPU memory info discovered with get_drm_card_info()
			 * for the non-boot adapter. amdgpu driver exposes the GPU memory 
			 * info via sysfs, NVIDIA does not.
			 */
			data.dedicated_video_memory = card.dedicated_vram_total;
			data.shared_system_memory = card.shared_system_memory_total;
			adapter_info.push_back(data);
		}
		++i;
	}
	pci_cleanup(pacc);

	return adapter_info;
}

void system_info(GoLiveApi::Capabilities &capabilities)
{
	// Determine the GPU capabilities
	capabilities.gpu = system_gpu_data();

	// Determine the CPU capabilities
	{
		auto &cpu_data = capabilities.cpu;
		cpu_data.physical_cores = os_get_physical_cores();
		cpu_data.logical_cores = os_get_logical_cores();

		if (!get_cpu_name(cpu_data.name)) {
			cpu_data.name = "Unknown";
		}

		uint32_t cpu_freq;
		if (get_cpu_freq(&cpu_freq)) {
			cpu_data.speed = cpu_freq;
		} else {
			cpu_data.speed = 0;
		}
	}

	// Determine the memory capabilities
	{
		auto &memory_data = capabilities.memory;
		memory_data.total = os_get_sys_total_size();
		memory_data.free = os_get_sys_free_size();
	}

	// Reporting of gaming features not supported on Linux
	UNUSED_PARAMETER(capabilities.gaming_features);

	// Determine the system capabilities
	{
		auto &system_data = capabilities.system;

		if (!get_distribution_info(system_data.name, system_data.release)) {
			system_data.name = "Linux-based distribution";
			system_data.release = "unknown";
		}

		struct utsname utsinfo;
		static const uint16_t max_sys_data_version_sz = 128;
		if (uname(&utsinfo) == 0) {
			/* To determine if the host is 64-bit, check if the machine
			 * name contains "64", as in "x86_64" or "aarch64".
			 */
			system_data.bits = strstr(utsinfo.machine, "64") ? 64 : 32;

			/* To determine if the host CPU is ARM based, check if the
			 * machine name contains "aarch".
			 */
			system_data.arm = strstr(utsinfo.machine, "aarch") ? true : false;

			/* Send the sysname (usually "Linux"), kernel version and
			 * release reported by utsname as the version string.
			 * The code below will produce something like:
			 *
			 * "Linux 6.5.0-41-generic #41~22.04.2-Ubuntu SMP PREEMPT_DYNAMIC
			 *  Mon Jun  3 11:32:55 UTC 2"
			 */
			system_data.version = utsinfo.sysname;
			system_data.version.append(" ");
			system_data.version.append(utsinfo.release);
			system_data.version.append(" ");
			system_data.version.append(utsinfo.version);
			// Ensure system_data.version string is within the maximum size
			if (system_data.version.size() > max_sys_data_version_sz) {
				system_data.version.resize(max_sys_data_version_sz);
			}
		} else {
			UNUSED_PARAMETER(system_data.bits);
			UNUSED_PARAMETER(system_data.arm);
			system_data.version = "unknown";
		}

		// On Linux-based distros, there's no build or revision info
		UNUSED_PARAMETER(system_data.build);
		UNUSED_PARAMETER(system_data.revision);

		system_data.armEmulation = os_get_emulation_status();
	}
}
