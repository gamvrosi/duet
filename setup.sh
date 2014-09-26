#!/bin/bash

# Check that we're running on Ubuntu. We're picky like that.
if [[ !`grep "DISTRIB_ID=Ubuntu" /etc/lsb-release` ]]; then
	echo "Duet is currently only supported on Ubuntu. Sorry." >&2
	exit 1
fi

while getopts ":dci" opt; do
	case $opt in
	d)
		echo "Installing dependencies..."

		# Install kernel dependencies
		sudo apt-get install build-essential kernel-package \
			libncurses5-dev

		# Install tool dependencies
		sudo apt-get install uuid-dev libblkid-dev libz-dev liblzo2-dev \
			libattr1-dev libacl1-dev e2fslibs-dev

		echo "Done processing dependencies. Exiting."
		exit 0
		;;
	c)
		# Prep the environment for future recompiles
		export CLEAN_SOURCE=no
		export CONCURRENCY_LEVEL=33
		echo CLEAN_SOURCE=$CLEAN_SOURCE
		echo CONCURRENCY_LEVEL=$CONCURRENCY_LEVEL

		# (re)compile the kernel
		cd linux-3.13.6+duet
		time fakeroot make-kpkg --initrd --append-to-version=+duet \
			kernel_image kernel_headers

		# ...and (re)compile the btrfs tools
		cd ../btrfs-progs-3.12+duet
		make

		# ...and (re)compile the duet tools
		cd ../duet-progs
		make

		cd ..
		;;
	i)
		# Install the kernel
		sudo dpkg -i linux-headers-3.13.6+duet_3.13.6+duet-10.00.Custom_amd64.deb
		sudo dpkg -i linux-image-3.13.6+duet_3.13.6+duet-10.00.Custom_amd64.deb

		# Install the btrfs tools (in /usr/local/bin)
		cd btrfs-progs-3.12+duet
		sudo make install

		# Install the duet tools (in /usr/local/bin)
		cd ../duet-progs
		sudo make install

		cd ..
		;;
	\?)
		echo "Invalid option: -$OPTARG" >&2
		;;
	esac
done