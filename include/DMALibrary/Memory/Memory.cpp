#include "Memory.h"
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <chrono>
#include <ctime>
#include <fstream>

#include <thread>
#include <iostream>
#pragma execution_character_set("utf-8")
Memory::Memory()
{
	LOG("loading libraries...\n");
	modules.VMM = LoadLibraryA("vmm.dll");
	modules.FTD3XX = LoadLibraryA("FTD3XX.dll");
	modules.LEECHCORE = LoadLibraryA("leechcore.dll");

	if (!modules.VMM || !modules.FTD3XX || !modules.LEECHCORE)
	{
		LOG("vmm: %p\n", modules.VMM);
		LOG("ftd: %p\n", modules.FTD3XX);
		LOG("leech: %p\n", modules.LEECHCORE);
		THROW("[!] Could not load a library\n");
	}

	this->key = std::make_shared<c_keys>();

	LOG("Successfully loaded libraries!\n");
}

Memory::~Memory()
{
	VMMDLL_Close(this->vHandle);
	DMA_INITIALIZED = false;
	PROCESS_INITIALIZED = false;
}

bool Memory::DumpMemoryMap(bool debug)
{
	LPCSTR args[] = { const_cast<LPCSTR>(""), const_cast<LPCSTR>("-device"), const_cast<LPCSTR>("fpga://algo=0"), const_cast<LPCSTR>(""), const_cast<LPCSTR>("") };
	int argc = 3;
	if (debug)
	{
		args[argc++] = const_cast<LPCSTR>("-v");
		args[argc++] = const_cast<LPCSTR>("-printf");
	}

	VMM_HANDLE handle = VMMDLL_Initialize(argc, args);
	if (!handle)
	{
		LOG("[!] Failed to open a VMM Handle\n");
		return false;
	}

	PVMMDLL_MAP_PHYSMEM pPhysMemMap = NULL;
	if (!VMMDLL_Map_GetPhysMem(handle, &pPhysMemMap))
	{
		LOG("[!] Failed to get physical memory map\n");
		VMMDLL_Close(handle);
		return false;
	}

	if (pPhysMemMap->dwVersion != VMMDLL_MAP_PHYSMEM_VERSION)
	{
		LOG("[!] Invalid VMM Map Version\n");
		VMMDLL_MemFree(pPhysMemMap);
		VMMDLL_Close(handle);
		return false;
	}

	if (pPhysMemMap->cMap == 0)
	{
		printf("[!] Failed to get physical memory map\n");
		VMMDLL_MemFree(pPhysMemMap);
		VMMDLL_Close(handle);
		return false;
	}
	//Dump map to file
	std::stringstream sb;
	for (DWORD i = 0; i < pPhysMemMap->cMap; i++)
	{
		sb << std::hex << pPhysMemMap->pMap[i].pa << " " << (pPhysMemMap->pMap[i].pa + pPhysMemMap->pMap[i].cb - 1) << std::endl;
	}

	auto temp_path = std::filesystem::temp_directory_path();
	std::ofstream nFile(temp_path.string() + "\\mmap.txt");
	nFile << sb.str();
	nFile.close();

	VMMDLL_MemFree(pPhysMemMap);
	LOG("Successfully dumped memory map to file!\n");
	//Little sleep to make sure it's written to file.
	Sleep(3000);
	VMMDLL_Close(handle);
	return true;
}

unsigned char abort2[4] = { 0x10, 0x00, 0x10, 0x00 };

bool Memory::SetFPGA()
{
	ULONG64 qwID = 0, qwVersionMajor = 0, qwVersionMinor = 0;
	if (!VMMDLL_ConfigGet(this->vHandle, LC_OPT_FPGA_FPGA_ID, &qwID) && VMMDLL_ConfigGet(this->vHandle, LC_OPT_FPGA_VERSION_MAJOR, &qwVersionMajor) && VMMDLL_ConfigGet(this->vHandle, LC_OPT_FPGA_VERSION_MINOR, &qwVersionMinor))
	{
		LOG("[!] Failed to lookup FPGA device, Attempting to proceed\n\n");
		return false;
	}

	if ((qwVersionMajor >= 4) && ((qwVersionMajor >= 5) || (qwVersionMinor >= 7)))
	{
		HANDLE handle;
		LC_CONFIG config = { .dwVersion = LC_CONFIG_VERSION, .szDevice = "existing" };
		handle = LcCreate(&config);

		if (!handle)
		{
			LOG("[!] Failed to create FPGA device\n");
			return false;
		}

		LcCommand(handle, LC_CMD_FPGA_CFGREGPCIE_MARKWR | 0x002, 4, reinterpret_cast<PBYTE>(&abort2), NULL, NULL);
		LOG("[-] Register auto cleared\n");
		LcClose(handle);
	}

	return true;
}

