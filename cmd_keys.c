/*
 * cmd_keys.c  –  SET / GET / DEL
 *
 *  args[0]=명령어  args[1]=key  args[2]=value(SET 만)
 */
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <fnmatch.h>
#include "shm_types.h"
#include "shm_core.h"
#include "cmd_keys.h"

s_replyObject *cmd_keys(ShmHandle *h, string_t *args[], uint32_t argc)	{
    if (argc < 2) return reply_error(SHM_ERR_ARGC, "usage: SET key value");
	const char	*pattern = args[1]->ptr;

	ShmHeader *s = (ShmHeader*)h->base;
	s_replyObject *arr = reply_array(0);

	BucketEntry *bucket = core_get_bucket(h, 0);
	for (uint32_t i=0;i<s->hash_table_size;i++)	{
		if (bucket->head_offset == OFFSET_NULL)	{
			bucket ++;
			continue;
		}
		uint64_t	offset = bucket->head_offset;
		while (offset != OFFSET_NULL)	{
			NameEntry *entry = (NameEntry*)OFF2PTR(h, offset);
			if (fnmatch (pattern, OFF2PTR(h, entry->key_offset), 0) == 0)	{
				reply_array_append (arr, reply_string(OFF2PTR(h, entry->key_offset), entry->key_len));
			}
			offset = entry->next_offset;
		}
		bucket ++;
	}
	return arr;
}
