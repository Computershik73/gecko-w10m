/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gecko_w10m_nrappkit_win_types_h
#define gecko_w10m_nrappkit_win_types_h

/* nrappkit uses INT8/UINT8 to mean *eight byte* integers, while the Windows SDK
 * uses the same names for one-byte ones. nrappkit copes by renaming the Windows
 * spellings out of the way while the Windows headers are parsed (see
 * csi_platform.h and transport_addr.h), then defining its own afterwards.
 *
 * That only works if nothing has already parsed <basetsd.h> under the real
 * names, and in this directory the include order does not guarantee it: several
 * translation units reach Gecko headers -- and through them <windows.h> --
 * before the first nrappkit header. Force-including this file makes the rename
 * happen first in every translation unit, which is both less fragile and less
 * invasive than reordering includes one file at a time.
 *
 * The sentinel names must be real types: since Windows SDK 10.0.22621,
 * winsock2.h uses UINT8 as a struct field type, not only in a typedef.
 */
#ifdef _WIN32

typedef unsigned char UBLAH_IGNORE_ME_PLEASE;
typedef signed char BLAH_IGNORE_ME_PLEASE;

#  define UINT8 UBLAH_IGNORE_ME_PLEASE
#  define INT8 BLAH_IGNORE_ME_PLEASE
#  include <winsock2.h>
#  undef UINT8
#  undef INT8

#endif /* _WIN32 */

#endif /* gecko_w10m_nrappkit_win_types_h */
