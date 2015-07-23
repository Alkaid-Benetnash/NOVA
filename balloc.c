/*
 * PMFS emulated persistence. This file contains code to 
 * handle data blocks of various sizes efficiently.
 *
 * Persistent Memory File System
 * Copyright (c) 2012-2013, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <linux/fs.h>
#include <linux/bitops.h>
#include "pmfs.h"

int pmfs_alloc_block_free_lists(struct pmfs_sb_info *sbi)
{
	struct free_list *free_list;
	int i;

	sbi->cpus = num_online_cpus();
	pmfs_dbg("%s: %d cpus online\n", __func__, sbi->cpus);

	sbi->free_lists = kzalloc(sbi->cpus * sizeof(struct free_list),
							GFP_KERNEL);

	if (!sbi->free_lists)
		return -ENOMEM;

	for (i = 0; i < sbi->cpus; i++) {
		free_list = &sbi->free_lists[i];
		INIT_LIST_HEAD(&free_list->block_free_head);
		free_list->block_free_tree = RB_ROOT;
	}

	return 0;
}

void pmfs_init_blockmap(struct super_block *sb, unsigned long init_used_size)
{
	struct pmfs_sb_info *sbi = PMFS_SB(sb);
	unsigned long num_used_block;
	struct pmfs_blocknode *blknode;

	num_used_block = (init_used_size + sb->s_blocksize - 1) >>
		sb->s_blocksize_bits;

	blknode = pmfs_alloc_blocknode(sb);
	if (blknode == NULL)
		PMFS_ASSERT(0);
	blknode->block_low = sbi->block_start + num_used_block;
	blknode->block_high = sbi->block_end;
	sbi->num_free_blocks -= num_used_block;
	pmfs_insert_blocknode_blocktree(sbi, blknode);
	list_add(&blknode->link, &sbi->shared_block_free_head);
	pmfs_dbg_verbose("%s: Add: %lx %lx\n", __func__, blknode->block_low,
					blknode->block_high);
}

#if 0
static struct pmfs_blocknode *pmfs_next_blocknode(struct pmfs_blocknode *i,
						  struct list_head *head)
{
	if (list_is_last(&i->link, head))
		return NULL;
	return list_first_entry(&i->link, typeof(*i), link);
}
#endif

static inline void pmfs_free_dram_page(unsigned long page_addr)
{
	if ((page_addr & DRAM_BIT) == 0) {
		pmfs_dbg("Error: free a non-DRAM page? 0x%lx\n", page_addr);
		dump_stack();
		return;
	}

	pmfs_dbg_verbose("Free DRAM page 0x%lx\n", page_addr);

	if (page_addr & KMALLOC_BIT)
		kfree((void *)DRAM_ADDR(page_addr));
	else if (page_addr & VMALLOC_BIT)
		vfree((void *)DRAM_ADDR(page_addr));
	else
		free_page(DRAM_ADDR(page_addr));
}

static int pmfs_alloc_dram_page(struct super_block *sb,
	enum alloc_type type, unsigned long *page_addr, struct page **page,
	int zero, int nosleep)
{
	unsigned long addr = 0;
	int flags;

	/*
	 * Must use NOIO because we don't want to recurse back into the
	 * block or filesystem layers from page reclaim.
	 */
	if (nosleep)
		flags = GFP_ATOMIC | GFP_NOIO;
	else
		flags = GFP_NOIO;

	switch (type) {
		case KMALLOC:
			if (zero == 1)
				addr = (unsigned long)kzalloc(PAGE_SIZE,
							flags);
			else
				addr = (unsigned long)kmalloc(PAGE_SIZE,
							flags);
			if (addr && addr == DRAM_ADDR(addr)) {
				addr |= DRAM_BIT | KMALLOC_BIT;
				pmfs_dbg_verbose("Kmalloc DRAM page 0x%lx\n",
							addr);
				break;
			}
			if (addr) {
				kfree((void *)addr);
				addr = 0;
			}
			/* Fall through */
		case VMALLOC:
			if (zero == 1)
				addr = (unsigned long)vzalloc(flags);
			else
				addr = (unsigned long)vmalloc(flags);
			if (addr && addr == DRAM_ADDR(addr)) {
				addr |= DRAM_BIT | VMALLOC_BIT;
				pmfs_dbg_verbose("vmalloc DRAM page 0x%lx\n",
							addr);
				break;
			}
			if (addr) {
				vfree((void *)addr);
				addr = 0;
			}
			/* Fall through */
		case GETPAGE:
			if (zero == 1)
				addr = get_zeroed_page(flags);
			else
				addr = __get_free_page(flags);
			if (addr && addr == DRAM_ADDR(addr)) {
				addr |= DRAM_BIT | GETPAGE_BIT;
				pmfs_dbg_verbose("Get DRAM page 0x%lx\n",
							addr);
				break;
			}
			if (addr) {
				free_page(addr);
				addr = 0;
			}
			break;
		case ALLOCPAGE:
			if (zero)
				flags |= __GFP_ZERO;
			*page = alloc_page(flags);
			*page_addr |= DRAM_BIT;
			if (*page == NULL)
				return -ENOMEM;
			return 0;
		default:
			break;
	}

	if (addr == 0)
		return -ENOMEM;

	*page_addr = addr;
	return 0;
}

