/*
 * Copyright (C) 2024 Orange
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License v2.1. See the file LICENSE in the top level
 * directory for more details.
 */

#include "fmt.h"

#include "blob/blob/blob_subdir/blobtest_subdir.txt.h"

void blob_print(void) 
{
    print((char *)blobtest_subdir_txt, blobtest_subdir_txt_len);
    print("\n", 1);
}