bool Memory::Init(std::string process_name, bool memMap, bool debug)
{
	//清理一次就可以了应该
	CleanMmap();
	if (!DMA_INITIALIZED)
	{
		LOG("正在初始化DMA，如果等待过久请重新打开软件。\nInit DMA,if stuck here for a while,please restart cheat.\n");
	reinit:
		LPCSTR args[] = { const_cast<LPCSTR>(""), const_cast<LPCSTR>("-device"), const_cast<LPCSTR>("fpga://algo=0"), const_cast<LPCSTR>(""), const_cast<LPCSTR>(""), const_cast<LPCSTR>(""), const_cast<LPCSTR>("") };
		DWORD argc = 3;
		if (debug)
		{
			args[argc++] = const_cast<LPCSTR>("-v");
			args[argc++] = const_cast<LPCSTR>("-printf");
		}

		std::string path = "";
		if (memMap)
		{
			auto temp_path = std::filesystem::temp_directory_path();
			path = (temp_path.string() + "\\mmap.txt");
			bool dumped = false;
			if (!std::filesystem::exists(path))
				dumped = this->DumpMemoryMap(debug);
			else
				dumped = true;
			LOG("dumping memory map to file...\n");
			if (!dumped)
			{
				LOG("[!] ERROR: Could not dump memory map!\n");
				LOG("Defaulting to no memory map!\n");
			}
			else
			{
				LOG("Dumped memory map!\n");

				//Add the memory map to the arguments and increase arg count.
				args[argc++] = const_cast<LPSTR>("-memmap");
				args[argc++] = const_cast<LPSTR>(path.c_str());
			}
		}

		args[argc++] = const_cast<LPSTR>("-norefresh");
		this->vHandle = VMMDLL_Initialize(argc, args);

		if (!this->vHandle)
		{
			if (memMap)
			{
				memMap = false;
				LOG("[!] Initialization failed with Memory map, retrying without MMap\n");
				goto reinit;
			}
			LOG("[!] Initialization failed! Is the DMA in use or disconnected?\n");
			return false;
		}

		ULONG64 FPGA_ID = 0, DEVICE_ID = 0;

		VMMDLL_ConfigGet(this->vHandle, LC_OPT_FPGA_FPGA_ID, &FPGA_ID);
		VMMDLL_ConfigGet(this->vHandle, LC_OPT_FPGA_DEVICE_ID, &DEVICE_ID);

		if (!this->SetFPGA())
		{
			LOG("[!] Could not set FPGA!\n");
			VMMDLL_Close(this->vHandle);
			return false;
		}

		//VMMDLL_ConfigSet(this->vHandle, LC_OPT_FPGA_MAX_SIZE_RX, 0x1000);    // 最大接收大小
		//VMMDLL_ConfigSet(this->vHandle, LC_OPT_FPGA_MAX_SIZE_TX, 0x1000);    // 最大发送大小
		//VMMDLL_ConfigSet(this->vHandle, LC_OPT_FPGA_DELAY_READ, 0);          // 最小化读取延迟
		//VMMDLL_ConfigSet(this->vHandle, LC_OPT_FPGA_RETRY_ON_ERROR, 1);      // 错误时自动重试


		//if (FPGA_ID >= 4) {
		//	// 现代FPGA支持高级算法
		//	VMMDLL_ConfigSet(this->vHandle, LC_OPT_FPGA_ALGO_TINY, 1);           // 使用tiny算法提高效率
		//}
		//else {
		//	// 旧款FPGA可能需要其他设置
		//	VMMDLL_ConfigSet(this->vHandle, LC_OPT_FPGA_ALGO_SYNCHRONOUS, 1);    // 使用同步算法
		//}

		//// 添加这些配置可以进一步优化性能
		//VMMDLL_ConfigSet(this->vHandle, VMMDLL_OPT_CONFIG_READCACHE_TICKS, 5);       // 减少缓存生命周期
		//VMMDLL_ConfigSet(this->vHandle, VMMDLL_OPT_CONFIG_TICK_PERIOD, 20);          // 更短的tick周期
		//VMMDLL_ConfigSet(this->vHandle, VMMDLL_OPT_CONFIG_TLBCACHE_TICKS, 10);       // 减少TLB缓存时间
		//VMMDLL_ConfigSet(this->vHandle, VMMDLL_OPT_CORE_PRINTF_ENABLE, 0);           // 禁用打印提高性能

		DMA_INITIALIZED = TRUE;
	}
	else
		LOG("DMA already initialized!\n");

	if (PROCESS_INITIALIZED)
	{
		LOG("Process already initialized!\n");
		return true;
	}

	current_process.PID = GetPidFromName(process_name);
	if (!current_process.PID)
	{
		LOG("[!] Could not get PID from name!\n");
		return false;
	}
	current_process.process_name = process_name;
	while (true)
	{
		if (!mem.FixCr3())
			std::cout << "Failed to fix CR3" << std::endl;
		else
		{
			std::cout << "CR3 fixed" << std::endl;
			VMMDLL_ConfigSet(mem.vHandle, VMMDLL_OPT_REFRESH_ALL, 1);
			break;
		}
		
		Sleep(1000);
	}


	current_process.base_address = GetBaseDaddy(process_name);
	if (!current_process.base_address)
	{
		LOG("[!] Could not get base address!\n");
		return false;
	}
	

	current_process.base_size = GetBaseSize(process_name);
	if (!current_process.base_size)
	{
		LOG("[!] Could not get base size!\n");
		return false;
	}

	mem.OFF_BASE = current_process.base_address;

	PROCESS_INITIALIZED = TRUE;

	return true;
}

