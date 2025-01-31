#define _CRT_SECURE_NO_WARNINGS
#include "ctrl.h"

CNVMe::CNVMe(std::string&& v)
	:find(true)
{
	std::smatch sm;

	if (std::regex_match(v, sm, std::regex(R"(([a-f0-9]+):([a-f0-9]+):([a-f0-9]+).([a-f0-9]+))"))) {
		bdf.bus = std::stoul(sm[2].str(), nullptr, 16);
		bdf.dev = std::stoul(sm[3].str(), nullptr, 16);
		bdf.func = std::stoul(sm[4].str(), nullptr, 16);
	}
	else if (std::regex_match(v, sm, std::regex(R"(([a-f0-9]+):([a-f0-9]+).([a-f0-9]+))"))) {
		bdf.bus = std::stoul(sm[1].str(), nullptr, 16);
		bdf.dev = std::stoul(sm[2].str(), nullptr, 16);
		bdf.func = std::stoul(sm[3].str(), nullptr, 16);
		std::cout << bdf.bus << std::endl;
		std::cout << bdf.dev << std::endl;
		std::cout << bdf.func << std::endl;


	}
	else {
		std::cerr << "No such a device" << std::endl;
		find = false;
	}
}


CNVMe::~CNVMe() {
	releaseEvent();
	deleteAdminSQ();
	deleteAdminCQ();
	unmapNVMeReg();
	unmapPciReg();
	CloseHandle(ghSyncCommand);
	CloseHandle(ghThreadStart);
	CloseHandle(h);
}

bool CNVMe::init() {
	bool ret = false;

	do {
		if (find == false) {
			std::cerr << "Device not found." << std::endl;
			break;
		}

		if (initDevice() == false) break;
		if (mapPciReg() == false) break;
		if (mapNVMeReg() == false) break;

		initAdminQ(64, 64);
		if (createAdminCQ() == false) break;
		if (createAdminSQ() == false)break;

		if (initCtrl() == false) break;
		if (acquireEvent() == false)break;

		allocateUserEvent();

		if (initThread() == false) break;
		ret = true;
	
	} while (0);

	return ret;
}

bool CNVMe::initDevice() {
	const char* dev_ = "\\\\.\\winpci";
	DWORD dwBytes;
	bool ret = false;

	for (int i = 0; i < 26; ++i) {
		char dev[MAX_PATH] = { 0 };
		sprintf(dev, "%s%d", dev_, i);

		HANDLE handle = CreateFile(
			dev,
			GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE,
			NULL, // No security attributes
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL,
			NULL
		);

		if (handle == INVALID_HANDLE_VALUE) {
			printf("INVALID_HANDLE_VALUE\n");

			continue;
		}

		BDF bdf_temp;

		bool bRet = DeviceIoControl(handle, IOCTL_WINPCI_GET_BDF, nullptr, 0, &bdf_temp, sizeof(BDF), &dwBytes, nullptr);

		if (bRet) {
			if (bdf.bus == bdf_temp.bus && bdf.dev == bdf_temp.dev && bdf.func == bdf_temp.func) {
				ret = true;
				h = handle;
				dev_no = i;
				printf("Success IOCTL_WINPCI_GET_BDF : %x, %x, %x, %d\n", bdf.bus, bdf.dev, bdf.func, dwBytes);
				break;
			}
		}
		else {
			printf("Failure IOCTL_WINPCI_GET_BDF\n");

			CloseHandle(handle);
		}
	}
	return ret;
}

bool CNVMe::initThread() {
	ghThreadStart = CreateEvent(NULL, FALSE, TRUE, THREAD_START);
	if (ghThreadStart == nullptr) return false;
	ResetEvent(ghThreadStart);

	ghSyncCommand = CreateEvent(NULL, FALSE, TRUE, SYNC_COMMAND);
	if (ghSyncCommand == nullptr) return false;
	ResetEvent(ghSyncCommand);

	std::thread th(&CNVMe::isr_thread, this);
	th.detach();

	WaitForSingleObject(ghThreadStart, INFINITE);
	return true;

}

