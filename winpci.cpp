#include <ntddk.h>
#include <wdm.h>
#include<ntstrsafe.h>
#include "winpci.h"
#include "nvme.h"
#include "FastMutex.h"
#include "SpinLock.h"

#define arraysize( p ) ( sizeof( p ) / sizeof( ( p )[0] ) )

#define DEVICE_NAME					( L"\\Device\\WINPCI" )
#define DEVICE_SYMLINKNAME	( L"\\DosDevices\\WINPCI" )

const int MAX_EVENT_SZ = 16;
const int  EVENTNAMEMAXLEN = 100;
const int BUFFER_SIZE = 256;
const int MAX_DEVICE_COUNT = 100;
bool gbDeviceNumber[MAX_DEVICE_COUNT] = { FALSE };
FastMutex	gDeviceCountLocker;
//LONG gucDeviceCounter;

//Mapped memory information list
typedef struct tagMAPINFO
{
	LIST_ENTRY	ListEntry;
	PMDL				pMdl;			//allocated mdl
	PVOID				pvk;				//kernel mode virtual address
	PVOID				pvu;				//user mode virtual address
	ULONG			memSize;	//memory size in bytes
} MAPINFO, * PMAPINFO;

typedef struct _MEMORY {
	LIST_ENTRY					ListEntry;
	ULONG							Length;
	PHYSICAL_ADDRESS	   dmaAddr;
	PVOID								pvk;
	PVOID								pvu;
	PMDL								pMdl;
}MEMORY, * PMEMORY;

typedef struct _DEVICE_EXTENSION
{
	PDEVICE_OBJECT		   fdo;
	PDEVICE_OBJECT		   PhyDevice;
	PDEVICE_OBJECT		   NextStackDevice;
	UNICODE_STRING	   ustrDeviceName;
	UNICODE_STRING	   ustrSymLinkName;
	struct _KINTERRUPT* InterruptObject;
	BOOLEAN					    bInterruptEnable;
	_DMA_ADAPTER*       dmaAdapter;
	ULONG						    NumOfMappedRegister;
	LIST_ENTRY				    winpci_dma_linkListHead;
	FastMutex					    winpci_dma_locker;
	LIST_ENTRY				    winpci_mmap_linkListHead;
	FastMutex					    winpci_mmap_locker;
	HANDLE						    eventHandle[MAX_EVENT_SZ];
	PKEVENT					    pEvent[MAX_EVENT_SZ];
	UINT8							    DeviceCounter;
	LONG							    InterruptCount;
	ULONG						    IsrType;
	ULONG						    MessageId;
	ULONG						    bus;
	USHORT						    dev;
	USHORT						    func;
} DEVICE_EXTENSION, * PDEVICE_EXTENSION;

VOID WINPCILogger(char* text);
void WINPCIDelay(long long millsecond);
NTSTATUS WINPCIAddDevice(IN PDRIVER_OBJECT DriverObject, IN PDEVICE_OBJECT PhysicalDeviceObject);
NTSTATUS WINPCIPnp(IN PDEVICE_OBJECT fdo, IN PIRP Irp);
NTSTATUS WINPCIDeviceControl(IN PDEVICE_OBJECT fdo, IN PIRP Irp);
NTSTATUS WINPCIDispatchRoutine(IN PDEVICE_OBJECT fdo, IN PIRP Irp);
NTSTATUS WINPCICleanUp(IN PDEVICE_OBJECT fdo, IN PIRP Irp);
NTSTATUS WINPCIPower(IN PDEVICE_OBJECT fdo, IN PIRP Irp);
void WINPCIUnload(IN PDRIVER_OBJECT DriverObject);

NTSTATUS ReadWriteConfigSpace(IN PDEVICE_OBJECT DeviceObject, IN ULONG ReadOrWrite, // 0 for read 1 for write
	IN PVOID Buffer, IN ULONG Offset, IN ULONG Length);

extern "C"
NTSTATUS DriverEntry(IN PDRIVER_OBJECT pDriverObject, IN PUNICODE_STRING pRegistryPath)
{

	UNREFERENCED_PARAMETER(pRegistryPath);

	pDriverObject->DriverExtension->AddDevice = WINPCIAddDevice;
	pDriverObject->MajorFunction[IRP_MJ_PNP] = WINPCIPnp;
	pDriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = WINPCIDeviceControl;
	pDriverObject->MajorFunction[IRP_MJ_CREATE] = WINPCIDispatchRoutine;
	pDriverObject->MajorFunction[IRP_MJ_CLOSE] = WINPCIDispatchRoutine;
	pDriverObject->MajorFunction[IRP_MJ_READ] = WINPCIDispatchRoutine;
	pDriverObject->MajorFunction[IRP_MJ_WRITE] = WINPCIDispatchRoutine;
	pDriverObject->MajorFunction[IRP_MJ_CLEANUP] = WINPCICleanUp;
	pDriverObject->MajorFunction[IRP_MJ_POWER] = WINPCIPower;
	pDriverObject->DriverUnload = WINPCIUnload;

	gDeviceCountLocker.Init();
	//KeInitializeSpinLock(&interrupt_lock);

	return STATUS_SUCCESS;
}

BOOLEAN
MSI_ISR(
	IN  PKINTERRUPT  Interrupt,
	PVOID  ServiceContext,
	ULONG  MessageId
)
{
	UNREFERENCED_PARAMETER(Interrupt);
	//DbgPrint("Interrupt Occured: %d\n", MessageId);
	PDEVICE_EXTENSION p = (PDEVICE_EXTENSION)ServiceContext;

	p->MessageId = MessageId;
	IoRequestDpc(p->fdo, NULL, p);

	return TRUE;
}

BOOLEAN
FdoInterruptCallback(
	IN  PKINTERRUPT             InterruptObject,
	IN  PVOID                   Context
)
{
	UNREFERENCED_PARAMETER(InterruptObject);
	//UNREFERENCED_PARAMETER(Context);
	PDEVICE_EXTENSION p = (PDEVICE_EXTENSION)Context;

	DbgPrint("interrupt\n");
	return TRUE;
}

VOID DPC(
	IN PKDPC Dpc,
	IN PDEVICE_OBJECT DeviceObject,
	IN PIRP irp,
	IN PVOID context
)
{
	UNREFERENCED_PARAMETER(Dpc);
	UNREFERENCED_PARAMETER(DeviceObject);
	UNREFERENCED_PARAMETER(irp);
	//UNREFERENCED_PARAMETER(context);
	PDEVICE_EXTENSION p = (PDEVICE_EXTENSION)context;
	
	DbgPrint("interrupt\n");

	//if (KeGetCurrentIrql() == DISPATCH_LEVEL) {
	//	DbgPrint("%s :  DISPATCH_LEVEL\n", __func__);
	//}
	//else if (KeGetCurrentIrql() == PASSIVE_LEVEL) {
	//	DbgPrint("%s :  PASSIVE_LEVEL\n", __func__);
	//}

	KeSetEvent(p->pEvent[p->MessageId], IO_NO_INCREMENT, FALSE);
	KeClearEvent(p->pEvent[p->MessageId]);

	KeStallExecutionProcessor(50);

	KeSetEvent(p->pEvent[p->MessageId], IO_NO_INCREMENT, FALSE);
	KeClearEvent(p->pEvent[p->MessageId]);

	return;
}