DWORD Memory::GetPidFromName(std::string process_name)
{
	DWORD pid = 0;
	VMMDLL_PidGetFromName(this->vHandle, (LPSTR)process_name.c_str(), &pid);
	return pid;
}

std::vector<int> Memory::GetPidListFromName(std::string name)
{
	PVMMDLL_PROCESS_INFORMATION process_info = NULL;
	DWORD total_processes = 0;
	std::vector<int> list = { };

	if (!VMMDLL_ProcessGetInformationAll(this->vHandle, &process_info, &total_processes))
	{
		LOG("[!] Failed to get process list\n");
		return list;
	}

	for (size_t i = 0; i < total_processes; i++)
	{
		auto process = process_info[i];
		if (strstr(process.szNameLong, name.c_str()))
			list.push_back(process.dwPID);
	}

	return list;
}

std::vector<std::string> Memory::GetModuleList(std::string process_name)
{
	std::vector<std::string> list = { };
	PVMMDLL_MAP_MODULE module_info = NULL;
	if (!VMMDLL_Map_GetModuleU(this->vHandle, current_process.PID, &module_info, VMMDLL_MODULE_FLAG_NORMAL))
	{
		LOG("[!] Failed to get module list\n");
		return list;
	}

	for (size_t i = 0; i < module_info->cMap; i++)
	{
		auto module = module_info->pMap[i];
		list.push_back(module.uszText);
	}

	return list;
}

VMMDLL_PROCESS_INFORMATION Memory::GetProcessInformation()
{
	VMMDLL_PROCESS_INFORMATION info = { };
	SIZE_T process_information = sizeof(VMMDLL_PROCESS_INFORMATION);
	ZeroMemory(&info, sizeof(VMMDLL_PROCESS_INFORMATION));
	info.magic = VMMDLL_PROCESS_INFORMATION_MAGIC;
	info.wVersion = VMMDLL_PROCESS_INFORMATION_VERSION;

	if (!VMMDLL_ProcessGetInformation(this->vHandle, current_process.PID, &info, &process_information))
	{
		LOG("[!] Failed to find process information\n");
		return { };
	}

	LOG("[+] Found process information\n");
	return info;
}

PEB Memory::GetProcessPeb()
{
	auto info = GetProcessInformation();
	if (info.win.vaPEB)
	{
		LOG("[+] Found process PEB ptr at 0x%p\n", info.win.vaPEB);
		return Read<PEB>(info.win.vaPEB);
	}
	LOG("[!] Failed to find the processes PEB\n");
	return { };
}

size_t Memory::GetBaseDaddy(std::string module_name)
{
	std::wstring str(module_name.begin(), module_name.end());

	PVMMDLL_MAP_MODULEENTRY module_info;
	if (!VMMDLL_Map_GetModuleFromNameW(this->vHandle, current_process.PID, const_cast<LPWSTR>(str.c_str()), &module_info, VMMDLL_MODULE_FLAG_NORMAL))
	{
		LOG("[!] Couldn't find Base Address for %s\n", module_name.c_str());
		return 0;
	}

	LOG("[+] Found Base Address for %s at 0x%p\n", module_name.c_str(), module_info->vaBase);
	return module_info->vaBase;
}

size_t Memory::GetBaseSize(std::string module_name)
{
	std::wstring str(module_name.begin(), module_name.end());

	PVMMDLL_MAP_MODULEENTRY module_info;
	auto bResult = VMMDLL_Map_GetModuleFromNameW(this->vHandle, current_process.PID, const_cast<LPWSTR>(str.c_str()), &module_info, VMMDLL_MODULE_FLAG_NORMAL);
	if (bResult)
	{
		LOG("[+] Found Base Size for %s at 0x%p\n", module_name.c_str(), module_info->cbImageSize);
		return module_info->cbImageSize;
	}
	return 0;
}

