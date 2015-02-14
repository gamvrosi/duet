/*
 * Copyright (C) 2014-2015 George Amvrosiadis.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */
#include <linux/fs.h>
#include "common.h"

/*
 * Notification models are models defining the behaviour of the framework when a
 * new page event happens. Tasks can subscribe for a specific notification model
 * at session registration time. Each state in the model represents the page
 * status that is returned by the fetch call.
 *
 *
 * Differential model (DUET_PAGE_DIFF): Notify on page status changes (additions
 * or removals) that have occurred in the cache, that the task has not been
 * informed about.
 * Example users: rsync, defrag, garbage collection, backup.
 *
 *  +---------+  fetch,ADD  +------------+     ADD     +-------+
 *  | Page    | ----------> |  Page is   | ----------> | Page  |
 *  | removed | <---------- | up-to-date | <---------- | added |
 *  +---------+     REM     +------------+  fetch,REM  +-------+
 *
 * Access model (DUET_PAGE_AXS): Notify on page data accesses (additions or
 * modifications) that have taken place in the cache, that the task has not been
 * informed about.
 * Example users: scrubbing, any inotify-based application.
 *
 *  +----------+      ADD       +-------+
 *  | Page     |--------------->| Page  |---+ ADD
 *  | uptodate |<---------------| added |<--+
 *  +----------+     fetch      +-------+
 *     | ^                         ^ |
 *     | |                         | |
 *     | | fetch  +----------+ ADD | |
 *     | +--------| Page     |-----+ |
 *     +--------->| modified |<------+
 *       MOD      +----------+   MOD
 *                    | ^
 *                    | | MOD
 *                    +-+
 *
 * Other possible models:
 * Insertion model (DUET_PAGE_ADD): Only notify on page insertion events.
 *
 * +------------+    DUET_EVT_ADD     +-------+
 * |  Page is   | ------------------> | Page  | ---+
 * | up-to-date | <------------------ | added | <--+ PAGE_EVT_ADD
 * +------------+       fetch()       +-------+
 *
 * Removal model (DUET_PAGE_REM): Only notify on page removal events.
 *
 * +------------+    DUET_EVT_REM     +---------+
 * |  Page is   | ------------------> | Page    | ---+
 * | up-to-date | <------------------ | removed | <--+ PAGE_EVT_REM
 * +------------+       fetch()       +---------+
 *
 * Union model (DUET_PAGE_BOTH): Notify on all insertion/removal events.
 *
 *  REM +---------+  fetch  +------------+   ADD   +-------+ ADD
 * +--- | Page    | ------> |  Page is   | ------> | Page  | ---+
 * +--> | removed | <------ | up-to-date | <------ | added | <--+
 *      +---------+   REM   +------------+  fetch  +-------+
 *           |                     ^                   |
 *           |                     | fetch             |
 *           |  ADD   +----------------------+    REM  |
 *           +------->| Page added & removed |<--------+
 *                    +----------------------+
 */

/*
 * Fetches up to itreq items from the ItemTree. The number of items fetched is
 * given by itret. Items are checked against the bitmap, and discarded if they
 * have been marked; this is possible because an insertion could have happened
 * between the last fetch and the last mark.
 * TODO
 */
int duet_fetch(__u8 taskid, __u16 itreq, struct duet_item *items, __u16 *itret)
{
	struct rb_node *rbnode;
	struct item_rbnode *tnode;
	struct duet_task *task = duet_find_task(taskid);

	if (!task) {
		printk(KERN_ERR "duet_fetch: invalid taskid (%d)\n", taskid);
		return 1;	
	}

	/*
	 * We'll either run out of items, or grab itreq items.
	 * We also skip the outer lock. Suck it interrupts.
	 */
	*itret = 0;

again:
	spin_lock_irq(&task->itm_lock);

	/* Grab first item from the tree */
	if (!RB_EMPTY_ROOT(&task->itmtree)) {
		rbnode = rb_first(&task->itmtree);
		tnode = rb_entry(rbnode, struct item_rbnode, node);
		rb_erase(rbnode, &task->itmtree);
		spin_unlock_irq(&task->itm_lock);
	} else {
		spin_unlock_irq(&task->itm_lock);
		goto done;
	}

	/* Copy fields off to items array */
	items[*itret].ino = tnode->item->ino;
	items[*itret].idx = tnode->item->idx;
	items[*itret].evt = tnode->item->evt;
	tnode_dispose(tnode, NULL, NULL);

	(*itret)++;
	if (*itret < itreq)
		goto again;

done:
	/* decref and wake up cleaner if needed */
	if (atomic_dec_and_test(&task->refcount))
		wake_up(&task->cleaner_queue);

	return 0;
}
EXPORT_SYMBOL_GPL(duet_fetch);

/*
 * Inserts a node in an ItemTree of pages. Assumes the relevant locks have been
 * obtained. Returns 1 on failure.
 */
