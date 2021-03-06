#	$OpenBSD: Makefile,v 1.37 2014/03/18 22:36:37 miod Exp $

# Makefile for kernel tags files, init_sysent, etc.

ARCH=	amd64 armv7

all:
	@echo "Doing nothing.  Did you want make syscalls?"

syscalls:	init_sysent.c

SYSCALLSRC = makesyscalls.sh syscalls.conf syscalls.master
init_sysent.c syscalls.c ../sys/syscall.h ../sys/syscallargs.h: ${SYSCALLSRC}
	sh makesyscalls.sh syscalls.conf syscalls.master

# Kernel tags:
# tags files are built in the top-level directory for each architecture.
# Links to the correct tags file are placed in each source directory.
# We need to have links to tags files
# from the generic directories that are relative to the machine type, even
# via remote mounts; therefore we use symlinks to $SYSTAGS, which points at
# ${SYSDIR}/${MACHINE}/tags.

SYSTAGS=/var/db/sys_tags
SYSDIR=/sys

# Directories in which to place tags links (other than machine-dependent)
DGEN=	compat compat/common \
	compat/linux \
	conf \
	ddb \
	dev dev/eisa dev/ic dev/isa dev/pci dev/pcmcia dev/cardbus \
	dev/sun dev/tc \
	isofs isofs/cd9660 \
	kern \
	lib/libkern lib/libkern/arch/${MACHINE} \
	lib/libkern/arch/${MACHINE_ARCH} \
	msdosfs \
	net netinet netmpls \
	nfs \
	scsi \
	sys \
	tmpfs \
	ufs ufs/ffs ufs/ufs \
	uvm

tags::
	-for i in ${ARCH}; do \
	    (cd ../arch/$$i && make ${MFLAGS} tags); done

links::
	rm -f ${SYSTAGS}
	ln -s ${SYSDIR}/arch/${MACHINE}/tags ${SYSTAGS}
	-for i in ${DGEN}; do \
	    (cd ../$$i && { rm -f tags; ln -s ${SYSTAGS} tags; }) done
	-for i in ${ARCH}; do \
	    (cd ../arch/$$i && make ${MFLAGS} SYSTAGS=${SYSTAGS} links); done