uintptr_t Memory::GetExportTableAddress(std::string import, std::string process, std::string module)
{
	PVMMDLL_MAP_EAT eat_map = NULL;
	PVMMDLL_MAP_EATENTRY export_entry = NULL;
	bool result = VMMDLL_Map_GetEATU(mem.vHandle, mem.GetPidFromName(process) /*| VMMDLL_PID_PROCESS_WITH_KERNELMEMORY*/, const_cast<LPSTR>(module.c_str()), &eat_map);
	if (!result)
	{
		LOG("[!] Failed to get Export Table\n");
		return 0;
	}

	if (eat_map->dwVersion != VMMDLL_MAP_EAT_VERSION)
	{
		VMMDLL_MemFree(eat_map);
		eat_map = NULL;
		LOG("[!] Invalid VMM Map Version\n");
		return 0;
	}

	uintptr_t addr = 0;
	for (int i = 0; i < eat_map->cMap; i++)
	{
		export_entry = eat_map->pMap + i;
		if (strcmp(export_entry->uszFunction, import.c_str()) == 0)
		{
			addr = export_entry->vaFunction;
			break;
		}
	}

	VMMDLL_MemFree(eat_map);
	eat_map = NULL;

	return addr;
}

uintptr_t Memory::GetImportTableAddress(std::string import, std::string process, std::string module)
{
	PVMMDLL_MAP_IAT iat_map = NULL;
	PVMMDLL_MAP_IATENTRY import_entry = NULL;
	bool result = VMMDLL_Map_GetIATU(mem.vHandle, mem.GetPidFromName(process) /*| VMMDLL_PID_PROCESS_WITH_KERNELMEMORY*/, const_cast<LPSTR>(module.c_str()), &iat_map);
	if (!result)
	{
		LOG("[!] Failed to get Import Table\n");
		return 0;
	}

	if (iat_map->dwVersion != VMMDLL_MAP_IAT_VERSION)
	{
		VMMDLL_MemFree(iat_map);
		iat_map = NULL;
		LOG("[!] Invalid VMM Map Version\n");
		return 0;
	}

	uintptr_t addr = 0;
	for (int i = 0; i < iat_map->cMap; i++)
	{
		import_entry = iat_map->pMap + i;
		if (strcmp(import_entry->uszFunction, import.c_str()) == 0)
		{
			addr = import_entry->vaFunction;
			break;
		}
	}

	VMMDLL_MemFree(iat_map);
	iat_map = NULL;

	return addr;
}

uint64_t cbSize = 0x80000;
//callback for VfsFileListU
VOID cbAddFile(_Inout_ HANDLE h, _In_ LPCSTR uszName, _In_ ULONG64 cb, _In_opt_ PVMMDLL_VFS_FILELIST_EXINFO pExInfo)
{
	if (strcmp(uszName, "dtb.txt") == 0)
		cbSize = cb;
}

struct Info
{
	uint32_t index;
	uint32_t process_id;
	uint64_t dtb;
	uint64_t kernelAddr;
	std::string name;
};


bool deleteIfOlderThan8Hours(const std::string& filename) {
	try {
		// Check if file exists
		if (!fs::exists(filename)) {
			return false;
		}

		std::time_t currentTime = std::time(nullptr);

		// 获取文件状态信息
		struct stat fileInfo;
		if (stat(filename.c_str(), &fileInfo) != 0) {
			std::cerr << "Cant get file status: " << filename << std::endl;
			return false;
		}

		// 获取文件最后修改时间
		std::time_t lastModTime = fileInfo.st_mtime;

		// 计算时间差（秒）
		double diffSeconds = std::difftime(currentTime, lastModTime);

		// 转换为小时
		double diffHours = diffSeconds / 3600.0;

		// If difference is more than 8 hours, delete the file
		if (diffHours >= 8) {
			std::cout << "Cache removing..." << std::endl;
			fs::remove(filename);
			std::cout << "Cache deleted successfully." << std::endl;
			return true;
		}
		else {
			std::cout << "Cycle DTB Check: OK." << std::endl;
			return false;
		}
	}
	catch (const fs::filesystem_error& e) {
		std::cerr << "Filesystem error: " << e.what() << std::endl;
		return false;
	}
	catch (const std::exception& e) {
		std::cerr << "Error: " << e.what() << std::endl;
		return false;
	}
}