inline int pmfs_rbtree_compare_blocknode(struct pmfs_blocknode *curr,
	unsigned long new_block_low)
{
	if (new_block_low < curr->block_low)
		return -1;
	if (new_block_low > curr->block_high)
		return 1;

	return 0;
}

static int pmfs_find_blocknode(struct pmfs_sb_info *sbi,
	unsigned long new_block_low, unsigned long *step,
	struct pmfs_blocknode **ret_node)
{
	struct pmfs_blocknode *curr = NULL;
	struct rb_node *temp;
	int compVal;

	temp = sbi->shared_block_free_tree.rb_node;

	while (temp) {
		curr = container_of(temp, struct pmfs_blocknode, node);
		compVal = pmfs_rbtree_compare_blocknode(curr, new_block_low);
		(*step)++;

		if (compVal == -1) {
			temp = temp->rb_left;
		} else if (compVal == 1) {
			temp = temp->rb_right;
		} else {
			return 1;
		}
	}

	*ret_node = curr;
	return 0;
}

inline int pmfs_find_blocknode_blocktree(struct pmfs_sb_info *sbi,
	unsigned long new_block_low, unsigned long *step,
	struct pmfs_blocknode **ret_node)
{
	return pmfs_find_blocknode(sbi, new_block_low, step, ret_node);
}

static int pmfs_insert_blocknode(struct pmfs_sb_info *sbi,
	struct pmfs_blocknode *new_node)
{
	struct pmfs_blocknode *curr;
	struct rb_node **temp, *parent;
	int compVal;

	temp = &(sbi->shared_block_free_tree.rb_node);
	parent = NULL;

	while (*temp) {
		curr = container_of(*temp, struct pmfs_blocknode, node);
		compVal = pmfs_rbtree_compare_blocknode(curr,
					new_node->block_low);
		parent = *temp;

		if (compVal == -1) {
			temp = &((*temp)->rb_left);
		} else if (compVal == 1) {
			temp = &((*temp)->rb_right);
		} else {
			pmfs_dbg("%s: entry %lu - %lu already exists\n",
				__func__, new_node->block_low,
				new_node->block_high);
			return -EINVAL;
		}
	}

	rb_link_node(&new_node->node, parent, temp);
	rb_insert_color(&new_node->node, &sbi->shared_block_free_tree);

	return 0;
}

inline int pmfs_insert_blocknode_blocktree(struct pmfs_sb_info *sbi,
	struct pmfs_blocknode *new_node)
{
	return pmfs_insert_blocknode(sbi, new_node);
}

static int pmfs_find_free_slot(struct pmfs_sb_info *sbi,
	unsigned long new_block_low, unsigned long new_block_high,
	struct pmfs_blocknode **prev, struct pmfs_blocknode **next,
	struct pmfs_blocknode **start_hint)
{
	struct list_head *head = &(sbi->shared_block_free_head);
	struct pmfs_blocknode *ret_node = NULL;
	unsigned long step = 0;
	int ret;

	if (start_hint && *start_hint &&
	    new_block_low > (*start_hint)->block_high) {
		*prev = *start_hint;

		while (step <= 3) {
			if ((*prev)->link.next == head) {
				*next = NULL;
				return 0;
			}

			*next = list_entry((*prev)->link.next,
					struct pmfs_blocknode, link);

			if (new_block_high < (*next)->block_low)
				return 0;

			*prev = *next;
			step++;
		}
	}