bool CNVMe::getBDF(BDF &X) {
	bool ret = true;
	DWORD dwBytes;

	bool bRet = DeviceIoControl(h, IOCTL_WINPCI_GET_BDF, nullptr, 0, &X, sizeof(BDF), &dwBytes, nullptr);

	if (bRet) {
		if(sizeof(WINPCI) == dwBytes) {
			fprintf(stdout, "Success get BDF\n");
		}
		else {
			fprintf(stderr, "Invalid return size (error code : %u)\n", GetLastError());
			ret = false;
		}
	}
	else {
		fprintf(stderr, "Failure get BDF (error code : %u)\n", GetLastError());
		ret = false;	
	}
	return ret;
}
bool CNVMe::mapPciReg() {
	bool ret = true;
	DWORD dwBytes;

	EFI_ACPI_MEMORY_MAPPED_CONFIGURATION_BASE_ADDRESS_TABLE  MCFG;

	DWORD MCFGDataSize = GetSystemFirmwareTable(
		'ACPI',
		*(DWORD*)"MCFG",
		&MCFG,
		sizeof(EFI_ACPI_MEMORY_MAPPED_CONFIGURATION_BASE_ADDRESS_TABLE)
	);

	UINT64 base_addr = MCFG.Segment.BaseAddress;
	printf("BaseAddress = 0x%llx  \n", base_addr);

	DWORD map_address = base_addr + 4096 * (bdf.func + 8 * (bdf.dev + 32 * bdf.bus));

	pcie_reg.phyAddr = (PVOID)map_address;
	pcie_reg.dwSize = 4096;
	pcie_reg.pvu = nullptr;

	bool bRet = DeviceIoControl(h, IOCTL_WINPCI_MAP_MEMORY, &pcie_reg, sizeof(WINPCI), &pcie_reg, sizeof(WINPCI), &dwBytes, nullptr);

	if (bRet) {
		if (sizeof(WINPCI) == dwBytes) {
			fprintf(stdout, "Success map pcie config register\n");
		}
		else {
			fprintf(stderr, "Invalid return size (error code : %u)\n", GetLastError());
			ret = false;
		}
	}
	else {
		fprintf(stderr, "Failure map pcie config register (error code : %u)\n", GetLastError());
		ret = false;
	}
	return ret;
}

bool CNVMe::unmapPciReg() {
	bool ret = true;

	if (pcie_reg.pvu == nullptr) return true;
	bool bRet = DeviceIoControl(h, IOCTL_WINPCI_UNMAP_MEMORY, &pcie_reg, sizeof(WINPCI), nullptr, 0, nullptr, nullptr);

	if (bRet) {
			fprintf(stdout, "Success unmap pcie config register\n");
	}
	else {
		fprintf(stderr, "Failure unmap pcie config register (error code : %u)\n", GetLastError());
		ret = false;
	}
	return ret;

}

bool CNVMe::mapNVMeReg() {
	bool ret = true;
	DWORD dwBytes;

	DWORD bar0 = *(DWORD*)((UINT8*)pcie_reg.pvu + 0x10) & 0xfffffff0;
	printf("BAR: %lx\n", bar0);

	nvme_reg.phyAddr = (PVOID)bar0;
	nvme_reg.dwSize = 8192;
	nvme_reg.pvu = nullptr;

	bool bRet = DeviceIoControl(h, IOCTL_WINPCI_MAP_MEMORY, &nvme_reg, sizeof(WINPCI), &nvme_reg, sizeof(WINPCI), &dwBytes, nullptr);

	if (bRet) {
		if (sizeof(WINPCI) == dwBytes) {
			fprintf(stdout, "Success map nvme contoller register\n");
			ctrl_reg = (nvme_controller_reg_t*)nvme_reg.pvu;
		}
		else {
			fprintf(stderr, "Invalid return size (error code : %u)\n", GetLastError());
			ret = false;
		}
	}
	else {
		fprintf(stderr, "Failure map  nvme contoller register (error code : %u)\n", GetLastError());
		ret = false;
	}
	return ret;
}