bool Memory::FixCr3()
{
    PVMMDLL_MAP_MODULEENTRY module_entry = NULL;
    bool result = VMMDLL_Map_GetModuleFromNameU(this->vHandle, current_process.PID, const_cast<LPSTR>(current_process.process_name.c_str()), &module_entry, NULL);
    if (result)
        return true; //Doesn't need to be patched lol

    if (fs::exists("dtb_cache.bin") && deleteIfOlderThan8Hours("dtb_cache.bin")) //刷新
        DeleteFileA("dtb_cache.bin");

    if (!VMMDLL_InitializePlugins(this->vHandle))
        return false;

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    for (int attempt = 0; attempt < 100; ++attempt)
    {
        BYTE bytes[4] = { 0 };
        DWORD i = 0;
        auto nt = VMMDLL_VfsReadW(this->vHandle, const_cast<LPWSTR>(L"\\misc\\procinfo\\progress_percent.txt"), bytes, 3, &i, 0);
        if (nt == VMMDLL_STATUS_SUCCESS && atoi(reinterpret_cast<LPSTR>(bytes)) == 100)
            break;

        if (attempt == 99)
            return false;

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    VMMDLL_VFS_FILELIST2 VfsFileList = { };
    VfsFileList.dwVersion = VMMDLL_VFS_FILELIST_VERSION;
    VfsFileList.h = 0;
    VfsFileList.pfnAddDirectory = 0;
    VfsFileList.pfnAddFile = cbAddFile;

    cbSize = 0x80000;
    result = VMMDLL_VfsListU(this->vHandle, const_cast<LPSTR>("\\misc\\procinfo\\"), &VfsFileList);
    if (!result)
        return false;

    const size_t buffer_size = cbSize;
    std::unique_ptr<BYTE[]> bytes(new BYTE[buffer_size]);
    DWORD j = 0;
    auto nt = VMMDLL_VfsReadW(this->vHandle, const_cast<LPWSTR>(L"\\misc\\procinfo\\dtb.txt"), bytes.get(), buffer_size - 1, &j, 0);
    if (nt != VMMDLL_STATUS_SUCCESS)
        return false;

    std::vector<uint64_t> possible_dtbs = { };
    std::string lines(reinterpret_cast<char*>(bytes.get()));
    std::istringstream iss(lines);
    std::string line = "";

    while (std::getline(iss, line))
    {
        Info info = { };

        std::istringstream info_ss(line);
        if (info_ss >> std::hex >> info.index >> std::dec >> info.process_id >> std::hex >> info.dtb >> info.kernelAddr >> info.name)
        {
            if (info.process_id == 0)
                possible_dtbs.push_back(info.dtb);
            if (current_process.process_name.find(info.name) != std::string::npos)
                possible_dtbs.push_back(info.dtb);
        }
    }

    for (size_t i = 0; i < possible_dtbs.size(); i++)
    {
        auto dtb = possible_dtbs[i];
        VMMDLL_ConfigSet(this->vHandle, VMMDLL_OPT_PROCESS_DTB | current_process.PID, dtb);
        result = VMMDLL_Map_GetModuleFromNameU(this->vHandle, current_process.PID, const_cast<LPSTR>(current_process.process_name.c_str()), &module_entry, NULL);
        if (result)
            return true;
    }

    return false;
	//			LOG("[-] Failed to set DTB configuration\n");
	//			continue;
	//		}

	//		std::this_thread::sleep_for(std::chrono::milliseconds(100));

	//		result = VMMDLL_Map_GetModuleFromNameU(this->vHandle, current_process.PID,
	//			const_cast<LPSTR>(current_process.process_name.c_str()), &module_entry, NULL);

	//		if (result) {
	//			LOG("[+] DTB Fixed successfully with address: %016llx\n", safeDtb);
	//			return true;
	//		}
	//	}	
	//}

	//LOG("[-] Failed to patch module\n");
	//return false;
}

bool Memory::DumpMemory(uintptr_t address, std::string path)
{
	LOG("[!] Memory dumping currently does not rebuild the IAT table, imports will be missing from the dump.\n");
	IMAGE_DOS_HEADER dos{ };
	Read(address, &dos, sizeof(IMAGE_DOS_HEADER));

	//Check if memory has a PE 
	if (dos.e_magic != 0x5A4D) //Check if it starts with MZ
	{
		LOG("[-] Invalid PE Header\n");
		return false;
	}

	IMAGE_NT_HEADERS64 nt;
	Read(address + dos.e_lfanew, &nt, sizeof(IMAGE_NT_HEADERS64));

	//Sanity check
	if (nt.Signature != IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
	{
		LOG("[-] Failed signature check\n");
		return false;
	}
	//Shouldn't change ever. so const 
	const size_t target_size = nt.OptionalHeader.SizeOfImage;
	//Crashes if we don't make it a ptr :(
	auto target = std::unique_ptr<uint8_t[]>(new uint8_t[target_size]);

	//Read whole modules memory
	Read(address, target.get(), target_size);
	auto nt_header = (PIMAGE_NT_HEADERS64)(target.get() + dos.e_lfanew);
	auto sections = (PIMAGE_SECTION_HEADER)(target.get() + dos.e_lfanew + FIELD_OFFSET(IMAGE_NT_HEADERS, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader);

	for (size_t i = 0; i < nt.FileHeader.NumberOfSections; i++, sections++)
	{
		//Rewrite the file offsets to the virtual addresses
		LOG("[!] Rewriting file offsets at 0x%p size 0x%p\n", sections->VirtualAddress, sections->Misc.VirtualSize);
		sections->PointerToRawData = sections->VirtualAddress;
		sections->SizeOfRawData = sections->Misc.VirtualSize;
	}

	auto debug = (PIMAGE_DEBUG_DIRECTORY)(target.get() + nt_header->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG].VirtualAddress);
	debug->PointerToRawData = debug->AddressOfRawData;

	//Find all modules used by this process
	//auto descriptor = Read<IMAGE_IMPORT_DESCRIPTOR>(address + ntHeader->OptionalHeader.DataDirectory[1].VirtualAddress);

	//int descriptor_count = 0;
	//int thunk_count = 0;

	/*std::vector<ModuleData> modulelist;
	while (descriptor.Name) {
		auto first_thunk = Read<IMAGE_THUNK_DATA>(moduleAddr + descriptor.FirstThunk);
		auto original_first_thunk = Read<IMAGE_THUNK_DATA>(moduleAddr + descriptor.OriginalFirstThunk);
		thunk_count = 0;

		char ModuleName[256];
		ReadMemory(moduleAddr + descriptor.Name, (void*)&ModuleName, 256);

		std::string DllName = ModuleName;

		ModuleData tmpModuleData;

		//if(std::find(modulelist.begin(), modulelist.end(), tmpModuleData) == modulelist.end())
		//	modulelist.push_back(tmpModuleData);
		while (original_first_thunk.u1.AddressOfData) {
			char name[256];
			ReadMemory(moduleAddr + original_first_thunk.u1.AddressOfData + 0x2, (void*)&name, 256);

			std::string str_name = name;
			auto thunk_offset{ thunk_count * sizeof(uintptr_t) };

			//if (str_name.length() > 0)
			//	imports[str_name] = moduleAddr + descriptor.FirstThunk + thunk_offset;

			++thunk_count;
			first_thunk = Read<IMAGE_THUNK_DATA>(moduleAddr + descriptor.FirstThunk + sizeof(IMAGE_THUNK_DATA) * thunk_count);
			original_first_thunk = Read<IMAGE_THUNK_DATA>(moduleAddr + descriptor.OriginalFirstThunk + sizeof(IMAGE_THUNK_DATA) * thunk_count);
		}

		++descriptor_count;
		descriptor = Read<IMAGE_IMPORT_DESCRIPTOR>(moduleAddr + ntHeader->OptionalHeader.DataDirectory[1].VirtualAddress + sizeof(IMAGE_IMPORT_DESCRIPTOR) * descriptor_count);
	}*/

	//Rebuild import table

	//LOG("[!] Creating new import section\n");

	//Create New Import Section

	//Build new import Table

	//Dump file
	const auto dumped_file = CreateFileW(std::wstring(path.begin(), path.end()).c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_COMPRESSED, NULL);
	if (dumped_file == INVALID_HANDLE_VALUE)
	{
		LOG("[!] Failed creating file: %i\n", GetLastError());
		return false;
	}

	if (!WriteFile(dumped_file, target.get(), static_cast<DWORD>(target_size), NULL, NULL))
	{
		LOG("[!] Failed writing file: %i\n", GetLastError());
		CloseHandle(dumped_file);
		return false;
	}

	LOG("[+] Successfully dumped memory at %s\n", path.c_str());
	CloseHandle(dumped_file);
	return true;
}

static const char* hexdigits =
"\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000"
"\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000"
"\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000"
"\000\001\002\003\004\005\006\007\010\011\000\000\000\000\000\000"
"\000\012\013\014\015\016\017\000\000\000\000\000\000\000\000\000"
"\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000"
"\000\012\013\014\015\016\017\000\000\000\000\000\000\000\000\000"
"\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000"
"\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000"
"\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000"
"\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000"
"\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000"
"\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000"
"\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000"
"\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000"
"\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000";

static uint8_t GetByte(const char* hex)
{
	return static_cast<uint8_t>((hexdigits[hex[0]] << 4) | (hexdigits[hex[1]]));
}

uint64_t Memory::FindSignature(const char* signature, uint64_t range_start, uint64_t range_end, int PID)
{
	if (!signature || signature[0] == '\0' || range_start >= range_end)
		return 0;

	if (PID == 0)
		PID = current_process.PID;

	std::vector<uint8_t> buffer(range_end - range_start);
	if (!VMMDLL_MemReadEx(this->vHandle, PID, range_start, buffer.data(), buffer.size(), 0, VMMDLL_FLAG_NOCACHE))
		return 0;

	const char* pat = signature;
	uint64_t first_match = 0;
	for (uint64_t i = range_start; i < range_end; i++)
	{
		if (*pat == '?' || buffer[i - range_start] == GetByte(pat))
		{
			if (!first_match)
				first_match = i;

			if (!pat[2])
				break;

			pat += (*pat == '?') ? 2 : 3;
		}
		else
		{
			pat = signature;
			first_match = 0;
		}
	}

	return first_match;
}

bool Memory::Write(uintptr_t address, void* buffer, size_t size) const
{
	if (!VMMDLL_MemWrite(this->vHandle, current_process.PID, address, static_cast<PBYTE>(buffer), size))
	{
		LOG("[!] Failed to write Memory at 0x%p\n", address);
		return false;
	}
	return true;
}

bool Memory::Write(uintptr_t address, void* buffer, size_t size, int pid) const
{
	if (!VMMDLL_MemWrite(this->vHandle, pid, address, static_cast<PBYTE>(buffer), size))
	{
		LOG("[!] Failed to write Memory at 0x%p\n", address);
		return false;
	}
	return true;
}

bool Memory::ReadCacheEx(uintptr_t address, void* buffer, size_t size) const
{
	if (!VMMDLL_MemReadEx(this->vHandle, this->current_process.PID, address, (PBYTE)buffer, size, NULL, NULL))
	{
		LOG("[!] 读取内存失败 0x%p\n", address);
		return false;
	}
	return true;
}


bool Memory::Read(uintptr_t address, void* buffer, size_t size) const
{
	DWORD read_size = 0;
	if (!VMMDLL_MemReadEx(this->vHandle, current_process.PID, address, static_cast<PBYTE>(buffer), size, &read_size, VMMDLL_FLAG_NOCACHE))
	{
		LOG("[!] Failed to read Memory at 0x%p\n", address);
		return false;
	}

	return (read_size == size);
}

#include <DbgHelp.h>
std::string GetCallStack()
{
	std::stringstream callstack;
	callstack << "=== Error Details ===\n";

	// 获取最后的错误码和描述
	DWORD error = GetLastError();
	char errorMsg[256];
	FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, NULL, error,
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		errorMsg, sizeof(errorMsg), NULL);

	callstack << "System Error: " << error << " - " << errorMsg << "\n\n";
	callstack << "=== Call Stack ===\n";

	// 初始化符号处理
	if (!SymInitialize(GetCurrentProcess(), NULL, TRUE)) {
		callstack << "Failed to initialize symbol handler. Error: " << GetLastError() << "\n";
		return callstack.str();
	}

	// 捕获当前上下文
	CONTEXT context;
	RtlCaptureContext(&context);

	// 设置栈帧
	STACKFRAME64 frame = {};
	frame.AddrPC.Offset = context.Rip;
	frame.AddrPC.Mode = AddrModeFlat;
	frame.AddrFrame.Offset = context.Rbp;
	frame.AddrFrame.Mode = AddrModeFlat;
	frame.AddrStack.Offset = context.Rsp;
	frame.AddrStack.Mode = AddrModeFlat;

	// 分配符号信息缓冲区
	char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)];
	PSYMBOL_INFO symbol = (PSYMBOL_INFO)buffer;
	symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
	symbol->MaxNameLen = MAX_SYM_NAME;

	DWORD frame_number = 0;

	// 获取每一帧的信息
	while (StackWalk64(IMAGE_FILE_MACHINE_AMD64,
		GetCurrentProcess(),
		GetCurrentThread(),
		&frame,
		&context,
		NULL,
		SymFunctionTableAccess64,
		SymGetModuleBase64,
		NULL))
	{
		frame_number++;
		callstack << "\nFrame " << frame_number << ":\n";

		// 获取模块信息
		IMAGEHLP_MODULE64 moduleInfo;
		moduleInfo.SizeOfStruct = sizeof(moduleInfo);
		if (SymGetModuleInfo64(GetCurrentProcess(), frame.AddrPC.Offset, &moduleInfo)) {
			callstack << "Module: " << moduleInfo.ModuleName << "\n";
		}

		DWORD64 displacement = 0;
		if (SymFromAddr(GetCurrentProcess(), frame.AddrPC.Offset, &displacement, symbol))
		{
			callstack << "Function: " << symbol->Name << "\n";

			// 获取函数参数和局部变量信息
			IMAGEHLP_STACK_FRAME imagehlpFrame;
			imagehlpFrame.InstructionOffset = frame.AddrPC.Offset;
			SymSetContext(GetCurrentProcess(), &imagehlpFrame, 0);

			IMAGEHLP_LINE64 line;
			line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
			DWORD displacement_line = 0;
			if (SymGetLineFromAddr64(GetCurrentProcess(), frame.AddrPC.Offset, &displacement_line, &line))
			{
				callstack << "Source: " << line.FileName << ":" << line.LineNumber << "\n";

				// 尝试读取源代码行
				std::ifstream sourceFile(line.FileName);
				if (sourceFile.is_open()) {
					std::string sourceLine;
					for (int i = 1; i <= line.LineNumber; i++) {
						std::getline(sourceFile, sourceLine);
					}
					callstack << "Code: " << sourceLine << "\n";
				}
			}

			// 添加内存地址信息
			callstack << "Address: 0x" << std::hex << frame.AddrPC.Offset
				<< " (Displacement: " << displacement << ")\n";
		}
		else {
			callstack << "Failed to get symbol info. Error: " << GetLastError() << "\n";
		}
	}

	// 添加系统信息
	SYSTEM_INFO sysInfo;
	GetSystemInfo(&sysInfo);
	callstack << "\n=== System Info ===\n";
	callstack << "Processor Architecture: " << sysInfo.wProcessorArchitecture << "\n";
	callstack << "Number of Processors: " << sysInfo.dwNumberOfProcessors << "\n";
	callstack << "Page Size: " << sysInfo.dwPageSize << "\n";

	SymCleanup(GetCurrentProcess());
	return callstack.str();
}

