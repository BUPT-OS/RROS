// SPDX-License-Identifier: GPL-2.0
/*
 * SCLP line mode console driver
 *
 * Copyright IBM Corp. 1999, 2009
 * Author(s): Martin Peschke <mpeschke@de.ibm.com>
 *	      Martin Schwidefsky <schwidefsky@de.ibm.com>
 */

#include <linux/kmod.h>
#include <linux/console.h>
#include <linux/init.h>
#include <linux/panic_notifier.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/termios.h>
#include <linux/err.h>
#include <linux/reboot.h>
#include <linux/gfp.h>

#include "sclp.h"
#include "sclp_rw.h"
#include "sclp_tty.h"

#define sclp_console_major 4		/* TTYAUX_MAJOR */
#define sclp_console_minor 64
#define sclp_console_name  "ttyS"

/* Lock to guard over changes to global variables */
static DEFINE_SPINLOCK(sclp_con_lock);
/* List of free pages that can be used for console output buffering */
static LIST_HEAD(sclp_con_pages);
/* List of full struct sclp_buffer structures ready for output */
static LIST_HEAD(sclp_con_outqueue);
/* Pointer to current console buffer */
static struct sclp_buffer *sclp_conbuf;
/* Timer for delayed output of console messages */
static struct timer_list sclp_con_timer;
/* Flag that output queue is currently running */
static int sclp_con_queue_running;

/* Output format for console messages */
#define SCLP_CON_COLUMNS	320
#define SPACES_PER_TAB		8

static void
sclp_conbuf_callback(struct sclp_buffer *buffer, int rc)
{
	unsigned long flags;
	void *page;

	do {
		page = sclp_unmake_buffer(buffer);
		spin_lock_irqsave(&sclp_con_lock, flags);

		/* Remove buffer from outqueue */
		list_del(&buffer->list);
		list_add_tail((struct list_head *) page, &sclp_con_pages);

		/* Check if there is a pending buffer on the out queue. */
		buffer = NULL;
		if (!list_empty(&sclp_con_outqueue))
			buffer = list_first_entry(&sclp_con_outqueue,
						  struct sclp_buffer, list);
		if (!buffer) {
			sclp_con_queue_running = 0;
			spin_unlock_irqrestore(&sclp_con_lock, flags);
			break;
		}
		spin_unlock_irqrestore(&sclp_con_lock, flags);
	} while (sclp_emit_buffer(buffer, sclp_conbuf_callback));
}

/*
 * Finalize and emit first pending buffer.
 */
static void sclp_conbuf_emit(void)
{
	struct sclp_buffer* buffer;
	unsigned long flags;
	int rc;

	spin_lock_irqsave(&sclp_con_lock, flags);
	if (sclp_conbuf)
		list_add_tail(&sclp_conbuf->list, &sclp_con_outqueue);
	sclp_conbuf = NULL;
	if (sclp_con_queue_running)
		goto out_unlock;
	if (list_empty(&sclp_con_outqueue))
		goto out_unlock;
	buffer = list_first_entry(&sclp_con_outqueue, struct sclp_buffer,
				  list);
	sclp_con_queue_running = 1;
	spin_unlock_irqrestore(&sclp_con_lock, flags);

	rc = sclp_emit_buffer(buffer, sclp_conbuf_callback);
	if (rc)
		sclp_conbuf_callback(buffer, rc);
	return;
out_unlock:
	spin_unlock_irqrestore(&sclp_con_lock, flags);
}

/*
 * Wait until out queue is empty
 */
static void sclp_console_sync_queue(void)
{
	unsigned long flags;

	spin_lock_irqsave(&sclp_con_lock, flags);
	del_timer(&sclp_con_timer);
	while (sclp_con_queue_running) {
		spin_unlock_irqrestore(&sclp_con_lock, flags);
		sclp_sync_wait();
		spin_lock_irqsave(&sclp_con_lock, flags);
	}
	spin_unlock_irqrestore(&sclp_con_lock, flags);
}

/*
 * When this routine is called from the timer then we flush the
 * temporary write buffer without further waiting on a final new line.
 */
static void
sclp_console_timeout(struct timer_list *unused)
{
	sclp_conbuf_emit();
}

/*
 * Drop oldest console buffer if sclp_con_drop is set
 */
static int
sclp_console_drop_buffer(void)
{
	struct list_head *list;
	struct sclp_buffer *buffer;
	void *page;

	if (!sclp_console_drop)
		return 0;
	list = sclp_con_outqueue.next;
	if (sclp_con_queue_running)
		/* The first element is in I/O */
		list = list->next;
	if (list == &sclp_con_outqueue)
		return 0;
	list_del(list);
	buffer = list_entry(list, struct sclp_buffer, list);
	page = sclp_unmake_buffer(buffer);
	list_add_tail((struct list_head *) page, &sclp_con_pages);
	return 1;
}

