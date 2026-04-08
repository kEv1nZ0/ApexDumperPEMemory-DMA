# APEX LEGENDS DUMPER (DMA VERSION)

Dump PE Memory —— **DMA 版本**，无需在目标机加载内核驱动，通过 PCILeech / FPGA 等 DMA 设备读取内存。

本项目基于 [vortxcore/ApexDumperPEMemory](https://github.com/vortxcore/ApexDumperPEMemory) 改造，把原版的内核驱动读写替换为 DMA (LeechCore / VMM) 读写。

> 仅供学习与研究使用。

## 使用方法

1. 连接好你的 DMA 设备，目标机运行 Apex Legends (`r5apex_dx12.exe`)。
2. 管理员运行 `ApexPEDumper.exe`。
3. 进游戏靶场，左右移动、开枪、拾取物品。然后去匹配中正常玩一局，确保 dumper 拿到所有 offsets。
4. 完成后会在软件当前目录生成 `game_dumped.exe`。
5. 把 `game_dumped.exe` 上传到 https://dump.ccdescipline.cfd 。
6. ENJOY !

## 编译

- Visual Studio 2022，`Release | x64`
- 打开 `ApexPEDumper.slnx` 或 `Project1.vcxproj` 编译
- 把 LeechCore/VMM 的运行时 DLL (`leechcore.dll`, `vmm.dll`, `FTD3XX.dll` 等) 放到生成目录

## 致谢

- 原项目: [vortxcore/ApexDumperPEMemory](https://github.com/vortxcore/ApexDumperPEMemory)
- DMA: [ufrisk/MemProcFS](https://github.com/ufrisk/MemProcFS)