bool Memory::Read(uintptr_t address, void* buffer, size_t size, int pid) const
{
	DWORD read_size = 0;
	if (!VMMDLL_MemReadEx(this->vHandle, pid, address, static_cast<PBYTE>(buffer), size, &read_size, VMMDLL_FLAG_NOCACHE))
	{
		
		LOG("[!] Failed to read Memory at 0x%p\n", address);
		//if (DEBUG)
		//{
			//std::string stack = GetCallStack();
			//LOG("[!] Call Stack:\n%s\n", stack.c_str());

		//}
		return false;
	}
	return (read_size == size);
}

VMMDLL_SCATTER_HANDLE Memory::CreateScatterHandle() const
{
	const VMMDLL_SCATTER_HANDLE ScatterHandle = VMMDLL_Scatter_Initialize(
		this->vHandle,
		current_process.PID,
		VMMDLL_FLAG_NOCACHE
	);
	if (!ScatterHandle)
		LOG("[!] Failed to create scatter handle\n");
	return ScatterHandle;
}

VMMDLL_SCATTER_HANDLE Memory::CreateScatterHandle(int pid) const
{
	const VMMDLL_SCATTER_HANDLE ScatterHandle = VMMDLL_Scatter_Initialize(
		this->vHandle,
		current_process.PID,
		VMMDLL_FLAG_NOCACHE
	);
	if (!ScatterHandle)
		LOG("[!] Failed to create scatter handle\n");
	return ScatterHandle;
}