NTSTATUS WINPCIAddDevice(IN PDRIVER_OBJECT DriverObject, IN PDEVICE_OBJECT PhysicalDeviceObject)
{
	NTSTATUS			status;
	PDEVICE_OBJECT		fdo;
	PDEVICE_EXTENSION	pdx;
	UNICODE_STRING		devName;
	UNICODE_STRING		symLinkName;

	DbgPrint("WINPCIAddDevice\n");
	WINPCILogger("WINPCIAddDevice\n");

	//DECLARE_UNICODE_STRING_SIZE(devName, 64);
	//DECLARE_UNICODE_STRING_SIZE(symLinkName, 64);

	wchar_t  devNameReal[64] = { 0 };
	wchar_t  symLinkNameReal[64] = { 0 };

	int devCounter = 0;
	gDeviceCountLocker.Lock();
	for (int i = 0; i < MAX_DEVICE_COUNT;++i) {
		if (gbDeviceNumber[i] == FALSE) {
			devCounter = i;
			gbDeviceNumber[i] = TRUE;
			break;
		}
	}
	gDeviceCountLocker.UnLock();

	swprintf(devNameReal, L"%s%d", DEVICE_NAME, devCounter);
	DbgPrint("%ls\n", devNameReal);
	RtlInitUnicodeString(&devName, devNameReal);
	//RtlUnicodeStringPrintf(&devName, L"\\Device\\WINPCI%d", gucDeviceCounter);

	//DbgPrint("Add Device\n");
	status = IoCreateDevice(DriverObject, sizeof(DEVICE_EXTENSION), &devName, FILE_DEVICE_UNKNOWN, 0, FALSE, &fdo);
	if (!NT_SUCCESS(status))
	{
		DbgPrint("Failure IoCreateDevice\n");
		WINPCILogger("Failure IoCreateDevice\n");

		return status;
	}
	pdx = (PDEVICE_EXTENSION)fdo->DeviceExtension;
	pdx->fdo = fdo;
	pdx->PhyDevice = PhysicalDeviceObject;
	pdx->DeviceCounter = devCounter;

	for(int i=0; i< MAX_EVENT_SZ; ++i) pdx->eventHandle[i] = nullptr;

	pdx->bInterruptEnable = FALSE;

	InitializeListHead(&pdx->winpci_dma_linkListHead);
	pdx->winpci_dma_locker.Init();

	InitializeListHead(&pdx->winpci_mmap_linkListHead);
	pdx->winpci_mmap_locker.Init();

	IoInitializeDpcRequest(fdo, DPC);
	//pdx->NextStackDevice = IoAttachDeviceToDeviceStack(fdo, PhysicalDeviceObject);
	status = IoAttachDeviceToDeviceStackSafe(fdo, PhysicalDeviceObject, &pdx->NextStackDevice);
	if (!NT_SUCCESS(status))
	{
		DbgPrint("Failure IoAttachDeviceToDeviceStackSafe");
		WINPCILogger("Failure IoAttachDeviceToDeviceStackSafe\n");

		return status;
	}

	swprintf(symLinkNameReal, L"%s%d", DEVICE_SYMLINKNAME, devCounter);
	DbgPrint("%ls\n", symLinkNameReal);
	RtlInitUnicodeString(&symLinkName, symLinkNameReal);

	pdx->ustrDeviceName = devName;
	pdx->ustrSymLinkName = symLinkName;
	status = IoCreateSymbolicLink(&symLinkName, &devName);

	if (!NT_SUCCESS(status))
	{
		DbgPrint("Failure IoCreateSymbolicLink\n");
		WINPCILogger("Failure IoCreateSymbolicLink\n");

		IoDeleteSymbolicLink(&pdx->ustrSymLinkName);
		status = IoCreateSymbolicLink(&symLinkName, &devName);
		if (!NT_SUCCESS(status))
		{
			DbgPrint("Failure IoCreateSymbolicLink\n");
			return status;
		}
	}

	//fdo->Flags |= DO_BUFFERED_IO | DO_POWER_PAGABLE;
	fdo->Flags |= DO_DIRECT_IO | DO_POWER_PAGABLE;
	fdo->Flags &= ~DO_DEVICE_INITIALIZING;

	DbgPrint("Success WINPCIAddDevice\n");
	WINPCILogger("Success WINPCIAddDevice\n");
	//gucDeviceCounter++;
	//InterlockedIncrement(&gucDeviceCounter);
	return STATUS_SUCCESS;
}

NTSTATUS DefaultPnpHandler(PDEVICE_EXTENSION pdx, PIRP Irp)
{
	IoSkipCurrentIrpStackLocation(Irp);
	return IoCallDriver(pdx->NextStackDevice, Irp);
}

