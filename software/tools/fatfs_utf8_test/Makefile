# Builds the firmware's own FatFs against its own unmodified ffconf.h, on the
# host. Nothing is copied, so this cannot drift out of sync with the firmware.
FATFS := ../../infones/drivers/fatfs

CFLAGS += -O1 -Wall -Wextra -Wno-unused-parameter -I$(FATFS)

SRCS := main.c $(FATFS)/ff.c $(FATFS)/ffunicode.c $(FATFS)/ffsystem.c

# ffconf.h is a prerequisite, not just an include: the whole point of this test
# is to catch a change to it, which it cannot do if a stale binary gets reused.
fatfs_utf8_test: $(SRCS) $(FATFS)/ffconf.h $(FATFS)/ff.h
	$(CC) $(CFLAGS) -o $@ $(SRCS)

.PHONY: check clean
check: fatfs_utf8_test
	./fatfs_utf8_test

clean:
	rm -f fatfs_utf8_test