void Memory::CloseScatterHandle(VMMDLL_SCATTER_HANDLE handle)
{
	VMMDLL_Scatter_CloseHandle(handle);
}

void Memory::AddScatterReadRequest(VMMDLL_SCATTER_HANDLE handle, uint64_t address, void* buffer, size_t size)
{
	if (!VMMDLL_Scatter_PrepareEx(handle, address, size, static_cast<PBYTE>(buffer), NULL))
	{
		LOG("[!] Failed to prepare scatter read at 0x%p\n", address);
	}
}

void Memory::AddScatterWriteRequest(VMMDLL_SCATTER_HANDLE handle, uint64_t address, void* buffer, size_t size)
{
	if (!VMMDLL_Scatter_PrepareWrite(handle, address, static_cast<PBYTE>(buffer), size))
	{
		LOG("[!] Failed to prepare scatter write at 0x%p\n", address);
	}
}

void Memory::ExecuteReadScatter(VMMDLL_SCATTER_HANDLE handle, int pid, bool print)
{
	if (pid == 0)
		pid = current_process.PID;

	if (!VMMDLL_Scatter_ExecuteRead(handle))
	{
		if (print)
		{
			std::string stack = GetCallStack();
			LOG("[!] Call Stack:\n%s\n", stack.c_str());
			//	DWORD error = GetLastError();
			//	LOG("Scatter execute failed with error: %d\n", error);
			//}

			LOG("[-] Failed to Execute Scatter Read2\n");
		}
	}
	//Clear after using it
	if (!VMMDLL_Scatter_Clear(handle, pid, VMMDLL_FLAG_NOCACHE))
	{
		LOG("[-] Failed to clear Scatter\n");
	}
}

