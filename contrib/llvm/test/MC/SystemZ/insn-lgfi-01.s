# RUN: llvm-mc -triple s390x-linux-gnu -show-encoding %s | FileCheck %s

#CHECK: lgfi	%r0, -2147483648        # encoding: [0xc0,0x01,0x80,0x00,0x00,0x00]
#CHECK: lgfi	%r0, -1                 # encoding: [0xc0,0x01,0xff,0xff,0xff,0xff]
#CHECK: lgfi	%r0, 0                  # encoding: [0xc0,0x01,0x00,0x00,0x00,0x00]
#CHECK: lgfi	%r0, 1                  # encoding: [0xc0,0x01,0x00,0x00,0x00,0x01]
#CHECK: lgfi	%r0, 2147483647         # encoding: [0xc0,0x01,0x7f,0xff,0xff,0xff]
#CHECK: lgfi	%r15, 0                 # encoding: [0xc0,0xf1,0x00,0x00,0x00,0x00]

	lgfi	%r0, -1 << 31
	lgfi	%r0, -1
	lgfi	%r0, 0
	lgfi	%r0, 1
	lgfi	%r0, (1 << 31) - 1
	lgfi	%r15, 0