bool CNVMe::unmapNVMeReg() {
	bool ret = true;

	if (nvme_reg.pvu == nullptr) return true;
	bool bRet = DeviceIoControl(h, IOCTL_WINPCI_UNMAP_MEMORY, &nvme_reg, sizeof(WINPCI), nullptr, 0, nullptr, nullptr);

	if (bRet) {
		fprintf(stdout, "Success unmap nvme controller register\n");
	}
	else {
		fprintf(stderr, "Failure unmap nvme controller register (error code : %u)\n", GetLastError());
		ret = false;
	}
	return ret;
}

void CNVMe::initAdminQ(size_t cq_sz, size_t sq_sz) {
	admin_sq_tail = 0;
	admin_cq_head = 0;
	admin_cq_phase = 1;

	admin_cq_size = cq_sz;
	admin_sq_size = sq_sz;
}

bool CNVMe::initCtrl() {
	
	nvme_controller_cap_t cap = { 0 };
	nvme_adminq_attr_t	aqa = { 0 };
	nvme_controller_config_t cc = { 0 };

	cap.val = ctrl_reg->cap.val;

	// wait controller disable
	ctrl_reg->cc.a.en = 0;
	int cnt = 0;

	while (ctrl_reg->csts.rdy == 1) {
		printf("Waiting  controller disable: %d\n", ctrl_reg->csts.rdy);
		if (cnt++ > cap.a.to) {
			std::cerr << "timeout: controller disable" << std::endl;
			return false;
		}
		Sleep(500);
	}

	printf("Controller is disabled\n");

	aqa.a.acqs = admin_cq_size - 1;
	aqa.a.asqs = admin_sq_size - 1;
	ctrl_reg->aqa.val = aqa.val;

	printf("AQA is set\n");

	ctrl_reg->acq = (u64)adminCQ.phyAddr;
	ctrl_reg->asq = (u64)adminSQ.phyAddr;

	printf("Admin Address  is set\n");

	admin_sq_doorbell = &ctrl_reg->sq0tdbl[0];
	admin_cq_doorbell = &ctrl_reg->sq0tdbl[0] + ((LONGLONG)1 << cap.a.dstrd);

	cc.val = NVME_CC_CSS_NVM;
	cc.val |= 0 << NVME_CC_MPS_SHIFT;
	cc.val |= NVME_CC_AMS_RR | NVME_CC_SHN_NONE;
	cc.val |= NVME_CC_IOSQES | NVME_CC_IOCQES;
	cc.a.en = 1;

	ctrl_reg->cc.val = cc.val;

	cnt = 0;
	
	while (ctrl_reg->csts.rdy == 0) {
		if (cnt++ > cap.a.to) {
			std::cerr << "timeout: controller enable" << std::endl;
			return false;
		}
		Sleep(500);
	}

	printf("Controller is ready\n");

	return true;
}

bool CNVMe::createAdminCQ() {
	bool ret = true;
	DWORD dwBytes = 0;

	adminCQ.dwSize = sizeof(nvme_cq_entry_t) * admin_cq_size;
	adminCQ.phyAddr = nullptr;
	adminCQ.pvu = nullptr;

	bool bRet = DeviceIoControl(h, IOCTL_WINPCI_ALLOCATE_DMA_MEMORY, &adminCQ, sizeof(WINPCI), &adminCQ, sizeof(WINPCI), &dwBytes, nullptr);

	if (bRet) {
		if (sizeof(WINPCI) == dwBytes) {
			fprintf(stdout, "Success allocate adminCQ\n");
			admin_cq_entry = (nvme_cq_entry_t*)adminCQ.pvu;
			ZeroMemory(adminCQ.pvu, sizeof(nvme_cq_entry_t) * admin_cq_size);
		}
		else {
			fprintf(stderr, "Invalid return size (error code : %u)\n", GetLastError());
			ret = false;
		}
	}
	else {
		fprintf(stderr, "Failure allocate adminCQ (error code : %u)\n", GetLastError());
		ret = false;
	}
	return ret;

}

