/* ==== queue.h ============================================================
 * Copyright (c) 1993 by Chris Provenzano, proven@mit.edu
 * All rights reserved.
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
 *  This product includes software developed by Chris Provenzano.
 * 4. The name of Chris Provenzano may not be used to endorse or promote 
 *	  products derived from this software without specific prior written
 *	  permission.
 *
 * THIS SOFTWARE IS PROVIDED BY CHRIS PROVENZANO ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL CHRIS PROVENZANO BE LIABLE FOR ANY 
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR 
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT 
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF 
 * SUCH DAMAGE.
 *
 * $Id: queue.h,v 1.1 1998/07/21 13:19:12 peter Exp $
 *
 * Description : mutex header.
 *
 *  1.00 93/07/20 proven
 *      -Started coding this file.
 */
 
/*
 * New queue structures
 */
struct pthread_queue {
    struct pthread  *q_next;
    struct pthread  *q_last;
    void            *q_data;
};

/*
 * Static queue initialization values.
 */
#define PTHREAD_QUEUE_INITIALIZER	{ NULL, NULL, NULL }

/*
 * New functions
 * Should make pthread_queue_get a macro
 */

__BEGIN_DECLS

void    pthread_queue_init  	__P_((struct pthread_queue *));
void    pthread_queue_enq		__P_((struct pthread_queue *, struct pthread *));
int     pthread_queue_remove 	__P_((struct pthread_queue *, struct pthread *));
struct pthread *pthread_queue_get    __P_((struct pthread_queue *));
struct pthread *pthread_queue_deq    __P_((struct pthread_queue *));

__END_DECLS
