#
# Makefile for the Williams Uncountable File System
# (c) 2011, 2015 duane a. bailey
#
now:
	make -C ~/linux M=$(PWD)


obj-$(CONFIG_WUFS_FS) += wufs.o

wufs-objs := bitmap.o indirect.o namei.o inode.o file.o dir.o

clean:	
	make -C ~/linux M=$(PWD) clean
	@rm -f *.o *~ \#*
	@echo Clean.

love:
	@echo "Not war, baby."