NTSTATUS OnRequestComplete(PDEVICE_OBJECT junk, PIRP Irp, PKEVENT pev)
{
	UNREFERENCED_PARAMETER(junk);
	UNREFERENCED_PARAMETER(Irp);

	KeSetEvent(pev, 0, FALSE);
	return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS ForwardAndWait(PDEVICE_EXTENSION pdx, PIRP Irp)
{
	KEVENT event;

	KeInitializeEvent(&event, NotificationEvent, FALSE);
	IoCopyCurrentIrpStackLocationToNext(Irp);
	IoSetCompletionRoutine(Irp, (PIO_COMPLETION_ROUTINE)OnRequestComplete, (PVOID) & event, TRUE, TRUE, TRUE);

	IoCallDriver(pdx->NextStackDevice, Irp);
	KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
	return Irp->IoStatus.Status;
}

#if 0
VOID ShowResources(IN PCM_PARTIAL_RESOURCE_LIST list, IN PDEVICE_EXTENSION pdx)
{
	ULONG							i;
	ULONG							nres = list->Count;
	//NTSTATUS			status;

	PCM_PARTIAL_RESOURCE_DESCRIPTOR resource = list->PartialDescriptors;

	IO_CONNECT_INTERRUPT_PARAMETERS     Connect;
	IO_DISCONNECT_INTERRUPT_PARAMETERS  Disconnect;

	//UNREFERENCED_PARAMETER(pdx);
	PIO_INTERRUPT_MESSAGE_INFO  p;
	PIO_INTERRUPT_MESSAGE_INFO_ENTRY pp;
	NTSTATUS status;

	for (i = 0; i < nres; ++i, ++resource)
	{
		ULONG		type = resource->Type;

		static char* name[] = {
			"CmResourceTypeNull",
			"CmResourceTypePort",
			"CmResourceTypeInterrupt",
			"CmResourceTypeMemory",
			"CmResourceTypeDma",
			"CmResourceTypeDeviceSpecific",
			"CmResourceTypeBusNumber",
			"CmResourceTypeDevicePrivate",
			"CmResourceTypeAssignedResource",
			"CmResourceTypeSubAllocateFrom",
		};

		//DbgPrint("type=%d, typeName=%s \n", type, type < arraysize(name) ? name[type] : "unknown");

		switch (type)
		{   // select on resource type
		case CmResourceTypePort:
		case CmResourceTypeMemory:
			pdx->bar_size = resource->u.Port.Length;
			pdx->bar0 = MmMapIoSpace(resource->u.Port.Start, resource->u.Port.Length, MmNonCached);
			//DbgPrint("bar0  kernel virtual address:  %p", pdx->bar0);

			//DbgPrint("CmResourceTypeMemory ===> start 0x%lX 0x%lX length:%d\n",
			//	resource->u.Port.Start.HighPart,
			//	resource->u.Port.Start.LowPart,
			//	resource->u.Port.Length);
			break;
		case CmResourceTypeBusNumber:
			//DbgPrint("CmResourceTypeBusNumber:::");
			break;
		case CmResourceTypeInterrupt:
			
#if  0
			RtlZeroMemory(&Connect, sizeof(IO_CONNECT_INTERRUPT_PARAMETERS));
			RtlZeroMemory(&Disconnect, sizeof(IO_DISCONNECT_INTERRUPT_PARAMETERS));

			//Connect.Version = CONNECT_FULLY_SPECIFIED_GROUP;
			Connect.Version = CONNECT_FULLY_SPECIFIED;
			Connect.FullySpecified.PhysicalDeviceObject = pdx->PhyDevice;

			Connect.FullySpecified.InterruptObject = &pdx->InterruptObject;
			Connect.FullySpecified.ServiceRoutine = FdoInterruptCallback;
			Connect.FullySpecified.ServiceContext = pdx;

			Connect.FullySpecified.FloatingSave = FALSE;
			Connect.FullySpecified.SpinLock = NULL;


			if (resource->Flags & CM_RESOURCE_INTERRUPT_MESSAGE) {
				Connect.FullySpecified.Vector = resource->u.MessageInterrupt.Translated.Vector;
				Connect.FullySpecified.Irql = (KIRQL)resource->u.MessageInterrupt.Translated.Level;
				Connect.FullySpecified.SynchronizeIrql = (KIRQL)resource->u.MessageInterrupt.Translated.Level;
				//Connect.FullySpecified.Group = resource->u.MessageInterrupt.Translated.Group;
				Connect.FullySpecified.ProcessorEnableMask = resource->u.MessageInterrupt.Translated.Affinity;
			}
			else {
				Connect.FullySpecified.Vector = resource->u.Interrupt.Vector;
				Connect.FullySpecified.Irql = (KIRQL)resource->u.Interrupt.Level;
				Connect.FullySpecified.SynchronizeIrql = (KIRQL)resource->u.Interrupt.Level;
				//Connect.FullySpecified.Group = resource->u.Interrupt.Group;
				Connect.FullySpecified.ProcessorEnableMask = resource->u.Interrupt.Affinity;
			}

			//Connect.Version = (Connect.FullySpecified.Group != 0) ? CONNECT_FULLY_SPECIFIED_GROUP : CONNECT_FULLY_SPECIFIED;
			Connect.FullySpecified.InterruptMode == (resource->Flags & CM_RESOURCE_INTERRUPT_LATCHED) ? Latched : LevelSensitive;
			Connect.FullySpecified.ShareVector = (BOOLEAN)(resource->ShareDisposition == CmResourceShareShared);

			status = IoConnectInterruptEx(&Connect);

			if (NT_SUCCESS(status)) {
				DbgPrint("Success IoConnectInterruptEx");
				pdx->IsrType = Connect.Version;
				pdx->bInterruptEnable = TRUE;
				p = (PIO_INTERRUPT_MESSAGE_INFO)pdx->InterruptObject;
				pp = p->MessageInfo;
				DbgPrint("interrupt version: %d", Connect.Version);

				for (i = 0; i < p->MessageCount; ++i) {
					DbgPrint("IoConnectInterruptEx params ===> Irql:%X, Vector:%X, Proc:%llX, MessageData:%lX, MessageAddress:%lX\n",
						(pp + i)->Irql,
						(pp + i)->Vector,
						(pp + i)->TargetProcessorSet,
						(pp + i)->MessageData,
						(pp + i)->MessageAddress.LowPart
					);
				}

				//Disconnect.Version = Connect.Version;
				//Disconnect.ConnectionContext.InterruptObject = pdx->InterruptObject;
				//IoDisconnectInterruptEx(&Disconnect);
			}
			else {
				DbgPrint("Failure  IoConnectInterruptEx:   %x", status);

			}

			//}
			//else {
			//	Connect.FullySpecified.Vector = resource->u.Interrupt.Vector;
			//	Connect.FullySpecified.Irql = (KIRQL)resource->u.Interrupt.Level;
			//	Connect.FullySpecified.SynchronizeIrql = (KIRQL)resource->u.Interrupt.Level;
			//	Connect.FullySpecified.Group = resource->u.Interrupt.Group;
			//	Connect.FullySpecified.ProcessorEnableMask = resource->u.Interrupt.Affinity;

			//}

			//if (resource->Flags & CM_RESOURCE_INTERRUPT_MESSAGE) {
			//	DbgPrint("CM_RESOURCE_INTERRUPT_MESSAGE\n");
			//}

			//DbgPrint("resource flag: %x\n", resource->Flags);
		//	DbgPrint("CmResourceTypeInterrupt   Translated ===> level:%X, vector:%X, affinity:%llX\n",
			//	resource->u.MessageInterrupt.Translated.Level,
			//	resource->u.MessageInterrupt.Translated.Vector,
			//	resource->u.MessageInterrupt.Translated.Affinity);

			//DbgPrint("CmResourceTypeInterrupt   Raw ===> level:%X, vector:%X, affinity:%llX\n",
			//	resource->u.MessageInterrupt.Translated.Level,
			//	resource->u.MessageInterrupt.Translated.Vector,
			//	resource->u.MessageInterrupt.Translated.Affinity);

#endif

			//if (resource->u.MessageInterrupt.Translated.Vector <= 128) {
			//	pdx->vector = resource->u.MessageInterrupt.Translated.Vector;
			//}

			break;

		case CmResourceTypeDma:
			DbgPrint("CmResourceTypeDma ===> channel %d, port %X\n", resource->u.Dma.Channel, resource->u.Dma.Port);
		} // select on resource type

	}    // for each resource
}       // ShowResources

#endif

BOOLEAN OnInterrupt(PKINTERRUPT InterruptObject, PDEVICE_EXTENSION pdx)
{
	UNREFERENCED_PARAMETER(InterruptObject);
	UNREFERENCED_PARAMETER(pdx);

	return TRUE;
}

NTSTATUS HandleStartDevice(PDEVICE_EXTENSION pdx, PIRP Irp)
{
	NTSTATUS	status;
	PIO_STACK_LOCATION	stack;
	PCM_PARTIAL_RESOURCE_LIST		translated;
	PCM_FULL_RESOURCE_DESCRIPTOR	pfrd;
	IO_CONNECT_INTERRUPT_PARAMETERS     Connect;
	PIO_INTERRUPT_MESSAGE_INFO		p;
	PIO_INTERRUPT_MESSAGE_INFO_ENTRY	pp;
	UINT16  command_reg;
	int i;

	DbgPrint("HandleStartDevice\n");
	WINPCILogger("HandleStartDevice\n");

	status = ForwardAndWait(pdx, Irp);

	if (!NT_SUCCESS(status))
	{
		Irp->IoStatus.Status = status;
		IoCompleteRequest(Irp, IO_NO_INCREMENT);
		WINPCILogger("Failure ForwardAndWait\n");

		return status;
	}

	stack = IoGetCurrentIrpStackLocation(Irp);

	if (stack->Parameters.StartDevice.AllocatedResourcesTranslated)
	{
		translated = &stack->Parameters.StartDevice.AllocatedResourcesTranslated->List[0].PartialResourceList;
		pfrd = &stack->Parameters.StartDevice.AllocatedResourcesTranslated->List[0];
	}
	else
	{
		translated = nullptr;
	}

	// Show resource from PNP Manager
	// ShowResources(translated, pdx);

	if (translated == nullptr) {
		status = STATUS_UNSUCCESSFUL;
		Irp->IoStatus.Status = status;
		IoCompleteRequest(Irp, IO_NO_INCREMENT);
		WINPCILogger("Failure translated\n");

		return status;
	}

	PCM_PARTIAL_RESOURCE_DESCRIPTOR resource = translated->PartialDescriptors;

	ULONG  length;
	ULONG propertyAddress;

	status = IoGetDeviceProperty(pdx->PhyDevice,
		DevicePropertyBusNumber,
		sizeof(ULONG),
		(PVOID)&pdx->bus,
		&length
	);
	
	if (NT_SUCCESS(status)) {
		DbgPrint("BusNumber:%x\n", pdx->bus);
	}

	status = IoGetDeviceProperty(pdx->PhyDevice,
		DevicePropertyAddress,
		sizeof(ULONG),
		(PVOID)&propertyAddress,
		&length
	);

	if (NT_SUCCESS(status)) {
		pdx->func = (USHORT)((propertyAddress) & 0x0000FFFF);
		pdx->dev = (USHORT)(((propertyAddress) >> 16) & 0x0000FFFF);
		DbgPrint("DeviceNumber:%x\n", pdx->dev);
		DbgPrint("FunctionNumber:%x\n", pdx->func);

	}

	/* setting DMA adapter */
	DEVICE_DESCRIPTION DeviceDescription;
	RtlZeroMemory(&DeviceDescription, sizeof(DEVICE_DESCRIPTION));

	DeviceDescription.Version = DEVICE_DESCRIPTION_VERSION3;
	DeviceDescription.Master = TRUE;
	DeviceDescription.ScatterGather = TRUE;
	DeviceDescription.IgnoreCount = TRUE;
	DeviceDescription.DmaChannel = resource->u.Dma.Channel;
	DeviceDescription.Dma32BitAddresses = FALSE;
	DeviceDescription.Dma64BitAddresses = FALSE;
	DeviceDescription.InterfaceType = PCIBus;
	DeviceDescription.MaximumLength =  0x10000000;
	DeviceDescription.DmaAddressWidth = 64;

	if(pdx->dmaAdapter == nullptr)
		pdx-> dmaAdapter = IoGetDmaAdapter(pdx->PhyDevice, &DeviceDescription, &pdx ->NumOfMappedRegister);

	if (pdx->dmaAdapter == nullptr) {
		status = STATUS_UNSUCCESSFUL;
		Irp->IoStatus.Status = status;
		IoCompleteRequest(Irp, IO_NO_INCREMENT);
		WINPCILogger("Failure IoGetDmaAdapter\n");

		return status;
	}

	/* Setting  Interrupt */
#if 1
	RtlZeroMemory(&Connect, sizeof(IO_CONNECT_INTERRUPT_PARAMETERS));
	Connect.Version = CONNECT_MESSAGE_BASED;
	Connect.MessageBased.ConnectionContext.InterruptObject = &pdx->InterruptObject;
	Connect.MessageBased.PhysicalDeviceObject = pdx->PhyDevice;
	Connect.MessageBased.FloatingSave = FALSE;
	//Connect.MessageBased.SpinLock = &interrupt_lock;
	Connect.MessageBased.SpinLock = nullptr;
	Connect.MessageBased.MessageServiceRoutine = MSI_ISR;
	Connect.MessageBased.SynchronizeIrql = 0;
	Connect.MessageBased.ServiceContext = pdx;
	Connect.MessageBased.FallBackServiceRoutine = FdoInterruptCallback;

	status = IoConnectInterruptEx(&Connect);

	if (NT_SUCCESS(status)) {
		DbgPrint("Success IoConnectInterruptEx\n");
		pdx->IsrType = Connect.Version;
		pdx->bInterruptEnable = TRUE;
		p = (PIO_INTERRUPT_MESSAGE_INFO)pdx->InterruptObject;
		pp = p->MessageInfo;
		DbgPrint("interrupt version: %d", Connect.Version);

#if 0
		for (int i = 0; i < (int)p->MessageCount; ++i) {
			DbgPrint("IoConnectInterruptEx params ===> Irql:%X, Vector:%X, Proc:%llX, MessageData:%lX, MessageAddress:%lX\n",
				(pp + i)->Irql,
				(pp + i)->Vector,
				(pp + i)->TargetProcessorSet,
				(pp + i)->MessageData,
				(pp + i)->MessageAddress.LowPart
			);

			Logger("path 2\n");

			UNICODE_STRING name;
			UNICODE_STRING eventbase;
			UNICODE_STRING eventname;
			STRING eventnameString;

			char cEventName[EVENTNAMEMAXLEN] = { 0 };

			sprintf(cEventName, "Device%dEvent%d", pdx-> DeviceCounter, i);

			RtlInitUnicodeString(&eventbase, L"\\BaseNamedObjects\\");

			name.MaximumLength = EVENTNAMEMAXLEN + eventbase.Length;
			name.Length = 0;
			name.Buffer = (PWCH)ExAllocatePool(NonPagedPool, name.MaximumLength);
			RtlZeroMemory(name.Buffer, name.MaximumLength);

			RtlInitString(&eventnameString, cEventName);
			RtlAnsiStringToUnicodeString(&eventname, &eventnameString, TRUE);

			RtlCopyUnicodeString(&name, &eventbase);
			RtlAppendUnicodeStringToString(&name, &eventname);
			RtlFreeUnicodeString(&eventname);

			pdx->pEvent[i] = IoCreateNotificationEvent(&name, &pdx->eventHandle[i]);

			ExFreePool(name.Buffer);

			if (!pdx->pEvent[i]) {
				for (int j = 0; j < i - 1; ++j) {
					ZwClose(pdx->eventHandle[j]);
					pdx->eventHandle[j] = nullptr;
				}
				status = STATUS_UNSUCCESSFUL;
				Irp->IoStatus.Status = status;
				IoCompleteRequest(Irp, IO_NO_INCREMENT);
				Logger("Failure IoCreateNotificationEvent\n");
				return status;
			}

			KeClearEvent(pdx->pEvent[i]);

		}
#endif

	}
	else {
		WINPCILogger("Failure IoConnectInterruptEx\n");

		Irp->IoStatus.Status = status;
		IoCompleteRequest(Irp, IO_NO_INCREMENT);
		return status;

	}

#endif

	Irp->IoStatus.Status = STATUS_SUCCESS;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);

	return status;
}

