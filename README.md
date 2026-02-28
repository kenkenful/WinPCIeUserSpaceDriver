## WinPCIeUserSpaceDriver 
PCIe User Space driver on Windows

### Building Environment
- Visual Studio 2019
- Windows Software Development Kit 10.0.19041.685
- Windows Driver Kit 10.0.19041.685

###  Driver Install and UnInstall

Enter test mode and reboot OS.  
```
bcdedit.exe -set TESTSIGNING ON
```

Save **devcon.exe, winpci.sys, winpci.inf, winpci.cat** into one folder.  

Launch the command prompt in administrator mode and move to the above folder.  

Example for NVMe.
 
- Install Driver  
  ```
  devcon.exe update winpci.inf "PCI\CC_010802"
  ```
  or
  ```
  devcon.exe update winpci.inf "Hardware ID"
   #HWID can be checked from Device Manager.
  ```
- UnInstall Driver
  ```
  devcon.exe remove "PCI\CC_010802"
  ```
- Search oem file:
  ```
  devcon.exe  driverfiles "PCI\CC_010802"
  ```
- Delete oem file(Edit the OEM file name to suit your environment.)
  ```
  devcon.exe dp_delete oem24.inf    
  ``` 
- Scan Hardware(Do this before installing winpci.sys driver again.)
  ```
  devcon.exe rescan
  ```

###  Driver force UnInstall
``` 
pnputil /enum-drivers | Select-String "winpci.inf" -Context 5
pnputil /delete-driver oemNN.inf /uninstall /force
```

### How to create application
This driver does not initialize the device, so the application must do so.  
Map the PCI Config register and device registers to User Space, set the registers appropriately, and initialize the device. 

Please refer to test.cpp.
Target device is specified by PCIe Bus/Device/Function.
  ```
  test.exe 04:00.0
  ```

### Note
Do not install this driver on system disk.  
If a BSoD occurs when the OS starts due to some kind of bug, remove the device.  
Since the driver is not attached, BSoD can be avoided.  
After the OS starts normally, delete the driver and inf files.


