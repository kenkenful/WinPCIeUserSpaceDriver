#pragma once

#define	FILE_DEVICE_WINPCI		0x8000

#define	IOCTL_WINPCI_MAP_MEMORY									CTL_CODE(FILE_DEVICE_WINPCI, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define	IOCTL_WINPCI_UNMAP_MEMORY								CTL_CODE(FILE_DEVICE_WINPCI, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define	IOCTL_WINPCI_ALLOCATE_DMA_MEMORY				CTL_CODE(FILE_DEVICE_WINPCI, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define	IOCTL_WINPCI_UNALLOCATE_DMA_MEMORY			CTL_CODE(FILE_DEVICE_WINPCI, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define	IOCTL_WINPCI_GET_BDF												CTL_CODE(FILE_DEVICE_WINPCI, 0x804,	METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct tagWINPCI
{
	PVOID phyAddr;			// physical Address for map
	PVOID pvu;					// user space virtual address for unmap
	ULONG dwSize;				// memory size to map or unmap
	ULONG dwRegOff;		// register offset: 0-255
	ULONG dwBytes;			// bytes to read or write
} WINPCI, * PWINPCI;

typedef struct tagBDF {
	ULONG						bus;
	USHORT						dev;
	USHORT						func;
}BDF, *PBDF;