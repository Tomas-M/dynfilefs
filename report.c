#define _GNU_SOURCE
#include "report.h"
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Keep identities exact when reports are parsed by JavaScript.
static void write_id(FILE *stream, uint64_t id)
{
    if (id) fprintf(stream, "\"%016" PRIx64 "\"", id);
    else fputs("null", stream);
}

// Escape JSON punctuation and controls while preserving international UTF-8 bytes.
static void write_name(FILE *stream, const char *name)
{
    fputc('"', stream);
    for (const unsigned char *p=(const unsigned char *)name; *p; p++)
    {
        if (*p=='"' || *p=='\\') fputc('\\', stream);
        if (*p < 32) fprintf(stream, "\\u%04x", *p);
        else fputc(*p, stream);
    }
    fputc('"', stream);
}

// Sort copied catalog records without changing a live caller's list.
static int compare_records(const void *left, const void *right)
{
    uint64_t a=((const struct snapshot_record *)left)->id, b=((const struct snapshot_record *)right)->id;
    return a < b ? -1 : a!=b;
}

// Parent identities are independent of names and may refer to deleted history nodes.
static size_t find_record(const struct snapshot_record *records, size_t count, uint64_t id)
{
    size_t first=0, end=count;
    while (first < end)
    {
        size_t middle=first+(end-first)/2;
        if (records[middle].id < id) first=middle+1;
        else end=middle;
    }
    return first < count && records[first].id==id ? first : count;
}

// Render a complete history without recursion, even after many retained ancestor records.
int snapshot_report(const struct snapshot_record *records, size_t count, uint64_t parent, int json, char **result)
{
    if (count > (SIZE_MAX/sizeof(size_t)-2)/4 || count > SIZE_MAX/sizeof(*records)) return -EOVERFLOW;
    struct snapshot_record *sorted=count ? malloc(count*sizeof(*sorted)) : NULL;
    size_t *links=malloc((count*4+2)*sizeof(*links));
    if ((count && !sorted) || !links) { free(sorted); free(links); return -ENOMEM; }
    if (count)
    {
        memcpy(sorted, records, count*sizeof(*sorted));
        qsort(sorted, count, sizeof(*sorted), compare_records);
    }
    size_t *first=links, *last=first+count+1, *next=last+count+1, *stack=next+count;
    for (size_t i=0; i < count*4+2; i++) links[i]=SIZE_MAX;
    int error=0;
    for (size_t i=0; i < count; i++)
    {
        size_t owner=sorted[i].parent ? find_record(sorted, count, sorted[i].parent) : count;
        int duplicate=i && sorted[i-1].id==sorted[i].id;
        int missing=sorted[i].parent && owner==count;
        if (!sorted[i].id || sorted[i].parent >= sorted[i].id || duplicate || missing) { error=-EIO; break; }
        if (last[owner]!=SIZE_MAX) next[last[owner]]=i;
        else first[owner]=i;
        last[owner]=i;
    }
    if (!error && parent && find_record(sorted, count, parent)==count) error=-EIO;
    if (error) { free(sorted); free(links); return error; }
    char *bytes=NULL;
    size_t length=0;
    FILE *stream=open_memstream(&bytes, &length);
    if (!stream) { free(sorted); free(links); return -errno; }
    if (json)
    {
        fputs("{\"currentParent\":", stream); write_id(stream, parent); fputs(",\"snapshots\":[", stream);
        for (size_t i=0; i < count; i++)
        {
            if (i) fputc(',', stream);
            fputs("{\"id\":", stream); write_id(stream, sorted[i].id);
            fputs(",\"parent\":", stream); write_id(stream, sorted[i].parent);
            fputs(",\"name\":", stream); write_name(stream, sorted[i].name);
            fprintf(stream, ",\"created\":%" PRIu64 ",\"deleted\":%s,\"automatic\":%s,\"protected\":%s}", sorted[i].created,
                sorted[i].size ? "false" : "true", !strncmp(sorted[i].name, "auto-", 5) ? "true" : "false", sorted[i].protected ? "true" : "false");
        }
        fputs("]}\n", stream);
    }
    else
    {
        if (!count) fputs("(no snapshots)\n", stream);
        size_t node=first[count], depth=0;
        while (node!=SIZE_MAX)
        {
            for (size_t i=1; i < depth; i++) fputs(next[stack[i]]==SIZE_MAX ? "    " : "│   ", stream);
            if (depth) fputs(next[node]==SIZE_MAX ? "└── " : "├── ", stream);
            fprintf(stream, "%s [%016" PRIx64 "]%s%s\n", sorted[node].name, sorted[node].id,
                sorted[node].size ? "" : " (deleted)", sorted[node].id==parent ? " <- current parent" : "");
            if (first[node]!=SIZE_MAX) { stack[depth++]=node; node=first[node]; }
            else
            {
                while (next[node]==SIZE_MAX && depth) node=stack[--depth];
                node=next[node];
            }
        }
    }
    if (ferror(stream)) error=-EIO;
    if (fclose(stream) && !error) error=-errno;
    free(sorted); free(links);
    if (!error && length > INT_MAX) error=-EOVERFLOW;
    if (error) { free(bytes); return error; }
    *result=bytes;
    return (int)length;
}

// Use the same exact capacity fields for offline inspection and the live stats file.
int storage_info_report(const struct storage_info *info, int json, char **result)
{
    char *bytes=NULL;
    size_t length=0;
    FILE *stream=open_memstream(&bytes, &length);
    if (!stream) return -errno;
    if (json)
    {
        fprintf(stream, "{\"version\":%" PRIu64 ",\"size\":%" PRIu64 ",\"sizeMiB\":%" PRIu64
            ",\"split\":%" PRIu64 ",\"splitMiB\":%" PRIu64 ",\"blockSize\":%d,\"reusableBytes\":%" PRIu64
            ",\"currentParent\":", info->version, info->size, info->size/1048576, info->split, info->split/1048576,
            DYN_BLOCK_SIZE, info->reusable);
        write_id(stream, info->parent); fputs("}\n", stream);
    }
    else
    {
        fprintf(stream, "version:%" PRIu64 "\nsize:%" PRIu64 "\nsizeMiB:%" PRIu64 "\nsplit:%" PRIu64
            "\nsplitMiB:%" PRIu64 "\nblockSize:%d\nreusableBytes:%" PRIu64 "\ncurrentParent:",
            info->version, info->size, info->size/1048576, info->split, info->split/1048576, DYN_BLOCK_SIZE, info->reusable);
        if (info->parent) fprintf(stream, "%016" PRIx64 "\n", info->parent);
        else fputs("none\n", stream);
    }
    int error=ferror(stream) ? -EIO : 0;
    if (fclose(stream) && !error) error=-errno;
    if (error) { free(bytes); return error; }
    *result=bytes;
    return (int)length;
}