NTSTATUS HandleQueryRemoveDevice(PDEVICE_EXTENSION pdx, PIRP Irp) {
	//UNREFERENCED_PARAMETER(Irp);
	WINPCILogger("HandleQueryRemoveDevice\n");
	Irp->IoStatus.Status = STATUS_SUCCESS;
	return  DefaultPnpHandler(pdx, Irp);
}

NTSTATUS HandleRemoveDevice(PDEVICE_EXTENSION pdx, PIRP Irp)
{
	NTSTATUS status;
	DbgPrint("HandleRemoveDevice\n");
	WINPCILogger("HandleRemoveDevice\n");

	IO_DISCONNECT_INTERRUPT_PARAMETERS  Disconnect;

	if (pdx->bInterruptEnable) {
		DbgPrint("IoDisconnectInterruptEx\n");
		pdx->bInterruptEnable = false;
		RtlZeroMemory(&Disconnect, sizeof(IO_DISCONNECT_INTERRUPT_PARAMETERS));

		Disconnect.Version = pdx->IsrType;
		Disconnect.ConnectionContext.InterruptObject = (PKINTERRUPT)pdx->InterruptObject;
		IoDisconnectInterruptEx(&Disconnect);
	}

	pdx->winpci_dma_locker.Lock();

	while (!IsListEmpty(&pdx->winpci_dma_linkListHead))
	{
		DbgPrint("%s: IOCTL_WINPCI_UNALLOCATE_DMA_MEMORY\n", __func__);

		PLIST_ENTRY pEntry = RemoveTailList(&pdx->winpci_dma_linkListHead);
		PMEMORY pDmaMem = CONTAINING_RECORD(pEntry, MEMORY, ListEntry);
		//DbgPrint("Map physical 0x%p to virtual 0x%p, size %u\n", pMapInfo->pvk, pMapInfo->pvu , pMapInfo->memSize );

		MmUnmapLockedPages(pDmaMem->pvu, pDmaMem->pMdl);
		IoFreeMdl(pDmaMem->pMdl);
		pdx->dmaAdapter->DmaOperations->FreeCommonBuffer(
			pdx->dmaAdapter,
			pDmaMem->Length,
			pDmaMem->dmaAddr,
			pDmaMem->pvk,
			FALSE
		);

		ExFreePool(pDmaMem);
	}
	pdx->winpci_dma_locker.UnLock();

	pdx->winpci_mmap_locker.Lock();
	while (!IsListEmpty(&pdx->winpci_mmap_linkListHead))
	{
		DbgPrint("%s: IOCTL_WINPCI_UNMAP_MEMORY\n", __func__);
		PLIST_ENTRY pEntry = RemoveTailList(&pdx->winpci_mmap_linkListHead);
		PMAPINFO pMapInfo = CONTAINING_RECORD(pEntry, MAPINFO, ListEntry);

		//DbgPrint("Map physical 0x%p to virtual 0x%p, size %u\n", pMapInfo->pvk, pMapInfo->pvu , pMapInfo->memSize );

		MmUnmapLockedPages(pMapInfo->pvu, pMapInfo->pMdl);
		IoFreeMdl(pMapInfo->pMdl);
		MmUnmapIoSpace(pMapInfo->pvk, pMapInfo->memSize);

		ExFreePool(pMapInfo);

	}
	pdx->winpci_mmap_locker.UnLock();

	for (int i = 0; i < MAX_EVENT_SZ; ++i) {
		if (pdx->eventHandle[i]) {
			DbgPrint("ZwClose\n");

			ZwClose(pdx->eventHandle[i]);
			pdx->eventHandle[i] = nullptr;
		}
	}

	Irp->IoStatus.Status = STATUS_SUCCESS;
	status = DefaultPnpHandler(pdx, Irp);      

	int devCounter = pdx->DeviceCounter;
	wchar_t  symLinkNameReal[64] = { 0 };
	UNICODE_STRING symLinkName;

	swprintf(symLinkNameReal, L"%s%d", DEVICE_SYMLINKNAME, devCounter);
	RtlInitUnicodeString(&symLinkName, symLinkNameReal);

	status = IoDeleteSymbolicLink(&symLinkName);

	if (NT_SUCCESS(status)) {
		DbgPrint("Success IoDeleteSymbolicLink\n");
	}
	else {
		DbgPrint("Failure IoDeleteSymbolicLink\n");
	}

	if (pdx->NextStackDevice)
	{
		IoDetachDevice(pdx->NextStackDevice);
		pdx->NextStackDevice = nullptr;
	}

	if (pdx->fdo) {
		IoDeleteDevice(pdx->fdo);
		pdx->fdo = nullptr;
	}

	gDeviceCountLocker.Lock();
	gbDeviceNumber[devCounter] = FALSE;		
	gDeviceCountLocker.UnLock();

	return status;
}