static int itmtree_insert(struct duet_task *task, struct item_rbnode *tnode)
{
	int found = 0;
	struct rb_node **link, *parent = NULL;
	struct item_rbnode *cur;

	link = &task->itmtree.rb_node;

	while (*link) {
		parent = *link;
		cur = rb_entry(parent, struct item_rbnode, node);

		/* We order based on (inode, page index) */
		if (cur->item->ino > tnode->item->ino) {
			link = &(*link)->rb_left;
		} else if (cur->item->ino < tnode->item->ino) {
			link = &(*link)->rb_right;
		} else {
			/* Found inode, look for index */
			if (cur->item->idx > tnode->item->idx) {
				link = &(*link)->rb_left;
			} else if (cur->item->idx < tnode->item->idx) {
				link = &(*link)->rb_right;
			} else {
				found = 1;
				break;
			}
		}
	}

	duet_dbg(KERN_DEBUG "duet: %s page node (ino%lu, idx%lu, e%u)\n",
		found ? "will not insert" : "will insert",
		tnode->item->ino, tnode->item->idx, tnode->item->evt);

	if (found)
		goto out;

	/* Insert node in tree */
	rb_link_node(&tnode->node, parent, link);
	rb_insert_color(&tnode->node, &task->itmtree);

out:
	return found;
}

/*
 * This handles page events of interest for an ItemTree. Indexing is based on
 * the inode number, and the index of the page within said inode.
 */
static void duet_handle_page(struct duet_task *task, __u8 evtcode,
	struct page *page)
{
	int found = 0;
	struct rb_node *node = NULL;
	struct item_rbnode *tnode = NULL;
	struct inode *inode = page->mapping->host;

	/* Verify that the event refers to the fs we're interested in */
	if (task->sb && task->sb != inode->i_sb) {
		duet_dbg(KERN_INFO "duet: event not on fs of interest\n");
		return;
	}

	/* First, look up the node in the ItemTree */
	//spin_lock_irq(&task->itm_outer_lock);
	spin_lock_irq(&task->itm_lock);
	node = task->itmtree.rb_node;

	while (node) {
		tnode = rb_entry(node, struct item_rbnode, node);

		/* We order based on (inode, page index) */
		if (tnode->item->ino > inode->i_ino) {
			node = node->rb_left;
		} else if (tnode->item->ino < inode->i_ino) {
			node = node->rb_right;
		} else {
			/* Found inode, look for index */
			if (tnode->item->idx > page->index) {
				node = node->rb_left;
			} else if (tnode->item->idx < page->index) {
				node = node->rb_right;
			} else {
				found = 1;
				break;
			}
		}
	}

	duet_dbg(KERN_DEBUG "duet-page: %s node (#%lu, i%lu, e%u)\n",
		found ? "found" : "didn't find",
		found ? tnode->item->ino : inode->i_ino,
		found ? tnode->item->idx : page->index,
		found ? tnode->item->evt : evtcode);

	/* If we found it, we might have to remove it. Otherwise, insert. */
	if (!found) {
		tnode = tnode_init(task, inode->i_ino, page->index, evtcode);
		if (!tnode) {
			printk(KERN_ERR "duet: tnode alloc failed\n");
			goto out;
		}

		if (itmtree_insert(task, tnode)) {
			printk(KERN_ERR "duet: itmtree insert failed\n");
			tnode_dispose(tnode, NULL, NULL);
		}
	} else if (found) {
		switch (task->nmodel) {
		case DUET_MODEL_DIFF:
			/* A different event code negates the current one */
			if (evtcode != tnode->item->evt)
				tnode_dispose(tnode, node, &task->itmtree);
			break;
		case DUET_MODEL_AXS:
			/* A different event code just changes the state */
			if (evtcode != tnode->item->evt)
				tnode->item->evt = evtcode;
			break;
		case DUET_MODEL_ADD:
		case DUET_MODEL_REM:
			/* Nothing to do here */
			break;
		case DUET_MODEL_BOTH:
			/* Just unify the event codes */
			tnode->item->evt |= evtcode;
			break;
		}
	}

out:
	spin_unlock_irq(&task->itm_lock);
	//spin_unlock_irq(&task->itm_outer_lock);
}

void duet_hook(__u8 evtcode, void *data)
{
	__u8 evtmask = 0;
	struct page *page = (struct page *)data;
	struct duet_task *cur;

	if (!duet_online())
		return;

	BUG_ON(!page);
	BUG_ON(!page->mapping);

	/* We're in RCU context so whatever happens, stay awake! */
	duet_dbg(KERN_INFO "duet hook: evt %u, data %p\n", evtcode, data);

	/* Look for tasks interested in this event type and invoke callbacks */
	rcu_read_lock();
	list_for_each_entry_rcu(cur, &duet_env.tasks, task_list) {
		switch(cur->nmodel) {
		case DUET_MODEL_ADD:
			evtmask = DUET_MODEL_ADD_MASK;
			break;
		case DUET_MODEL_REM:
			evtmask = DUET_MODEL_REM_MASK;
			break;
		case DUET_MODEL_BOTH:
			evtmask = DUET_MODEL_BOTH_MASK;
			break;
		case DUET_MODEL_DIFF:
			evtmask = DUET_MODEL_DIFF_MASK;
			break;
		case DUET_MODEL_AXS:
			evtmask = DUET_MODEL_AXS_MASK;
			break;
		}

		if (evtmask & evtcode)
			duet_handle_page(cur, evtcode, page);
	}
	rcu_read_unlock();
}
EXPORT_SYMBOL_GPL(duet_hook);
