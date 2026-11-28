#pragma once

#include <ntddk.h>

#pragma warning(push)
#pragma warning(disable: 4471) // forward declaration of an unscoped enumeration
#include <wdf.h>
#pragma warning(pop)

#include <netadaptercx.h>

// DWC_EQOS_NETCX20: the Windows 10 build (dwc_eqos10.vcxproj), against
// NetAdapterCx 2.0, the version Windows 10 2004 to 22H2 (19041 to 19045) has.
// 2.0 has no receive filter control, no GSO or 802.1Q offload, and one
// checksum offload switch for both directions.
#ifndef DWC_EQOS_NETCX20
#define DWC_EQOS_NETCX20 0
#endif
#include <net/gso.h>
#include <net/ieee8021q.h>
#include <net/logicaladdress.h>
#include <net/virtualaddress.h>
#include <net/checksum.h>
#include <rk_devmem.h>
#include <initguid.h>
#include <TraceLoggingProvider.h>
#include <winmeta.h>
