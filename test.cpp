#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <iostream>
#include <process.h>
#include <timeapi.h>
#include "winpci.h"
#include "acpi.h"
#include "nvme.h"

#define ACTION_EVENT "Global\\Device0Event0"

nvme_controller_reg_t* ctrl_reg;

WINPCI adminSQ;
WINPCI adminCQ;
PVOID admin_cq_doorbell;
PVOID admin_sq_doorbell;
int     admin_cq_size;
int     admin_sq_size;
int	  admin_sq_tail;
int	  admin_cq_head;
int	  admin_cq_phase;
nvme_sq_entry_t* admin_sq_entry;
nvme_cq_entry_t* admin_cq_entry;

unsigned __stdcall isr_thread(LPVOID param)
{
    HANDLE    event = nullptr;
    event = OpenEvent(SYNCHRONIZE, FALSE, ACTION_EVENT);
    if (event == nullptr) {
        std::cerr << "failure open" << std::endl;
        return 1;
    }else {
        std::cout << "success open" << std::endl;
    }

    printf("Thread Start\n");

    while (1) {

      /* admin コマンドとIOコマンドでeventを分ける必要があるので、 WaitForMultiを使い、戻り値でどのeventがシグナル状態になったかを判別する。
      IO Queue 2個の場合、
      event[0]  ---- Admin Command
      event[1]  ---- IO Command
      event[2]  ---- IO Command
      */
        DWORD ret = WaitForMultipleObjects(1, &event, FALSE, INFINITE);

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
                }
            }
        }
        else {
        }
    }
    CloseHandle(event);
    printf("Thread Finish\n");

    return 0;
}