NTSTATUS HandleStopDevice(PDEVICE_EXTENSION pdx, PIRP Irp) {
	DbgPrint("HandleStopDevice\n");
	WINPCILogger("HandleStopDevice\n");

	NTSTATUS status;
	IO_DISCONNECT_INTERRUPT_PARAMETERS  Disconnect;

	if (pdx->bInterruptEnable) {
		DbgPrint("IoDisconnectInterruptEx\n");
		pdx->bInterruptEnable = false;
		RtlZeroMemory(&Disconnect, sizeof(IO_DISCONNECT_INTERRUPT_PARAMETERS));

		Disconnect.Version = pdx->IsrType;
		Disconnect.ConnectionContext.InterruptObject = (PKINTERRUPT)pdx->InterruptObject;
		IoDisconnectInterruptEx(&Disconnect);
	}

	pdx->winpci_dma_locker.Lock();

	while (!IsListEmpty(&pdx->winpci_dma_linkListHead))
	{
		DbgPrint("%s: IOCTL_WINPCI_UNALLOCATE_DMA_MEMORY\n", __func__);

		PLIST_ENTRY pEntry = RemoveTailList(&pdx->winpci_dma_linkListHead);
		PMEMORY pDmaMem = CONTAINING_RECORD(pEntry, MEMORY, ListEntry);
		//DbgPrint("Map physical 0x%p to virtual 0x%p, size %u\n", pMapInfo->pvk, pMapInfo->pvu , pMapInfo->memSize );

		MmUnmapLockedPages(pDmaMem->pvu, pDmaMem->pMdl);
		IoFreeMdl(pDmaMem->pMdl);
		pdx->dmaAdapter->DmaOperations->FreeCommonBuffer(
			pdx->dmaAdapter,
			pDmaMem->Length,
			pDmaMem->dmaAddr,
			pDmaMem->pvk,
			FALSE
		);

		ExFreePool(pDmaMem);
	}
	pdx->winpci_dma_locker.UnLock();

	pdx->winpci_mmap_locker.Lock();
	while (!IsListEmpty(&pdx->winpci_mmap_linkListHead))
	{
		DbgPrint("%s: IOCTL_WINPCI_UNMAP_MEMORY\n", __func__);
		PLIST_ENTRY pEntry = RemoveTailList(&pdx->winpci_mmap_linkListHead);
		PMAPINFO pMapInfo = CONTAINING_RECORD(pEntry, MAPINFO, ListEntry);
		//DbgPrint("Map physical 0x%p to virtual 0x%p, size %u\n", pMapInfo->pvk, pMapInfo->pvu , pMapInfo->memSize );

		MmUnmapLockedPages(pMapInfo->pvu, pMapInfo->pMdl);
		IoFreeMdl(pMapInfo->pMdl);
		MmUnmapIoSpace(pMapInfo->pvk, pMapInfo->memSize);

		ExFreePool(pMapInfo);

	}
	pdx->winpci_mmap_locker.UnLock();

	for (int i = 0; i < MAX_EVENT_SZ; ++i) {
		if (pdx->eventHandle[i]) {
			DbgPrint("ZwClose\n");

			ZwClose(pdx->eventHandle[i]);
			pdx->eventHandle[i] = nullptr;
		}
	}

	if (pdx->dmaAdapter) {
		pdx->dmaAdapter->DmaOperations->PutDmaAdapter(pdx->dmaAdapter);
		pdx->dmaAdapter = nullptr;
	}
	
	Irp->IoStatus.Status = STATUS_SUCCESS;
	return DefaultPnpHandler(pdx, Irp);      

}

NTSTATUS HandleQueryStopDevice(PDEVICE_EXTENSION pdx, PIRP Irp) {
	WINPCILogger("HandleQueryStopDevice\n");
	Irp->IoStatus.Status = STATUS_SUCCESS;
	return  DefaultPnpHandler(pdx, Irp);
}

NTSTATUS HandleReadConfig(PDEVICE_EXTENSION pdx, PIRP Irp) {
	DbgPrint("HandleReadConfig\n");
	Irp->IoStatus.Status = STATUS_SUCCESS;
	return  DefaultPnpHandler(pdx, Irp);
}

NTSTATUS HandleWriteConfig(PDEVICE_EXTENSION pdx, PIRP Irp) {
	DbgPrint("HandleWriteConfig\n");
	Irp->IoStatus.Status = STATUS_SUCCESS;
	return  DefaultPnpHandler(pdx, Irp);

}

NTSTATUS HandleSupriseRemoval(PDEVICE_EXTENSION pdx, PIRP Irp) {
	DbgPrint("HandleSupriseRemoval\n");
	Irp->IoStatus.Status = STATUS_SUCCESS;
	return  DefaultPnpHandler(pdx, Irp);

}

