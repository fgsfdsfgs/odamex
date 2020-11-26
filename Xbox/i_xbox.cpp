// Emacs style mode select   -*- C++ -*- 
//-----------------------------------------------------------------------------
//
// $Id$
//
// Copyright (C) 2006-2020 by The Odamex Team.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
//
// DESCRIPTION:
//	Xbox Support
//
//-----------------------------------------------------------------------------
#ifdef _XBOX

#include <windows.h>
#include <nxdk/mount.h>
#include <hal/debug.h>
#include <hal/video.h>
#include <hal/xbox.h>
#include <xboxkrnl/xboxkrnl.h>
#include <errno.h>
#include <string.h>

#include <iostream>
#include <string>
#include <sstream>
#include <list>

#include <SDL.h>
#include <SDL_mixer.h>

#include "i_xbox.h"
#include "i_system.h"
#include "c_console.h"

using namespace std;

// Partition device mapping
#define DeviceC "\\Device\\Harddisk0\\Partition2"
#define CdRom   "\\Device\\CdRom0"
#define DeviceE "\\Device\\Harddisk0\\Partition1"
#define DeviceF "\\Device\\Harddisk0\\Partition6"
#define DeviceG "\\Device\\Harddisk0\\Partition7"
#define DeviceT "\\Device\\Harddisk0\\Partition1\\TDATA\\" XBOX_TITLE_ID
#define DeviceU "\\Device\\Harddisk0\\Partition1\\UDATA\\" XBOX_TITLE_ID
#define DeviceZ "\\Device\\Harddisk0\\Partition5"

// Custom LAUNCH_DATA struct for external XBE execution from AG_Execute()
#define AG_LAUNCH_MAGIC 0x41474152

// nxdk lacks these definitions for now
#define MAX_LAUNCH_DATA_SIZE 3072
#define LDT_TITLE 0
#define LDT_FROM_DASHBOARD 2
#define LDT_FROM_DEBUGGER_CMDLINE 3

typedef struct _LAUNCH_DATA
{
	BYTE Data[MAX_LAUNCH_DATA_SIZE];
} LAUNCH_DATA, *PLAUNCH_DATA;

typedef struct _LD_FROM_DEBUGGER_CMDLINE
{
	CHAR szCmdLine[MAX_LAUNCH_DATA_SIZE];
} LD_FROM_DEBUGGER_CMDLINE, *PLD_FROM_DEBUGGER_CMDLINE;

typedef struct {
	DWORD magic;               // Test this against AG_LAUNCH_MAGIC to know this special struct was used
	DWORD dwID;                // The Title ID of the launcher XBE
	CHAR  szLauncherXBE[256];  // The full path to the launcher XBE
	CHAR  szLaunchedXBE[256];  // The full path to the launched XBE
	CHAR  szCmdLine[MAX_LAUNCH_DATA_SIZE - 520]; // The command-line parameters
} AG_LAUNCH_DATA, *PAG_LAUNCH_DATA;

// Standard homebrew custom LAUNCH_DATA designed to launch emu's directly into a game.
// This is used to pass custom parameters from many popular dashboards such as XBMC.
#define CUSTOM_LAUNCH_MAGIC 0xEE456777

typedef struct {
	DWORD magic;
	CHAR szFilename[300];
	CHAR szLaunchXBEOnExit[100];
	CHAR szRemap_D_As[350];
	BYTE country;
	BYTE launchInsertedMedia;
	BYTE executionType;
	CHAR reserved[MAX_LAUNCH_DATA_SIZE-757];
} CUSTOM_LAUNCH_DATA, *PCUSTOM_LAUNCH_DATA;

// External function declarations
extern int    I_Main(int argc, char *argv[]);		// i_main.cpp
extern size_t I_BytesToMegabytes (size_t Bytes);	// i_system.cpp

// Globals
static std::list<void (*)(void)>  ExitFuncList;
static DWORD                      LauncherID;
static char                      *LauncherXBE = NULL;
static bool                       Xbox_RROD = false; // Novelty - Red Ring of DOOM!
static int                        Xbox_VidMode = 0; // 640, 720, 1080, default 640

// Network settings
// I have no idea how to read these from EEPROM or wherever they are stored,
// so for now we just pass them in as command line
static char net_xbox_ip[20] = "0.0.0.0";
static char net_xbox_netmask[20] = "0.0.0.0";
static char net_xbox_gateway[20] = "0.0.0.0";
static char net_xbox_dns[20] = "0.0.0.0";