int main()
{
    DWORD busNum = 0x03;
    DWORD devNum = 0x00;
    DWORD funcNum = 0x00;

    DWORD MCFGDataSize;

    MCFGDataSize = GetSystemFirmwareTable(
        'ACPI',
        *(DWORD*)"MCFG",
        &MCFG,
        sizeof(EFI_ACPI_MEMORY_MAPPED_CONFIGURATION_BASE_ADDRESS_TABLE)
    );

    UINT64 base_addr = MCFG.Segment.BaseAddress;
    printf("BaseAddress = 0x%llx  \n", base_addr);

    DWORD map_address = base_addr + 4096 * (funcNum + 8 * (devNum + 32 * busNum));

    HANDLE handle = CreateFile(
        "\\\\.\\winpci0",
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, // No security attributes
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (handle == INVALID_HANDLE_VALUE)
        std::cerr << "failure " << std::endl;

   DWORD dwBytes = 0;
   BOOL bRet = FALSE;

    BDF bdf = { 0 };

    /* Get BDF */
    bRet = DeviceIoControl(handle, IOCTL_WINPCI_GET_BDF, &bdf, sizeof(BDF), &bdf, sizeof(BDF), &dwBytes, nullptr);

    if (bRet) {
        printf("Success IOCTL_WINPCI_GET_BDF : %x, %x, %x, %d\n", bdf.bus, bdf.dev, bdf.func, dwBytes);
    }
    else {
        printf("Failure  IOCTL_WINPCI_GET_BDF : %d\n", GetLastError());
        CloseHandle(handle);
        return 1;
    }
    
    /* Map pci config register  */
    WINPCI pcie_config = { 0 };
    pcie_config.phyAddr = (PVOID)map_address;
    pcie_config.dwSize = 4096;	    //memory size
    PVOID pcie_config_va = nullptr;	//mapped virtual addr

    bRet = DeviceIoControl(handle, IOCTL_WINPCI_MAP_MEMORY, &pcie_config, sizeof(WINPCI), &pcie_config_va, sizeof(PVOID), &dwBytes, nullptr);

    if (bRet) {
        printf("Success map  pci config regsiter: %p, %p, %d, %d\n", pcie_config.pvu, pcie_config.phyAddr, pcie_config.dwSize, dwBytes);
    }
    else {
        printf("Failure  map  pci config regsiter : %d\n", GetLastError());
        CloseHandle(handle);
        return 1;
    }

    /* Map nvme register */
    DWORD bar0 = *(DWORD*)((UINT8*)pcie_config_va + 0x10) & 0xfffffff0;
    printf("BAR: %lx\n", bar0);

    WINPCI nvme_reg = { 0 };
    nvme_reg.phyAddr = (PVOID)bar0;
    nvme_reg.dwSize = 8192;
    PVOID nvme_reg_va = nullptr;	//mapped virtual addr

    bRet = DeviceIoControl(handle, IOCTL_WINPCI_MAP_MEMORY, &nvme_reg, sizeof(WINPCI), &nvme_reg_va, sizeof(PVOID), &dwBytes, nullptr);

    if (bRet) {
        printf("Success map  nvme regsiter: %p, %p, %d, %d\n", nvme_reg.pvu, nvme_reg.phyAddr, nvme_reg.dwSize, dwBytes);
    }
    else {
        printf("Failure  map  pci nvme regsiter : %d\n", GetLastError());
        CloseHandle(handle);
        return 1;
    }

    ctrl_reg = (nvme_controller_reg_t*)nvme_reg_va;
    
   printf("CAP TO: %d\n", ctrl_reg->cap.a.to);

    admin_sq_tail = 0;
    admin_cq_head = 0;
    admin_cq_phase = 1;

    admin_cq_size = 64;
    admin_sq_size = 64;

    /* allocate admin CQ  */
    adminCQ.dwSize = sizeof(nvme_cq_entry_t) * admin_cq_size;	    //memory size

    bRet = DeviceIoControl(handle, IOCTL_WINPCI_ALLOCATE_DMA_MEMORY, &adminCQ, sizeof(WINPCI), &adminCQ, sizeof(WINPCI), &dwBytes, nullptr);

    if (bRet)
       printf("Success create CQ: %p, %p, %d, %d\n", adminCQ.pvu, adminCQ.phyAddr, adminCQ.dwSize, dwBytes);
    else
       printf("Failure  create CQ : %d\n",  GetLastError());
   

    /* allocate admin SQ  */
    adminSQ.dwSize = sizeof(nvme_sq_entry_t) * admin_sq_size;	    //memory size

    bRet = DeviceIoControl(handle, IOCTL_WINPCI_ALLOCATE_DMA_MEMORY, &adminSQ, sizeof(WINPCI), &adminSQ, sizeof(WINPCI), &dwBytes, nullptr);

    if (bRet)
        printf("Success create SQ: %p, %p, %d, %d\n", adminSQ.pvu, adminSQ.phyAddr, adminSQ.dwSize, dwBytes);
    else
        printf("Failure  create SQ : %d\n", GetLastError());
   
    admin_cq_entry = (nvme_cq_entry_t*)adminCQ.pvu;
    memset(adminCQ.pvu, 0, sizeof(nvme_cq_entry_t) * admin_cq_size);

    admin_sq_entry = (nvme_sq_entry_t*)adminSQ.pvu;
    memset(adminSQ.pvu, 0, sizeof(nvme_sq_entry_t) * admin_sq_size);

    nvme_controller_cap_t cap = { 0 };
    nvme_adminq_attr_t	aqa = { 0 };
    nvme_controller_config_t cc = { 0 };

    cap.val = ctrl_reg->cap.val;

    // wait controller disable
    ctrl_reg->cc.a.en = 0;
    Sleep(1000);

    while (ctrl_reg->csts.rdy == 1) {
        printf("Waiting  controller disable: %d\n", ctrl_reg->csts.rdy);
        Sleep(1000);
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

    Sleep(1000);
    while (ctrl_reg->csts.rdy == 0) {
        printf("Waiting  controller ready\n");
        Sleep(1000);
    }
    printf("Controller is ready\n");


    /*  kick thread  */
    UINT ThreadId = 0;
    HANDLE isrthread = (HANDLE)_beginthreadex(NULL, 0, isr_thread, nullptr, 0, &ThreadId);

    Sleep(1000);

    /* allocate data buffer */
    WINPCI dataBuffer = { 0 };
    dataBuffer.dwSize = 4096;	    //memory size

    bRet = DeviceIoControl(handle, IOCTL_WINPCI_ALLOCATE_DMA_MEMORY, &dataBuffer, sizeof(WINPCI), &dataBuffer, sizeof(WINPCI), &dwBytes, nullptr);

    if (bRet)
        printf("Success IOCTL_WINPCI_ALLOCATE_DMA_MEMORY: %p, %p, %d, %d\n", dataBuffer.pvu, dataBuffer.phyAddr, dataBuffer.dwSize, dwBytes);
    else
        printf("Failure  IOCTL_WINPCI_ALLOCATE_DMA_MEMORY : %d\n", GetLastError());

    memset(dataBuffer.pvu, 0, 4096);

    int cid = admin_sq_tail;

#if 1
    admin_sq_entry[cid].identify.opcode = nvme_admin_identify;
    admin_sq_entry[cid].identify.command_id = (u16)cid;
    admin_sq_entry[cid].identify.cns = NVME_ID_CNS_CTRL;
    admin_sq_entry[cid].identify.prp1 = (u64)dataBuffer.phyAddr;
    admin_sq_entry[cid].identify.nsid = 0;

    if (++admin_sq_tail == admin_sq_size) admin_sq_tail = 0;
    *(volatile u32*)admin_sq_doorbell = admin_sq_tail;

    Sleep(2000);

    nvme_id_ctrl* ctrl = (nvme_id_ctrl*)dataBuffer.pvu;
    printf("vendor id: %x\n", ctrl->vid);



#else
    /* issue Get Log command */
   
    admin_sq_entry[cid].get_log_page.opcode = nvme_admin_get_log_page;
    admin_sq_entry[cid].get_log_page.command_id = (u16)cid;
    admin_sq_entry[cid].get_log_page.nsid = 0xffffffff;
    admin_sq_entry[cid].get_log_page.dptr.prp1 = (u64)dataBuffer.phyAddr;
    admin_sq_entry[cid].get_log_page.lid = 2;
    admin_sq_entry[cid].get_log_page.numdl = (512 / sizeof(u32) - 1) & 0xff;
    admin_sq_entry[cid].get_log_page.numdu = ((512 / sizeof(u32) - 1) >> 16) & 0xff;

   if (++admin_sq_tail == admin_sq_size) admin_sq_tail = 0;
   *(volatile u32*)admin_sq_doorbell = admin_sq_tail;
   //ctrl_reg->sq0tdbl[0] = admin_sq_tail;

   Sleep(2000);
   nvme_smart_log* log = (nvme_smart_log*)dataBuffer.pvu;
   printf("available spare: %d\n", log ->avail_spare);
   printf("critical_comp_time: %d\n", log->critical_comp_time);
   printf("critical_warning: %d\n", log->critical_warning);
   printf("percent_used: %d\n", log->percent_used);
   printf("data_units_written: %d\n",
       log->data_units_written[7] << 56 +
       log->data_units_written[6] << 48 +
       log->data_units_written[5] << 40 +
       log->data_units_written[4] << 32 +       
       log->data_units_written[3] << 24 +
       log->data_units_written[2] << 16 +
       log->data_units_written[1] << 8 +
       log->data_units_written[0] << 0
   );
#endif
   
    system("pause");
    CloseHandle(isrthread);
    CloseHandle(handle);

}