bool Memory::ExecuteReadScatterBOOL(VMMDLL_SCATTER_HANDLE handle, int pid)
{
	if (pid == 0)
		pid = current_process.PID;

	if (!VMMDLL_Scatter_ExecuteRead(handle))
	{
		//if (DEBUG)
		//{
			//std::string stack = GetCallStack();
			//LOG("[!] Call Stack:\n%s\n", stack.c_str());
		//	DWORD error = GetLastError();
		//	LOG("Scatter execute failed with error: %d\n", error);
		//}
		LOG("[-] Failed to Execute Scatter Read1\n");
		return false;
	}
	//Clear after using it
	if (!VMMDLL_Scatter_Clear(handle, pid, VMMDLL_FLAG_NOCACHE))
	{
		LOG("[-] Failed to clear Scatter\n");
		return false;
	}
	return true;
}

void Memory::ExecuteWriteScatter(VMMDLL_SCATTER_HANDLE handle, int pid)
{
	if (pid == 0)
		pid = current_process.PID;

	if (!VMMDLL_Scatter_Execute(handle))
	{
		LOG("[-] Failed to Execute Scatter Write\n");
	}
	//Clear after using it
	if (!VMMDLL_Scatter_Clear(handle, pid, VMMDLL_FLAG_NOCACHE))
	{
		LOG("[-] Failed to clear Scatter\n");
	}
}
