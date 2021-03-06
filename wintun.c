/*
 * OpenConnect (SSL + DTLS) VPN client
 *
 * Copyright © 2021 David Woodhouse.
 *
 * Author: David Woodhouse <dwmw2@infradead.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * version 2.1, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 */

#include <config.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <netioapi.h>

#include <errno.h>
#include <stdio.h>

#include "openconnect-internal.h"

static WINTUN_CREATE_ADAPTER_FUNC WintunCreateAdapter;
static WINTUN_DELETE_ADAPTER_FUNC WintunDeleteAdapter;
static WINTUN_DELETE_POOL_DRIVER_FUNC WintunDeletePoolDriver;
static WINTUN_ENUM_ADAPTERS_FUNC WintunEnumAdapters;
static WINTUN_FREE_ADAPTER_FUNC WintunFreeAdapter;
static WINTUN_OPEN_ADAPTER_FUNC WintunOpenAdapter;
static WINTUN_GET_ADAPTER_LUID_FUNC WintunGetAdapterLUID;
static WINTUN_GET_ADAPTER_NAME_FUNC WintunGetAdapterName;
static WINTUN_SET_ADAPTER_NAME_FUNC WintunSetAdapterName;
static WINTUN_GET_RUNNING_DRIVER_VERSION_FUNC WintunGetRunningDriverVersion;
static WINTUN_SET_LOGGER_FUNC WintunSetLogger;
static WINTUN_START_SESSION_FUNC WintunStartSession;
static WINTUN_END_SESSION_FUNC WintunEndSession;
static WINTUN_GET_READ_WAIT_EVENT_FUNC WintunGetReadWaitEvent;
static WINTUN_RECEIVE_PACKET_FUNC WintunReceivePacket;
static WINTUN_RELEASE_RECEIVE_PACKET_FUNC WintunReleaseReceivePacket;
static WINTUN_ALLOCATE_SEND_PACKET_FUNC WintunAllocateSendPacket;
static WINTUN_SEND_PACKET_FUNC WintunSendPacket;

static struct openconnect_info *logger_vpninfo;

#define WINTUN_POOL_NAME L"OpenConnect"
#define WINTUN_RING_SIZE 0x400000

static CALLBACK void wintun_log_fn(WINTUN_LOGGER_LEVEL wlvl, const WCHAR *wmsg)
{
	int lvl = (wlvl == WINTUN_LOG_INFO) ? PRG_INFO : PRG_ERR;

	/* Sadly, Wintun doesn't provide any context information in the callback */
	if (!logger_vpninfo)
		return;

	vpn_progress(logger_vpninfo, lvl, "%d: %S\n", wlvl, wmsg);
}

static int init_wintun(struct openconnect_info *vpninfo)
{
	if (!vpninfo->wintun) {
		vpninfo->wintun = LoadLibraryExW(L"wintun.dll", NULL,
						 LOAD_LIBRARY_SEARCH_APPLICATION_DIR |
						 LOAD_LIBRARY_SEARCH_SYSTEM32);
		if (!vpninfo->wintun) {
			vpn_progress(vpninfo, PRG_DEBUG, _("Could not load wintun.dll\n"));
			return -ENOENT;
		}
#define Resolve(Name) ((Name = (void *)GetProcAddress(vpninfo->wintun, #Name)) == NULL)

		if (Resolve(WintunCreateAdapter) || Resolve(WintunDeleteAdapter) ||
		    Resolve(WintunDeletePoolDriver) || Resolve(WintunEnumAdapters) ||
		    Resolve(WintunFreeAdapter) || Resolve(WintunOpenAdapter) ||
		    Resolve(WintunGetAdapterLUID) || Resolve(WintunGetAdapterName) ||
		    Resolve(WintunSetAdapterName) || Resolve(WintunGetRunningDriverVersion) ||
		    Resolve(WintunSetLogger) || Resolve(WintunStartSession) ||
		    Resolve(WintunEndSession) || Resolve(WintunGetReadWaitEvent) ||
		    Resolve(WintunReceivePacket) || Resolve(WintunReleaseReceivePacket) ||
		    Resolve(WintunAllocateSendPacket) || Resolve(WintunSendPacket)) {

			vpn_progress(vpninfo, PRG_ERR, _("Could not resolve functions from wintun.dll\n"));
			FreeLibrary(vpninfo->wintun);
			vpninfo->wintun = NULL;
			return -EIO;
		}

		logger_vpninfo = vpninfo;
		WintunSetLogger(wintun_log_fn);
	}

	return 0;
}

int create_wintun(struct openconnect_info *vpninfo)
{
	if (init_wintun(vpninfo))
		return -1;

	vpninfo->wintun_adapter = WintunCreateAdapter(WINTUN_POOL_NAME,
						      vpninfo->ifname_w, NULL, NULL);
	if (vpninfo->wintun_adapter)
		return 0;

	char *errstr = openconnect__win32_strerror(GetLastError());
	vpn_progress(vpninfo, PRG_ERR, "Could not create Wintun adapter '%S': %s\n",
		     vpninfo->ifname_w, errstr);
	free(errstr);
	return -EIO;
}

