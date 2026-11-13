#ifndef DYNFILEFS_REPORT_H
#define DYNFILEFS_REPORT_H

#include "storage.h"

struct snapshot_record
{
    uint64_t id, parent, created, size;
    const char *name;
};

int snapshot_report(const struct snapshot_record *records, size_t count, uint64_t parent, int json, char **result);
int storage_info_report(const struct storage_info *info, int json, char **result);

#endif
