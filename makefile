#
# Makefile to streamline testing on VM
# (c) 2015 Tony Liu and Reid Pryzant 
#

all:
	scp 16tl4@$(PANIC):linux/drivers/block/loop.ko ~/;
	scp 16tl4@$(PANIC):linux/fs/wufs/wufs.ko ~/;
	scp 16tl4@$(PANIC):linux/fs/wufs/cow.img ~/; #this can be ours soon

	insmod loop.ko;
	insmod wufs.ko;
	dmesg | tail;
	losetup /dev/loop0 cow.img;
	mount /dev/loop0 /mnt;

clean:
	umount /mnt
	losetup -d /dev/loop0
	rmmod wufs 
	rmmod loop
	rm loop.ko
	rm wufs.ko
	rm cow.img