NTSTATUS WINPCIPnp(IN PDEVICE_OBJECT fdo, IN PIRP Irp)
{
	NTSTATUS			status = STATUS_SUCCESS;
	PDEVICE_EXTENSION	pdx = (PDEVICE_EXTENSION)fdo->DeviceExtension;
	PIO_STACK_LOCATION	stack = IoGetCurrentIrpStackLocation(Irp);
	static NTSTATUS(*fcntab[])(PDEVICE_EXTENSION pdx, PIRP Irp) =
	{
		HandleStartDevice,  // IRP_MN_START_DEVICE
		HandleQueryRemoveDevice,  // IRP_MN_QUERY_REMOVE_DEVICE
		HandleRemoveDevice, // IRP_MN_REMOVE_DEVICE
		DefaultPnpHandler,  // IRP_MN_CANCEL_REMOVE_DEVICE
		HandleStopDevice,  // IRP_MN_STOP_DEVICE
		HandleQueryStopDevice,  // IRP_MN_QUERY_STOP_DEVICE
		DefaultPnpHandler,  // IRP_MN_CANCEL_STOP_DEVICE

		DefaultPnpHandler,  // IRP_MN_QUERY_DEVICE_RELATIONS
		DefaultPnpHandler,  // IRP_MN_QUERY_INTERFACE
		DefaultPnpHandler,  // IRP_MN_QUERY_CAPABILITIES
		DefaultPnpHandler,  // IRP_MN_QUERY_RESOURCES
		DefaultPnpHandler,  // IRP_MN_QUERY_RESOURCE_REQUIREMENTS
		DefaultPnpHandler,  // IRP_MN_QUERY_DEVICE_TEXT
		DefaultPnpHandler,  // IRP_MN_FILTER_RESOURCE_REQUIREMENTS

		DefaultPnpHandler,  // 0x0E

		HandleReadConfig,  // IRP_MN_READ_CONFIG
		HandleWriteConfig,  // IRP_MN_WRITE_CONFIG
		DefaultPnpHandler,  // IRP_MN_EJECT
		DefaultPnpHandler,  // IRP_MN_SET_LOCK
		DefaultPnpHandler,  // IRP_MN_QUERY_ID
		DefaultPnpHandler,  // IRP_MN_QUERY_PNP_DEVICE_STATE
		DefaultPnpHandler,  // IRP_MN_QUERY_BUS_INFORMATION
		DefaultPnpHandler,  // IRP_MN_DEVICE_USAGE_NOTIFICATION
		HandleSupriseRemoval,  // IRP_MN_SURPRISE_REMOVAL
	};
	static char* fcnname[] =
	{
		"IRP_MN_START_DEVICE",
		"IRP_MN_QUERY_REMOVE_DEVICE",
		"IRP_MN_REMOVE_DEVICE",
		"IRP_MN_CANCEL_REMOVE_DEVICE",
		"IRP_MN_STOP_DEVICE",
		"IRP_MN_QUERY_STOP_DEVICE",
		"IRP_MN_CANCEL_STOP_DEVICE",
		"IRP_MN_QUERY_DEVICE_RELATIONS",
		"IRP_MN_QUERY_INTERFACE",
		"IRP_MN_QUERY_CAPABILITIES",
		"IRP_MN_QUERY_RESOURCES",
		"IRP_MN_QUERY_RESOURCE_REQUIREMENTS",
		"IRP_MN_QUERY_DEVICE_TEXT",
		"IRP_MN_FILTER_RESOURCE_REQUIREMENTS",
		"",
		"IRP_MN_READ_CONFIG",
		"IRP_MN_WRITE_CONFIG",
		"IRP_MN_EJECT",
		"IRP_MN_SET_LOCK",
		"IRP_MN_QUERY_ID",
		"IRP_MN_QUERY_PNP_DEVICE_STATE",
		"IRP_MN_QUERY_BUS_INFORMATION",
		"IRP_MN_DEVICE_USAGE_NOTIFICATION",
		"IRP_MN_SURPRISE_REMOVAL",
	};
	ULONG				fcn = stack->MinorFunction;

	if (fcn >= arraysize(fcntab))
	{
		status = DefaultPnpHandler(pdx, Irp);
		return status;
	}

	status = (*fcntab[fcn])(pdx, Irp);

	return status;
}

NTSTATUS WINPCIDispatchRoutine(IN PDEVICE_OBJECT fdo, IN PIRP Irp)
{
	UNREFERENCED_PARAMETER(fdo);
	Irp->IoStatus.Status = STATUS_SUCCESS;
	Irp->IoStatus.Information = 0;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);

	return STATUS_SUCCESS;
}

NTSTATUS WINPCICleanUp(IN PDEVICE_OBJECT fdo, IN PIRP Irp)
{
	UNREFERENCED_PARAMETER(fdo);
	DbgPrint("WINPCICleanUp\n");

	Irp->IoStatus.Status = STATUS_SUCCESS;
	Irp->IoStatus.Information = 0;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);

	return STATUS_SUCCESS;
}

NTSTATUS WINPCIPower(IN PDEVICE_OBJECT fdo, IN PIRP Irp)
{
	UNREFERENCED_PARAMETER(fdo);
	DbgPrint("WINPCIPower\n");

	Irp->IoStatus.Status = STATUS_SUCCESS;
	Irp->IoStatus.Information = 0;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);

	return STATUS_SUCCESS;
}

NTSTATUS ReadWriteConfigSpace(
	IN PDEVICE_OBJECT DeviceObject,
	IN ULONG ReadOrWrite, // 0 for read 1 for write
	IN PVOID Buffer,
	IN ULONG Offset,
	IN ULONG Length
)
{
	KEVENT				event;
	NTSTATUS			status;
	PIRP				irp;
	IO_STATUS_BLOCK		ioStatusBlock;
	PIO_STACK_LOCATION	irpStack;
	PDEVICE_OBJECT		targetObject;

	//PAGED_CODE();

	KeInitializeEvent(&event, NotificationEvent, FALSE);

	targetObject = IoGetAttachedDeviceReference(DeviceObject);

	irp = IoBuildSynchronousFsdRequest(IRP_MJ_PNP, targetObject, NULL, 0, NULL, &event, &ioStatusBlock);

	if (irp == NULL)
	{
		status = STATUS_INSUFFICIENT_RESOURCES;
		goto End;
	}

	irpStack = IoGetNextIrpStackLocation(irp);

	if (ReadOrWrite == 0)
	{
		irpStack->MinorFunction = IRP_MN_READ_CONFIG;
	}
	else
	{
		irpStack->MinorFunction = IRP_MN_WRITE_CONFIG;
	}

	irpStack->Parameters.ReadWriteConfig.WhichSpace = PCI_WHICHSPACE_CONFIG;
	irpStack->Parameters.ReadWriteConfig.Buffer = Buffer;
	irpStack->Parameters.ReadWriteConfig.Offset = Offset;
	irpStack->Parameters.ReadWriteConfig.Length = Length;

	//
	// Initialize the status to error in case the bus driver does not
	// set it correctly.
	//

	irp->IoStatus.Status = STATUS_NOT_SUPPORTED;

	status = IoCallDriver(targetObject, irp);

	if (status == STATUS_PENDING)
	{
		KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
		status = ioStatusBlock.Status;
	}

End:
	// Done with reference
	ObDereferenceObject(targetObject);

	return status;
}