/*
 * Writes the given message to S390 system console
 */
static void
sclp_console_write(struct console *console, const char *message,
		   unsigned int count)
{
	unsigned long flags;
	void *page;
	int written;

	if (count == 0)
		return;
	spin_lock_irqsave(&sclp_con_lock, flags);
	/*
	 * process escape characters, write message into buffer,
	 * send buffer to SCLP
	 */
	do {
		/* make sure we have a console output buffer */
		if (sclp_conbuf == NULL) {
			if (list_empty(&sclp_con_pages))
				sclp_console_full++;
			while (list_empty(&sclp_con_pages)) {
				if (sclp_console_drop_buffer())
					break;
				spin_unlock_irqrestore(&sclp_con_lock, flags);
				sclp_sync_wait();
				spin_lock_irqsave(&sclp_con_lock, flags);
			}
			page = sclp_con_pages.next;
			list_del((struct list_head *) page);
			sclp_conbuf = sclp_make_buffer(page, SCLP_CON_COLUMNS,
						       SPACES_PER_TAB);
		}
		/* try to write the string to the current output buffer */
		written = sclp_write(sclp_conbuf, (const unsigned char *)
				     message, count);
		if (written == count)
			break;
		/*
		 * Not all characters could be written to the current
		 * output buffer. Emit the buffer, create a new buffer
		 * and then output the rest of the string.
		 */
		spin_unlock_irqrestore(&sclp_con_lock, flags);
		sclp_conbuf_emit();
		spin_lock_irqsave(&sclp_con_lock, flags);
		message += written;
		count -= written;
	} while (count > 0);
	/* Setup timer to output current console buffer after 1/10 second */
	if (sclp_conbuf != NULL && sclp_chars_in_buffer(sclp_conbuf) != 0 &&
	    !timer_pending(&sclp_con_timer)) {
		mod_timer(&sclp_con_timer, jiffies + HZ / 10);
	}
	spin_unlock_irqrestore(&sclp_con_lock, flags);
}

static struct tty_driver *
sclp_console_device(struct console *c, int *index)
{
	*index = c->index;
	return sclp_tty_driver;
}

/*
 * This panic/reboot notifier makes sure that all buffers
 * will be flushed to the SCLP.
 */
static int sclp_console_notify(struct notifier_block *self,
			       unsigned long event, void *data)
{
	/*
	 * Perform the lock check before effectively getting the
	 * lock on sclp_conbuf_emit() / sclp_console_sync_queue()
	 * to prevent potential lockups in atomic context.
	 */
	if (spin_is_locked(&sclp_con_lock))
		return NOTIFY_DONE;

	sclp_conbuf_emit();
	sclp_console_sync_queue();

	return NOTIFY_DONE;
}

static struct notifier_block on_panic_nb = {
	.notifier_call = sclp_console_notify,
	.priority = INT_MIN + 1, /* run the callback late */
};

static struct notifier_block on_reboot_nb = {
	.notifier_call = sclp_console_notify,
	.priority = INT_MIN + 1, /* run the callback late */
};

/*
 * used to register the SCLP console to the kernel and to
 * give printk necessary information
 */
static struct console sclp_console =
{
	.name = sclp_console_name,
	.write = sclp_console_write,
	.device = sclp_console_device,
	.flags = CON_PRINTBUFFER,
	.index = 0 /* ttyS0 */
};

/*
 * called by console_init() in drivers/char/tty_io.c at boot-time.
 */
static int __init
sclp_console_init(void)
{
	void *page;
	int i;
	int rc;

	/* SCLP consoles are handled together */
	if (!(CONSOLE_IS_SCLP || CONSOLE_IS_VT220))
		return 0;
	rc = sclp_rw_init();
	if (rc)
		return rc;
	/* Allocate pages for output buffering */
	for (i = 0; i < sclp_console_pages; i++) {
		page = (void *) get_zeroed_page(GFP_KERNEL | GFP_DMA);
		list_add_tail(page, &sclp_con_pages);
	}
	sclp_conbuf = NULL;
	timer_setup(&sclp_con_timer, sclp_console_timeout, 0);

	/* enable printk-access to this driver */
	atomic_notifier_chain_register(&panic_notifier_list, &on_panic_nb);
	register_reboot_notifier(&on_reboot_nb);
	register_console(&sclp_console);
	return 0;
}

console_initcall(sclp_console_init);
