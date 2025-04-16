#pragma once

#include <windows.h>
#include <iostream>
#include <regex>
#include <thread>
#include <process.h>
#include <timeapi.h>
#include "winpci.h"
#include "acpi.h"
#include "nvme.h"

#define ACTION_EVENT  "Global\\Device"
#define THREAD_START "thread start"
#define SYNC_COMMAND "sync command"

constexpr int MAX_IO_Q_NUM = 8;
constexpr int MAX_VECTOR_NUM = 9;


class CNVMe {

public:
	CNVMe(std::string && v);
	CNVMe(std::string& v) = delete;

	~CNVMe();

	bool init();
	bool initDevice();
	bool initThread();
	bool getBDF(BDF& X);
	bool mapPciReg();
	bool unmapPciReg();

	bool mapNVMeReg();
	bool unmapNVMeReg();

	void initAdminQ(size_t cq_sz, size_t sq_sz);

	bool initCtrl();
	bool createAdminCQ();
	bool deleteAdminCQ();

	bool createIOCQ(int cqid, int qsize, int flags, int vector);
	bool deleteIOCQ(int cqid);
	bool createIOSQ(int sqid, int qsize, int flags, int related_cqid);
	bool deleteIOSQ(int sqid);

	bool SetFeaturesFID7();

	bool createAdminSQ();
	bool deleteAdminSQ();

	void allocateUserEvent();
	bool acquireEvent();
	bool releaseEvent();

	bool allocateDMA(WINPCI & mem, size_t sz);
	bool releaseDMA(WINPCI& mem);

	bool identify_ctrl(void* buf, size_t sz);
	bool getLog4K(UINT8 logid, void*buf, size_t sz);

	void isr_thread();
	void initIoQ(int cqid, size_t cq_depth, int sqid,  size_t sq_depth, int vector);

	void freePool(std::vector<WINPCI>& pool) {
		for (auto& p : pool) {
			releaseDMA(p);
		}
	}

	bool read(int sqid, int nsid);


private:
	HANDLE h;
	int dev_no;
	BDF bdf;
	bool find;


	std::vector<WINPCI> mem_pool;
	nvme_controller_reg_t* ctrl_reg;

	WINPCI pcie_reg;
	WINPCI nvme_reg;
	HANDLE ghThreadStart;
	HANDLE ghSyncCommand;

	WINPCI adminSQ;
	WINPCI adminCQ;
	PVOID admin_cq_doorbell;
	PVOID admin_sq_doorbell;

	int admin_cq_size;
	int admin_sq_size;
	int admin_sq_tail;
	int admin_cq_head;
	int admin_cq_phase;

	nvme_sq_entry_t* admin_sq_entry;
	nvme_cq_entry_t* admin_cq_entry;

	typedef struct {
		WINPCI ioSQ;
		WINPCI ioCQ;
		PVOID io_cq_doorbell;
		PVOID io_sq_doorbell;
		int io_cq_size;
		int io_sq_size;
		int io_sq_tail;
		int io_cq_head;
		int io_cq_phase;
		nvme_sq_entry_t* io_sq_entry;
		nvme_cq_entry_t* io_cq_entry;
	}io_queue_t;

	io_queue_t ioQ[MAX_VECTOR_NUM];
	u8  vector2cqid[MAX_VECTOR_NUM];

	int eventnum;
	char action_event[MAX_EVENT_SZ][MAX_PATH] = { {0} };
};