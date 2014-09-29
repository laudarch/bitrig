/*	$OpenBSD: intr.c,v 1.37 2014/07/12 18:44:41 tedu Exp $	*/
/*	$NetBSD: intr.c,v 1.3 2003/03/03 22:16:20 fvdl Exp $	*/

/*
 * Copyright 2002 (c) Wasabi Systems, Inc.
 * All rights reserved.
 *
 * Written by Frank van der Linden for Wasabi Systems, Inc.
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
 *      This product includes software developed for the NetBSD Project by
 *      Wasabi Systems, Inc.
 * 4. The name of Wasabi Systems, Inc. may not be used to endorse
 *    or promote products derived from this software without specific prior
 *    written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY WASABI SYSTEMS, INC. ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL WASABI SYSTEMS, INC
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/* #define	INTRDEBUG */

#include <sys/param.h> 
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/syslog.h>
#include <sys/device.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/errno.h>
#include <sys/ithread.h>

#include <machine/atomic.h>
#include <machine/i8259.h>
#include <machine/cpu.h>
#include <machine/pio.h>
#include <machine/cpufunc.h>

#include "ioapic.h"
#include "lapic.h"

#if NIOAPIC > 0
#include <machine/i82093var.h> 
#include <machine/mpbiosvar.h>
#endif

#if NLAPIC > 0
#include <machine/i82489var.h>
#endif

/*
 * Protects the interrupt handler chains from modification while we are
 * accessing them.
 */
struct mutex	intr_lock = MUTEX_INITIALIZER(IPL_NONE);

/*
 * Fill in default interrupt table (in case of spurious interrupt
 * during configuration of kernel), setup interrupt control unit
 */
void
intr_default_setup(void)
{
	int i;

	mtx_enter(&intr_lock);
	/* icu vectors */
	for (i = 0; i < NUM_LEGACY_IRQS; i++) {
		idt_allocmap[ICU_OFFSET + i] = 1;
		setgate(&idt[ICU_OFFSET + i],
		    i8259_stubs[i].ist_entry, 0, SDT_SYS386IGT,
		    SEL_KPL, GSEL(GCODE_SEL, SEL_KPL));
	}

	/*
	 * Eventually might want to check if it's actually there.
	 */
	i8259_default_setup();
	mtx_leave(&intr_lock);
}

/*
 * Handle a NMI, possibly a machine check.
 * return true to panic system, false to ignore.
 */
int
x86_nmi(void)
{
	log(LOG_CRIT, "NMI port 61 %x, port 70 %x\n", inb(0x61), inb(0x70));
	return(0);
}

/* 
 * Allocate an interrupt slot on the given cpu.
 * - Called with intr_lock held.
 */
int
intr_allocate_slot_cpu(struct cpu_info *ci, struct pic *pic, int pin,
    int *index)
{
	int start, slot, i;
	struct intrsource *isp;

	MUTEX_ASSERT_LOCKED(&intr_lock);

	start = CPU_IS_PRIMARY(ci) ? NUM_LEGACY_IRQS : 0;
	slot = -1;

	for (i = 0; i < start; i++) {
		isp = ci->ci_isources[i];
		if (isp != NULL && isp->is_pic == pic && isp->is_pin == pin) {
			slot = i;
			start = MAX_INTR_SOURCES;
			break;
		}
	}
	for (i = start; i < MAX_INTR_SOURCES ; i++) {
		isp = ci->ci_isources[i];
		if (isp != NULL && isp->is_pic == pic && isp->is_pin == pin) {
			slot = i;
			break;
		}
		if (isp == NULL && slot == -1) {
			slot = i;
			continue;
		}
	}
	if (slot == -1)
		return EBUSY;

	isp = ci->ci_isources[slot];
	if (isp == NULL) {
		isp = malloc(sizeof (struct intrsource), M_DEVBUF,
		    M_NOWAIT | M_ZERO);
		if (isp == NULL)
			return ENOMEM;
		snprintf(isp->is_evname, sizeof (isp->is_evname),
		    "pin %d", pin);
		ci->ci_isources[slot] = isp;
	}

	*index = slot;
	return 0;
}

/*
 * A simple round-robin allocator to assign interrupts to CPUs.
 */
int
intr_allocate_slot(struct pic *pic, int legacy_irq, int pin, int level,
    struct cpu_info **cip, int *index, int *idt_slot)
{
	CPU_INFO_ITERATOR cii;
	struct cpu_info *ci;
	struct intrsource *isp;
	int slot, idtvec, error = 0;

