# APEX LEGENDS DUMPER (DMA VERSION)

Dump PE Memory —— **DMA version**. No kernel driver needed on the target machine; memory is read through a DMA device (PCILeech / FPGA, etc.).

This project is a DMA-based fork of [vortxcore/ApexDumperPEMemory](https://github.com/vortxcore/ApexDumperPEMemory), replacing the original kernel-driver read/write with DMA (LeechCore / VMM).

> For educational and research use only.

## Usage

1. Connect your DMA device and run Apex Legends (`r5apex_dx12.exe`) on the target machine.
2. Run `ApexPEDumper.exe` as Administrator.
3. Go into the firing range, move left/right, shoot, pick up items. Then play a normal match to make sure the dumper captures all offsets.
4. `game_dumped.exe` will be generated in the program's working directory.
5. Upload `game_dumped.exe` to https://dump.ccdescipline.cfd .
6. ENJOY !

## Build

- Visual Studio 2022, `Release | x64`
- Open `ApexPEDumper.slnx` or `Project1.vcxproj` and build
- Copy the LeechCore/VMM runtime DLLs (`leechcore.dll`, `vmm.dll`, `FTD3XX.dll`, etc.) into the output directory

## Credits

- Original project: [vortxcore/ApexDumperPEMemory](https://github.com/vortxcore/ApexDumperPEMemory)
- DMA: [ufrisk/MemProcFS](https://github.com/ufrisk/MemProcFS)