	ret = pmfs_find_blocknode_blocktree(sbi, new_block_low,
						&step, &ret_node);
	if (ret) {
		pmfs_dbg("%s ERROR: %lu - %lu already in free list\n",
			__func__, new_block_low, new_block_high);
		return -EINVAL;
	}

	if (!ret_node) {
		*prev = *next = NULL;
	} else if (ret_node->block_high < new_block_low) {
		*prev = ret_node;
		if ((*prev)->link.next == head)
			*next = NULL;
		else
			*next = list_entry((*prev)->link.next,
					struct pmfs_blocknode, link);
	} else if (ret_node->block_low > new_block_high) {
		*next = ret_node;
		if ((*next)->link.prev == head)
			*prev = NULL;
		else
			*prev = list_entry((*next)->link.prev,
					struct pmfs_blocknode, link);
	} else {
		pmfs_dbg("%s ERROR: %lu - %lu overlaps with existing node "
			"%lu - %lu\n", __func__, new_block_low,
			new_block_high, ret_node->block_low,
			ret_node->block_high);
		return -EINVAL;
	}

	return 0;
}

/* Caller must hold the super_block lock.  If start_hint is provided, it is
 * only valid until the caller releases the super_block lock. */
static void pmfs_free_blocks(struct super_block *sb, unsigned long blocknr,
	int num, unsigned short btype, struct pmfs_blocknode **start_hint,
	int log_block)
{
	struct pmfs_sb_info *sbi = PMFS_SB(sb);
	struct list_head *head = &(sbi->shared_block_free_head);
	unsigned long new_block_low;
	unsigned long new_block_high;
	unsigned long num_blocks = 0;
	struct pmfs_blocknode *prev = NULL;
	struct pmfs_blocknode *next = NULL;
	struct pmfs_blocknode *free_blocknode= NULL;
	struct pmfs_blocknode *curr_node;
	unsigned long step = 0;
	int ret;

	if (num <= 0) {
		pmfs_dbg("%s ERROR: free %d\n", __func__, num);
		return;
	}

	num_blocks = pmfs_get_numblocks(btype) * num;
	new_block_low = blocknr;
	new_block_high = blocknr + num_blocks - 1;

	BUG_ON(list_empty(head));

	pmfs_dbgv("Free: %lu - %lu\n", new_block_low, new_block_high);

	ret = pmfs_find_free_slot(sbi, new_block_low, new_block_high,
					&prev, &next, start_hint);

	if (ret) {
		pmfs_dbg("%s: find free slot fail: %d\n", __func__, ret);
		return;
	}

	if (prev && next && (new_block_low == prev->block_high + 1) &&
		(new_block_high + 1 == next->block_low)) {
		/* fits the hole */
		rb_erase(&next->node, &sbi->shared_block_free_tree);
		list_del(&next->link);
		free_blocknode = next;
		sbi->num_blocknode--;
		prev->block_high = next->block_high;
		if (start_hint)
			*start_hint = prev;
		goto block_found;
	}
	if (prev && (new_block_low == prev->block_high + 1)) {
		/* Aligns left */
		prev->block_high += num_blocks;
		if (start_hint)
			*start_hint = prev;
		goto block_found;
	}
	if (next && (new_block_high + 1 == next->block_low)) {
		/* Aligns right */
		next->block_low -= num_blocks;
		if (start_hint)
			*start_hint = next;
		goto block_found;
	}

	/* Aligns somewhere in the middle */
	curr_node = pmfs_alloc_blocknode(sb);
	PMFS_ASSERT(curr_node);
	if (curr_node == NULL) {
		/* returning without freeing the block*/
		goto block_found;
	}
	curr_node->block_low = new_block_low;
	curr_node->block_high = new_block_high;
	pmfs_insert_blocknode_blocktree(sbi, curr_node);
	if (prev)
		list_add(&curr_node->link, &prev->link);
	else if (next)
		list_add_tail(&curr_node->link, &next->link);
	else
		list_add(&curr_node->link, &sbi->shared_block_free_head);

	if (start_hint)
		*start_hint = curr_node;
	goto block_found;

	if (log_block)
		pmfs_error_mng(sb, "Unable to free log block %lu - %lu\n",
				 new_block_low, new_block_high);
	else
		pmfs_error_mng(sb, "Unable to free data block %lu - %lu\n",
				 new_block_low, new_block_high);
//	dump_stack();
	return;

block_found:
	sbi->num_free_blocks += num_blocks;
	if (free_blocknode)
		__pmfs_free_blocknode(free_blocknode);
	free_steps += step;
}