	/*
	 * This function is always called with the intr_lock locked because
	 * a slot it not allocated until the handler has been inserted to
	 * the chain and these two operations must be atomic. Perhaps when
	 * we move to allocating vectors more equally over the different
	 * cpus then this locking could be a bit more fine grained.
	 * establishing new interrupt handlers is a rare enough event that
	 * it should not matter.
	 */
	MUTEX_ASSERT_LOCKED(&intr_lock);

	/*
	 * If a legacy IRQ is wanted, try to use a fixed slot pointing
	 * at the primary CPU. In the case of IO APICs, multiple pins
	 * may map to one legacy IRQ, but they should not be shared
	 * in that case, so the first one gets the legacy slot, but
	 * a subsequent allocation with a different pin will get
	 * a different slot.
	 */
	if (legacy_irq != -1) {
		ci = &cpu_info_primary;
		/* must check for duplicate pic + pin first */
		for (slot = 0 ; slot < MAX_INTR_SOURCES ; slot++) {
			isp = ci->ci_isources[slot];
			if (isp != NULL && isp->is_pic == pic &&
			    isp->is_pin == pin ) {
				goto duplicate;
			}
		}
		slot = legacy_irq;
		isp = ci->ci_isources[slot];
		if (isp == NULL) {
			isp = malloc(sizeof (struct intrsource), M_DEVBUF,
			     M_NOWAIT|M_ZERO);
			if (isp == NULL) {
				return (ENOMEM);
			}
			snprintf(isp->is_evname, sizeof (isp->is_evname),
			    "pin %d", pin);

			ci->ci_isources[slot] = isp;
		} else {
			if (isp->is_pic != pic || isp->is_pin != pin) {
				if (pic == &i8259_pic) {
					return (EINVAL);
				}
				goto other;
			}
		}
duplicate:
		if (pic == &i8259_pic)
			idtvec = ICU_OFFSET + legacy_irq;
		else if (isp->is_idtvec == 0)
			idtvec = idt_vec_alloc();
		else
			idtvec = isp->is_idtvec;
	} else {
other:
		/*
		 * Otherwise, look for a free slot elsewhere. Do the primary
		 * CPU first.
		 */
		ci = &cpu_info_primary;
		error = intr_allocate_slot_cpu(ci, pic, pin, &slot);
		if (error == 0)
			goto found;

		/*
		 * ..now try the others.
		 */
		CPU_INFO_FOREACH(cii, ci) {
			if (CPU_IS_PRIMARY(ci))
				continue;
			error = intr_allocate_slot_cpu(ci, pic, pin, &slot);
			if (error == 0)
				goto found;
		}
		return (EBUSY);
found:
		idtvec = idt_vec_alloc();
		if (idtvec == 0) {
			free(ci->ci_isources[slot], M_DEVBUF, 0);
			ci->ci_isources[slot] = NULL;
			return (EBUSY);
		}
	}
	*idt_slot = idtvec;
	*index = slot;
	*cip = ci;

	return (0);
}

/*
 * True if the system has any non-level interrupts which are shared
 * on the same pin.
 */
int	intr_shared_edge;

