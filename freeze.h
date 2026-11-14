#ifndef DYNFILEFS_FREEZE_H
#define DYNFILEFS_FREEZE_H

struct filesystem_freeze;

// Freeze direct loop-mounted ext4/XFS consumers, or an image without loop consumers.
int filesystem_freeze(int directory, const char *disk_path, struct filesystem_freeze **result);
// Resume owned freezes; a helper also performs this cleanup if the caller dies.
int filesystem_thaw(struct filesystem_freeze *state);

#endif
