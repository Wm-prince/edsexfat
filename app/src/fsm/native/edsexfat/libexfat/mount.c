/*
	mount.c (22.10.09)
	exFAT file system implementation library.

	Free exFAT implementation.
	Copyright (C) 2010-2017  Andrew Nayenko

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License along
	with this program; if not, write to the Free Software Foundation, Inc.,
	51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "exfat.h"
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <inttypes.h>
#include <unistd.h>
#include <sys/types.h>

static uint64_t rootdir_size(const struct exfat* ef)
{
	uint32_t clusters = 0;
	uint32_t clusters_max = le32_to_cpu(ef->sb->cluster_count);
	cluster_t rootdir_cluster = le32_to_cpu(ef->sb->rootdir_cluster);

	/* Iterate all clusters of the root directory to calculate its size.
	   It can't be contiguous because there is no flag to indicate this. */
	do
	{
		if (clusters == clusters_max) /* infinite loop detected */
		{
			exfat_error("root directory cannot occupy all %d clusters",
					clusters);
			return 0;
		}
		if (CLUSTER_INVALID(*ef->sb, rootdir_cluster))
		{
			exfat_error("bad cluster %#x while reading root directory",
					rootdir_cluster);
			return 0;
		}
		rootdir_cluster = exfat_next_cluster(ef, ef->root, rootdir_cluster);
		clusters++;
	}
	while (rootdir_cluster != EXFAT_CLUSTER_END);

	return (uint64_t) clusters * CLUSTER_SIZE(*ef->sb);
}

static const char* get_option(const char* options, const char* option_name)
{
	const char* p;
	size_t length = strlen(option_name);

	for (p = strstr(options, option_name); p; p = strstr(p + 1, option_name))
		if ((p == options || p[-1] == ',') && p[length] == '=')
			return p + length + 1;
	return NULL;
}

static int get_int_option(const char* options, const char* option_name,
		int base, int default_value)
{
	const char* p = get_option(options, option_name);

	if (p == NULL)
		return default_value;
	return strtol(p, NULL, base);
}

static bool match_option(const char* options, const char* option_name)
{
	const char* p;
	size_t length = strlen(option_name);

	for (p = strstr(options, option_name); p; p = strstr(p + 1, option_name))
		if ((p == options || p[-1] == ',') &&
				(p[length] == ',' || p[length] == '\0'))
			return true;
	return false;
}

static void parse_options(struct exfat* ef, const char* options)
{
	int opt_umask;

	opt_umask = get_int_option(options, "umask", 8, 0);
	ef->dmask = get_int_option(options, "dmask", 8, opt_umask);
	ef->fmask = get_int_option(options, "fmask", 8, opt_umask);

	ef->uid = get_int_option(options, "uid", 10, geteuid());
	ef->gid = get_int_option(options, "gid", 10, getegid());

	ef->noatime = match_option(options, "noatime");
}

static bool verify_vbr_checksum(struct exfat_dev* dev, void* sector,
		off64_t sector_size)
{
	uint32_t vbr_checksum;
	int i;

	if (exfat_pread(dev, sector, sector_size, 0) < 0)
	{
		exfat_error("failed to read boot sector");
		return false;
	}
	vbr_checksum = exfat_vbr_start_checksum(sector, sector_size);
	for (i = 1; i < 11; i++)
	{
		if (exfat_pread(dev, sector, sector_size, i * sector_size) < 0)
		{
			exfat_error("failed to read VBR sector");
			return false;
		}
		vbr_checksum = exfat_vbr_add_checksum(sector, sector_size,
				vbr_checksum);
	}
	if (exfat_pread(dev, sector, sector_size, i * sector_size) < 0)
	{
		exfat_error("failed to read VBR checksum sector");
		return false;
	}
	for (i = 0; i < sector_size / sizeof(vbr_checksum); i++)
		if (le32_to_cpu(((const le32_t*) sector)[i]) != vbr_checksum)
		{
			exfat_error("invalid VBR checksum 0x%x (expected 0x%x)",
					le32_to_cpu(((const le32_t*) sector)[i]), vbr_checksum);
			return false;
		}
	return true;
}

static int commit_super_block(const struct exfat* ef)
{
	if (exfat_pwrite(ef->dev, ef->sb, sizeof(struct exfat_super_block), 0) < 0)
	{
		exfat_error("failed to write super block");
		return 1;
	}
	return exfat_fsync(ef->dev);
}

static int prepare_super_block(const struct exfat* ef)
{
	if (le16_to_cpu(ef->sb->volume_state) & EXFAT_STATE_MOUNTED)
		exfat_warn("volume was not unmounted cleanly");

	if (ef->ro)
		return 0;

	ef->sb->volume_state = cpu_to_le16(
			le16_to_cpu(ef->sb->volume_state) | EXFAT_STATE_MOUNTED);
	return commit_super_block(ef);
}

static void exfat_free(struct exfat* ef)
{
	exfat_close(ef->dev);	/* first of all, close the descriptor */
	ef->dev = NULL;			/* struct exfat_dev is freed by exfat_close() */
	free(ef->root);
	ef->root = NULL;
	free(ef->zero_cluster);
	ef->zero_cluster = NULL;
	free(ef->cmap.chunk);
	ef->cmap.chunk = NULL;
	free(ef->upcase);
	ef->upcase = NULL;
	free(ef->sb);
	ef->sb = NULL;
}

static void finalize_super_block(struct exfat* ef)
{
	if (ef->ro)
		return;

	ef->sb->volume_state = cpu_to_le16(
			le16_to_cpu(ef->sb->volume_state) & ~EXFAT_STATE_MOUNTED);

	/* Some implementations set the percentage of allocated space to 0xff
	   on FS creation and never update it. In this case leave it as is. */
	if (ef->sb->allocated_percent != 0xff)
	{
		uint32_t free, total;

		free = exfat_count_free_clusters(ef);
		total = le32_to_cpu(ef->sb->cluster_count);
		ef->sb->allocated_percent = ((total - free) * 100 + total / 2) / total;
	}

	commit_super_block(ef);	/* ignore return code */
}

void exfat_unmount(struct exfat* ef)
{
	exfat_flush_nodes(ef);	/* ignore return code */
	exfat_flush(ef);		/* ignore return code */
	exfat_put_node(ef, ef->root);
	exfat_reset_cache(ef);
	finalize_super_block(ef);
	exfat_free(ef);			/* will close the descriptor */
}