/* LWIP network driver stuff */

// nxdk lacks something like XNet, but instead has the much lower level LWIP
// this means we have to bring up the network interface manually

static BOOL xnet_inited = FALSE;

extern "C"
{

#include <lwip/debug.h>
#include <lwip/dhcp.h>
#include <lwip/init.h>
#include <lwip/netif.h>
#include <lwip/sys.h>
#include <lwip/tcpip.h>
#include <lwip/timeouts.h>
#include <netif/etharp.h>
#include <pktdrv.h>

// interface settings
static ip4_addr_t xnet_ip;
static ip4_addr_t xnet_netmask;
static ip4_addr_t xnet_gateway;
static ip4_addr_t xnet_dns;

// this is referenced in the LWIP driver
struct netif *g_pnetif;
struct netif nforce_netif;

// this is not defined in any headers
err_t nforceif_init(struct netif *netif);

static void tcpip_init_done(void *arg)
{
	sys_sem_t *init_complete = (sys_sem_t *)arg;
	sys_sem_signal(init_complete);
}

static void packet_timer(void *arg)
{
	#define PKT_TMR_INTERVAL 5 /* ms */
	LWIP_UNUSED_ARG(arg);
	Pktdrv_ReceivePackets();
	sys_timeout(PKT_TMR_INTERVAL, packet_timer, NULL);
}

}

/* end LWIP stuff */

//
// xbox_GetLaunchData
// nxdk doesn't have XGetLaunchData yet, so we make do
//
DWORD xbox_GetLaunchData(DWORD *outType, void *outData)
{
	if(!LaunchDataPage)
		return EFAULT;
	*outType = LaunchDataPage->Header.dwLaunchDataType;
	memcpy(outData, LaunchDataPage->LaunchData, MAX_LAUNCH_DATA_SIZE);
	return ERROR_SUCCESS;
}

//
// xbox_RecordLauncherXBE
//
void xbox_RecordLauncherXBE(char *szLauncherXBE, DWORD dwID)
{
	if(szLauncherXBE  && !LauncherXBE)
	{
		LauncherXBE = strdup(szLauncherXBE);
		LauncherID = dwID;
	}
}

//
// xbox_Getenv 
// Environment variables don't exist on Xbox. Return NULL.
//
char *xbox_Getenv(const char *)
{
	return NULL;
}

//
// xbox_Putenv 
//
// Environment variables don't exist on Xbox. Just return success.
//
int xbox_Putenv(const char *)
{
	return 0;
}

//
// xbox_GetCWD 
//
// Return working directory which is always D:\
//
char *xbox_GetCWD(char *buf, size_t size)
{
	if(size > 0 && buf)
	{
		if(size >= 3)
		{
			strcpy(buf, "D:\\");
			return buf;
		}
		else
			errno = ERANGE;
	}
	else
		errno = EINVAL;

	return NULL;
}

//
// xbox_GetHostname
//
// Custom implementation for Xbox
//
int xbox_GetHostname(char *name, int namelen)
{
	if (!xnet_inited || !name)
	{
		errno = EFAULT;
		return -1;
	}

	const ip4_addr_t *localaddr = netif_ip4_addr(g_pnetif);
	if (localaddr)
	{
		char *ipstr = ip4addr_ntoa(localaddr);
		if (ipstr && *ipstr)
		{
			strncpy(name, ipstr, namelen);
			name[namelen - 1] = 0;
			return 0;
		}
	}

	errno = EINVAL;
	return -1;
}

//
// xbox_PrintMemoryDebug
//
void xbox_PrintMemoryDebug()
{
	extern size_t got_heapsize;
	static ULONG lastmem;
	MM_STATISTICS mem_stats;
	ULONG used_bytes, avail_bytes, total_bytes;

	// Get the memory status.
	mem_stats.Length = sizeof(mem_stats);
	MmQueryStatistics(&mem_stats);

	if (mem_stats.AvailablePages != lastmem)
	{
		avail_bytes = mem_stats.AvailablePages << 12;
		total_bytes = mem_stats.TotalPhysicalPages << 12;
		used_bytes = total_bytes - avail_bytes;

		debugPrint("\nMemory Debug:\n");
		debugPrint("Heap Size:             %4u MB\n", got_heapsize);
		debugPrint("Total Physical Memory: %8u bytes / %4u MB\n", total_bytes, I_BytesToMegabytes(total_bytes));
		debugPrint("Used Physical Memory : %8u bytes / %4u MB\n", used_bytes, I_BytesToMegabytes(used_bytes));
		debugPrint("Free Physical Memory : %8u bytes / %4u MB\n", avail_bytes, I_BytesToMegabytes(avail_bytes));
		debugPrint("\n");

		lastmem = mem_stats.AvailablePages;
	}
}

