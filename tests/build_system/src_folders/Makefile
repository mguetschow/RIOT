include ../Makefile.build_system_common

## Add fmt to read blob
USEMODULE += fmt

# Add blobs 
BLOBS += blob/blob_subdir/blobtest_subdir.txt

SRC += main.c
SRC += blob/blob_test.c
# # Alternative method to add files in subfolders using wildcards
# # SRC += $(wildcard *.c blob/*.c folder/*.c folder/**/*.c)

include $(RIOTBASE)/Makefile.include
