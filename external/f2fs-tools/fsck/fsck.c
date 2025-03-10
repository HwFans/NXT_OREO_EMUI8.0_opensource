/**
 * fsck.c
 *
 * Copyright (c) 2013 Samsung Electronics Co., Ltd.
 *             http://www.samsung.com/
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include "fsck.h"
#include "xattr.h"

char *tree_mark;
uint32_t tree_mark_size = 256;

static inline int f2fs_set_main_bitmap(struct f2fs_sb_info *sbi, u32 blk,
								int type)
{
	struct f2fs_fsck *fsck = F2FS_FSCK(sbi);
	struct seg_entry *se;
	int fix = 0;

	se = get_seg_entry(sbi, GET_SEGNO(sbi, blk));
	if (se->type >= NO_CHECK_TYPE)
		fix = 1;
	else if (IS_DATASEG(se->type) != IS_DATASEG(type))
		fix = 1;

	/* just check data and node types */
	if (fix) {
		DMD_ADD_ERROR(LOG_TYP_FSCK, PR_SIT_TYPE_IS_ERROR);
		DBG(1, "Wrong segment type [0x%x] %x -> %x",
				GET_SEGNO(sbi, blk), se->type, type);
		se->type = type;
	}
	return f2fs_set_bit(BLKOFF_FROM_MAIN(sbi, blk), fsck->main_area_bitmap);
}

static inline int f2fs_test_main_bitmap(struct f2fs_sb_info *sbi, u32 blk)
{
	struct f2fs_fsck *fsck = F2FS_FSCK(sbi);

	return f2fs_test_bit(BLKOFF_FROM_MAIN(sbi, blk),
						fsck->main_area_bitmap);
}

static inline int f2fs_test_sit_bitmap(struct f2fs_sb_info *sbi, u32 blk)
{
	struct f2fs_fsck *fsck = F2FS_FSCK(sbi);

	return f2fs_test_bit(BLKOFF_FROM_MAIN(sbi, blk), fsck->sit_area_bitmap);
}

static int add_into_hard_link_list(struct f2fs_sb_info *sbi,
						u32 nid, u32 link_cnt)
{
	struct f2fs_fsck *fsck = F2FS_FSCK(sbi);
	struct hard_link_node *node = NULL, *tmp = NULL, *prev = NULL;

	node = calloc(sizeof(struct hard_link_node), 1);
	ASSERT(node != NULL);

	node->nid = nid;
	node->links = link_cnt;
	node->actual_links = 1;
	node->next = NULL;

	if (fsck->hard_link_list_head == NULL) {
		fsck->hard_link_list_head = node;
		goto out;
	}

	tmp = fsck->hard_link_list_head;

	/* Find insertion position */
	while (tmp && (nid < tmp->nid)) {
		ASSERT(tmp->nid != nid);
		prev = tmp;
		tmp = tmp->next;
	}

	if (tmp == fsck->hard_link_list_head) {
		node->next = tmp;
		fsck->hard_link_list_head = node;
	} else {
		prev->next = node;
		node->next = tmp;
	}

out:
	DBG(2, "ino[0x%x] has hard links [0x%x]\n", nid, link_cnt);
	return 0;
}

static int find_and_dec_hard_link_list(struct f2fs_sb_info *sbi, u32 nid)
{
	struct f2fs_fsck *fsck = F2FS_FSCK(sbi);
	struct hard_link_node *node = NULL, *prev = NULL;

	if (fsck->hard_link_list_head == NULL)
		return -EINVAL;

	node = fsck->hard_link_list_head;

	while (node && (nid < node->nid)) {
		prev = node;
		node = node->next;
	}

	if (node == NULL || (nid != node->nid))
		return -EINVAL;

	/* Decrease link count */
	node->links = node->links - 1;
	node->actual_links++;

	/* if link count becomes one, remove the node */
	if (node->links == 1) {
		if (fsck->hard_link_list_head == node)
			fsck->hard_link_list_head = node->next;
		else
			prev->next = node->next;
		free(node);
	}
	return 0;
}

static int is_valid_ssa_node_blk(struct f2fs_sb_info *sbi, u32 nid,
							u32 blk_addr)
{
	struct f2fs_summary_block *sum_blk;
	struct f2fs_summary *sum_entry;
	struct seg_entry * se;
	u32 segno, offset;
	int need_fix = 0, ret = 0;
	int type;

	segno = GET_SEGNO(sbi, blk_addr);
	offset = OFFSET_IN_SEG(sbi, blk_addr);

	sum_blk = get_sum_block(sbi, segno, &type);

	if (type != SEG_TYPE_NODE && type != SEG_TYPE_CUR_NODE) {
		/* can't fix current summary, then drop the block */
		if (!config.fix_on || type < 0) {
			ASSERT_MSG("Summary footer is not for node segment");
			ret = -EINVAL;
			goto out;
		}

		need_fix = 1;
		se = get_seg_entry(sbi, segno);
		if(IS_NODESEG(se->type)) {
			FIX_MSG("Summary footer indicates a node segment: 0x%x", segno);
			sum_blk->footer.entry_type = SUM_TYPE_NODE;
		} else {
			ret = -EINVAL;
			goto out;
		}
	}

	sum_entry = &(sum_blk->entries[offset]);

	if (le32_to_cpu(sum_entry->nid) != nid) {
		if (!config.fix_on || type < 0) {
			DBG(0, "nid                       [0x%x]\n", nid);
			DBG(0, "target blk_addr           [0x%x]\n", blk_addr);
			DBG(0, "summary blk_addr          [0x%x]\n",
						GET_SUM_BLKADDR(sbi,
						GET_SEGNO(sbi, blk_addr)));
			DBG(0, "seg no / offset           [0x%x / 0x%x]\n",
						GET_SEGNO(sbi, blk_addr),
						OFFSET_IN_SEG(sbi, blk_addr));
			DBG(0, "summary_entry.nid         [0x%x]\n",
						le32_to_cpu(sum_entry->nid));
			DBG(0, "--> node block's nid      [0x%x]\n", nid);
			ASSERT_MSG("Invalid node seg summary\n");
			ret = -EINVAL;
		} else {
			FIX_MSG("Set node summary 0x%x -> [0x%x] [0x%x]",
						segno, nid, blk_addr);
			sum_entry->nid = cpu_to_le32(nid);
			need_fix = 1;
		}
	}
	if (need_fix && !config.ro) {
		u64 ssa_blk;
		int ret2;

		ssa_blk = GET_SUM_BLKADDR(sbi, segno);
		ret2 = dev_write_block(sum_blk, ssa_blk);
		ASSERT(ret2 >= 0);
	}
out:
	if (type == SEG_TYPE_NODE || type == SEG_TYPE_DATA ||
					type == SEG_TYPE_MAX)
		free(sum_blk);
	return ret;
}

static int is_valid_summary(struct f2fs_sb_info *sbi, struct f2fs_summary *sum,
							u32 blk_addr)
{
	u16 ofs_in_node = le16_to_cpu(sum->ofs_in_node);
	u32 nid = le32_to_cpu(sum->nid);
	struct f2fs_node *node_blk = NULL;
	__le32 target_blk_addr;
	struct node_info ni;
	int ret = 0;

	node_blk = (struct f2fs_node *)calloc(BLOCK_SZ, 1);
	ASSERT(node_blk != NULL);

	if (!IS_VALID_NID(sbi, nid)) {
		DMD_ADD_ERROR(LOG_TYP_FSCK, PR_INVALID_NID);
		goto out;
	}

	get_node_info(sbi, nid, &ni);

	if (!IS_VALID_BLK_ADDR(sbi, ni.blk_addr))
		goto out;

	/* read node_block */
	ret = dev_read_block(node_blk, ni.blk_addr);
	ASSERT(ret >= 0);

	if (le32_to_cpu(node_blk->footer.nid) != nid)
		goto out;

	/* check its block address */
	if (node_blk->footer.nid == node_blk->footer.ino)
		target_blk_addr = node_blk->i.i_addr[ofs_in_node];
	else
		target_blk_addr = node_blk->dn.addr[ofs_in_node];

	if (blk_addr == le32_to_cpu(target_blk_addr))
		ret = 1;
out:
	free(node_blk);
	return ret;
}

static int is_valid_ssa_data_blk(struct f2fs_sb_info *sbi, u32 blk_addr,
		u32 parent_nid, u16 idx_in_node, u8 version)
{
	struct f2fs_summary_block *sum_blk;
	struct f2fs_summary *sum_entry;
	struct seg_entry * se;
	u32 segno, offset;
	int need_fix = 0, ret = 0;
	int type;

	segno = GET_SEGNO(sbi, blk_addr);
	offset = OFFSET_IN_SEG(sbi, blk_addr);

	sum_blk = get_sum_block(sbi, segno, &type);

	if (type != SEG_TYPE_DATA && type != SEG_TYPE_CUR_DATA) {
		/* can't fix current summary, then drop the block */
		if (!config.fix_on || type < 0) {
			ASSERT_MSG("Summary footer is not for data segment");
			ret = -EINVAL;
			goto out;
		}

		need_fix = 1;
		se = get_seg_entry(sbi, segno);
		if (IS_DATASEG(se->type)) {
			FIX_MSG("Summary footer indicates a data segment: 0x%x", segno);
			sum_blk->footer.entry_type = SUM_TYPE_DATA;
		} else {
			ret = -EINVAL;
			goto out;
		}
	}

	sum_entry = &(sum_blk->entries[offset]);

	if (le32_to_cpu(sum_entry->nid) != parent_nid ||
			sum_entry->version != version ||
			le16_to_cpu(sum_entry->ofs_in_node) != idx_in_node) {
		if (!config.fix_on || type < 0) {
			DBG(0, "summary_entry.nid         [0x%x]\n",
					le32_to_cpu(sum_entry->nid));
			DBG(0, "summary_entry.version     [0x%x]\n",
					sum_entry->version);
			DBG(0, "summary_entry.ofs_in_node [0x%x]\n",
					le16_to_cpu(sum_entry->ofs_in_node));
			DBG(0, "parent nid                [0x%x]\n",
					parent_nid);
			DBG(0, "version from nat          [0x%x]\n", version);
			DBG(0, "idx in parent node        [0x%x]\n",
					idx_in_node);

			DBG(0, "Target data block addr    [0x%x]\n", blk_addr);
			ASSERT_MSG("Invalid data seg summary\n");
			ret = -EINVAL;
		} else if (is_valid_summary(sbi, sum_entry, blk_addr)) {
			/* delete wrong index */
			ret = -EINVAL;
		} else {
			FIX_MSG("Set data summary 0x%x -> [0x%x] [0x%x] [0x%x]",
					segno, parent_nid, version, idx_in_node);
			sum_entry->nid = cpu_to_le32(parent_nid);
			sum_entry->version = version;
			sum_entry->ofs_in_node = cpu_to_le16(idx_in_node);
			need_fix = 1;
		}
	}
	if (need_fix && !config.ro) {
		u64 ssa_blk;
		int ret2;

		ssa_blk = GET_SUM_BLKADDR(sbi, segno);
		ret2 = dev_write_block(sum_blk, ssa_blk);
		ASSERT(ret2 >= 0);
	}
out:
	if (type == SEG_TYPE_NODE || type == SEG_TYPE_DATA ||
					type == SEG_TYPE_MAX)
		free(sum_blk);
	return ret;
}

static int __check_inode_mode(u32 nid, enum FILE_TYPE ftype, u32 mode)
{
	if (ftype >= F2FS_FT_MAX)
		return 0;
	if (S_ISLNK(mode) && ftype != F2FS_FT_SYMLINK)
		goto err;
	if (S_ISREG(mode) && ftype != F2FS_FT_REG_FILE)
		goto err;
	if (S_ISDIR(mode) && ftype != F2FS_FT_DIR)
		goto err;
	if (S_ISCHR(mode) && ftype != F2FS_FT_CHRDEV)
		goto err;
	if (S_ISBLK(mode) && ftype != F2FS_FT_BLKDEV)
		goto err;
	if (S_ISFIFO(mode) && ftype != F2FS_FT_FIFO)
		goto err;
	if (S_ISSOCK(mode) && ftype != F2FS_FT_SOCK)
		goto err;
	return 0;
err:
	ASSERT_MSG("mismatch i_mode [0x%x] [0x%x vs. 0x%x]", nid, ftype, mode);
	return -1;
}

static int sanity_check_nid(struct f2fs_sb_info *sbi, u32 nid,
			struct f2fs_node *node_blk,
			enum FILE_TYPE ftype, enum NODE_TYPE ntype,
			struct node_info *ni)
{
	struct f2fs_fsck *fsck = F2FS_FSCK(sbi);
	int ret;