//
// xbox_MountDevice
//
// XBox device mounting
//
BOOL xbox_MountDevice(char cDriveLetter, const char *sDeviceName)
{
	const BOOL res = nxMountDrive(cDriveLetter, sDeviceName);
	if (!res) debugPrint("mount failed: %s -> %c:\n", sDeviceName, cDriveLetter);
	return res;
}

//
// xbox_UnMountDevice
//
BOOL xbox_UnMountDevice(char cDriveLetter)
{
	return nxUnmountDrive(cDriveLetter);
}

//
// xbox_MountPartitions
//
// Some of these partitions are automatically mounted but just 
// to be on the safe side the mount will be attempted anyway
//
// Some of these partitions are only found on some modded consoles.
// These partitions will only successfully mount on consoles where
// the modder has created these partitions.
//
// F is likely to be found on systems that shipped with stock hdd's
// that were 10GB in size. Since the Xbox hdd was advertised as 8GB,
// 2GB were left unpartitioned on these and that extra space will
// typically be partitioned into F: during modding and games or apps
// will be installed there (like this one.) F: will also be found
// on non-stock hdd's of any size where any space above 8GB and below
// 137GB will be partitioned into F:
//
// G is likely to be found on non-stock hdd's that are larger than 137GB
// due to the partition size limitation. Anything above 137GB is partitioned
// into G: and it is used, typically, for the same purposes as F.
//
void xbox_MountPartitions()
{
	xbox_MountDevice('D', CdRom);   // XBE location -- automounted; will mount CdRom if not
	xbox_MountDevice('E', DeviceE); // Standard save partition
	xbox_MountDevice('F', DeviceF); // Non-stock partition - modded consoles only
	xbox_MountDevice('G', DeviceG); // Non-stock partition - modded consoles only
	xbox_MountDevice('Z', DeviceZ); // Cache partition - appropriate place for temporary files - automounted

	// the TDATA and UDATA directories are not in fact automounted like they are in XDK,
	// so we have to ensure that they exist first
	CreateDirectory("E:\\TDATA\\" XBOX_TITLE_ID, NULL);
	CreateDirectory("E:\\UDATA\\" XBOX_TITLE_ID, NULL);

	xbox_MountDevice('T', DeviceT); // Odamex's unique TDATA - peristent save data (configs, etc.)
	xbox_MountDevice('U', DeviceU); // Odamex's unique UDATA - user save data (save games)
}

//
// xbox_UnMountPartitions
//
void xbox_UnMountPartitions()
{
	xbox_UnMountDevice('D');
	xbox_UnMountDevice('E');
	xbox_UnMountDevice('F');
	xbox_UnMountDevice('G');
	xbox_UnMountDevice('T');
	xbox_UnMountDevice('U');
	xbox_UnMountDevice('Z');
}

//
// xbox_CloseNetwork
//
void xbox_CloseNetwork()
{
	sys_untimeout(packet_timer, NULL);
	Pktdrv_Quit();
	xnet_inited = FALSE;
}

