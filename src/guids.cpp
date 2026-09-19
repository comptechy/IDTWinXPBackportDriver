/*****************************************************************************
 * guids.cpp
 *****************************************************************************
 * Exactly one translation unit must define INITGUID before pulling in the
 * GUID declarations, so that DEFINE_GUID (in shared.h's IID_IHdaAdapterCommon,
 * and any future custom GUIDs added to guids.h) actually allocates storage
 * instead of just declaring an extern reference. Every other .cpp includes
 * shared.h without INITGUID and links against the storage defined here.
 */

#define INITGUID
#include "guids.h"