bool CNVMe::deleteAdminCQ() {
	bool ret = true;

	if (adminCQ.pvu == nullptr) return true;
	bool bRet = DeviceIoControl(h, IOCTL_WINPCI_UNALLOCATE_DMA_MEMORY, &adminCQ, sizeof(WINPCI), nullptr, 0, nullptr, nullptr);

	if (bRet) {
		fprintf(stdout, "Success release adminCQ\n");
	}
	else {
		fprintf(stderr, "Failure  release adminCQ (error code : %u)\n", GetLastError());
		ret = false;
	}
	return ret;
}

bool CNVMe::createAdminSQ() {
	bool ret = true;
	DWORD dwBytes = 0;

	adminSQ.dwSize = sizeof(nvme_sq_entry_t) * admin_sq_size;
	adminSQ.phyAddr = nullptr;
	adminSQ.pvu = nullptr;

	bool bRet = DeviceIoControl(h, IOCTL_WINPCI_ALLOCATE_DMA_MEMORY, &adminSQ, sizeof(WINPCI), &adminSQ, sizeof(WINPCI), &dwBytes, nullptr);

	if (bRet) {
		if (sizeof(WINPCI) == dwBytes) {
			fprintf(stdout, "Success allocate adminCQ\n");
			admin_sq_entry = (nvme_sq_entry_t*)adminSQ.pvu;
			ZeroMemory(adminSQ.pvu, sizeof(nvme_sq_entry_t) * admin_sq_size);
		}
		else {
			fprintf(stderr, "Invalid return size (error code : %u)\n", GetLastError());
			ret = false;
		}
	}
	else {
		fprintf(stderr, "Failure allocate adminSQ (error code : %u)\n", GetLastError());
		ret = false;
	}
	return ret;
}

bool CNVMe::deleteAdminSQ() {
	bool ret = true;

	if (adminSQ.pvu == nullptr) return true;
	bool bRet = DeviceIoControl(h, IOCTL_WINPCI_UNALLOCATE_DMA_MEMORY, &adminSQ, sizeof(WINPCI), nullptr, 0, nullptr, nullptr);

	if (bRet) {
		fprintf(stdout, "Success release adminSQ\n");
	}
	else {
		fprintf(stderr, "Failure  release adminSQ (error code : %u)\n", GetLastError());
		ret = false;
	}
	return ret;
}

void CNVMe::allocateUserEvent() {
	for (int i = 0; i < eventnum; ++i) {
		sprintf(action_event[i], "%s%dEvent%d", ACTION_EVENT, dev_no, i);
		printf("%s\n", action_event[i]);
	}
}

bool CNVMe::acquireEvent() {
	bool ret = true;
	DWORD eventnum_temp = 0;

	/* create event */
	bool bRet = DeviceIoControl(h, IOCTL_WINPCI_CREATE_EVENT, nullptr, 0, nullptr, 0, &eventnum_temp, nullptr);

	if (bRet) {
		if (eventnum_temp == 0) return false;
		eventnum = eventnum_temp;
		std::cout << "event num: " << eventnum << std::endl;
	}
	else {
		printf("Failure  create event: %d\n", GetLastError());
		ret = false;
	}
		
	return ret;
}
bool CNVMe::releaseEvent() {
	bool ret = true;

	if (eventnum == 0) return true;
	/* delete event */
	bool bRet = DeviceIoControl(h, IOCTL_WINPCI_DELETE_EVENT, nullptr, 0, nullptr, 0, nullptr, nullptr);

	if (bRet)
		printf("Success delete event\n");
	else {
		printf("Failure  delete event: %d\n", GetLastError());
		ret = false;
	}
		
	return ret;
}