void *
intr_establish(int legacy_irq, struct pic *pic, int pin, int type, int level,
    int (*handler)(void *), void *arg, const char *what)
{
	struct intrhand **p, *q, *ih;
	struct cpu_info *ci;
	int slot, error, idt_vec;
	struct intrsource *source;
	struct intrstub *stubp;
	int flags;

#ifdef DIAGNOSTIC
	if (legacy_irq != -1 && (legacy_irq < 0 || legacy_irq > 15))
		panic("intr_establish: bad legacy IRQ value");

	if (legacy_irq == -1 && pic == &i8259_pic)
		panic("intr_establish: non-legacy IRQ on i8259");
#endif
	/* no point in sleeping unless someone can free memory. */
	ih = malloc(sizeof *ih, M_DEVBUF, cold ? M_NOWAIT : M_WAITOK);
	if (ih == NULL) {
		printf("intr_establish: can't allocate handler info\n");
		return NULL;
	}

	flags = level & IPL_FLAGS;
	level &= ~IPL_FLAGS;

	KASSERT(level <= IPL_TTY || level >= IPL_CLOCK || flags & IPL_MPSAFE);

	/*
	 * From here on we are messing with the handler chains and must be
	 * locked.
	 */
	mtx_enter(&intr_lock);
	error = intr_allocate_slot(pic, legacy_irq, pin, level, &ci, &slot,
	    &idt_vec);
	if (error != 0) {
		mtx_leave(&intr_lock);
		free(ih, M_DEVBUF, 0);
		printf("failed to allocate interrupt slot for PIC %s pin %d\n",
		    pic->pic_dev.dv_xname, pin);
		return NULL;
	}

	source = ci->ci_isources[slot];

	if (source->is_handlers != NULL &&
	    source->is_pic->pic_type != pic->pic_type) {
		mtx_leave(&intr_lock);
		free(ih, M_DEVBUF, 0);
		printf("intr_establish: can't share intr source between "
		       "different PIC types (legacy_irq %d pin %d slot %d)\n",
		    legacy_irq, pin, slot);
		return NULL;
	}


	source->is_pin = pin;
	source->is_pic = pic;

	switch (source->is_type) {
	case IST_NONE:
		source->is_type = type;
		break;
	case IST_EDGE:
		intr_shared_edge = 1;
		/* FALLTHROUGH */
	case IST_LEVEL:
		if (source->is_type == type)
			break;
	case IST_PULSE:
		if (type != IST_NONE) {
			mtx_leave(&intr_lock);
			printf("intr_establish: pic %s pin %d: can't share "
			       "type %d with %d\n", pic->pic_name, pin,
				source->is_type, type);
			free(ih, M_DEVBUF, 0);
			return NULL;
		}
		break;
	default:
		mtx_leave(&intr_lock);
		panic("intr_establish: bad intr type %d for pic %s pin %d",
		    source->is_type, pic->pic_dev.dv_xname, pin);
	}

	if (!cold)
		pic->pic_hwmask(pic, pin);

	/*
	 * Figure out where to put the handler.
	 * This is O(N^2), but we want to preserve the order, and N is
	 * generally small.
	 */
	for (p = &source->is_handlers;
	     (q = *p) != NULL && q->ih_level > level;
	     p = &q->ih_next)
		;

	ih->ih_fun = handler;
	ih->ih_arg = arg;
	ih->ih_next = *p;
	ih->ih_level = level;
	ih->ih_flags = flags; 
	ih->ih_pin = pin;
	ih->ih_cpu = ci;
	ih->ih_slot = slot;
	evcount_attach(&ih->ih_count, what, &source->is_idtvec);

	*p = ih;

	if (source->is_idtvec != idt_vec) {
		if (source->is_idtvec != 0 && source->is_idtvec != idt_vec)
			idt_vec_free(source->is_idtvec);
		source->is_idtvec = idt_vec;
		stubp = type == IST_LEVEL ?
		    &pic->pic_level_stubs[slot] : &pic->pic_edge_stubs[slot];
		setgate(&idt[idt_vec], stubp->ist_entry, 0, SDT_SYS386IGT,
		    SEL_KPL, GSEL(GCODE_SEL, SEL_KPL));
		ithread_register(source);
	}

	pic->pic_addroute(pic, ci, pin, idt_vec, type);

	if (!cold)
		pic->pic_hwunmask(pic, pin);
	mtx_leave(&intr_lock);

#ifdef INTRDEBUG
	printf("allocated pic %s type %s pin %d level %d to cpu%u slot %d idt entry %d\n",
	    pic->pic_name, type == IST_EDGE ? "edge" : "level", pin, level,
	    ci->ci_apicid, slot, idt_vec);
#endif

	return (ih);
}

/*
 * Deregister an interrupt handler.
 */
void
intr_disestablish(struct intrhand *ih)
{
	struct intrhand **p, *q;
	struct cpu_info *ci;
	struct pic *pic;
	struct intrsource *source;
	int idtvec;

	if (!cold)
		panic("ithreads do not support intr_disestablish if not cold yet");

	ci = ih->ih_cpu;

	mtx_enter(&intr_lock);
	source = ci->ci_isources[ih->ih_slot];
	pic = source->is_pic;
	idtvec = source->is_idtvec;

	pic->pic_hwmask(pic, ih->ih_pin);	
	x86_atomic_clearbits_u64(&ci->ci_ipending, (1UL << ih->ih_slot));

	/*
	 * Remove the handler from the chain.
	 */
	for (p = &source->is_handlers; (q = *p) != NULL && q != ih;
	     p = &q->ih_next)
		;
	if (q == NULL) {
		mtx_leave(&intr_lock);
		panic("intr_disestablish: handler not registered");
	}

	*p = q->ih_next;

	if (source->is_handlers == NULL)
		pic->pic_delroute(pic, ci, ih->ih_pin, idtvec, source->is_type);
	else
		pic->pic_hwunmask(pic, ih->ih_pin);

#ifdef INTRDEBUG
	printf("cpu%u: remove slot %d (pic %s pin %d vec %d)\n",
	    ci->ci_apicid, ih->ih_slot, pic->pic_dev.dv_xname, ih->ih_pin,
	    idtvec);
#endif

	if (source->is_handlers == NULL) {
		free(source, M_DEVBUF, 0);
		ithread_deregister(source);
		ci->ci_isources[ih->ih_slot] = NULL;
		if (pic != &i8259_pic)
			idt_vec_free(idtvec);
	}

	evcount_detach(&ih->ih_count);
	mtx_leave(&intr_lock);

	free(ih, M_DEVBUF, 0);
}