	if (!IS_VALID_NID(sbi, nid)) {
		DMD_ADD_ERROR(LOG_TYP_FSCK, PR_INVALID_NID);
		ASSERT_MSG("nid is not valid. [0x%x]", nid);
		return -EINVAL;
	}

	get_node_info(sbi, nid, ni);
	if (ni->ino == 0) {
		DMD_ADD_ERROR(LOG_TYP_FSCK, PR_INO_IS_ZERO);
		ASSERT_MSG("nid[0x%x] ino is 0", nid);
		return -EINVAL;
	}

	if (ni->blk_addr == NEW_ADDR) {
		DMD_ADD_ERROR(LOG_TYP_FSCK, PR_BLKADDR_IS_NEW_ADDR);
		ASSERT_MSG("nid is NEW_ADDR. [0x%x]", nid);
		return -EINVAL;
	}

	if (!IS_VALID_BLK_ADDR(sbi, ni->blk_addr)) {
		DMD_ADD_ERROR(LOG_TYP_FSCK, PR_NODE_INVALID_BLKADDR);
		ASSERT_MSG("blkaddres is not valid. [0x%x]", ni->blk_addr);
		return -EINVAL;
	}

	ret = dev_read_block(node_blk, ni->blk_addr);
	ASSERT(ret >= 0);

	if (ntype == TYPE_INODE &&
			node_blk->footer.nid != node_blk->footer.ino) {
		DMD_ADD_ERROR(LOG_TYP_FSCK, PR_INODE_FOOTER_INO_NOT_EQUAL_NID);
		ASSERT_MSG("nid[0x%x] footer.nid[0x%x] footer.ino[0x%x]",
				nid, le32_to_cpu(node_blk->footer.nid),
				le32_to_cpu(node_blk->footer.ino));
		return -EINVAL;
	}
	if (ni->ino != le32_to_cpu(node_blk->footer.ino)) {
		DMD_ADD_ERROR(LOG_TYP_FSCK, PR_NODE_INO_NOT_EQUAL_FOOTER_INO);
		ASSERT_MSG("nid[0x%x] nat_entry->ino[0x%x] footer.ino[0x%x]",
				nid, ni->ino, le32_to_cpu(node_blk->footer.ino));
		return -EINVAL;
	}
	if (ntype != TYPE_INODE &&
			node_blk->footer.nid == node_blk->footer.ino) {
		DMD_ADD_ERROR(LOG_TYP_FSCK, PR_NON_INODE_FOOTER_INO_EQUAL_NID);
		ASSERT_MSG("nid[0x%x] footer.nid[0x%x] footer.ino[0x%x]",
				nid, le32_to_cpu(node_blk->footer.nid),
				le32_to_cpu(node_blk->footer.ino));
		return -EINVAL;
	}

	if (le32_to_cpu(node_blk->footer.nid) != nid) {
		DMD_ADD_ERROR(LOG_TYP_FSCK, PR_NODE_NID_NOT_EQUAL_FOOTER_NID);
		ASSERT_MSG("nid[0x%x] blk_addr[0x%x] footer.nid[0x%x]",
				nid, ni->blk_addr,
				le32_to_cpu(node_blk->footer.nid));
		return -EINVAL;
	}

	if (ntype == TYPE_XATTR) {
		u32 flag = le32_to_cpu(node_blk->footer.flag);

		if ((flag >> OFFSET_BIT_SHIFT) != XATTR_NODE_OFFSET) {
			DMD_ADD_ERROR(LOG_TYP_FSCK, PR_INVALID_XATTR_OFFSET);
			ASSERT_MSG("xnid[0x%x] has wrong ofs:[0x%x]",
					nid, flag);
			return -EINVAL;
		}
	}

	if ((ntype == TYPE_INODE && ftype == F2FS_FT_DIR) ||
			(ntype == TYPE_XATTR && ftype == F2FS_FT_XATTR)) {
		/* not included '.' & '..' */
		if (f2fs_test_main_bitmap(sbi, ni->blk_addr) != 0) {
			DMD_ADD_ERROR(LOG_TYP_FSCK, PR_DUPLICATE_NODE_BLKADDR_IN_MAIN_BITMAP);
			ASSERT_MSG("Duplicated node blk. nid[0x%x][0x%x]\n",
					nid, ni->blk_addr);
			return -EINVAL;
		}
	}

	/* this if only from fix_hard_links */
	if (ftype == F2FS_FT_MAX)
		return 0;

	if (ntype == TYPE_INODE &&
		__check_inode_mode(nid, ftype, le32_to_cpu(node_blk->i.i_mode))) {
		DMD_ADD_ERROR(LOG_TYP_FSCK, PR_INODE_MISMATCH_MODE);
		return -EINVAL;
	}

	/* workaround to fix later */
	if (ftype != F2FS_FT_ORPHAN ||
			f2fs_test_bit(nid, fsck->nat_area_bitmap) != 0)
		f2fs_clear_bit(nid, fsck->nat_area_bitmap);
	else {
		DMD_ADD_ERROR(LOG_TYP_FSCK, PR_DUPLICATE_ORPHAN_OR_XATTR_NID);
		ASSERT_MSG("orphan or xattr nid is duplicated [0x%x]\n",
				nid);
	}

	if (is_valid_ssa_node_blk(sbi, nid, ni->blk_addr)) {
		DMD_ADD_ERROR(LOG_TYP_FSCK, PR_INVALID_SUM_NODE_BLOCK);
		ASSERT_MSG("summary node block is not valid. [0x%x]", nid);
		return -EINVAL;
	}

	if (f2fs_test_sit_bitmap(sbi, ni->blk_addr) == 0) {
		DMD_ADD_ERROR(LOG_TYP_FSCK, PR_NAT_BLKADDR_OUT_SIT_BITMAP);
		ASSERT_MSG("SIT bitmap is 0x0. blk_addr[0x%x]",
				ni->blk_addr);
	}

	if (f2fs_test_main_bitmap(sbi, ni->blk_addr) == 0) {
		fsck->chk.valid_blk_cnt++;
		fsck->chk.valid_node_cnt++;
	}
	return 0;
}

static struct f2fs_xattr_entry* grab_correct_encrypt_xattr(void *xattr)
{
	struct f2fs_xattr_entry *e;
	int e_value_size = 0;
	struct fscrypt_context *c = NULL;
	struct f2fs_xattr_header *hdr = XATTR_HDR(xattr);
	u32 crc = 0;

	e = find_xattr(xattr, F2FS_XATTR_INDEX_ENCRYPTION, 1,
		       F2FS_XATTR_NAME_ENCRYPTION_CONTEXT);
	if (IS_XATTR_LAST_ENTRY(e)) {
		e = NULL;
		DBG(1, "fscrypt xattr not exists\n");
	} else {
		c = (struct fscrypt_context *)(e->e_name + e->e_name_len);/*lint !e826*/
		e_value_size = le16_to_cpu(e->e_value_size);
	}

	if (hdr->h_ctx_crc == 0)
		DBG(1, "crc xattr not exists\n");
	else
		crc = le32_to_cpu(hdr->h_ctx_crc);

	if (e && c && crc) {
		if (!f2fs_crc_valid(crc, c, e_value_size))
			return e;
	}

	return NULL;/*lint !e438*/
}

/*
 * return 0: not found
 * return 1: found from parent
 * return 2: found from children
 */
static int find_correct_encrypt_xattr(struct f2fs_sb_info *sbi,
				      struct f2fs_node *node,
				      struct f2fs_xattr_entry *ret)
{
	struct f2fs_inode *inode = &node->i;
	nid_t ino = le32_to_cpu(node->footer.ino);
	struct f2fs_xattr_entry *src_e;

	__u8 *dentry_bitmap;
	void *de_blk = NULL;
	struct f2fs_dir_entry *dentries;
	unsigned int dentry_nr;

	struct node_info ni;
	struct f2fs_node *blk;
	void *src_xattr;
	nid_t pino = le32_to_cpu(inode->i_pino), cino;
	unsigned int i;
	int found = 0;
	int alloc = 0, err;

	blk = calloc(F2FS_BLKSIZE, 1);/*lint !e826 !e433*/
	ASSERT(blk);

	get_node_info(sbi, pino, &ni);
	err = dev_read_block(blk, ni.blk_addr);/*lint !e747*/
	ASSERT(err >= 0);

	if (file_is_encrypt(&blk->i)) {
		/* parent is encrypted */
		src_xattr = read_all_xattrs(sbi, blk);
		src_e = find_xattr(src_xattr, F2FS_XATTR_INDEX_ENCRYPTION, 1,
				   F2FS_XATTR_NAME_ENCRYPTION_CONTEXT);
		if (!IS_XATTR_LAST_ENTRY(src_e)) {
			memcpy(ret, src_e, F2FS_XATTR_ENCRYPTION_SIZE);/*lint !e665*/
			found = 1;
			MSG(0, "Inode %u find correct encrypt xattr from parent %u\n",
			    ino, pino);
			free(src_xattr);
			goto out;
		} else {
			DBG(1, "Parent inode %u has no encrpyt xattr\n", pino);
		}
		free(src_xattr);
	} else {
		DBG(1, "Parent inode %u is not encrypted\n", pino);
	}

	/* parent is not encrypted, check children */
	if (inode->i_inline & F2FS_INLINE_DENTRY) {
		de_blk = inline_data_addr(node);
		dentry_bitmap = ((struct f2fs_inline_dentry *)de_blk)->dentry_bitmap;
		dentries = ((struct f2fs_inline_dentry *)de_blk)->dentry;
		dentry_nr = NR_INLINE_DENTRY;/*lint !e834*/
	} else {
		if (inode->i_addr[0]) {
			ERR_MSG("Do not rebuild encrypt for empty directory %u\n",
				ino);
			goto out;
		}
		de_blk = calloc(F2FS_BLKSIZE, 1);/*lint !e826 !e433*/
		ASSERT(de_blk);
		alloc = 1;
		/* we check only one data block to lookup a correct key */
		err = dev_read_block(de_blk, le32_to_cpu(inode->i_addr[0]));/*lint !e747*/
		ASSERT(err >= 0);

		dentry_bitmap = ((struct f2fs_dentry_block *)de_blk)->dentry_bitmap;/*lint !e826*/
		dentries = ((struct f2fs_dentry_block *)de_blk)->dentry;/*lint !e826*/
		dentry_nr = NR_DENTRY_IN_BLOCK;
	}

	for (i = 0; i < dentry_nr; i++) {
		int name_len;

		if (!test_bit_le(i, dentry_bitmap))
			continue;
		if (dentries[i].hash_code == 0)
			/* skip . and .. */
			continue;

		cino = le32_to_cpu(dentries[i].ino);
		get_node_info(sbi, cino, &ni);
		err = dev_read_block(blk, ni.blk_addr);/*lint !e747*/
		ASSERT(err >= 0);

		if (file_is_encrypt(&blk->i) && !file_is_encrypt_corrupt(&blk->i)) {
			/* child is encrypted */
			src_xattr = read_all_xattrs(sbi, blk);
			src_e = grab_correct_encrypt_xattr(src_xattr);
			if (!src_e) {
				ERR_MSG("Child inode %u has wrong backup fscrypt context\n",
					cino);
				free(src_xattr);
				name_len = le16_to_cpu(dentries[i].name_len);
				i += (((int)name_len + (int)F2FS_SLOT_LEN - 1) / (int)F2FS_SLOT_LEN - 1);
				continue;
			}
			memcpy(ret, src_e, F2FS_XATTR_ENCRYPTION_SIZE);/*lint !e665*/
			found = 2;
			MSG(0, "Inode %u find correct encrypt xattr from child %u\n",
			    ino, cino);
			free(src_xattr);
			break;
		} else {
			DBG(1, "Child inode %u is not encrypted\n", cino);
			name_len = le16_to_cpu(dentries[i].name_len);
			i += (((int)name_len + (int)F2FS_SLOT_LEN - 1) / (int)F2FS_SLOT_LEN - 1);
		}
	}
	if (i >= dentry_nr)/*lint !e850*/
		ERR_MSG("All children have no valid fscrypt context");
out:
	if (alloc)
		free(de_blk);
	free(blk);
	return found;/*lint !e593*/
}

/*
 * If there is enough space, return the position of new xattr will be put.
 * Otherwise, return NULL, which means there is not enough space left.
 */
