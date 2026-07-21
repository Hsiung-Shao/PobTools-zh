#pragma once
/***************************************************************************
 *                                  _   _ ____  _
 *  Project                     ___| | | |  _ \| |
 *                             / __| | | | |_) | |
 *                            | (__| |_| |  _ <| |___
 *                             \___|\___/|_| \_\_____|
 *
 * Copyright (C) Daniel Stenberg, <daniel@haxx.se>, et al.
 *
 * This software is licensed as described in the file LICENSE, which
 * you should have received as part of this distribution. The terms
 * are also available at https://curl.se/docs/copyright.html.
 *
 * You may opt to use, copy, modify, merge, publish, distribute and/or sell
 * copies of the Software, and permit persons to whom the Software is
 * furnished to do so, under the terms of the LICENSE file.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 * SPDX-License-Identifier: curl
 *
 ***************************************************************************/

// Modified for standalone inclusion in SimpleGraphic.

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool Base64Encode(const char* inputbuff, size_t insize,
    char** outptr, size_t* outlen);
bool Base64UrlEncode(const char* inputbuff, size_t insize,
    char** outptr, size_t* outlen);
bool Base64Decode(const char* src,
    unsigned char** outptr, size_t* outlen);

#ifdef __cplusplus
}
#endif
