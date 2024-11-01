include ../Makefile.build_system_common

# Add subfolders as modules
DIRS += module_testing
USEMODULE += my_module

# Add fmt to read blob
USEMODULE += fmt

# Add blobs 
BLOBS += blob/blob_subdir/blobtest_subdir.txt

SRC += main.c
SRC += folder/a.c blob/blob_test.c folder/subfolder/b.c
# # Alternative method to add files in subfolders using wildcards
# # SRC += $(wildcard *.c blob/*.c folder/*.c folder/**/*.c)

include $(RIOTBASE)/Makefile.include