static struct f2fs_xattr_entry *may_append_xattr(struct f2fs_node *node,
						 void *xattr, size_t size)
{
	struct f2fs_inode *inode = &node->i;
	size_t free_size, total_size;
	struct f2fs_xattr_entry *last;

	last = XATTR_FIRST_ENTRY(xattr);
	while (!IS_XATTR_LAST_ENTRY(last))
		last = XATTR_NEXT_ENTRY(last);/*lint !e826*/

	total_size = (size_t)inline_xattr_size(inode);
	if (inode->i_xattr_nid)
		total_size += (F2FS_BLKSIZE - sizeof(struct node_footer));
	free_size = total_size - (int)((char *)last - (char *)xattr);
	if (free_size < size)
		return NULL;
	return last;
}

#define REPLACE_CTX 0x1
#define REPLACE_CRC 0x2
#define REPLACE_ALL (REPLACE_CTX | REPLACE_CRC)

static void replace_encrypt_xattr(struct f2fs_node *node, void *xattr,
				  struct f2fs_xattr_entry *src, int which)
{
	nid_t ino = le32_to_cpu(node->footer.ino);
	struct f2fs_xattr_entry *dst;
	int fix_crc = 0;

	if (which & REPLACE_CTX) {
		dst = find_xattr(xattr, F2FS_XATTR_INDEX_ENCRYPTION, 1,
				 F2FS_XATTR_NAME_ENCRYPTION_CONTEXT);
		if (!IS_XATTR_LAST_ENTRY(dst)) {
			ASSERT(dst->e_name_index == src->e_name_index);
			ASSERT(dst->e_name_len == src->e_name_len);
			memcpy(dst, src, ENTRY_SIZE(src));
			FIX_MSG("Inode %u replace encrypt xattr", ino);
			fix_crc = 1;
		}
	}

	if (fix_crc || which & REPLACE_CRC) {
		struct f2fs_xattr_header *hdr = XATTR_HDR(xattr);
		u32 old_crc = le32_to_cpu(hdr->h_ctx_crc);
		u32 new_crc = f2fs_cal_crc32(F2FS_SUPER_MAGIC,
					     src->e_name + src->e_name_len,
					     FSCRYPT_CTX_SIZE);

		if (old_crc == new_crc)
			return;
		hdr->h_ctx_crc = cpu_to_le32(new_crc);
		FIX_MSG("Inode %u re-calculate crc 0x%x -> 0x%x",
			ino, old_crc, new_crc);
	}
}

static int append_encrypt_xattr(struct f2fs_node *node, void *xattr,
				struct f2fs_xattr_entry *new)
{
	nid_t ino = le32_to_cpu(node->footer.ino);
	struct f2fs_xattr_entry *last;

	last = may_append_xattr(node, xattr, F2FS_XATTR_ENCRYPTION_SIZE);
	if (!last) {
		ERR_MSG("Inode %u not enough space left for new xattr\n", ino);
		return -1;
	}

	memcpy(last, new, ENTRY_SIZE(new));
	FIX_MSG("Inode %u append new encrypt xattr", ino);
	replace_encrypt_xattr(node, xattr, new, REPLACE_ALL);
	return 0;
}

/* regenerate fscrypt context for inode */
static int rebuild_encrypt_inline(struct f2fs_node *node, void *xattr,
				  struct f2fs_xattr_entry *src_e)
{
	struct f2fs_inode *inode = &node->i;
	nid_t ino = le32_to_cpu(node->footer.ino);
	int inline_size = inline_xattr_size(inode);
	struct f2fs_xattr_header *hdr;
	struct f2fs_xattr_entry *ent;
	int found, ret = 0;

	if (!(inode->i_inline & F2FS_INLINE_XATTR)) {
		ERR_MSG("Inode %u does not support inline xattr", ino);
		return 0;
	}

	if ((size_t)inline_size < F2FS_XATTR_ENCRYPTION_SIZE +
	    XATTR_ALIGN(sizeof(*hdr))) {
		ERR_MSG("Inode %u inline xattr size not enough", ino);
		return 0;
	}

	/* rebuild header first */
	memset(xattr, 0, (size_t)inline_size + BLOCK_SZ);
	hdr = XATTR_HDR(xattr);
	hdr->h_magic = cpu_to_le32(F2FS_XATTR_MAGIC);
	hdr->h_refcount = cpu_to_le32(1);
	ent = XATTR_FIRST_ENTRY(xattr);

	/* rebuild encrypt xattr */
	memcpy(ent, src_e, F2FS_XATTR_ENCRYPTION_SIZE);
	replace_encrypt_xattr(node, xattr, ent, REPLACE_CRC);
	FIX_MSG("Inode %u rebuild inline encrypt", ino);
	file_set_encrypt_fixed(inode);
	FIX_MSG("Inode %u set encrypt_fixed bit", ino);

	/* remove xattr block */
	ret = -EINVAL;
out:
	return ret;
}

/*
 * if @append is 1, we rebuild encrypt by appending a new encrypt xattr
 * if @append is 0, we rebuild encrypt by replacing the old encrypt xattr
 */
static int rebuild_encrypt(struct f2fs_sb_info *sbi,
			   struct f2fs_node *node,
			   void *xattr, int append)
{
	struct f2fs_inode *inode = &node->i;
	nid_t ino = le32_to_cpu(node->footer.ino);
	struct f2fs_xattr_entry *src;
	int found, ret;

	if (!S_ISDIR(le16_to_cpu(inode->i_mode))) {
		ERR_MSG("Do not rebuild encrypt for non-directory file %u\n",
			ino);
		return 0;
	}

	src = calloc(F2FS_XATTR_ENCRYPTION_SIZE, 1);
	ASSERT(src);
	found = find_correct_encrypt_xattr(sbi, node, src);
	if (!found) {
		free(src);
		return 0;
	}

	if (append) {
		if (append_encrypt_xattr(node, xattr, src) < 0) {
			ret = rebuild_encrypt_inline(node, xattr, src);
			free(src);
			return ret;
		}
	} else {
		replace_encrypt_xattr(node, xattr, src, REPLACE_ALL);
	}

	file_set_encrypt_fixed(inode);
	FIX_MSG("Inode %u set encrypt_fixed bit", ino);
	free(src);
	return 1;
}

/*
 * return negative: remove i_xattr_nid, and need to fix
 * return 1: do not remove i_xattr_nid, need to fix
 * return 0: no need to fix
 */
static int fsck_chk_encrypt(struct f2fs_sb_info *sbi,
			    struct f2fs_node *node, void *xattr)
{
	struct f2fs_inode *inode = &node->i;
	nid_t ino = le32_to_cpu(node->footer.ino);
	struct f2fs_xattr_entry *e;
	int e_value_size;
	struct fscrypt_context *c;
	struct f2fs_xattr_header *hdr;
	u32 crc = 0;
	int fix_header = 0;

	if (!file_is_encrypt(inode)) {
		DBG(1, "Inode %u is not encrypted\n", ino);
		return 0;
	}

	hdr = XATTR_HDR(xattr);
	if (le32_to_cpu(hdr->h_magic) != F2FS_XATTR_MAGIC &&
	    le32_to_cpu(hdr->h_refcount) != 0 &&
	    !IS_XATTR_LAST_ENTRY(XATTR_FIRST_ENTRY(xattr))) {
		ASSERT_MSG("Inode %u has wrong inline xattr magic 0x%x ref 0x%x",
			   ino, le32_to_cpu(hdr->h_magic),
			   le32_to_cpu(hdr->h_refcount));
		hdr->h_magic = cpu_to_le32(F2FS_XATTR_MAGIC);
		hdr->h_refcount = cpu_to_le32(1);
		fix_header = 1;
		FIX_MSG("Inode %u xattr magic 0x%x ref 1\n",
			ino, F2FS_XATTR_MAGIC);
	}

	/* find and check fscrypt context */
	e = find_xattr(xattr, F2FS_XATTR_INDEX_ENCRYPTION, 1,
		       F2FS_XATTR_NAME_ENCRYPTION_CONTEXT);
	if (IS_XATTR_LAST_ENTRY(e))
		e = NULL;

	if (e) {
		c = (struct fscrypt_context *)(e->e_name + e->e_name_len);/*lint !e826*/
		e_value_size = le16_to_cpu(e->e_value_size);
		crc = le32_to_cpu(hdr->h_ctx_crc);
		if (crc == 0) {
			DBG(0, "Inode %u crc not exist\n", ino);
			return rebuild_encrypt(sbi, node, xattr, 0);
		} else {
			f2fs_crc_valid(crc, c, e_value_size);
			return rebuild_encrypt(sbi, node, xattr, 0);
		}
	} else {
		return rebuild_encrypt(sbi, node, xattr, 1);
	}
out:
	return fix_header;
}

/*
 * return negative: remove i_xattr_nid, and need to fix
 * return 1: do not remove i_xattr_nid, and need to fix
 * return 0: no need to fix
 */
static int fsck_chk_xattr(struct f2fs_sb_info *sbi, u32 ino,
			  struct f2fs_node *node, u32 *blk_cnt)
{
	struct f2fs_fsck *fsck = F2FS_FSCK(sbi);
	struct f2fs_node *node_blk = NULL;
	void *xattr = NULL;
	struct node_info ni;
	struct f2fs_inode *inode = &node->i;
	nid_t x_nid = le32_to_cpu(inode->i_xattr_nid);
	int ret = 0, err, remove_xattr_blk = 0, clear_corrupt_bit = 0;

	if (x_nid) {
		node_blk = (struct f2fs_node *)calloc(BLOCK_SZ, 1);/*lint !e826 !e433*/
		ASSERT(node_blk != NULL);

		/* Sanity check */
		if (sanity_check_nid(sbi, x_nid, node_blk, F2FS_FT_XATTR,
				     TYPE_XATTR, &ni)) {
			/*
			 * skip corrupted xattr block, verify fscrypt info
			 * if inlie xattr contains fscrypt context. We need
			 * always return -EINVAL to drop the xattr block
			 */
			f2fs_set_bit(x_nid, fsck->nat_area_bitmap);
			fsck->chk.valid_blk_cnt--;
			fsck->chk.valid_node_cnt--;
			node->i.i_xattr_nid = 0;
			x_nid = 0;
			remove_xattr_blk = 1;
		}
	}

	/* do not check encrypt if CORRUPT_BIT is not set */
	if (!file_is_encrypt_corrupt(inode))
		goto skip_chk_encrypt;
	else {
		file_clear_encrypt_corrupt(inode);
		clear_corrupt_bit = 1;
		ASSERT_MSG("clear and skip check CORRUPT_BIT for inode %u", ino);
		goto skip_chk_encrypt;
	}

	ASSERT_MSG("Inode %u FADVISE_ENCRYPT_CORRUPT_BIT set", ino);
	xattr = read_all_xattrs(sbi, node);
	ret = fsck_chk_encrypt(sbi, node, xattr);
	if (ret < 0) {
		if (x_nid) {
			f2fs_set_bit(x_nid, fsck->nat_area_bitmap);
			fsck->chk.valid_blk_cnt--;
			fsck->chk.valid_node_cnt--;
		}
		x_nid = 0;
	}

skip_chk_encrypt:
	if (x_nid) {
		*blk_cnt = *blk_cnt + 1;
		f2fs_set_main_bitmap(sbi, ni.blk_addr, CURSEG_COLD_NODE);/*lint !e644*/
		DBG(2, "ino[0x%x] x_nid[0x%x]\n", ino, x_nid);
	}

	if (ret && config.fix_on) {
		void *inline_xattr = inline_xattr_addr(inode);
		int inline_size = inline_xattr_size(inode);
		memcpy(inline_xattr, xattr, (size_t)inline_size);
		if (x_nid) {
			err = dev_write_block((char *)xattr + inline_size,
					      ni.blk_addr);/*lint !e747*/
			ASSERT(err >= 0);
		}
		file_clear_encrypt_corrupt(inode);
		FIX_MSG("Inode %u clear encrypt_corrupt bit", ino);
	}
	if (xattr)
		free(xattr);
	free(node_blk);
	if (clear_corrupt_bit)
		ret = 1;
	return remove_xattr_blk ? -EINVAL : ret;
}

static int fsck_chk_xattr_entries(struct f2fs_sb_info *sbi,
				  struct f2fs_node *node)
{
	struct f2fs_inode *inode = &node->i;
	void *xattr_blk;
	struct f2fs_xattr_entry *ent;
	nid_t ino = le32_to_cpu(node->footer.ino);
	size_t max_xattr_size, offs = 0, inline_size = (size_t)inline_xattr_size(inode);/*lint !e737 !e571*/
	int err = 0;

