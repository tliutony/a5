#
# Makefile to streamline testing on VM
# (c) 2015 Tony Liu and Reid Pryzant 
#

all:
	scp 16tl4@$PANIC:linux/drivers/block/loop.ko ~/
	scp 16tl4@$PANIC:linux/fs/wufs/wufs.ko ~/
	scp 16tl4@$PANIC:linux/fs/wufs/dab-wufs.img ~/ #this can be ours soon