NTSTATUS WINPCIDeviceControl(IN PDEVICE_OBJECT fdo, IN PIRP irp)
{
	//DbgPrint("Enter WINPCIDeviceControl\n");

	NTSTATUS			status;
	PIO_STACK_LOCATION irpStack;
	PDEVICE_EXTENSION	pdx;
	ULONG dwIoCtlCode;

	pdx = (PDEVICE_EXTENSION)fdo->DeviceExtension;

	irp->IoStatus.Status = STATUS_SUCCESS;
	irp->IoStatus.Information = 0;
	irpStack = IoGetCurrentIrpStackLocation(irp);

	PVOID pSysBuf = (PVOID)irp->AssociatedIrp.SystemBuffer;
	PWINPCI pMem = (PWINPCI)pSysBuf;

	ULONG dwInBufLen = irpStack->Parameters.DeviceIoControl.InputBufferLength;
	ULONG dwOutBufLen = irpStack->Parameters.DeviceIoControl.OutputBufferLength;

	PIO_INTERRUPT_MESSAGE_INFO p;
	PIO_INTERRUPT_MESSAGE_INFO_ENTRY pp;

	switch (irpStack->MajorFunction)
	{

	case IRP_MJ_DEVICE_CONTROL:

		dwIoCtlCode = irpStack->Parameters.DeviceIoControl.IoControlCode;

		switch (dwIoCtlCode) {
		case IOCTL_WINPCI_MAP_MEMORY:

			if (dwInBufLen == sizeof(WINPCI) && dwOutBufLen == sizeof(WINPCI))
			{
				PHYSICAL_ADDRESS phyAddr;
				PVOID pvk, pvu;

				phyAddr.QuadPart = (ULONGLONG)pMem->phyAddr;

				//get mapped kernel address
				pvk = MmMapIoSpace(phyAddr, pMem->dwSize, MmNonCached);

				if (pvk)
				{
					//allocate mdl for the mapped kernel address
					PMDL pMdl = IoAllocateMdl(pvk, pMem->dwSize, FALSE, FALSE, NULL);
					if (pMdl)
					{
						PMAPINFO pMapInfo;

						//build mdl and map to user space
						MmBuildMdlForNonPagedPool(pMdl);

						pvu = MmMapLockedPagesSpecifyCache(pMdl, UserMode, MmNonCached, NULL, FALSE, NormalPagePriority);

						if (pvu) {
							//insert mapped infomation to list
							pMapInfo = (PMAPINFO)ExAllocatePool(NonPagedPool, sizeof(MAPINFO));
							pMapInfo->pMdl = pMdl;
							pMapInfo->pvk = pvk;
							pMapInfo->pvu = pvu;
							pMapInfo->memSize = pMem->dwSize;

							pdx->winpci_mmap_locker.Lock();
							InsertHeadList(&pdx->winpci_mmap_linkListHead, &pMapInfo->ListEntry);
							pdx->winpci_mmap_locker.UnLock();

							WINPCI   mem;
							mem.phyAddr = (PVOID)phyAddr.QuadPart;
							mem.pvu = pvu;
							mem.dwSize = pMem->dwSize;

							RtlCopyMemory(pSysBuf, &mem, sizeof(WINPCI));

							irp->IoStatus.Information = sizeof(WINPCI);

						}
						else {
							IoFreeMdl(pMdl);
							MmUnmapIoSpace(pvk, pMem->dwSize);
							irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
						}
					}
					else
					{
						//allocate mdl error, unmap the mapped physical memory
						MmUnmapIoSpace(pvk, pMem->dwSize);
						irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
					}
				}
				else{
					irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
				}
			}
			else{
				irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
			}
			
			break;

		case IOCTL_WINPCI_UNMAP_MEMORY:

			//DbgPrint("IOCTL_WINPCI_UNMAP\n");
			if (dwInBufLen == sizeof(WINPCI))
			{
				PMAPINFO pMapInfo;
				PLIST_ENTRY pLink;

				//initialize to head
				pLink = pdx->winpci_mmap_linkListHead.Flink;

				while (pLink)
				{
					pMapInfo = CONTAINING_RECORD(pLink, MAPINFO, ListEntry);

					if (pMapInfo->pvu == pMem->pvu)
					{
						if (pMapInfo->memSize == pMem->dwSize)
						{
							//free mdl, unmap mapped memory
							MmUnmapLockedPages(pMapInfo->pvu, pMapInfo->pMdl);
							IoFreeMdl(pMapInfo->pMdl);
							MmUnmapIoSpace(pMapInfo->pvk, pMapInfo->memSize);

							pdx->winpci_mmap_locker.Lock();
							RemoveEntryList(&pMapInfo->ListEntry);
							pdx->winpci_mmap_locker.UnLock();

							ExFreePool(pMapInfo);
						}
						else {
							irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
						}
						break;
					}
					pLink = pLink->Flink;
				}
			}
			else{
				irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
			}
			
			break;

		case IOCTL_WINPCI_ALLOCATE_DMA_MEMORY:
			DbgPrint("Enter  IOCTL_WINPCI_ALLOCATE_DMA_MEMORY\n");

			if (dwInBufLen == sizeof(WINPCI) && dwOutBufLen == sizeof(WINPCI))
			{
				PVOID pvu = nullptr;
				PVOID pvk = nullptr;
				PHYSICAL_ADDRESS phyAddr;

				if (pdx->dmaAdapter) {
#if 1
					pvk = pdx->dmaAdapter->DmaOperations->AllocateCommonBuffer(
						pdx->dmaAdapter,
						pMem->dwSize,
						&phyAddr,
						FALSE
					);
#else
					pvk = pdx->dmaAdapter->DmaOperations->AllocateCommonBufferEx(
						pdx->dmaAdapter,
						nullptr,
						pMem->dwSize,
						&phyAddr,
						FALSE,
						KeGetCurrentNodeNumber()
					);
#endif
				}
				else {
					irp->IoStatus.Status = STATUS_UNSUCCESSFUL;
					break;
				}

				if (pvk)
				{
					//allocate mdl for the mapped kernel address
					PMDL pMdl = IoAllocateMdl(pvk, pMem->dwSize, FALSE, FALSE, NULL);
					if (pMdl)
					{
						PMEMORY pDmaMapInfo;

						//build mdl and map to user space
						MmBuildMdlForNonPagedPool(pMdl);

						pvu = MmMapLockedPagesSpecifyCache(pMdl, UserMode, MmNonCached, NULL, FALSE, NormalPagePriority);

						if (pvu) {
							//insert mapped infomation to list
							pDmaMapInfo = (PMEMORY)ExAllocatePool(NonPagedPool, sizeof(MEMORY));
							pDmaMapInfo->pMdl = pMdl;
							pDmaMapInfo->pvk = pvk;
							pDmaMapInfo->pvu = pvu;
						
							pDmaMapInfo->dmaAddr.QuadPart = phyAddr.QuadPart;
							pDmaMapInfo->Length = pMem->dwSize;

							pdx->winpci_dma_locker.Lock();
							InsertHeadList(&pdx->winpci_dma_linkListHead, &pDmaMapInfo->ListEntry);
							pdx->winpci_dma_locker.UnLock();

							//DbgPrint("phy addr: 0x%llx\n", phyAddr.QuadPart);

							WINPCI   mem;
							mem.phyAddr = (PVOID)phyAddr.QuadPart;
							mem.pvu = pvu;
							mem.dwSize = pMem->dwSize;

							RtlCopyMemory(pSysBuf, &mem, sizeof(WINPCI));

							irp->IoStatus.Information = sizeof(WINPCI);
							DbgPrint("Leave IOCTL_WINPCI_ALLOCATE_DMA_MEMORY\n");

						}
						else {
							IoFreeMdl(pMdl);
							pdx->dmaAdapter->DmaOperations->FreeCommonBuffer(
								pdx->dmaAdapter,
								pMem->dwSize,
								phyAddr,
								pvk,
								FALSE
							);
							//pdx->dmaAdapter->DmaOperations->PutDmaAdapter(pdx->dmaAdapter);
							irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
						}
					}
					else
					{
						//allocate mdl error, unmap the mapped physical memory
						pdx->dmaAdapter->DmaOperations->FreeCommonBuffer(
							pdx->dmaAdapter,
							pMem->dwSize,
							phyAddr,
							pvk,
							FALSE
						);
						irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
					}
				}
				else {
					irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
				}
			}
			else{
				irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
			}
			
			break;

		case IOCTL_WINPCI_UNALLOCATE_DMA_MEMORY:
			DbgPrint("Enter  IOCTL_WINPCI_UNALLOCATE_DMA_MEMORY\n");

			if (dwInBufLen == sizeof(WINPCI))
			{
				PMEMORY pDmaMapInfo;
				PLIST_ENTRY pLink = pdx->winpci_dma_linkListHead.Flink;

				while (pLink)
				{
					pDmaMapInfo = CONTAINING_RECORD(pLink, MEMORY, ListEntry);

					if (pDmaMapInfo->pvu == pMem->pvu)
					{
						if (pDmaMapInfo->Length == pMem->dwSize)
						{
							//free mdl, unmap mapped memory
							MmUnmapLockedPages(pDmaMapInfo->pvu, pDmaMapInfo->pMdl);
							IoFreeMdl(pDmaMapInfo->pMdl);

							pdx->dmaAdapter->DmaOperations->FreeCommonBuffer(
								pdx->dmaAdapter,
								pDmaMapInfo->Length,
								pDmaMapInfo->dmaAddr,
								pDmaMapInfo->pvk,
								FALSE
							);

							pdx->winpci_dma_locker.Lock();
							RemoveEntryList(&pDmaMapInfo->ListEntry);
							pdx->winpci_dma_locker.UnLock();

							ExFreePool(pDmaMapInfo);
							DbgPrint("Leave IOCTL_WINPCI_UNALLOCATE_DMA_MEMORY\n");
						}
						else {
							irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
						}
						break;
					}
					pLink = pLink->Flink;
				}
			}
			else
				irp->IoStatus.Status = STATUS_INVALID_PARAMETER;

			break;

		case IOCTL_WINPCI_GET_BDF:
			BDF   bdf;
			bdf.bus = pdx->bus;
			bdf.dev = pdx->dev;
			bdf.func = pdx->func;

			RtlCopyMemory(pSysBuf, &bdf, sizeof(BDF));

			irp->IoStatus.Information = sizeof(BDF);
		
			break;

		case IOCTL_WINPCI_CREATE_EVENT:
			p = (PIO_INTERRUPT_MESSAGE_INFO)pdx->InterruptObject;
			pp = p->MessageInfo;

			for (int i = 0; i < (int)p->MessageCount; ++i) {
				if (i == MAX_EVENT_SZ)break;
				DbgPrint("IoConnectInterruptEx params ===> Irql:%X, Vector:%X, Proc:%llX, MessageData:%lX, MessageAddress:%lX\n",
					(pp + i)->Irql,
					(pp + i)->Vector,
					(pp + i)->TargetProcessorSet,
					(pp + i)->MessageData,
					(pp + i)->MessageAddress.LowPart
				);
				
				UNICODE_STRING name;
				UNICODE_STRING eventbase;
				UNICODE_STRING eventname;
				STRING eventnameString;

				char cEventName[EVENTNAMEMAXLEN] = { 0 };

				sprintf(cEventName, "Device%dEvent%d", pdx->DeviceCounter, i);

				RtlInitUnicodeString(&eventbase, L"\\BaseNamedObjects\\");

				name.MaximumLength = EVENTNAMEMAXLEN + eventbase.Length;
				name.Length = 0;
				name.Buffer = (PWCH)ExAllocatePool(NonPagedPool, name.MaximumLength);
				RtlZeroMemory(name.Buffer, name.MaximumLength);

				RtlInitString(&eventnameString, cEventName);
				RtlAnsiStringToUnicodeString(&eventname, &eventnameString, TRUE);

				RtlCopyUnicodeString(&name, &eventbase);
				RtlAppendUnicodeStringToString(&name, &eventname);
				RtlFreeUnicodeString(&eventname);

				pdx->pEvent[i] = IoCreateNotificationEvent(&name, &pdx->eventHandle[i]);

				ExFreePool(name.Buffer);

				if (!pdx->pEvent[i]) {
					for (int j = 0; j < i - 1; ++j) {
						ZwClose(pdx->eventHandle[j]);
						pdx->eventHandle[j] = nullptr;
					}
					WINPCILogger("Failure IoCreateNotificationEvent\n");
					irp->IoStatus.Status = STATUS_UNSUCCESSFUL;
					break;
				}
				KeClearEvent(pdx->pEvent[i]);
			}
			break;
		case IOCTL_WINPCI_DELETE_EVENT:
			for (int i = 0; i < MAX_EVENT_SZ; ++i) {
				if (pdx->eventHandle[i]) {
					ZwClose(pdx->eventHandle[i]);
					pdx->eventHandle[i] = nullptr;
				}
			}
			break;
		default:
			break;
		}
	}

	status = irp->IoStatus.Status;

	IoCompleteRequest(irp, IO_NO_INCREMENT);
	return status;
}