	if (inode->i_xattr_nid)
		max_xattr_size = inline_size + MIN_OFFSET;/*lint !e665*/
	else
		max_xattr_size = inline_size;

	if (max_xattr_size == 0)
		return 0;

	xattr_blk = read_all_xattrs(sbi, node);
	ASSERT(xattr_blk);
	/*lint -save -e737 -e732 -e665 -e559*/
	list_for_each_xattr(ent, xattr_blk) {
		offs = (char *)ent - (char *)xattr_blk;
		if (le16_to_cpu(ent->e_value_size) > MAX_VALUE_LEN) {
			ASSERT_MSG("inode[0x%x] has wrong xattr e_value_size 0x%x",
				   ino, le16_to_cpu(ent->e_value_size));
			err = -ERANGE;
			break;
		} else if (offs + ENTRY_SIZE(ent) > max_xattr_size) {
			ASSERT_MSG("inode[0x%x] xattr (offs %zu size %zu) exceeds max xattr size(%zu)",
				   ino, offs, ENTRY_SIZE(ent), max_xattr_size);
			err = -ERANGE;
			break;
		}
	}/*lint -restore*/

	if (err) {
		struct node_info ni;
		nid_t xattr_nid = le32_to_cpu(inode->i_xattr_nid);
		int ret;

		DBG(0, "inode[0x%x] remove xattrs from offset %zu\n", ino, offs);/*lint !e559*/

		if (inline_size && offs <= inline_size)
			memset((char *)inline_xattr_addr(inode) + offs, 0, inline_size - offs);
		else
			err = 0;
		if (xattr_nid && config.fix_on) {
			/* set 4 bytes before node_footer as 0 diretly */
			size_t left = inline_size + BLOCK_SZ - offs -
				      sizeof(struct node_footer);
			memset(ent, 0, left);
			get_node_info(sbi, xattr_nid, &ni);
			ret = dev_write_block((char *)xattr_blk + inline_size, ni.blk_addr);
			ASSERT(ret >= 0);
			FIX_MSG("inode[0x%x] write xattr node block [0x%x]",
				ino, xattr_nid);
		}
	}

	free(xattr_blk);
	return err;
}

int fsck_chk_node_blk(struct f2fs_sb_info *sbi, struct f2fs_inode *inode,
		u32 nid, enum FILE_TYPE ftype, enum NODE_TYPE ntype,
		u32 *blk_cnt, struct child_info *child,
		struct extent_info *i_ext)
{
	struct node_info ni;
	struct f2fs_node *node_blk;

	node_blk = (struct f2fs_node *)calloc(BLOCK_SZ, 1);
	ASSERT(node_blk != NULL);

	if (sanity_check_nid(sbi, nid, node_blk, ftype, ntype, &ni))
		goto err;

	if (ntype == TYPE_INODE) {
		fsck_chk_inode_blk(sbi, nid, ftype, node_blk, blk_cnt, &ni);
	} else {
		switch (ntype) {
		case TYPE_DIRECT_NODE:
			f2fs_set_main_bitmap(sbi, ni.blk_addr,
							CURSEG_WARM_NODE);
			fsck_chk_dnode_blk(sbi, inode, nid, ftype, node_blk,
					blk_cnt, child, &ni, i_ext);
			break;
		case TYPE_INDIRECT_NODE:
			f2fs_set_main_bitmap(sbi, ni.blk_addr,
							CURSEG_COLD_NODE);
			fsck_chk_idnode_blk(sbi, inode, ftype, node_blk,
					blk_cnt, child, i_ext);
			break;
		case TYPE_DOUBLE_INDIRECT_NODE:
			f2fs_set_main_bitmap(sbi, ni.blk_addr,
							CURSEG_COLD_NODE);
			fsck_chk_didnode_blk(sbi, inode, ftype, node_blk,
					blk_cnt, child, i_ext);
			break;
		default:
			ASSERT(0);
		}
	}
	free(node_blk);
	return 0;
err:
	free(node_blk);
	return -EINVAL;
}

static void update_i_extent(struct extent_info *i_ext, block_t blkaddr)
{
	block_t end_addr;

	if (!i_ext)
		return;

	end_addr = i_ext->ext.blk_addr + i_ext->ext.len;

	/* TODO: check its file offset later */
	if (blkaddr >= i_ext->ext.blk_addr && blkaddr < end_addr) {
		unsigned int offset = blkaddr - i_ext->ext.blk_addr;

		if (f2fs_set_bit(offset, i_ext->map))
			i_ext->fail = 1;
		else
			i_ext->len--;
	}
}

static int missing_inline_xattr(struct f2fs_inode *inode)
{
	int idx = DEF_ADDRS_PER_INODE_INLINE_XATTR;
	u32 magic, refcount;

	if (inode->i_inline & F2FS_INLINE_XATTR)
		return 0;

	/*lint -save -e409 -e650 -e679 -e737*/
	magic = le32_to_cpu(inode->i_addr[idx]);
	refcount = le32_to_cpu(inode->i_addr[idx + 1]);
	if ((magic == F2FS_XATTR_MAGIC) && (refcount == 1))
		return 1;
	/*lint -restore*/
	return 0;
}

/* start with valid nid and blkaddr */
void fsck_chk_inode_blk(struct f2fs_sb_info *sbi, u32 nid,
		enum FILE_TYPE ftype, struct f2fs_node *node_blk,
		u32 *blk_cnt, struct node_info *ni)
{
	struct f2fs_fsck *fsck = F2FS_FSCK(sbi);
	struct child_info child = {2, 0, 0, 0, 0};
	enum NODE_TYPE ntype;
	u32 i_links = le32_to_cpu(node_blk->i.i_links);
	u64 i_size = le64_to_cpu(node_blk->i.i_size);
	u64 i_blocks = le64_to_cpu(node_blk->i.i_blocks);
	unsigned char en[F2FS_NAME_LEN + 1];
	u32 namelen;
	child.p_ino = nid;
	child.pp_ino = le32_to_cpu(node_blk->i.i_pino);
	nid_t old_nid = le32_to_cpu(node_blk->i.i_xattr_nid);
	struct extent_info i_extent;
	unsigned int idx = 0;
	int need_fix = 0;
	int ret;

	i_extent.map = NULL;

	if (f2fs_test_main_bitmap(sbi, ni->blk_addr) == 0)
		fsck->chk.valid_inode_cnt++;

	if (ftype == F2FS_FT_DIR) {
		f2fs_set_main_bitmap(sbi, ni->blk_addr, CURSEG_HOT_NODE);
	} else {
		if (f2fs_test_main_bitmap(sbi, ni->blk_addr) == 0) {
			f2fs_set_main_bitmap(sbi, ni->blk_addr,
							CURSEG_WARM_NODE);
			if (i_links > 1 && ftype != F2FS_FT_ORPHAN) {
				/* First time. Create new hard link node */
				add_into_hard_link_list(sbi, nid, i_links);
				fsck->chk.multi_hard_link_files++;
			}
		} else {
			DBG(3, "[0x%x] has hard links [0x%x]\n", nid, i_links);
			if (find_and_dec_hard_link_list(sbi, nid)) {
				DMD_ADD_ERROR(LOG_TYP_FSCK, PR_HARD_LINK_NUM_IS_ERROR);
				ASSERT_MSG("[0x%x] needs more i_links=0x%x",
						nid, i_links);
				if (config.fix_on) {
					node_blk->i.i_links =
						cpu_to_le32(i_links + 1);
					need_fix = 1;
					FIX_MSG("File: 0x%x "
						"i_links= 0x%x -> 0x%x",
						nid, i_links, i_links + 1);
				}
				goto skip_blkcnt_fix;
			}
			/* No need to go deep into the node */
			return;
		}
	}

	ret = fsck_chk_xattr(sbi, nid, node_blk, blk_cnt);
	if (ret && config.fix_on) {
		if (ret < 0) {
			/* clear inline xattr */
			node_blk->i.i_xattr_nid = 0;
			FIX_MSG("Remove xattr block: 0x%x, x_nid = 0x%x",
				nid, old_nid);
		} else {
			FIX_MSG("Fix xattr block: 0x%x, x_nid = 0x%x",
				nid, le32_to_cpu(node_blk->i.i_xattr_nid));
		}
		need_fix = 1;
	}

	if (ftype == F2FS_FT_CHRDEV || ftype == F2FS_FT_BLKDEV ||
			ftype == F2FS_FT_FIFO || ftype == F2FS_FT_SOCK)
		goto check;

	if ((node_blk->i.i_inline & F2FS_INLINE_DATA)) {
		if (le32_to_cpu(node_blk->i.i_addr[0]) != 0) {
			DMD_ADD_ERROR(LOG_TYP_FSCK, PR_INLINE_DATA_ADDR0_NOT_ZERO);
			/* should fix this bug all the time */
			FIX_MSG("inline_data has wrong 0'th block = %x",
					le32_to_cpu(node_blk->i.i_addr[0]));
			node_blk->i.i_addr[0] = 0;
			node_blk->i.i_blocks = cpu_to_le64(*blk_cnt);
			need_fix = 1;
		}
		if (!(node_blk->i.i_inline & F2FS_DATA_EXIST)) {
			char buf[MAX_INLINE_DATA];
			memset(buf, 0, MAX_INLINE_DATA);

			if (memcmp(buf, &node_blk->i.i_addr[1],
							MAX_INLINE_DATA)) {
				DMD_ADD_ERROR(LOG_TYP_FSCK, PR_INLINE_DATA_INEXISTENCE);
				FIX_MSG("inline_data has DATA_EXIST");
				node_blk->i.i_inline |= F2FS_DATA_EXIST;
				need_fix = 1;
			}
		}
		DBG(3, "ino[0x%x] has inline data!\n", nid);
		goto check;
	}

	/* check F2FS_INLINE_XATTR flag before check data */
	if (missing_inline_xattr(&node_blk->i)) {/*lint !e613*/
		ASSERT_MSG("ino[0x%x] missing F2FS_INLINE_XATTR flag\n", nid);
		node_blk->i.i_inline |= F2FS_INLINE_XATTR;
		if (config.fix_on) {
			FIX_MSG("ino[0x%x] set F2FS_INLINE_XATTR\n", nid);
			need_fix = 1;
		}
	}

	/* check inline_dentry errors */
	/*lint -save -e409 -e737*/
	if ((node_blk->i.i_inline & F2FS_INLINE_DENTRY) &&
			le32_to_cpu(node_blk->i.i_addr[0]) != 0) {
		nid_t xnid = le32_to_cpu(node_blk->i.i_xattr_nid);

		/* inline_dentry, but i_addr[0] !=0 */
		if (((xnid && i_blocks == 2) || (xnid == 0 && i_blocks == 1)) &&
				i_size <= MAX_INLINE_DATA) {
			FIX_MSG("ino[0x%x] inline_dentry has wrong 0'th block = %x",
				nid, le32_to_cpu(node_blk->i.i_addr[0]));
			node_blk->i.i_addr[0] = cpu_to_le32(0);
			need_fix = 1;
			config.bug_on = 1;
		}

		/* not inline_dentry, but i_inline has F2FS_INLINE_DENTRY flag */
		if (((xnid && i_blocks > 2) || (xnid == 0 && i_blocks > 1)) &&
				(i_size == 0 || i_size >= PAGE_SIZE)) {
			FIX_MSG("ino[0x%x] not inline_dentry but has inline dentry flag", nid);
			node_blk->i.i_inline &= ~(F2FS_INLINE_DENTRY);
			need_fix = 1;
			config.bug_on = 1;
		}
	}/*lint -restore*/

	if ((node_blk->i.i_inline & F2FS_INLINE_DENTRY)) {
		DBG(3, "ino[0x%x] has inline dentry!\n", nid);
		ret = fsck_chk_inline_dentries(sbi, node_blk, &child);
		if (ret < 0) {
			DMD_ADD_ERROR(LOG_TYP_FSCK, PR_INVALID_INLINE_DENTRY);
			/* should fix this bug all the time */
			need_fix = 1;
		}
		goto check;
	}

	if (fsck_chk_xattr_entries(sbi, node_blk)) {
		if (config.fix_on) {
			FIX_MSG("ino[0x%x] rewrite inline xattr", nid);
			need_fix = 1;
		}
	}

	/* readahead node blocks */
	for (idx = 0; idx < 5; idx++) {
		u32 _nid = le32_to_cpu(node_blk->i.i_nid[idx]);

		if (_nid != 0) {
			struct node_info _ni;

			get_node_info(sbi, _nid, &_ni);
			if (IS_VALID_BLK_ADDR(sbi, _ni.blk_addr))
				dev_reada_block(_ni.blk_addr);
		}
	}

	i_extent.ext = node_blk->i.i_ext;
	i_extent.len = le32_to_cpu(i_extent.ext.len);
	i_extent.fail = 0;
	if (i_extent.len) {
		/* max 126KB */
		i_extent.map = calloc(i_extent.len, 1);
		ASSERT(i_extent.map != NULL);
	}

	/* check data blocks in inode */
	for (idx = 0; idx < ADDRS_PER_INODE(&node_blk->i); idx++) {
		block_t blkaddr = le32_to_cpu(node_blk->i.i_addr[idx]);

		if (blkaddr != 0) {
			ret = fsck_chk_data_blk(sbi,
					blkaddr,
					&child, (i_blocks == *blk_cnt),
					ftype, nid, idx, ni->version,
					file_is_encrypt(&node_blk->i));
			if (!ret) {
				*blk_cnt = *blk_cnt + 1;
				update_i_extent(&i_extent, blkaddr);
			} else if (config.fix_on) {
				node_blk->i.i_addr[idx] = 0;
				need_fix = 1;
				FIX_MSG("[0x%x] i_addr[%d] = 0", nid, idx);
			}
		}
	}

	/* check node blocks in inode */
	for (idx = 0; idx < 5; idx++) {
		block_t blkaddr = le32_to_cpu(node_blk->i.i_nid[idx]);

		if (idx == 0 || idx == 1)
			ntype = TYPE_DIRECT_NODE;
		else if (idx == 2 || idx == 3)
			ntype = TYPE_INDIRECT_NODE;
		else if (idx == 4)
			ntype = TYPE_DOUBLE_INDIRECT_NODE;
		else
			ASSERT(0);

		if (blkaddr != 0) {
			ret = fsck_chk_node_blk(sbi, &node_blk->i,
					blkaddr, ftype, ntype, blk_cnt,
					&child, &i_extent);
			if (!ret) {
				*blk_cnt = *blk_cnt + 1;
			} else if (config.fix_on) {
				node_blk->i.i_nid[idx] = 0;
				need_fix = 1;
				FIX_MSG("[0x%x] i_nid[%d] = 0", nid, idx);
			}
		}
	}
	if (i_extent.len || i_extent.fail) {
		DMD_ADD_ERROR(LOG_TYP_FSCK, PR_INVALID_EXTENT_VALUE);
		ASSERT_MSG("ino: 0x%x has wrong ext: untouched=%d, overlap=%d",
					nid, i_extent.len, i_extent.fail);
		if (config.fix_on)
			need_fix = 1;
	}
check:
	if (i_blocks != *blk_cnt) {
		DMD_ADD_ERROR(LOG_TYP_FSCK, PR_INVALID_I_BLOCKS);
		ASSERT_MSG("ino: 0x%x has i_blocks: %08"PRIx64", "
				"but has %u blocks",
				nid, i_blocks, *blk_cnt);
		if (config.fix_on) {
			node_blk->i.i_blocks = cpu_to_le64(*blk_cnt);
			need_fix = 1;
			FIX_MSG("[0x%x] i_blocks=0x%08"PRIx64" -> 0x%x",
					nid, i_blocks, *blk_cnt);
		}
	}
skip_blkcnt_fix:
	namelen = convert_encrypted_name(node_blk->i.i_name,
					le32_to_cpu(node_blk->i.i_namelen),
					en, file_is_encrypt(&node_blk->i));
	en[namelen] = '\0';
	if (ftype == F2FS_FT_ORPHAN)
		DBG(1, "Orphan Inode: 0x%x [%s] i_blocks: %u\n\n",
				le32_to_cpu(node_blk->footer.ino),
				en, (u32)i_blocks);

	if (ftype == F2FS_FT_DIR) {
		DBG(1, "Directory Inode: 0x%x [%s] depth: %d has %d files\n\n",
				le32_to_cpu(node_blk->footer.ino), en,
				le32_to_cpu(node_blk->i.i_current_depth),
				child.files);

		if (i_links != child.links) {
			DMD_ADD_ERROR(LOG_TYP_FSCK, PR_INVALID_I_LINKS);
			ASSERT_MSG("ino: 0x%x i_links: %u, real links: %u",
					nid, i_links, child.links);
			if (config.fix_on) {
				node_blk->i.i_links = cpu_to_le32(child.links);
				need_fix = 1;
				FIX_MSG("Dir: 0x%x i_links= 0x%x -> 0x%x",
						nid, i_links, child.links);
			}
		}
		if (child.dots < 2 &&
				!(node_blk->i.i_inline & F2FS_INLINE_DOTS)) {
			DMD_ADD_ERROR(LOG_TYP_FSCK, PR_LOST_DOT_OR_DOTDOT);
			ASSERT_MSG("ino: 0x%x dots: %u",
					nid, child.dots);
			if (config.fix_on) {
				node_blk->i.i_inline |= F2FS_INLINE_DOTS;
				need_fix = 1;
				FIX_MSG("Dir: 0x%x set inline_dots", nid);
			}
		}
	}

	if (ftype == F2FS_FT_SYMLINK && i_blocks && i_size == 0) {
		DBG(1, "ino: 0x%x i_blocks: %lu with zero i_size",
							nid, i_blocks);
		if (config.fix_on) {
			u64 fix_size = i_blocks * F2FS_BLKSIZE;

			node_blk->i.i_size = cpu_to_le64(fix_size);
			need_fix = 1;
			FIX_MSG("Symlink: recover 0x%x with i_size=%lu",
							nid, fix_size);
		}
	}

	if (ftype == F2FS_FT_ORPHAN && i_links) {
        DMD_ADD_ERROR(LOG_TYP_FSCK, PR_ORPHAN_INODE_HAS_I_LINKS);
		MSG(0, "ino: 0x%x is orphan inode, but has i_links: %u",
				nid, i_links);
		if (config.fix_on) {
			node_blk->i.i_links = 0;
			need_fix = 1;
			FIX_MSG("ino: 0x%x orphan_inode, i_links= 0x%x -> 0",
					nid, i_links);
		}
	}
	if (need_fix && !config.ro) {
		/* drop extent information to avoid potential wrong access */
		node_blk->i.i_ext.len = 0;
		ret = dev_write_block(node_blk, (__u64)ni->blk_addr);
		ASSERT(ret >= 0);
	}
	if (i_extent.map)
		free(i_extent.map);
}

