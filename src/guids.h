/*****************************************************************************
 * guids.h
 *****************************************************************************
 * Driver-specific GUIDs. Most KS interface/category GUIDs used by this
 * driver are the standard PortCls/KS ones (already defined in the DDK's
 * ks.h/ksmedia.h via portcls.h); this file only holds the ones private to
 * this project (the adapter-common interface GUID lives in shared.h instead,
 * since it's needed before this file in the include order).
 *****************************************************************************/

#ifndef _GUIDS_H_
#define _GUIDS_H_

#include <initguid.h>
#include "shared.h"

// GUID_HDAUDIO_BUS_INTERFACE (DEFINE_GUID'd in the DDK's hdaudio.h, included
// by common.h) needs its storage allocated exactly once, same as every
// other DEFINE_GUID'd symbol in this file - guids.cpp is the one TU that
// includes this file with INITGUID active.
#include <hdaudio.h>

#endif // _GUIDS_H_