intptr_t open_wintun(struct openconnect_info *vpninfo, char *guid, wchar_t *wname)
{
	intptr_t ret;

	if (init_wintun(vpninfo))
		return 0;

	if (!vpninfo->wintun_adapter) {
		vpninfo->wintun_adapter = WintunOpenAdapter(WINTUN_POOL_NAME,
							    wname);
		if (!vpninfo->wintun_adapter) {
			char *errstr = openconnect__win32_strerror(GetLastError());
			vpn_progress(vpninfo, PRG_ERR, "Could not open Wintun adapter '%S': %s\n",
				     wname, errstr);
			free(errstr);

			ret = OPEN_TUN_SOFTFAIL;
			goto out;
		}
	}

	if (vpninfo->ip_info.addr) {
		/*
		 * For now, vpnc-script-win.js depends on us setting the Legacy IP
		 * address on the interface. Which of course assumes there *is* a
		 * Legacy IP configuration not just IPv6. This is kind of horrid
		 * but stay compatible with it for now. In order to set the address
		 * up, we may first need to *remove* it from any other interface
		 * that has it, even if the other interface is down. Testing with
		 * a TAP-Windows interface and then Wintun was failing until I made
		 * it explicitly delete the IP address first. The later call to
		 * CreateUnicastIpAddressEntry() was apparently succeeding, but
		 * wasn't changing anything. Yay Windows!
		 */
		MIB_UNICASTIPADDRESS_ROW AddressRow;
		InitializeUnicastIpAddressEntry(&AddressRow);
		WintunGetAdapterLUID(vpninfo->wintun_adapter, &AddressRow.InterfaceLuid);
		AddressRow.Address.Ipv4.sin_family = AF_INET;
		AddressRow.Address.Ipv4.sin_addr.S_un.S_addr = htonl(inet_addr(vpninfo->ip_info.addr));
		AddressRow.OnLinkPrefixLength = 32;

		PMIB_UNICASTIPADDRESS_TABLE pipTable = NULL;
		DWORD LastError = GetUnicastIpAddressTable(AF_INET, &pipTable);
		if (LastError == ERROR_SUCCESS) {
			for (int i = 0; i < pipTable->NumEntries; i++) {
				if (pipTable->Table[i].Address.Ipv4.sin_addr.S_un.S_addr ==
				    AddressRow.Address.Ipv4.sin_addr.S_un.S_addr) {
					DeleteUnicastIpAddressEntry(&pipTable->Table[i]);
				}
			}
		}

		LastError = CreateUnicastIpAddressEntry(&AddressRow);
		if (LastError != ERROR_SUCCESS) {
			char *errstr = openconnect__win32_strerror(GetLastError());
			vpn_progress(vpninfo, PRG_ERR,
				     _("Failed to set Legacy IP address on Wintun: %s\n"),
				     errstr);
			free(errstr);

			ret = OPEN_TUN_HARDFAIL;
			goto out;
		}
	}

	vpninfo->wintun_session = WintunStartSession(vpninfo->wintun_adapter, 0x400000);
	if (!vpninfo->wintun_session) {
		char *errstr = openconnect__win32_strerror(GetLastError());
		vpn_progress(vpninfo, PRG_ERR, _("Failed to create Wintun session: %s"),
			     errstr);
		free(errstr);

		ret = OPEN_TUN_HARDFAIL;
		goto out;
	}

	DWORD ver = WintunGetRunningDriverVersion();
	vpn_progress(vpninfo, PRG_DEBUG, _("Loaded Wintun v%d.%d\n"),
		     (int)ver >> 16, (int)ver & 0xff);

	return 1;
 out:
	os_shutdown_wintun(vpninfo);
	return ret;
}

int os_read_wintun(struct openconnect_info *vpninfo, struct pkt *pkt)
{
	DWORD tun_len;
	BYTE *tun_pkt = WintunReceivePacket(vpninfo->wintun_session,
					    &tun_len);
	if (tun_pkt && tun_len < pkt->len) {
		memcpy(pkt->data, tun_pkt, tun_len);
		pkt->len = tun_len;
		WintunReleaseReceivePacket(vpninfo->wintun_session, tun_pkt);
		return 0;
	}
	return -1;
}

int os_write_wintun(struct openconnect_info *vpninfo, struct pkt *pkt)
{
	BYTE *tun_pkt = WintunAllocateSendPacket(vpninfo->wintun_session,
						 pkt->len);
	if (tun_pkt) {
		memcpy(tun_pkt, pkt->data, pkt->len);
		WintunSendPacket(vpninfo->wintun_session, tun_pkt);
		return 0;
	}
	return -1;
}

void os_shutdown_wintun(struct openconnect_info *vpninfo)
{
	if (vpninfo->wintun_session) {
		WintunEndSession(vpninfo->wintun_session);
		vpninfo->wintun_session = NULL;
	}
	if (vpninfo->wintun_adapter) {
		BOOL rr;
		WintunDeleteAdapter(vpninfo->wintun_adapter, FALSE, &rr);
		vpninfo->wintun_adapter = NULL;
	}
	logger_vpninfo = NULL;
	FreeLibrary(vpninfo->wintun);
	vpninfo->wintun = NULL;
}

int setup_wintun_fd(struct openconnect_info *vpninfo, intptr_t tun_fd)
{
	vpninfo->tun_rd_overlap.hEvent = WintunGetReadWaitEvent(vpninfo->wintun_session);
	monitor_read_fd(vpninfo, tun);
	vpninfo->tun_fh = (HANDLE)tun_fd;
	return 0;
}