int fsck_chk_dnode_blk(struct f2fs_sb_info *sbi, struct f2fs_inode *inode,
		u32 nid, enum FILE_TYPE ftype, struct f2fs_node *node_blk,
		u32 *blk_cnt, struct child_info *child, struct node_info *ni,
		struct extent_info *i_ext)
{
	int ret;
	u16 idx;
	int need_fix = 0;

	for (idx = 0; idx < ADDRS_PER_BLOCK; idx++) {
		block_t blkaddr = le32_to_cpu(node_blk->dn.addr[idx]);

		if (blkaddr == 0x0)
			continue;
		ret = fsck_chk_data_blk(sbi,
			blkaddr, child,
			le64_to_cpu(inode->i_blocks) == *blk_cnt, ftype,
			nid, idx, ni->version,
			file_is_encrypt(inode));
		if (!ret) {
			*blk_cnt = *blk_cnt + 1;
			update_i_extent(i_ext, blkaddr);
		} else if (config.fix_on) {
			node_blk->dn.addr[idx] = 0;
			need_fix = 1;
			FIX_MSG("[0x%x] dn.addr[%d] = 0", nid, idx);
		}
	}
	if (need_fix && !config.ro) {
		ret = dev_write_block(node_blk, ni->blk_addr);
		ASSERT(ret >= 0);
	}
	return 0;
}

int fsck_chk_idnode_blk(struct f2fs_sb_info *sbi, struct f2fs_inode *inode,
		enum FILE_TYPE ftype, struct f2fs_node *node_blk, u32 *blk_cnt,
		struct child_info *child, struct extent_info *i_ext)
{
	int need_fix = 0, ret;
	int i = 0;

	for (i = 0 ; i < NIDS_PER_BLOCK; i++) {
		if (le32_to_cpu(node_blk->in.nid[i]) == 0x0)
			continue;
		ret = fsck_chk_node_blk(sbi, inode,
				le32_to_cpu(node_blk->in.nid[i]),
				ftype, TYPE_DIRECT_NODE, blk_cnt, child,
				i_ext);
		if (!ret)
			*blk_cnt = *blk_cnt + 1;
		else if (ret == -EINVAL) {
			if (!config.fix_on)
				MSG(0, "should delete in.nid[i] = 0;\n");
			else {
				node_blk->in.nid[i] = 0;
				need_fix = 1;
				FIX_MSG("Set indirect node 0x%x -> 0\n", i);
			}
		}
	}

	if (need_fix && !config.ro) {
		struct node_info ni;
		nid_t nid = le32_to_cpu(node_blk->footer.nid);

		get_node_info(sbi, nid, &ni);
		ret = dev_write_block(node_blk, ni.blk_addr);
		ASSERT(ret >= 0);
	}

	return 0;
}

int fsck_chk_didnode_blk(struct f2fs_sb_info *sbi, struct f2fs_inode *inode,
		enum FILE_TYPE ftype, struct f2fs_node *node_blk, u32 *blk_cnt,
		struct child_info *child, struct extent_info *i_ext)
{
	int i;
	int need_fix = 0, ret = 0;

	for (i = 0; i < NIDS_PER_BLOCK; i++) {
		if (le32_to_cpu(node_blk->in.nid[i]) == 0x0)
			continue;
		ret = fsck_chk_node_blk(sbi, inode,
				le32_to_cpu(node_blk->in.nid[i]),
				ftype, TYPE_INDIRECT_NODE, blk_cnt, child,
				i_ext);
		if (!ret)
			*blk_cnt = *blk_cnt + 1;
		else if (ret == -EINVAL) {
			if (!config.fix_on)
				MSG(0, "should delete in.nid[i] = 0;\n");
			else {
				node_blk->in.nid[i] = 0;
				need_fix = 1;
				FIX_MSG("Set double indirect node 0x%x -> 0\n", i);
			}
		}
	}

	if (need_fix && !config.ro) {
		struct node_info ni;
		nid_t nid = le32_to_cpu(node_blk->footer.nid);

		get_node_info(sbi, nid, &ni);
		ret = dev_write_block(node_blk, (__u64)ni.blk_addr);
		ASSERT(ret >= 0);
	}

	return 0;
}

static const char *lookup_table =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+,";

/**
 * digest_encode() -
 *
 * Encodes the input digest using characters from the set [a-zA-Z0-9_+].
 * The encoded string is roughly 4/3 times the size of the input string.
 */
static int digest_encode(const char *src, int len, char *dst)
{
	int i = 0, bits = 0, ac = 0;
	char *cp = dst;

	while (i < len) {
		ac += (((unsigned char) src[i]) << bits);
		bits += 8;
		do {
			*cp++ = lookup_table[ac & 0x3f];
			ac >>= 6;
			bits -= 6;
		} while (bits >= 6);
		i++;
	}
	if (bits)
		*cp++ = lookup_table[ac & 0x3f];
	*cp = 0;
	return cp - dst;
}
u32 convert_encrypted_name(unsigned char *name, unsigned int len,
				unsigned char *new, int encrypted)
{
	if (!encrypted) {
		memcpy(new, name, len);
		new[len] = 0;
		return len;
	}

	*new = '_';
	return digest_encode((const char *)name, 24, (char *)new + 1);
}

static void print_dentry(__u32 depth, __u8 *name,
		u8 *bitmap, struct f2fs_dir_entry *dentry,
		int max, int idx, int last_blk, int encrypted)
{
	int last_de = 0;
	int next_idx;
	int name_len;
	unsigned int i;
	u64 bit_offset;
	unsigned char new[F2FS_NAME_LEN + 1];

	if (config.dbg_lv != -1)
		return;

	name_len = le16_to_cpu(dentry[idx].name_len);
	next_idx = idx + (name_len + F2FS_SLOT_LEN - 1) / F2FS_SLOT_LEN;

	bit_offset = find_next_bit_le(bitmap, max, next_idx);
	if (bit_offset >= (u64)max && last_blk)
		last_de = 1;

	if (tree_mark_size <= depth) {
		tree_mark_size *= 2;
		ASSERT(tree_mark_size != 0);
		tree_mark = realloc(tree_mark, tree_mark_size);
		ASSERT(tree_mark != NULL);
	}

	if (last_de)
		tree_mark[depth] = '`';
	else
		tree_mark[depth] = '|';

	if (tree_mark[depth - 1] == '`')
		tree_mark[depth - 1] = ' ';

	for (i = 1; i < depth; i++)
		MSG(0, "%c   ", tree_mark[i]);

	convert_encrypted_name(name, (unsigned int)name_len, new, encrypted);

	MSG(0, "%c-- %s <ino = 0x%x>, <encrypted (%d)>\n",
			last_de ? '`' : '|',
			new, le32_to_cpu(dentry[idx].ino),
			encrypted);
}

