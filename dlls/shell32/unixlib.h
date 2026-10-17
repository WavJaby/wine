/*
 * Unix interface for shell32
 *
 * Copyright 2026 WavJaby
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#ifndef __WINE_SHELL32_UNIXLIB_H
#define __WINE_SHELL32_UNIXLIB_H

#include "windef.h"
#include "winternl.h"
#include "wine/unixlib.h"

/* Inline buffer, no pointers -> identical layout for 32- and 64-bit callers, so wow64 needs no thunk. */
struct reveal_in_host_params
{
    char unix_path[4096];
};

enum shell32_unix_funcs
{
    unix_reveal_in_host,
};

#endif /* __WINE_SHELL32_UNIXLIB_H */