//
// xbox_InitNet
//
void xbox_InitNet()
{
	sys_sem_t init_complete;
	const ip4_addr_t *ip;
	BOOL use_dhcp = !strcmp(net_xbox_ip, "0.0.0.0");

	if(use_dhcp)
	{
		IP4_ADDR(&xnet_gateway, 0, 0, 0, 0);
		IP4_ADDR(&xnet_ip, 0, 0, 0, 0);
		IP4_ADDR(&xnet_netmask, 0, 0, 0, 0);
	}
	else
	{
		ip4addr_aton(net_xbox_ip, &xnet_ip);
		ip4addr_aton(net_xbox_gateway, &xnet_gateway);
		ip4addr_aton(net_xbox_netmask, &xnet_netmask);
	}

	sys_sem_new(&init_complete, 0);
	tcpip_init(tcpip_init_done, &init_complete);
	sys_sem_wait(&init_complete);
	sys_sem_free(&init_complete);

	g_pnetif = netif_add(&nforce_netif, &xnet_ip, &xnet_netmask, &xnet_gateway,
			NULL, nforceif_init, ethernet_input);
	if (!g_pnetif)
	{
			debugPrint("xbox_InitNet: netif_add failed\n");
			xbox_CloseNetwork();
			return;
	}

	netif_set_default(g_pnetif);
	netif_set_up(g_pnetif);

	if (use_dhcp)
	{
		dhcp_start(g_pnetif);
	}

	packet_timer(NULL);

	if (use_dhcp)
	{
		DWORD timeout;
		debugPrint("xbox_InitNet: Waiting for DHCP...\n");
		timeout = GetTickCount() + 15000; // time out in 15 sec
		while (dhcp_supplied_address(g_pnetif) == 0)
		{
			if (GetTickCount() > timeout)
			{
				debugPrint("xbox_InitNet: DHCP timed out\n");
				xbox_CloseNetwork();
				return;
			}
			NtYieldExecution();
		}
		debugPrint("xbox_InitNet: DHCP bound!\n");
	}

	xnet_inited = TRUE;
}

//
// xbox_WriteSaveMeta
//
void xbox_WriteSaveMeta(string path, string text)
{
	if(!path.size() || !text.size())
		return;

	string   filename = path + PATHSEP + "SaveMeta.xbx";
	ofstream metafile(filename.c_str());

	if(metafile.fail())
	{
		I_Error ("Failed to create %s save meta:\n%s",
			   filename.c_str(), strerror (errno));
		return;
	}

	metafile << "Name=" << text;

	metafile.close();
}

//
// xbox_GetSavePath
//
string xbox_GetSavePath(string file, int slot)
{
	ostringstream path;
	DWORD         attrs;

	path << setiosflags(ios::right);
	path << setfill('0');

	path << "U:" << PATHSEP << setw(12) << slot;

	if((attrs = GetFileAttributes(path.str().c_str())) == INVALID_FILE_ATTRIBUTES)
	{
		if(!SUCCEEDED(CreateDirectory(path.str().c_str(), NULL)))
		{
			I_FatalError ("Failed to create %s directory:\n%s",
						   path.str().c_str(), strerror (errno));
		}

		xbox_WriteSaveMeta(path.str(), "Empty Slot");
	}
	else
	{
		if (!(attrs & FILE_ATTRIBUTE_DIRECTORY))
			I_FatalError ("%s must be a directory", path.str().c_str());
	}

	path << PATHSEP << file;

	return path.str();
}

//
// xbox_EnableCustomLED
//
void xbox_EnableCustomLED()
{
	// 0xF0 = Solid Red
	HalWriteSMBusValue(0x20, 0x08, 0, 0xF0);
	Sleep(10);
	HalWriteSMBusValue(0x20, 0x07, 0, 1);
}

//
// xbox_DisableCustomLED
//
void xbox_DisableCustomLED()
{
	HalWriteSMBusValue(0x20, 0x07, 0, 0);
}

//
// xbox_reboot 
//
// Exit Odamex and perform a warm reboot (no startup logo) to a launcher or dashboard
//
void xbox_Reboot()
{
	// if Odamex was started from a launcher, boot back into it, otherwise just reboot to dash
	// NOTE: path in LauncherXBE should be of the standard DOS path form ("E:\\launcher\\default.xbe");
	//       XLaunchXBE will allocate the launch data and set launch data type to 0xFFFFFFFF
	Sleep(5000);
	if (LauncherXBE)
		XLaunchXBE(LauncherXBE);
	else
		XReboot();
}

//
// xbox_AtExit 
//
// Custom atexit function for Xbox
//
void xbox_AtExit(void (*function)(void))
{
	if(function)
		ExitFuncList.push_back(function);
}

//
// xbox_Exit
//
// Custom exit function for Xbox
//
void xbox_Exit(int status)
{
	std::list<void (*)(void)>::iterator funcIter;

	for(funcIter = ExitFuncList.begin(); funcIter != ExitFuncList.end(); ++funcIter)
		(*funcIter)();

	xbox_CloseNetwork();

	xbox_UnMountPartitions();

	if(Xbox_RROD)
		xbox_DisableCustomLED();

	xbox_Reboot();
}