static int f2fs_check_hash_code(struct f2fs_dir_entry *dentry,
			const unsigned char *name, u32 len, int encrypted)
{
	f2fs_hash_t hash_code = f2fs_dentry_hash(name, len);

	/* fix hash_code made by old buggy code */
	if (dentry->hash_code != hash_code) {
		unsigned char new[F2FS_NAME_LEN + 1];

		convert_encrypted_name((unsigned char *)name, len,
							new, encrypted);
		FIX_MSG("Mismatch hash_code for \"%s\" [%x:%x]",
				new, le32_to_cpu(dentry->hash_code),
				hash_code);
		dentry->hash_code = cpu_to_le32(hash_code);
		return 1;
	}
	return 0;
}

static int __chk_dots_dentries(struct f2fs_sb_info *sbi,
			       struct f2fs_dir_entry *dentry,
			       struct child_info *child,
			       u8 *name, int len,
			       __u8 (*filename)[F2FS_SLOT_LEN],
			       int encrypted)
{
	int fixed = 0;

	if ((name[0] == '.' && len == 1)) {
		if (le32_to_cpu(dentry->ino) != child->p_ino) {
			ASSERT_MSG("Bad inode number[0x%x] for '.', parent_ino is [0x%x]\n",
				le32_to_cpu(dentry->ino), child->p_ino);
			dentry->ino = cpu_to_le32(child->p_ino);
			fixed = 1;
		}
	}

	if (name[0] == '.' && name[1] == '.' && len == 2) {
		if (child->p_ino == F2FS_ROOT_INO(sbi)) {
			if (le32_to_cpu(dentry->ino) != F2FS_ROOT_INO(sbi)) {
				ASSERT_MSG("Bad inode number[0x%x] for '..'\n",
					le32_to_cpu(dentry->ino));
				dentry->ino = cpu_to_le32(F2FS_ROOT_INO(sbi));
				fixed = 1;
			}
		} else if (le32_to_cpu(dentry->ino) != child->pp_ino) {
			ASSERT_MSG("Bad inode number[0x%x] for '..', parent parent ino is [0x%x]\n",
				le32_to_cpu(dentry->ino), child->pp_ino);
			dentry->ino = cpu_to_le32(child->pp_ino);
			fixed = 1;
		}
	}

	if (f2fs_check_hash_code(dentry, name, len, encrypted))
		fixed = 1;

	if ((*filename)[len] != '\0') {
		ASSERT_MSG("%s is not NULL terminated\n", len == 1 ? "." : "..");
		(*filename)[len] = '\0';
		memcpy(*filename, name, len);
		fixed = 1;
	}
	return fixed;
}

static void nullify_dentry(struct f2fs_dir_entry *dentry, int offs,
			   __u8 (*filename)[F2FS_SLOT_LEN], u8 **bitmap)
{
	memset(dentry, 0, sizeof(struct f2fs_dir_entry));
	test_and_clear_bit_le(offs, *bitmap);
	memset(*filename, 0, F2FS_SLOT_LEN);
}

static int __chk_dentries(struct f2fs_sb_info *sbi, struct child_info *child,
			u8 *bitmap, struct f2fs_dir_entry *dentry,
			__u8 (*filenames)[F2FS_SLOT_LEN],
			int max, int last_blk, int encrypted)
{
	struct f2fs_fsck *fsck = F2FS_FSCK(sbi);
	enum FILE_TYPE ftype;
	int dentries = 0;
	u32 blk_cnt;
	u8 *name;
	unsigned char en[F2FS_NAME_LEN + 1];
	u16 name_len;
	u32 en_len;
	int ret = 0;
	int fixed = 0;
	int i, slots;

	/* readahead inode blocks */
	for (i = 0; i < max; i++) {
		u32 ino;

		if (test_bit_le(i, bitmap) == 0)
			continue;

		ino = le32_to_cpu(dentry[i].ino);

		if (IS_VALID_NID(sbi, ino)) {
			struct node_info ni;

			get_node_info(sbi, ino, &ni);
			if (IS_VALID_BLK_ADDR(sbi, ni.blk_addr)) {
				dev_reada_block((__u64)ni.blk_addr);
				name_len = le16_to_cpu(dentry[i].name_len);
				if (name_len > 0)
					i += (name_len + F2FS_SLOT_LEN - 1) / F2FS_SLOT_LEN - 1;
			}
		}
	}

	for (i = 0; i < max;) {
		if (test_bit_le(i, bitmap) == 0) {
			i++;
			continue;
		}
		if (!IS_VALID_NID(sbi, le32_to_cpu(dentry[i].ino))) {
			DMD_ADD_ERROR(LOG_TYP_FSCK, PR_INVALID_NID);
			ASSERT_MSG("Bad dentry 0x%x with invalid NID/ino 0x%x",
				    i, le32_to_cpu(dentry[i].ino));
			if (config.fix_on) {
				FIX_MSG("Clear bad dentry 0x%x with bad ino 0x%x",
					i, le32_to_cpu(dentry[i].ino));
				test_and_clear_bit_le((u32)i, bitmap);
				fixed = 1;
			}
			i++;
			continue;
		}

		ftype = dentry[i].file_type;
		if ((ftype <= F2FS_FT_UNKNOWN || ftype > F2FS_FT_LAST_FILE_TYPE)) {
			DMD_ADD_ERROR(LOG_TYP_FSCK, PR_INVALID_FTYPE);
			ASSERT_MSG("Bad dentry 0x%x with unexpected ftype 0x%x",
						le32_to_cpu(dentry[i].ino), ftype);
			if (config.fix_on) {
				FIX_MSG("Clear bad dentry 0x%x with bad ftype 0x%x",
					i, ftype);
				test_and_clear_bit_le(i, bitmap);
				fixed = 1;
			}
			i++;
			continue;
		}

		name_len = le16_to_cpu(dentry[i].name_len);

		if (name_len == 0 || name_len > F2FS_NAME_LEN) {
			DMD_ADD_ERROR(LOG_TYP_FSCK, PR_NAME_LEN_IS_ZERO);
			ASSERT_MSG("Bad dentry 0x%x with invalid name_len", i);
			if (config.fix_on) {
				FIX_MSG("Clear bad dentry 0x%x", i);
				test_and_clear_bit_le(i, bitmap);
				fixed = 1;
			}
			i++;
			continue;
		}
		name = calloc(name_len + 1, 1);
		ASSERT(name != NULL);
		memcpy(name, filenames[i], name_len);
		slots = (name_len + F2FS_SLOT_LEN - 1) / F2FS_SLOT_LEN;

		/* Becareful. 'dentry.file_type' is not imode. */
		if (ftype == F2FS_FT_DIR) {
			if ((name[0] == '.' && name_len == 1) ||
				(name[0] == '.' && name[1] == '.' &&
							name_len == 2)) {
				ret = __chk_dots_dentries(sbi, &dentry[i],
					child, name, name_len, &filenames[i],
					encrypted);
				switch (ret) {
				case 1:
					fixed = 1;
				case 0:
					child->dots++;
					break;
				}

				if (child->dots > 2) {
					ASSERT_MSG("More than one '.' or '..', should delete the extra one\n");
					nullify_dentry(&dentry[i], i,
						       &filenames[i], &bitmap);
					child->dots--;
					fixed = 1;
				}

				i++;
				free(name);
				continue;
			}
		}

		if (f2fs_check_hash_code(dentry + i, name, name_len, encrypted)) {
			DMD_ADD_ERROR(LOG_TYP_FSCK, PR_INVALID_HASH_CODE);
			fixed = 1;
		}

		en_len = convert_encrypted_name(name, (unsigned int)name_len, en, encrypted);
		en[en_len] = '\0';
		DBG(1, "[%3u]-[0x%x] name[%s] len[0x%x] ino[0x%x] type[0x%x]\n",
				fsck->dentry_depth, i, en, name_len,
				le32_to_cpu(dentry[i].ino),
				dentry[i].file_type);

		print_dentry(fsck->dentry_depth, name, bitmap,
				dentry, max, i, last_blk, encrypted);

		blk_cnt = 1;
		ret = fsck_chk_node_blk(sbi,
				NULL, le32_to_cpu(dentry[i].ino),
				ftype, TYPE_INODE, &blk_cnt, NULL, NULL);

		if (ret && config.fix_on) {
			int j;

			for (j = 0; j < slots; j++)
				test_and_clear_bit_le(i + j, bitmap);
			FIX_MSG("Unlink [0x%x] - %s len[0x%x], type[0x%x]",
					le32_to_cpu(dentry[i].ino),
					name, name_len,
					dentry[i].file_type);
			fixed = 1;
		} else if (ret == 0) {
			if (ftype == F2FS_FT_DIR)
				child->links++;
			dentries++;
			child->files++;
		}

		i += slots;
		free(name);
	}
	return fixed ? -1 : dentries;
}

int fsck_chk_inline_dentries(struct f2fs_sb_info *sbi,
		struct f2fs_node *node_blk, struct child_info *child)
{
	struct f2fs_fsck *fsck = F2FS_FSCK(sbi);
	struct f2fs_inline_dentry *de_blk;
	int dentries;

	de_blk = inline_data_addr(node_blk);
	ASSERT(de_blk != NULL);

	fsck->dentry_depth++;
	dentries = __chk_dentries(sbi, child,
			de_blk->dentry_bitmap,
			de_blk->dentry, de_blk->filename,
			NR_INLINE_DENTRY, 1,
			file_is_encrypt(&node_blk->i));
	if (dentries < 0) {
		DBG(1, "[%3d] Inline Dentry Block Fixed hash_codes\n\n",
			fsck->dentry_depth);
	} else {
		DBG(1, "[%3d] Inline Dentry Block Done : "
				"dentries:%d in %d slots (len:%d)\n\n",
			fsck->dentry_depth, dentries,
			(int)NR_INLINE_DENTRY, F2FS_NAME_LEN);
	}
	fsck->dentry_depth--;
	return dentries;
}

int fsck_chk_dentry_blk(struct f2fs_sb_info *sbi, u32 blk_addr,
		struct child_info *child, int last_blk, int encrypted)
{
	struct f2fs_fsck *fsck = F2FS_FSCK(sbi);
	struct f2fs_dentry_block *de_blk;
	int dentries, ret;

	de_blk = (struct f2fs_dentry_block *)calloc(BLOCK_SZ, 1);
	ASSERT(de_blk != NULL);

	ret = dev_read_block(de_blk, blk_addr);
	ASSERT(ret >= 0);

	fsck->dentry_depth++;
	dentries = __chk_dentries(sbi, child,
			de_blk->dentry_bitmap,
			de_blk->dentry, de_blk->filename,
			NR_DENTRY_IN_BLOCK, last_blk, encrypted);

	if (dentries < 0 && !config.ro) {
		ret = dev_write_block(de_blk, blk_addr);
		ASSERT(ret >= 0);
		DBG(1, "[%3d] Dentry Block [0x%x] Fixed hash_codes\n\n",
			fsck->dentry_depth, blk_addr);
	} else {
		DBG(1, "[%3d] Dentry Block [0x%x] Done : "
				"dentries:%d in %d slots (len:%d)\n\n",
			fsck->dentry_depth, blk_addr, dentries,
			NR_DENTRY_IN_BLOCK, F2FS_NAME_LEN);
	}
	fsck->dentry_depth--;
	free(de_blk);
	return 0;
}