void pmfs_free_meta_block(struct super_block *sb, unsigned long page_addr)
{
	timing_t free_time;

	PMFS_START_TIMING(free_meta_t, free_time);
	pmfs_dbg_verbose("Free meta block 0x%lx\n", page_addr);
	pmfs_free_dram_page(page_addr);
	PMFS_END_TIMING(free_meta_t, free_time);
	atomic64_inc(&meta_free);
}

void pmfs_free_cache_block(struct mem_addr *pair)
{
	timing_t free_time;

	PMFS_START_TIMING(free_cache_t, free_time);
	if (pair->page)
		__free_page(pair->page);
	else
		pmfs_free_dram_page(pair->dram);
	pair->page = NULL;
	pair->dram = 0;
	PMFS_END_TIMING(free_cache_t, free_time);
	atomic64_inc(&cache_free);
}

void pmfs_free_data_blocks(struct super_block *sb, unsigned long blocknr,
	int num, unsigned short btype, struct pmfs_blocknode **start_hint,
	int needlock)
{
	struct pmfs_sb_info *sbi = PMFS_SB(sb);
	timing_t free_time;

	pmfs_dbgv("Free %d data block from %lu\n", num, blocknr);
	PMFS_START_TIMING(free_data_t, free_time);
	if (needlock)
		mutex_lock(&sbi->s_lock);
	pmfs_free_blocks(sb, blocknr, num, btype, start_hint, 0);
	free_data_pages += num;
	if (needlock)
		mutex_unlock(&sbi->s_lock);
	PMFS_END_TIMING(free_data_t, free_time);
}

void pmfs_free_log_blocks(struct super_block *sb, unsigned long blocknr,
	int num, unsigned short btype, struct pmfs_blocknode **start_hint,
	int needlock)
{
	struct pmfs_sb_info *sbi = PMFS_SB(sb);
	timing_t free_time;

	pmfs_dbgv("Free %d log block from %lu\n", num, blocknr);
	PMFS_START_TIMING(free_log_t, free_time);
	if (needlock)
		mutex_lock(&sbi->s_lock);
	pmfs_free_blocks(sb, blocknr, num, btype, start_hint, 1);
	free_log_pages += num;
	if (needlock)
		mutex_unlock(&sbi->s_lock);
	PMFS_END_TIMING(free_log_t, free_time);
}

int pmfs_new_meta_block(struct super_block *sb, unsigned long *blocknr,
	int zero, int nosleep)
{
	int ret;
	timing_t alloc_time;

	PMFS_START_TIMING(new_meta_block_t, alloc_time);
	ret = pmfs_alloc_dram_page(sb, KMALLOC, blocknr, NULL, zero, nosleep);
	if (ret) {
		PMFS_END_TIMING(new_meta_block_t, alloc_time);
		return ret;
	}

	pmfs_dbg_verbose("%s: 0x%lx\n", __func__, *blocknr);
	PMFS_END_TIMING(new_meta_block_t, alloc_time);
	atomic64_inc(&meta_alloc);
	return 0;
}

int pmfs_new_cache_block(struct super_block *sb,
	struct mem_addr *pair, int zero, int nosleep)
{
	unsigned long page_addr = 0;
	struct page *page = NULL;
	int err = 0;
	timing_t alloc_time;

	PMFS_START_TIMING(new_cache_page_t, alloc_time);
	/* Using vmalloc/allocpage because we need the page to do mmap */
	err = pmfs_alloc_dram_page(sb, ALLOCPAGE, &page_addr, &page,
							zero, nosleep);
	if (err) {
		PMFS_END_TIMING(new_cache_page_t, alloc_time);
		pmfs_dbg("%s: allocation failed\n", __func__);
		goto out;
	}

	pair->page = page;
	pair->dram = page_addr;
	atomic64_inc(&cache_alloc);
out:
	PMFS_END_TIMING(new_cache_page_t, alloc_time);
	return err;
}

