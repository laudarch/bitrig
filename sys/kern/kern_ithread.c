/*
 * Copyright (c) 2013 Christiano F. Haesbaert <haesbaert@haesbaert.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/ithread.h>
#include <sys/kthread.h>
#include <sys/queue.h>
#include <sys/malloc.h>

#include <uvm/uvm_extern.h>

#include <machine/intr.h>	/* XXX */

//#define ITHREAD_DEBUG
#ifdef ITHREAD_DEBUG
int ithread_debug = 10;
#define DPRINTF(l, x...)	do { if ((l) <= ithread_debug) printf(x); } while (0)
#else
#define DPRINTF(l, x...)
#endif	/* ITHREAD_DEBUG */

TAILQ_HEAD(, intrsource) ithreads = TAILQ_HEAD_INITIALIZER(ithreads);

void
ithread(void *v_is)
{
	struct intrsource *is = v_is;
	struct intrhand *ih;
	int irc, stray;

	KASSERT(curproc == is->is_proc);
	sched_peg_curproc(&cpu_info_primary);
	KERNEL_UNLOCK();

	DPRINTF(1, "ithread %u pin %d started\n",
	    curproc->p_pid, is->is_pin);

	for (; ;) {
		stray = 1;

		for (ih = is->is_handlers; ih != NULL; ih = ih->ih_next) {
			is->is_scheduled = 0;

			if ((ih->ih_flags & IPL_MPSAFE) == 0)
				KERNEL_LOCK();

			irc = (*ih->ih_fun)(ih->ih_arg);

			if ((ih->ih_flags & IPL_MPSAFE) == 0)
				KERNEL_UNLOCK();

			if (!irc)
				continue;
			ih->ih_count.ec_count++;
			stray = 0;

			if (!intr_shared_edge)
				break;
		}

		KASSERT(CRIT_DEPTH == 0);

		if (stray)
			DPRINTF(1, "stray interrupt pin %d ?\n", is->is_pin);

		intrsource_unmask(is);

		/*
		 * is->is_scheduled might have been set again (we got another
		 * interrupt), there would be a race here from checking
		 * is_scheduled and calling ithread_sleep(), but ithread sleep
		 * will double check this with a critical section, recovering
		 * from the race.
		 */
		if (!is->is_scheduled) {
			ithread_sleep(is);
			DPRINTF(20, "ithread %u woke up\n", curproc->p_pid);
		}
	}
}


/*
 * This is the hook that gets called in the interrupt frame.
 * XXX intr_shared_edge is not being used, and we're being pessimistic.
 */
void
ithread_run(struct intrsource *is)
{
	struct proc *p = is->is_proc;

	if (p == NULL) {
		is->is_scheduled = 1;
		DPRINTF(1, "received interrupt pin %d before ithread is ready",
		    is->is_pin);
		return;
	}

	DPRINTF(10, "ithread accepted interrupt pin %d\n", is->is_pin);

	SCHED_LOCK();		/* implies crit_enter() ! */

	is->is_scheduled = 1;

	switch (p->p_stat) {
	case SRUN:
	case SONPROC:
		break;
	case SSLEEP:
		/* XXX ithread may be blocked on a lock */
		if (p->p_wchan != p)
			goto unlock;
		p->p_wchan = NULL;
		p->p_stat = SRUN;
		p->p_slptime = 0;
		/*
		 * Setting the thread to runnable is cheaper than a normal
		 * process since the process state can be protected by blocking
		 * interrupts. There is also no need to choose a cpu since we're
		 * pinned. XXX we're not there yet and still rely on normal
		 * SCHED_LOCK crap.
		 */
		setrunqueue(p);
		resched_proc(p, p->p_priority);
		if (kernel_preemption) {
			if (p->p_priority < curproc->p_priority)
				curproc->p_preempt = 1;
		}
		break;
	default:
		SCHED_UNLOCK();
		panic("ithread_handler: unexpected thread state %d\n", p->p_stat);
	}
unlock:
	SCHED_UNLOCK();
}

void
ithread_register(struct intrsource *is)
{
	struct intrsource *is2;

	/* Prevent multiple inclusion */
	TAILQ_FOREACH(is2, &ithreads, entry) {
		if (is2 == is)
			return;
	}

	is->is_run = ithread_run;

	DPRINTF(1, "ithread_register intrsource pin %d\n", is->is_pin);

	TAILQ_INSERT_TAIL(&ithreads, is, entry);
}

void
ithread_deregister(struct intrsource *is)
{
	DPRINTF(1, "ithread_deregister intrsource pin %d\n", is->is_pin);

	is->is_run = NULL;

	TAILQ_REMOVE(&ithreads, is, entry);
}

void
ithread_forkall(void)
{
	struct intrsource *is;
	static int softs;
	char name[MAXCOMLEN+1];

	TAILQ_FOREACH(is, &ithreads, entry) {
		DPRINTF(1, "ithread forking intrsource pin %d\n", is->is_pin);

		if (is->is_pic == &softintr_pic) {
			snprintf(name, sizeof name, "ithread soft %d",
			    softs++);
			if (kthread_create(ithread, is, &is->is_proc, name))
				panic("ithread_forkall");
		} else {
			snprintf(name, sizeof name, "ithread pin %d",
			    is->is_pin);
			if (kthread_create(ithread, is, &is->is_proc, name))
				panic("ithread_forkall");
		}
	}
}

void
ithread_sleep(struct intrsource *is)
{
	struct proc *p = is->is_proc;

	KASSERT(curproc == p);
	KASSERT(p->p_stat == SONPROC);
	KERNEL_ASSERT_UNLOCKED();

	/*
	 * The check for is_scheduled and actually sleeping must be atomic.
	 */
	SCHED_LOCK();
	if (!is->is_scheduled) {
		p->p_wchan = p;
		p->p_wmesg = "interrupt";
		p->p_slptime = 0;
		p->p_priority = PVM & PRIMASK;
		p->p_stat = SSLEEP;
		mi_switch();
	} else
		SCHED_UNLOCK();
}