int fsck_chk_data_blk(struct f2fs_sb_info *sbi, u32 blk_addr,
		struct child_info *child, int last_blk,
		enum FILE_TYPE ftype, u32 parent_nid, u16 idx_in_node, u8 ver,
		int encrypted)
{
	struct f2fs_fsck *fsck = F2FS_FSCK(sbi);

	/* Is it reserved block? */
	if (blk_addr == NEW_ADDR) {
		fsck->chk.valid_blk_cnt++;
		return 0;
	}

	if (!IS_VALID_BLK_ADDR(sbi, blk_addr)) {
		DMD_ADD_ERROR(LOG_TYP_FSCK, PR_INVALID_NID);
		ASSERT_MSG("blkaddres is not valid. [0x%x]", blk_addr);
		return -EINVAL;
	}

	if (is_valid_ssa_data_blk(sbi, blk_addr, parent_nid,
						idx_in_node, ver)) {
		DMD_ADD_ERROR(LOG_TYP_FSCK, PR_INVALID_SUM_DATA_BLOCK);
		ASSERT_MSG("summary data block is not valid. [0x%x]",
						parent_nid);
		return -EINVAL;
	}

	if (f2fs_test_sit_bitmap(sbi, blk_addr) == 0) {
		DMD_ADD_ERROR(LOG_TYP_FSCK, PR_DATA_BLKADDR_OUT_SIT_BITMAP);
		ASSERT_MSG("SIT bitmap is 0x0. blk_addr[0x%x]", blk_addr);
	}

	if (f2fs_test_main_bitmap(sbi, blk_addr) != 0) {
		DMD_ADD_ERROR(LOG_TYP_FSCK, PR_DUPLICATE_DATA_BLKADDR_IN_MAIN_BITMAP);
		ASSERT_MSG("Duplicated data [0x%x]. pnid[0x%x] idx[0x%x]",
				blk_addr, parent_nid, idx_in_node);
	}

	fsck->chk.valid_blk_cnt++;

	if (ftype == F2FS_FT_DIR) {
		f2fs_set_main_bitmap(sbi, blk_addr, CURSEG_HOT_DATA);
		return fsck_chk_dentry_blk(sbi, blk_addr, child,
						last_blk, encrypted);
	} else {
		f2fs_set_main_bitmap(sbi, blk_addr, CURSEG_WARM_DATA);
	}
	return 0;
}

int fsck_chk_orphan_node(struct f2fs_sb_info *sbi)
{
	u32 blk_cnt = 0;
	block_t start_blk, orphan_blkaddr, i, j;
	struct f2fs_orphan_block *orphan_blk, *new_blk;
	struct f2fs_super_block *sb = F2FS_RAW_SUPER(sbi);
	u32 entry_count;

	if (!is_set_ckpt_flags(F2FS_CKPT(sbi), CP_ORPHAN_PRESENT_FLAG))
		return 0;

	start_blk = __start_cp_addr(sbi) + 1 + get_sb(cp_payload);
	orphan_blkaddr = __start_sum_addr(sbi) - 1;

	orphan_blk = calloc(BLOCK_SZ, 1);
	ASSERT(orphan_blk);

	new_blk = calloc(BLOCK_SZ, 1);
	ASSERT(new_blk);

	for (i = 0; i < orphan_blkaddr; i++) {
		int ret = dev_read_block(orphan_blk, start_blk + i);
		u32 new_entry_count = 0;

		ASSERT(ret >= 0);
		entry_count = le32_to_cpu(orphan_blk->entry_count);

		for (j = 0; j < entry_count; j++) {
			nid_t ino = le32_to_cpu(orphan_blk->ino[j]);
			DBG(1, "[%3d] ino [0x%x]\n", i, ino);
			struct node_info ni;
			blk_cnt = 1;

			if (config.preen_mode == PREEN_MODE_1 && !config.fix_on) {
				get_node_info(sbi, ino, &ni);
				if (!IS_VALID_NID(sbi, ino) ||
						!IS_VALID_BLK_ADDR(sbi, ni.blk_addr)) {
					free(orphan_blk);
					free(new_blk);
					return -EINVAL;
				}

				continue;
			}

			ret = fsck_chk_node_blk(sbi, NULL, ino,
					F2FS_FT_ORPHAN, TYPE_INODE, &blk_cnt,
					NULL, NULL);
			if (!ret)
				new_blk->ino[new_entry_count++] =
							orphan_blk->ino[j];
			else if (ret && config.fix_on)
				FIX_MSG("[0x%x] remove from orphan list", ino);
			else if (ret) {
				DMD_ADD_ERROR(LOG_TYP_FSCK, PR_ORPAHN_INODE_ERROR);
				ASSERT_MSG("[0x%x] wrong orphan inode", ino);
			}
		}
		if (!config.ro && config.fix_on &&
				entry_count != new_entry_count) {
			new_blk->entry_count = cpu_to_le32(new_entry_count);
			ret = dev_write_block(new_blk, start_blk + i);
			ASSERT(ret >= 0);
		}
		memset(orphan_blk, 0, BLOCK_SZ);
		memset(new_blk, 0, BLOCK_SZ);
	}
	free(orphan_blk);
	free(new_blk);

	return 0;
}

int fsck_chk_meta(struct f2fs_sb_info *sbi)
{
	struct f2fs_fsck *fsck = F2FS_FSCK(sbi);
	struct f2fs_checkpoint *cp = F2FS_CKPT(sbi);
	struct seg_entry *se;
	unsigned int sit_valid_segs = 0, sit_node_blks = 0;
	unsigned int i;

	/* 1. check sit usage with CP: curseg is lost? */
	for (i = 0; i < TOTAL_SEGS(sbi); i++) {
		se = get_seg_entry(sbi, i);
		if (se->valid_blocks != 0)
			sit_valid_segs++;
		else if (IS_CUR_SEGNO(sbi, i, NO_CHECK_TYPE)) {
			/* curseg has not been written back to device */
			MSG(1, "\tInfo: curseg %u is counted in valid segs\n", i);
			sit_valid_segs++;
		}
		if (IS_NODESEG(se->type))
			sit_node_blks += se->valid_blocks;
	}
	if (fsck->chk.sit_free_segs + sit_valid_segs != TOTAL_SEGS(sbi)) {
		DMD_ADD_ERROR(LOG_TYP_FSCK, PR_SIT_SEGMENT_COUNT_MISMATCH_WITH_TOTAL);
		ASSERT_MSG("SIT usage does not match: sit_free_segs %u, "
				"sit_valid_segs %u, total_segs %u",
			fsck->chk.sit_free_segs, sit_valid_segs,
			TOTAL_SEGS(sbi));
		return -EINVAL;
	}

	/* 2. check node count */
	if (fsck->chk.valid_nat_entry_cnt != sit_node_blks) {
		DMD_ADD_ERROR(LOG_TYP_FSCK, PR_NAT_NODE_COUNT_MISMATCH_WITH_SIT);
		ASSERT_MSG("node count does not match: valid_nat_entry_cnt %u,"
			" sit_node_blks %u",
			fsck->chk.valid_nat_entry_cnt, sit_node_blks);
		return -EINVAL;
	}

	/* 3. check SIT with CP */
	if (fsck->chk.sit_free_segs != le32_to_cpu(cp->free_segment_count)) {
		DMD_ADD_ERROR(LOG_TYP_FSCK, PR_SIT_FREESEG_COUNT_MISMATCH_WITH_CP);
		ASSERT_MSG("free segs does not match: sit_free_segs %u, "
				"free_segment_count %u",
				fsck->chk.sit_free_segs,
				le32_to_cpu(cp->free_segment_count));
		return -EINVAL;
	}

	/* 4. check NAT with CP */
	if (fsck->chk.valid_nat_entry_cnt !=
					le32_to_cpu(cp->valid_node_count)) {
		DMD_ADD_ERROR(LOG_TYP_FSCK, PR_NAT_NODE_COUNT_MISMATCH_WITH_CP);
		ASSERT_MSG("valid node does not match: valid_nat_entry_cnt %u,"
				" valid_node_count %u",
				fsck->chk.valid_nat_entry_cnt,
				le32_to_cpu(cp->valid_node_count));
		return -EINVAL;
	}

	/* 5. check orphan inode simply */
	if (fsck_chk_orphan_node(sbi))
		return -EINVAL;

	if (fsck->nat_valid_inode_cnt != le32_to_cpu(cp->valid_inode_count)) {
		DMD_ADD_ERROR(LOG_TYP_FSCK, PR_NAT_INODE_COUNT_MISMATCH_WITH_CP);
		ASSERT_MSG("valid inode does not match: nat_valid_inode_cnt %u,"
				" valid_inode_count %u",
				fsck->nat_valid_inode_cnt,
				le32_to_cpu(cp->valid_inode_count));
		return -EINVAL;
	}

	/*check nat entry with sit_area_bitmap*/
	for (i = 0; i < fsck->nr_nat_entries; i++) {
		u32 blk = le32_to_cpu(fsck->entries[i].block_addr);
		nid_t ino = le32_to_cpu(fsck->entries[i].ino);

		if (!blk)
			/*
			 * skip entry whose ino is 0, otherwise, we will
			 * get a negative number by BLKOFF_FROM_MAIN(sbi, blk)
			 */
			continue;

		if (!IS_VALID_BLK_ADDR(sbi, blk)) {
			DMD_ADD_ERROR(LOG_TYP_FSCK, PR_NODE_INVALID_BLKADDR);
			MSG(0, "\tError: nat entry[ino %u block_addr 0x%x]"
				" is in valid\n",
				ino, blk);
			return -EINVAL;
		}

		if (!f2fs_test_sit_bitmap(sbi, blk)) {
			DMD_ADD_ERROR(LOG_TYP_FSCK, PR_NAT_BLKADDR_OUT_SIT_BITMAP);
			MSG(0, "\tError: nat entry[ino %u block_addr 0x%x]"
				" not find it in sit_area_bitmap\n",
				ino, blk);
			return -EINVAL;
		}

		if (!IS_VALID_NID(sbi, ino)) {
			DMD_ADD_ERROR(LOG_TYP_FSCK, PR_INVALID_NID);
			MSG(0, "\tError: nat_entry->ino %u exceeds the range"
				" of nat entries %u\n",
				ino, fsck->nr_nat_entries);
			return -EINVAL;
		}

		if (!f2fs_test_bit(ino, fsck->nat_area_bitmap)) {
			DMD_ADD_ERROR(LOG_TYP_FSCK, PR_NAT_INO_OUT_NAT_BITMAP);
			MSG(0, "\tError: nat_entry->ino %u is not set in"
				" nat_area_bitmap\n", ino);
			return -EINVAL;
		}
	}

	return 0;
}

void fsck_init(struct f2fs_sb_info *sbi)
{
	struct f2fs_fsck *fsck = F2FS_FSCK(sbi);
	struct f2fs_sm_info *sm_i = SM_I(sbi);

	/*
	 * We build three bitmap for main/sit/nat so that may check consistency
	 * of filesystem.
	 * 1. main_area_bitmap will be used to check whether all blocks of main
	 *    area is used or not.
	 * 2. nat_area_bitmap has bitmap information of used nid in NAT.
	 * 3. sit_area_bitmap has bitmap information of used main block.
	 * At Last sequence, we compare main_area_bitmap with sit_area_bitmap.
	 */
	fsck->nr_main_blks = sm_i->main_segments << sbi->log_blocks_per_seg;
	fsck->main_area_bitmap_sz = (fsck->nr_main_blks + 7) / 8;
	fsck->main_area_bitmap = calloc(fsck->main_area_bitmap_sz, 1);
	ASSERT(fsck->main_area_bitmap != NULL);

	build_nat_area_bitmap(sbi);

	build_sit_area_bitmap(sbi);

	ASSERT(tree_mark_size != 0);
	tree_mark = calloc(tree_mark_size, 1);
	ASSERT(tree_mark != NULL);
}

static void fix_hard_links(struct f2fs_sb_info *sbi)
{
	struct f2fs_fsck *fsck = F2FS_FSCK(sbi);
	struct hard_link_node *tmp, *node;
	struct f2fs_node *node_blk = NULL;
	struct node_info ni;
	int ret;

	if (fsck->hard_link_list_head == NULL)
		return;

	node_blk = (struct f2fs_node *)calloc(BLOCK_SZ, 1);
	ASSERT(node_blk != NULL);

	node = fsck->hard_link_list_head;
	while (node) {
		/* Sanity check */
		if (sanity_check_nid(sbi, node->nid, node_blk,
					F2FS_FT_MAX, TYPE_INODE, &ni))
			FIX_MSG("Failed to fix, rerun fsck.f2fs");

		node_blk->i.i_links = cpu_to_le32(node->actual_links);

		FIX_MSG("File: 0x%x i_links= 0x%x -> 0x%x",
				node->nid, node->links, node->actual_links);

		ret = dev_write_block(node_blk, ni.blk_addr);
		ASSERT(ret >= 0);
		tmp = node;
		node = node->next;
		free(tmp);
	}
	free(node_blk);
}