void WINPCIUnload(IN PDRIVER_OBJECT DriverObject)
{
	UNREFERENCED_PARAMETER(DriverObject);
	DbgPrint("WINPCIUnload\n");
}

void WINPCIDelay(long long millsecond)
{
	LARGE_INTEGER	delayValue, delayTrue;
	NTSTATUS		ntRet;

	// 10*1000*1000 is 1 second, so 10*1000 is 1 millsecond
	delayValue.QuadPart = 10 * 1000 * millsecond; // 320 millisecond
	delayTrue.QuadPart = -(delayValue.QuadPart);
	ntRet = KeDelayExecutionThread(KernelMode, FALSE, &delayTrue);
}

VOID WINPCILogger(char* text) {

	UNICODE_STRING     uniName;
	OBJECT_ATTRIBUTES  objAttr;

	RtlInitUnicodeString(&uniName, L"\\SystemRoot\\winpcilog.txt");
	InitializeObjectAttributes(&objAttr, &uniName,
		OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
		NULL, NULL);

	HANDLE   handle;
	NTSTATUS ntstatus;
	IO_STATUS_BLOCK    ioStatusBlock;

	// Do not try to perform any file operations at higher IRQL levels.
	// Instead, you may use a work item or a system worker thread to perform file operations.

	if (KeGetCurrentIrql() != PASSIVE_LEVEL) return;
	//return STATUS_INVALID_DEVICE_STATE;

	ntstatus = ZwCreateFile(
		&handle,
		GENERIC_WRITE,
		&objAttr,
		&ioStatusBlock, 
		nullptr,
		FILE_ATTRIBUTE_NORMAL,
		0,
		FILE_OPEN_IF,
		FILE_SYNCHRONOUS_IO_NONALERT,
		nullptr,
		0
	);

	CHAR     buffer[BUFFER_SIZE];
	size_t  cb;

	if (NT_SUCCESS(ntstatus)) {
		ntstatus = RtlStringCbPrintfA(buffer, sizeof(buffer), text, 0x0);
		if (NT_SUCCESS(ntstatus)) {
			ntstatus = RtlStringCbLengthA(buffer, sizeof(buffer), &cb);
			if (NT_SUCCESS(ntstatus)) {

				FILE_STANDARD_INFORMATION info;
				IO_STATUS_BLOCK IoStatus;

				ntstatus = ZwQueryInformationFile(handle, &IoStatus, &info, sizeof(info), FileStandardInformation);

				//DbgPrint("file size: %d\n", info.EndOfFile.QuadPart);
				if (NT_SUCCESS(ntstatus)) {
					ntstatus = ZwWriteFile(
						handle, 
						nullptr,
						nullptr,
						nullptr,
						&ioStatusBlock,
						buffer,
						(ULONG)cb, 
						&info.EndOfFile, 
						nullptr
					);
				}
			}
		}
		ZwClose(handle);
	}
}
