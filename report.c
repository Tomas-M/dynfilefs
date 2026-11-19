#define _GNU_SOURCE
#include "report.h"
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

// Exact ID/name filters never hide a point that merely reused another point's reserved ID.
static int compare_hidden(const void *left, const void *right)
{
    return strcmp(*(const char *const *)left, *(const char *const *)right);
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
int snapshot_report(const struct snapshot_record *records, size_t count, uint64_t parent, enum snapshot_format format, const char *const *hidden, size_t hidden_count, char **result)
{
    if (count > (SIZE_MAX/sizeof(size_t)-2)/6 || count > SIZE_MAX/sizeof(*records)) return -EOVERFLOW;
    if (hidden_count > SIZE_MAX/sizeof(*hidden)) return -EOVERFLOW;
    struct snapshot_record *sorted=count ? malloc(count*sizeof(*sorted)) : NULL;
    size_t *links=malloc((count*6+2)*sizeof(*links));
    const char **filters=hidden_count ? malloc(hidden_count*sizeof(*filters)) : NULL;
    if ((count && !sorted) || !links || (hidden_count && !filters)) { free(sorted); free(links); free(filters); return -ENOMEM; }
    if (hidden_count) { memcpy(filters, hidden, hidden_count*sizeof(*filters)); qsort(filters, hidden_count, sizeof(*filters), compare_hidden); }
    if (count)
    {
        memcpy(sorted, records, count*sizeof(*sorted));
        qsort(sorted, count, sizeof(*sorted), compare_records);
    }
    size_t *first=links, *last=first+count+1, *next=last+count+1, *stack=next+count, *owners=stack+count, *levels=owners+count;
    for (size_t i=0; i < count*6+2; i++) links[i]=SIZE_MAX;
    int error=0;
    for (size_t i=0; i < count; i++)
    {
        size_t owner=sorted[i].parent ? find_record(sorted, count, sorted[i].parent) : count;
        int duplicate=i && sorted[i-1].id==sorted[i].id;
        int missing=sorted[i].parent && owner==count;
        if (!sorted[i].id || sorted[i].parent >= sorted[i].id || duplicate || missing) { error=-EIO; break; }
        owners[i]=owner;
    }
    if (!error && parent && find_record(sorted, count, parent)==count) error=-EIO;
    if (error) { free(sorted); free(links); free(filters); return error; }
    if (format==SNAPSHOT_TSV)
        for (size_t node=parent ? find_record(sorted, count, parent) : count; node<count; node=owners[node]) stack[node]=1;
    for (size_t i=0; i<count; i++)
    {
        if (format==SNAPSHOT_TSV && owners[i]<count) owners[i]=levels[owners[i]];
        levels[i]=i;
        if (format!=SNAPSHOT_TSV) continue;
        char filter[DYN_NAME_MAX+18]; snprintf(filter, sizeof(filter), "%016" PRIx64 ":%s", sorted[i].id, sorted[i].name);
        const char *key=filter;
        if (!sorted[i].size || (hidden_count && bsearch(&key, filters, hidden_count, sizeof(*filters), compare_hidden))) levels[i]=owners[i];
    }
    // Keep the current branch last and collapse hidden/deleted ancestors without losing its forks.
    for (int pass=0; pass<(format==SNAPSHOT_TSV ? 2 : 1); pass++)
        for (size_t i=0; i<count; i++)
        {
            if (levels[i]!=i || (format==SNAPSHOT_TSV && (stack[i]==1)!=pass)) continue;
            size_t owner=owners[i];
            if (last[owner]!=SIZE_MAX) next[last[owner]]=i;
            else first[owner]=i;
            last[owner]=i;
        }
    free(filters);
    char *bytes=NULL;
    size_t length=0;
    FILE *stream=open_memstream(&bytes, &length);
    if (!stream) { free(sorted); free(links); return -errno; }
    if (format==SNAPSHOT_JSON)
    {
        fputs("{\"currentParent\":", stream); write_id(stream, parent); fputs(",\"snapshots\":[", stream);
        for (size_t i=0; i < count; i++)
        {
            if (i) fputc(',', stream);
            fputs("{\"id\":", stream); write_id(stream, sorted[i].id);
            fputs(",\"parent\":", stream); write_id(stream, sorted[i].parent);
            fputs(",\"name\":", stream); write_name(stream, sorted[i].name);
            fprintf(stream, ",\"created\":%" PRIu64 ",\"deleted\":%s,\"automatic\":%s,\"protected\":%s,\"reclaimableBytes\":", sorted[i].created,
                sorted[i].size ? "false" : "true", !strncmp(sorted[i].name, "auto-", 5) ? "true" : "false", sorted[i].protected ? "true" : "false");
            if (sorted[i].reclaimable) fprintf(stream, "%" PRIu64, *sorted[i].reclaimable);
            else fputs("null", stream);
            fputc('}', stream);
        }
        fputs("]}\n", stream);
    }
    else
    {
        if (!count && format==SNAPSHOT_TEXT) fputs("(no snapshots)\n", stream);
        size_t node=first[count], depth=0;
        while (node!=SIZE_MAX)
        {
            if (format==SNAPSHOT_TSV)
            {
                time_t created=(time_t)sorted[node].created; struct tm utc; char date[32];
                if ((uint64_t)created!=sorted[node].created || !gmtime_r(&created, &utc) || !strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S UTC", &utc)) { error=-EOVERFLOW; break; }
                if (strpbrk(sorted[node].name, "\t\r\n")) { error=-EINVAL; break; }
                size_t owner=owners[node]; levels[node]=0;
                if (owner<count) levels[node]=levels[owner]+(next[first[owner]]!=SIZE_MAX && next[node]!=SIZE_MAX);
                fprintf(stream, "%016" PRIx64 "\t%s\t%s\t%zu\t", sorted[node].id, sorted[node].name, date, levels[node]);
                if (owner<count) fprintf(stream, "%016" PRIx64, sorted[owner].id);
                fputc('\n', stream);
            }
            else
            {
                for (size_t i=1; i < depth; i++) fputs(next[stack[i]]==SIZE_MAX ? "    " : "│   ", stream);
                if (depth) fputs(next[node]==SIZE_MAX ? "└── " : "├── ", stream);
                fprintf(stream, "%s [%016" PRIx64 "]%s%s\n", sorted[node].name, sorted[node].id,
                    sorted[node].size ? "" : " (deleted)", sorted[node].id==parent ? " <- current parent" : "");
            }
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
    const char *automatic="null";
    if (info->automatic_snapshots >= 0) automatic=info->automatic_snapshots ? "true" : "false";
    const char *same="null";
    if (info->matches_parent >= 0) same=info->matches_parent ? "true" : "false";
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
        write_id(stream, info->parent); fprintf(stream, ",\"automaticSnapshots\":%s,\"currentMatchesParent\":%s}\n", automatic, same);
    }
    else
    {
        fprintf(stream, "version:%" PRIu64 "\nsize:%" PRIu64 "\nsizeMiB:%" PRIu64 "\nsplit:%" PRIu64
            "\nsplitMiB:%" PRIu64 "\nblockSize:%d\nreusableBytes:%" PRIu64 "\ncurrentParent:",
            info->version, info->size, info->size/1048576, info->split, info->split/1048576, DYN_BLOCK_SIZE, info->reusable);
        if (info->parent) fprintf(stream, "%016" PRIx64 "\n", info->parent);
        else fputs("none\n", stream);
        fprintf(stream, "automaticSnapshots:%s\n", info->automatic_snapshots < 0 ? "none" : automatic);
        fprintf(stream, "currentMatchesParent:%s\n", info->matches_parent < 0 ? "none" : same);
    }
    int error=ferror(stream) ? -EIO : 0;
    if (fclose(stream) && !error) error=-errno;
    if (error) { free(bytes); return error; }
    *result=bytes;
    return (int)length;
}
