/*
 * Copyright 1997 Niels Provos <provos@physnet.uni-hamburg.de>
 * All rights reserved.
 *
 * This code is originally from Angelos D. Keromytis, kermit@forthnet.gr
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *      This product includes software developed by Niels Provos.
 * 4. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef lint
static char rcsid[] = "$Id: startkey.c,v 1.1 1997/07/18 22:50:23 provos Exp $";
#endif

#include <stdio.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "photuris.h"

void
usage(char *name)
{
	fprintf(stderr, "Usage: %s [-d dir] <ip address>\n", name);
	exit();
}


/*
 * Just a program to start a key establishment session
 */

int
main(int argc, char **argv)
{
	int fd, ch;
	char *dir = PHOTURIS_DIR;

	while ((ch = getopt(argc, argv, "d:")) != -1)
		switch((char)ch) {
			case 'd':
				dir = optarg;
				break;
			default:
				usage(argv[0]);
		}

        if (argc - optind != 1)
                usage(argv[0]);

	argc -= optind;
	argv += optind;


	if (chdir(dir) == -1) {
		fprintf(stderr, "Can't change dir to %s\n", dir);
		exit(-1);
	}

	fd = open(PHOTURIS_FIFO, O_WRONLY | O_NONBLOCK, 0);
	if (fd == -1)
	{
		perror("open()");
		exit(-1);
	}

	if (write(fd, argv[0], strlen(argv[0])) != strlen(argv[0]))
	{
		perror("write()");
		exit(-1);
	}

	close(fd);

	exit(0);
}