/* Return how many blocks allocated */
static int pmfs_new_blocks(struct super_block *sb, unsigned long *blocknr,
		unsigned int num, unsigned short btype, int zero, int log_page)
{
	struct pmfs_sb_info *sbi = PMFS_SB(sb);
	struct list_head *head = &(sbi->shared_block_free_head);
	struct pmfs_blocknode *curr;
	struct pmfs_blocknode *free_blocknode = NULL;
	void *bp;
	unsigned long num_blocks = 0;
	unsigned long curr_blocks;
	bool found = 0;
	unsigned long new_block_low;
	unsigned long step = 0;

	num_blocks = num * pmfs_get_numblocks(btype);
	if (num_blocks == 0)
		return -EINVAL;

	if (sbi->num_free_blocks < num_blocks)
		return -ENOSPC;

	mutex_lock(&sbi->s_lock);

	list_for_each_entry(curr, head, link) {
		step++;

		curr_blocks = curr->block_high - curr->block_low + 1;

		if (num_blocks >= curr_blocks) {
			/* Superpage allocation must succeed */
			if (btype > 0 && num_blocks > curr_blocks)
				continue;

			/* Otherwise, allocate the whole blocknode */
			rb_erase(&curr->node, &sbi->shared_block_free_tree);
			list_del(&curr->link);
			free_blocknode = curr;
			sbi->num_blocknode--;
			found = 1;
			num_blocks = curr_blocks;
			new_block_low = curr->block_low;
			break;
		}

		/* Allocate partial blocknode */
		new_block_low = curr->block_low;
		curr->block_low += num_blocks;
		found = 1;
		break;
	}

	if (found == 1) {
		sbi->num_free_blocks -= num_blocks;
		if (log_page)
			alloc_log_pages += num_blocks;
		else
			alloc_data_pages += num_blocks;
	}	

	mutex_unlock(&sbi->s_lock);

	if (free_blocknode)
		__pmfs_free_blocknode(free_blocknode);

	if (found == 0) {
		alloc_steps += step;
		return -ENOSPC;
	}

	if (zero) {
		size_t size;
		bp = pmfs_get_block(sb, pmfs_get_block_off(sb, new_block_low, btype));
		pmfs_memunlock_block(sb, bp); //TBDTBD: Need to fix this
		if (btype == PMFS_BLOCK_TYPE_4K)
			size = 0x1 << 12;
		else if (btype == PMFS_BLOCK_TYPE_2M)
			size = 0x1 << 21;
		else
			size = 0x1 << 30;
		memset_nt(bp, 0, PAGE_SIZE * num_blocks);
		pmfs_memlock_block(sb, bp);
	}
	*blocknr = new_block_low;

	pmfs_dbg_verbose("Alloc %u NVMM blocks 0x%lx\n", num, *blocknr);
	alloc_steps += step;
	return num_blocks / pmfs_get_numblocks(btype);
}

inline int pmfs_new_data_blocks(struct super_block *sb, struct pmfs_inode *pi,
	unsigned long *blocknr,	unsigned int num, unsigned long start_blk,
	unsigned short btype, int zero, int cow)
{
	int allocated;
	timing_t alloc_time;
	PMFS_START_TIMING(new_data_blocks_t, alloc_time);
	allocated = pmfs_new_blocks(sb, blocknr, num, btype, zero, 0);
	PMFS_END_TIMING(new_data_blocks_t, alloc_time);
	pmfs_dbgv("%s: inode %llu, start blk %lu, cow %d, %d blocks @ %lu\n",
				__func__, pi->pmfs_ino,	start_blk, cow,
				allocated, *blocknr);
	return allocated;
}

inline int pmfs_new_log_blocks(struct super_block *sb, unsigned long pmfs_ino,
	unsigned long *blocknr, unsigned int num, unsigned short btype,
	int zero)
{
	int allocated;
	timing_t alloc_time;
	PMFS_START_TIMING(new_log_blocks_t, alloc_time);
	allocated = pmfs_new_blocks(sb, blocknr, num, btype, zero, 1);
	PMFS_END_TIMING(new_log_blocks_t, alloc_time);
	pmfs_dbgv("%s: inode %lu, %d blocks @ %lu\n", __func__,
				pmfs_ino, allocated, *blocknr);
	return allocated;
}

unsigned long pmfs_count_free_blocks(struct super_block *sb)
{
	struct pmfs_sb_info *sbi = PMFS_SB(sb);
	return sbi->num_free_blocks; 
}