int
intr_handler(struct intrframe *frame, struct intrhand *ih)
{
	int rc;
#ifdef MULTIPROCESSOR
	int need_lock;

	if (ih->ih_flags & IPL_MPSAFE)
		need_lock = 0;
	else
		need_lock = frame->if_ppl < IPL_SCHED;

	if (need_lock)
		KERNEL_LOCK();
#endif
	rc = (*ih->ih_fun)(ih->ih_arg ? ih->ih_arg : frame);
#ifdef MULTIPROCESSOR
	if (need_lock)
		KERNEL_UNLOCK();
#endif
	return rc;
}

#define CONCAT(x,y)	__CONCAT(x,y)

/*
 * Fake interrupt handler structures for the benefit of symmetry with
 * other interrupt sources.
 */
struct intrhand fake_softclock_intrhand;
struct intrhand fake_softnet_intrhand;
struct intrhand fake_softtty_intrhand;
struct intrhand fake_timer_intrhand;
struct intrhand fake_ipi_intrhand;

#if NLAPIC > 0 && defined(MULTIPROCESSOR) && 0
static char *x86_ipi_names[X86_NIPI] = X86_IPI_NAMES;
#endif

/*
 * Initialize all handlers that aren't dynamically allocated, and exist
 * for each CPU.
 */
void
cpu_intr_init(struct cpu_info *ci)
{
#if NLAPIC > 0
	struct intrsource *isp;
#ifdef MULTIPROCESSOR
	isp = malloc(sizeof (struct intrsource), M_DEVBUF, M_NOWAIT|M_ZERO);
	if (isp == NULL)
		panic("can't allocate fixed interrupt source");
	fake_ipi_intrhand.ih_level = IPL_IPI;
	isp->is_handlers = &fake_ipi_intrhand;
	isp->is_pic = &local_pic;
	isp->is_run = x86_ipi_handler;
	ci->ci_isources[LIR_IPI] = isp;
#endif
	isp = malloc(sizeof (struct intrsource), M_DEVBUF, M_NOWAIT|M_ZERO);
	if (isp == NULL)
		panic("can't allocate fixed interrupt source");
	fake_timer_intrhand.ih_level = IPL_CLOCK;
	isp->is_handlers = &fake_timer_intrhand;
	isp->is_pic = &local_pic;
	isp->is_run = Xfakeclock;
	ci->ci_isources[LIR_TIMER] = isp;
#endif

}

void
intr_printconfig(void)
{
#ifdef INTRDEBUG
	int i;
	struct intrhand *ih;
	struct intrsource *isp;
	struct cpu_info *ci;
	CPU_INFO_ITERATOR cii;

	CPU_INFO_FOREACH(cii, ci) {
		printf("cpu%d: interrupt masks:\n", ci->ci_apicid);
		for (i = 0; i < NIPL; i++)
			printf("IPL %d mask %lx unmask %lx\n", i,
			    (u_long)ci->ci_imask[i], (u_long)ci->ci_iunmask[i]);
		mtx_enter(&intr_lock);
		for (i = 0; i < MAX_INTR_SOURCES; i++) {
			isp = ci->ci_isources[i];
			if (isp == NULL)
				continue;
			printf("cpu%u source %d is pin %d from pic %s\n",
			    ci->ci_apicid, i, isp->is_pin,
			    isp->is_pic->pic_name);
			for (ih = isp->is_handlers; ih != NULL;
			     ih = ih->ih_next)
				printf("\thandler %p level %d\n",
				    ih->ih_fun, ih->ih_level);

		}
		mtx_leave(&intr_lock);
	}
#endif
}

/*
 * Software interrupt registration
 *
 * We hand-code this to ensure that it's atomic.
 *
 * XXX always scheduled on the current CPU.
 */
void
softintr(int sir)
{
	struct cpu_info *ci = curcpu();

	__asm volatile("lock; orq %1, %0" :
	    "=m"(ci->ci_ipending) : "ir" (1UL << sir));
}

int
splraise(int s)
{
	return 0;
}

int
spllower(int s)
{
	return 0;
}

#ifdef DIAGNOSTIC
void
splassert_check(int wantipl, const char *func)
{
}
#endif