bool CNVMe::allocateDMA(WINPCI& mem, size_t sz) {
	bool ret = true;
	DWORD dwBytes = 0;

	mem.dwSize = sz;
	mem.phyAddr = nullptr;
	mem.pvu = nullptr;

	bool bRet = DeviceIoControl(h, IOCTL_WINPCI_ALLOCATE_DMA_MEMORY, &mem, sizeof(WINPCI), &mem, sizeof(WINPCI), &dwBytes, nullptr);

	if (bRet) {
		if (sizeof(WINPCI) == dwBytes) {
			fprintf(stdout, "Success allocate DMA\n");
			ZeroMemory(mem.pvu, sz);
		}
		else {
			fprintf(stderr, "Invalid return size (error code : %u)\n", GetLastError());
			ret = false;
		}
	}
	else {
		fprintf(stderr, "Failure allocate DMA (error code : %u)\n", GetLastError());
		ret = false;
	}
	return ret;

}

bool CNVMe::releaseDMA(WINPCI& mem) {

	bool ret = true;

	if (mem.pvu == nullptr) return true;
	bool bRet = DeviceIoControl(h, IOCTL_WINPCI_UNALLOCATE_DMA_MEMORY, &mem, sizeof(WINPCI), nullptr, 0, nullptr, nullptr);

	if (bRet) {
		fprintf(stdout, "Success unallocate DMA\n");
	}
	else {
		fprintf(stderr, "Failure  unallocate DMA (error code : %u)\n", GetLastError());
		ret = false;
	}
	return ret;

}

bool CNVMe::identify_ctrl(void* buf, size_t sz) {
	int cid = admin_sq_tail;
	bool ret = true;
	WINPCI dma;

	do {
		if (allocateDMA(dma, 4096) == false) { ret = false; break; }
		admin_sq_entry[cid].identify.opcode = nvme_admin_identify;
		admin_sq_entry[cid].identify.command_id = (u16)cid;
		admin_sq_entry[cid].identify.cns = NVME_ID_CNS_CTRL;
		admin_sq_entry[cid].identify.prp1 = (u64)dma.phyAddr;
		admin_sq_entry[cid].identify.nsid = 0;

		if (++admin_sq_tail == admin_sq_size) admin_sq_tail = 0;
		*(volatile u32*)admin_sq_doorbell = admin_sq_tail;

		WaitForSingleObject(ghSyncCommand, INFINITE);

		printf("res: %x\n",*(UINT16*) dma.pvu);

		if (releaseDMA(dma) == false) ret = false;
	
	} while (0);

	return ret;
}

bool CNVMe::getLog4K(UINT8 logid, void* buf, size_t sz) {}

void CNVMe::isr_thread() {
	HANDLE admin_event = nullptr;

	admin_event = OpenEvent(SYNCHRONIZE, FALSE, action_event[0]);
	if (admin_event == nullptr) {
		std::cerr << "failure open event" << std::endl;
		return;
	}

	SetEvent(ghThreadStart);
	while (1) {
		DWORD ret = WaitForMultipleObjects(1, &admin_event, FALSE, INFINITE);

		if (ret == WAIT_FAILED) {
			printf("wait failed\n");
			break;
		}
		else if (ret == WAIT_OBJECT_0) {
			nvme_cq_entry_t* admin_cq = (nvme_cq_entry_t*)adminCQ.pvu;

			if (admin_cq[admin_cq_head].u.a.p == admin_cq_phase) {
				while (admin_cq[admin_cq_head].u.a.p == admin_cq_phase) {
					printf("Interrupt Occured\n");

					//int head = p->admin_cq_head;
					if (++admin_cq_head == admin_cq_size) {
						admin_cq_head = 0;
						admin_cq_phase = !admin_cq_phase;
					}
					*(volatile u32*)(admin_cq_doorbell) = admin_cq_head;
					//ctrl_reg->sq0tdbl[1] = admin_cq_head;
					SetEvent(ghSyncCommand);
				}
			}
		}
		else {
		}
	}
	CloseHandle(admin_event);
	printf("Thread Finish\n");

}