static void fix_nat_entries(struct f2fs_sb_info *sbi)
{
	struct f2fs_fsck *fsck = F2FS_FSCK(sbi);
	u32 i;

	for (i = 0; i < fsck->nr_nat_entries; i++)
		if (f2fs_test_bit(i, fsck->nat_area_bitmap) != 0)
			nullify_nat_entry(sbi, i);
}

static void flush_curseg_sit_entries(struct f2fs_sb_info *sbi)
{
	struct sit_info *sit_i = SIT_I(sbi);
	int i;

	/* update curseg sit entries, since we may change
	 * a segment type in move_curseg_info
	 */
	for (i = 0; i < NO_CHECK_TYPE; i++) {
		struct curseg_info *curseg = CURSEG_I(sbi, i);
		struct f2fs_sit_block *sit_blk;
		struct f2fs_sit_entry *sit;
		struct seg_entry *se;

		se = get_seg_entry(sbi, curseg->segno);
		sit_blk = get_current_sit_page(sbi, curseg->segno);
		sit = &sit_blk->entries[SIT_ENTRY_OFFSET(sit_i, curseg->segno)];
		sit->vblocks = cpu_to_le16((se->type << SIT_VBLOCKS_SHIFT) |
							se->valid_blocks);
		rewrite_current_sit_page(sbi, curseg->segno, sit_blk);
		free(sit_blk);
	}
}

static void fix_checkpoint(struct f2fs_sb_info *sbi)
{
	struct f2fs_fsck *fsck = F2FS_FSCK(sbi);
	struct f2fs_super_block *sb = F2FS_RAW_SUPER(sbi);
	struct f2fs_checkpoint *cp = F2FS_CKPT(sbi);
	unsigned long long cp_blk_no;
	u32 flags = CP_UMOUNT_FLAG;
	block_t orphan_blks = 0;
	u32 i;
	int ret;
	u_int32_t crc = 0;

	if (is_set_ckpt_flags(cp, CP_ORPHAN_PRESENT_FLAG)) {
		orphan_blks = __start_sum_addr(sbi) - 1;
		flags |= CP_ORPHAN_PRESENT_FLAG;
	}

	set_cp(ckpt_flags, flags);
	set_cp(cp_pack_total_block_count, 8 + orphan_blks + get_sb(cp_payload));

	set_cp(free_segment_count, get_free_segments(sbi));
	set_cp(valid_block_count, fsck->chk.valid_blk_cnt);
	set_cp(valid_node_count, fsck->chk.valid_node_cnt);
	set_cp(valid_inode_count, fsck->chk.valid_inode_cnt);

	crc = f2fs_cal_crc32(F2FS_SUPER_MAGIC, cp, CHECKSUM_OFFSET);
	*((__le32 *)((unsigned char *)cp + CHECKSUM_OFFSET)) = cpu_to_le32(crc);

	cp_blk_no = get_sb(cp_blkaddr);
	if (sbi->cur_cp == 2)
		cp_blk_no += 1 << get_sb(log_blocks_per_seg);

	ret = dev_write_block(cp, cp_blk_no++);
	ASSERT(ret >= 0);

	for (i = 0; i < get_sb(cp_payload); i++) {
		ret = dev_write_block(((unsigned char *)cp) + i * F2FS_BLKSIZE,
								cp_blk_no++);
		ASSERT(ret >= 0);
	}

	cp_blk_no += orphan_blks;

	for (i = 0; i < NO_CHECK_TYPE; i++) {
		struct curseg_info *curseg = CURSEG_I(sbi, i);

		ret = dev_write_block(curseg->sum_blk, cp_blk_no++);
		ASSERT(ret >= 0);
	}

	ret = dev_write_block(cp, cp_blk_no++);
	ASSERT(ret >= 0);
}

int check_curseg_offset(struct f2fs_sb_info *sbi)
{
	int i;

	for (i = 0; i < NO_CHECK_TYPE; i++) {
		struct curseg_info *curseg = CURSEG_I(sbi, i);
		struct seg_entry *se;
		unsigned int j, nblocks;

		se = get_seg_entry(sbi, curseg->segno);
		if (f2fs_test_bit(curseg->next_blkoff,
					(const char *)se->cur_valid_map)) {
			DMD_ADD_ERROR(LOG_TYP_FSCK, PR_CUR_NEXT_BLK_IS_NOT_FREE);
			ASSERT_MSG("Next block offset is not free, type:%d", i);
			return -EINVAL;
		}
		if (curseg->alloc_type == SSR)
			return 0;

		nblocks = sbi->blocks_per_seg;
		for (j = curseg->next_blkoff + 1; j < nblocks; j++) {
			if (f2fs_test_bit(j, (const char *)se->cur_valid_map)) {
				DMD_ADD_ERROR(LOG_TYP_FSCK, PR_LFS_HAS_NO_FREE_SECTION);
				ASSERT_MSG("LFS must have free section:%d", i);
				return -EINVAL;
			}
		}
	}
	return 0;
}

int check_sit_types(struct f2fs_sb_info *sbi)
{
	unsigned int i;
	int err = 0;

	for (i = 0; i < TOTAL_SEGS(sbi); i++) {
		struct seg_entry *se;

		se = get_seg_entry(sbi, i);
		if (se->orig_type != se->type) {
			if (se->orig_type == CURSEG_COLD_DATA &&
					se->type <= CURSEG_COLD_DATA) {
				se->type = se->orig_type;
			} else {
				DMD_ADD_ERROR(LOG_TYP_FSCK, PR_SIT_TYPE_IS_ERROR);
				FIX_MSG("Wrong segment type [0x%x] %x -> %x",
						i, se->orig_type, se->type);
				err = -EINVAL;
			}
		}
	}
	return err;
}

int fsck_verify(struct f2fs_sb_info *sbi)
{
	unsigned int i;
	int ret = 0;
	int force = 0;
	u32 nr_unref_nid = 0;
	struct f2fs_fsck *fsck = F2FS_FSCK(sbi);
	struct hard_link_node *node = NULL;

	MSG(0, "\n");

	for (i = 0; i < fsck->nr_nat_entries; i++) {
		if (f2fs_test_bit(i, fsck->nat_area_bitmap) != 0) {
			if (nr_unref_nid == 0) {
				DMD_ADD_ERROR(LOG_TYP_FSCK, PR_NID_IS_UNREACHABLE);
				MSG(0, "Unreachable NIDs:\n");
				MSG(0, "[");
			}
		MSG(0, "%x,", i);
		nr_unref_nid++;
		}
	}
    if (nr_unref_nid)
        MSG(0, "]\n");

	if (fsck->hard_link_list_head != NULL) {
		node = fsck->hard_link_list_head;
		while (node) {
			DMD_ADD_ERROR(LOG_TYP_FSCK, PR_NID_HAS_MORE_UNREACHABLE_LINKS);
			MSG(0, "NID[0x%x] has [0x%x] more unreachable links\n",
					node->nid, node->links);
			node = node->next;
		}
		config.bug_on = 1;
	}

	MSG(0, "[FSCK] Unreachable nat entries                       ");
	if (nr_unref_nid == 0x0) {
		MSG(0, " [Ok..] [0x%x]\n", nr_unref_nid);
	} else {
		MSG(0, " [Fail] [0x%x]\n", nr_unref_nid);
		ret = EXIT_ERR_CODE;
		config.bug_on = 1;
	}

	MSG(0, "[FSCK] SIT valid block bitmap checking                ");
	if (memcmp(fsck->sit_area_bitmap, fsck->main_area_bitmap,
					fsck->sit_area_bitmap_sz) == 0x0) {
		MSG(0, "[Ok..]\n");
	} else {
		MSG(0, "[Fail]\n");
		ret = EXIT_ERR_CODE;
		config.bug_on = 1;
	}

	MSG(0, "[FSCK] Hard link checking for regular file           ");
	if (fsck->hard_link_list_head == NULL) {
		MSG(0, " [Ok..] [0x%x]\n", fsck->chk.multi_hard_link_files);
	} else {
		MSG(0, " [Fail] [0x%x]\n", fsck->chk.multi_hard_link_files);
		ret = EXIT_ERR_CODE;
		config.bug_on = 1;
	}

	MSG(0, "[FSCK] valid_block_count matching with CP            ");
	if (sbi->total_valid_block_count == fsck->chk.valid_blk_cnt) {
		MSG(0, " [Ok..] [0x%x]\n", (u32)fsck->chk.valid_blk_cnt);
	} else {
		MSG(0, " [Fail] [0x%x]\n", (u32)fsck->chk.valid_blk_cnt);
		ret = EXIT_ERR_CODE;
		config.bug_on = 1;
	}

	MSG(0, "[FSCK] valid_node_count matcing with CP (de lookup)  ");
	if (sbi->total_valid_node_count == fsck->chk.valid_node_cnt) {
		MSG(0, " [Ok..] [0x%x]\n", fsck->chk.valid_node_cnt);
	} else {
		MSG(0, " [Fail] [0x%x]\n", fsck->chk.valid_node_cnt);
		ret = EXIT_ERR_CODE;
		config.bug_on = 1;
	}

	MSG(0, "[FSCK] valid_node_count matcing with CP (nat lookup) ");
	if (sbi->total_valid_node_count == fsck->chk.valid_nat_entry_cnt) {
		MSG(0, " [Ok..] [0x%x]\n", fsck->chk.valid_nat_entry_cnt);
	} else {
		MSG(0, " [Fail] [0x%x]\n", fsck->chk.valid_nat_entry_cnt);
		ret = EXIT_ERR_CODE;
		config.bug_on = 1;
	}

	MSG(0, "[FSCK] valid_inode_count matched with CP             ");
	if (sbi->total_valid_inode_count == fsck->chk.valid_inode_cnt) {
		MSG(0, " [Ok..] [0x%x]\n", fsck->chk.valid_inode_cnt);
	} else {
		MSG(0, " [Fail] [0x%x]\n", fsck->chk.valid_inode_cnt);
		ret = EXIT_ERR_CODE;
		config.bug_on = 1;
	}

	MSG(0, "[FSCK] free segment_count matched with CP            ");
	if (le32_to_cpu(F2FS_CKPT(sbi)->free_segment_count) ==
						fsck->chk.sit_free_segs) {
		MSG(0, " [Ok..] [0x%x]\n", fsck->chk.sit_free_segs);
	} else {
		MSG(0, " [Fail] [0x%x]\n", fsck->chk.sit_free_segs);
		ret = EXIT_ERR_CODE;
		config.bug_on = 1;
	}

	MSG(0, "[FSCK] next block offset is free                     ");
	if (check_curseg_offset(sbi) == 0) {
		MSG(0, " [Ok..]\n");
	} else {
		MSG(0, " [Fail]\n");
		ret = EXIT_ERR_CODE;
		config.bug_on = 1;
	}

	MSG(0, "[FSCK] fixing SIT types\n");
	if (check_sit_types(sbi) != 0)
		force = 1;

	MSG(0, "[FSCK] other corrupted bugs                          ");
	if (config.bug_on == 0) {
		MSG(0, " [Ok..]\n");
	} else {
		MSG(0, " [Fail]\n");
		ret = EXIT_ERR_CODE;
	}

	/* fix global metadata */
	if (force || (config.fix_on && !config.ro)) {
		struct f2fs_checkpoint *cp = F2FS_CKPT(sbi);

		if (force || config.bug_on) {
			fix_hard_links(sbi);
			fix_nat_entries(sbi);
			rewrite_sit_area_bitmap(sbi);
			move_curseg_info(sbi, (u64)SM_I(sbi)->main_blkaddr);
			write_curseg_info(sbi);
			flush_curseg_sit_entries(sbi);
			fix_checkpoint(sbi);
		} else if (is_set_ckpt_flags(cp, CP_FSCK_FLAG)) {
			write_checkpoint(sbi);
			FIX_MSG("Let's update checkpoint, clean the CP_FSCK_FLAG");
		}
		clear_extra_flag(sbi, EXTRA_NEED_FSCK_FLAG);
	}
	return ret;
}

void fsck_free(struct f2fs_sb_info *sbi)
{
	struct f2fs_fsck *fsck = F2FS_FSCK(sbi);
	if (fsck->main_area_bitmap)
		free(fsck->main_area_bitmap);

	if (fsck->nat_area_bitmap)
		free(fsck->nat_area_bitmap);

	if (fsck->sit_area_bitmap)
		free(fsck->sit_area_bitmap);

	if (fsck->entries)
		free(fsck->entries);

	if (tree_mark)
		free(tree_mark);
}