//
// xbox_PrepareArgs
//
// Convert to standard C arguments
void xbox_PrepareArgs(string cmdline, char *argv[], int &argc)
{
	if(cmdline.size())
	{
		size_t pos, oldpos = 0;

		do
		{
			if(cmdline[oldpos] == '"')
				pos = cmdline.find('"', ++oldpos);
			else
				pos = cmdline.find(' ', oldpos);

			if(pos != oldpos)
				argv[argc++] = strdup(cmdline.substr(oldpos, pos - oldpos).c_str());

			oldpos = pos + 1;
		} while(pos != string::npos);
	}

	argv[argc] = NULL;
}

//
// xbox_CheckArgs
//
// Check for Xbox-specific args and read them
//
void xbox_CheckArgs(int argc, char *argv[])
{
	int i;
	for(i = 1; i < argc; ++i)
	{
		if(!stricmp(argv[i], "-rrod"))
			Xbox_RROD = true;
		else if(i < argc - 1)
		{
			if(!stricmp(argv[i], "-xnet_ip"))
				strncpy(net_xbox_ip, argv[i + 1], sizeof(net_xbox_ip) - 1);
			else if(!stricmp(argv[i], "-xnet_gateway"))
				strncpy(net_xbox_gateway, argv[i + 1], sizeof(net_xbox_gateway) - 1);
			else if(!stricmp(argv[i], "-xnet_mask"))
				strncpy(net_xbox_netmask, argv[i + 1], sizeof(net_xbox_netmask) - 1);
			else if(!stricmp(argv[i], "-xnet_dns"))
				strncpy(net_xbox_dns, argv[i + 1], sizeof(net_xbox_dns) - 1);
			else if(!stricmp(argv[i], "-xvidmode"))
				Xbox_VidMode = atoi(argv[i + 1]);
		}
	}
}

//
// main
//
// Entry point on Xbox
//
int main(void)
{
	DWORD            launchDataType;
	LAUNCH_DATA      launchData;
	char            *xargv[100];
	int              xargc = 0;
	char             fake_args[][32] =
	{
		"D:\\default.xbe",      // mimic argv[0]
		"-nomouse",             // FIXME: makes nxdk-sdl hang
		"-numparticles", "512", // less particles is always good
	};

	// dump our fake args into xargv
	for(xargc = 0; xargc < 100 && xargc < (int)(sizeof(fake_args) / sizeof(fake_args[0])); ++xargc)
		xargv[xargc] = fake_args[xargc];

	if(xbox_GetLaunchData(&launchDataType, &launchData) == ERROR_SUCCESS)
	{
		// Command line from debugger
		if(launchDataType == LDT_FROM_DEBUGGER_CMDLINE) 
			xbox_PrepareArgs((char*)((PLD_FROM_DEBUGGER_CMDLINE)&launchData)->szCmdLine, xargv, xargc);
		// Command line from homebrew dashboards (XBMC, etc.)
		else if((launchDataType == LDT_TITLE || launchDataType == LDT_FROM_DASHBOARD) && ((PCUSTOM_LAUNCH_DATA)&launchData)->magic == CUSTOM_LAUNCH_MAGIC)
			xbox_PrepareArgs((char*)((PCUSTOM_LAUNCH_DATA)&launchData)->szFilename, xargv, xargc);
		// Command line from Agar application (AG_Odalaunch)
		else if(launchDataType == LDT_TITLE && ((PAG_LAUNCH_DATA)&launchData)->magic == AG_LAUNCH_MAGIC)
		{
			xbox_RecordLauncherXBE(((PAG_LAUNCH_DATA)&launchData)->szLauncherXBE, ((PAG_LAUNCH_DATA)&launchData)->dwID);
			xbox_PrepareArgs((char*)((PAG_LAUNCH_DATA)&launchData)->szCmdLine, xargv, xargc);
		}
	}

	// our SDL2_mixer is new enough for this
	// assume the config also uses D:\ for current directory
	Mix_SetTimidityCfg("D:\\timidity.cfg");

	xbox_CheckArgs(xargc, xargv);

	if(Xbox_VidMode == 1080)
		XVideoSetMode(1920, 1080, 32, REFRESH_DEFAULT);
	else if(Xbox_VidMode == 720)
		XVideoSetMode(1280, 720, 32, REFRESH_DEFAULT);
	else
		XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);

	xbox_MountPartitions();

	xbox_InitNet();

	xbox_PrintMemoryDebug();

	if(Xbox_RROD)
		xbox_EnableCustomLED();

	I_Main(xargc, xargv); // Does not return

	return 0;
}

#endif // _XBOX
