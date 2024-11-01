/*
 * Copyright (C) 2024 Orange
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License v2.1. See the file LICENSE in the top level
 * directory for more details.
 */


/**
 * @ingroup     tests
 * @{
 *
 * @file
 * @brief       Test proving SRC subfolders compatibility with RIOT.
 *              
 * @author      Pierre Le Meur <pierre1.lemeur@orange.com>
 *
 *
 * @}
 */

#include <stdio.h>

void module_a(void);
void module_b(void);
void folder_a(void);
void folder_b(void);
void blob_print(void);

int main(void)
{
    puts("./main.c");
    
    /* call functions from RIOT module */
    module_a();
    module_b();
    /* call functions from subfolder */
    folder_a();
    folder_b();

    /* blob test */
    blob_print();

    return 0;
}
