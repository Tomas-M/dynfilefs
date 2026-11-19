#define _GNU_SOURCE
#include "storage.h"
#include "format.h"
#include "durable.h"
#include <errno.h>
#include <endian.h>
#include <sys/stat.h>
#include <assert.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int armed, event, cut;
ssize_t __real_pwrite(int, const void *, size_t, off_t);
ssize_t __real_pwrite64(int, const void *, size_t, off_t);
int __real_fsync(int);
int __real_unlinkat(int, const char *, int);
int __real_close(int);

// Stop before and after backing operations; the parent copies persisted bytes without flushing.
static void checkpoint(void)
{
    if (!armed || ++event!=cut) return;
    printf("CUT %d\n",event); fflush(stdout); raise(SIGSTOP);
}

// Split writes at sector boundaries to expose torn intent and canonical records too.
ssize_t __wrap_pwrite(int fd, const void *bytes, size_t count, off_t offset)
{
    checkpoint();
    ssize_t result=__real_pwrite(fd,bytes,count>512 ? 512 : count,offset);
    checkpoint(); return result;
}

// Glibc large-file builds use this symbol instead of ordinary pwrite.
ssize_t __wrap_pwrite64(int fd, const void *bytes, size_t count, off_t offset)
{
    checkpoint();
    ssize_t result=__real_pwrite64(fd,bytes,count>512 ? 512 : count,offset);
    checkpoint(); return result;
}

// Include both sides of data, inode and parent-directory synchronization.
int __wrap_fsync(int fd)
{
    checkpoint(); int result=__real_fsync(fd); checkpoint(); return result;
}

// Namespace removal and last-reference release must be ordered even under concurrent writeback.
int __wrap_unlinkat(int directory, const char *name, int flags)
{
    checkpoint(); int result=__real_unlinkat(directory,name,flags); checkpoint(); return result;
}

// FAT releases removed allocations when the last descriptor closes.
int __wrap_close(int fd)
{
    checkpoint(); int result=__real_close(fd); checkpoint(); return result;
}

// Resolve stable identities independently from their display names.
static uint64_t point(struct storage *store, const char *name)
{
    char *json=NULL,marker[128]; uint64_t result=0;
    assert(storage_snapshot_report(store,1,&json)>0);
    snprintf(marker,sizeof(marker),"\"name\":\"%s\"",name);
    for (char *entry=strstr(json,"{\"id\":\"");entry;entry=strstr(entry+1,"{\"id\":\""))
    {
        char *label=strstr(entry,marker),*live=strstr(entry,"\"deleted\":false"),*end=strchr(entry,'}');
        if (label && label<end && live && live<end) { assert(sscanf(entry+7,"%16" SCNx64,&result)==1); break; }
    }
    free(json); return result;
}

// Check data against independently chosen byte patterns, including immutable saved views.
static void verify(struct storage *store, int directory, const char *operation, int acknowledged)
{
    assert(!durable_recover(directory,".persistence"));
    unsigned char *policy=NULL; size_t length=0;
    int result=durable_read(directory,".persistence","policy.txt",&policy,&length);
    if (!strcmp(operation,"policy-clear") && acknowledged) assert(result==-ENOENT);
    else if (result!=-ENOENT)
    {
        assert(!result);
        while (length && policy[length-1]=='\n') length--;
        int old=length==26 && !memcmp(policy,"abcdefghijklmnopqrstuvwxyz",26);
        int next=length==3 && !memcmp(policy,"new",3);
        assert(old || next);
        if (!strcmp(operation,"policy-write") && acknowledged) assert(next);
    }
    else assert(!strcmp(operation,"policy-clear"));
    free(policy);
    struct stat marker; assert(!fstatat(directory,"resume.txt",&marker,AT_SYMLINK_NOFOLLOW));
    assert(marker.st_mtim.tv_sec==2100000000 || marker.st_mtim.tv_sec==2100000100);
    if (!strcmp(operation,"policy-mark") && acknowledged) assert(marker.st_mtim.tv_sec==2100000100);
    unsigned char bytes[8192]; struct storage_snapshot *view=NULL;
    assert(storage_read(store,bytes,sizeof(bytes),0)==sizeof(bytes));
    for (int i=0;i<4096;i++) assert(bytes[i]=='A');
    int current=bytes[4096];
    assert(current=='D' || current=='E');
    for (int i=4096;i<8192;i++) assert(bytes[i]==current);
    assert(!storage_snapshot_open(store,point(store,"Base"),&view));
    assert(storage_snapshot_read(store,view,bytes,sizeof(bytes),0)==sizeof(bytes));
    for (int i=0;i<8192;i++) assert(bytes[i]==(i<4096 ? 'A' : 'B'));
    storage_snapshot_close(store,view);
    uint64_t original=point(store,"Rename me"),renamed=point(store,"Renamed");
    assert(!!original!=!!renamed);
    if (!strcmp(operation,"rename") && acknowledged) assert(renamed && !original);
    if (!strcmp(operation,"delete") && acknowledged) assert(!point(store,"Delete me"));
    uint64_t created=point(store,"New point");
    if (!strcmp(operation,"create") && acknowledged) assert(created);
    if (created)
    {
        assert(!storage_snapshot_open(store,created,&view));
        assert(storage_snapshot_read(store,view,bytes,sizeof(bytes),0)==sizeof(bytes));
        for (int i=0;i<8192;i++) assert(bytes[i]==(i<4096 ? 'A' : 'D'));
        storage_snapshot_close(store,view);
    }
    if (acknowledged && (!strcmp(operation,"checkpoint") || !strcmp(operation,"finish"))) assert(current=='E');
}

// An independently encoded released format-500 fixture keeps one sparse legacy data block.
static void legacy_fixture(const char *path)
{
    unsigned char bytes[12288]={0}; uint64_t header[]={htole64(500),htole64(2*1024*1024),htole64(2*1024*1024)}, offset=htole64(8192);
    memcpy(bytes+2048,header,sizeof(header)); memcpy(bytes+4096+13*8,&offset,8); memset(bytes+8192,'A',4096);
    int fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0600); assert(fd>=0 && !write_at(fd,bytes,4096,0) && !fsync(fd)); close(fd);
    char part[4096]; snprintf(part,sizeof(part),"%s.0",path);
    fd=open(part,O_WRONLY|O_CREAT|O_EXCL,0600); assert(fd>=0 && !write_at(fd,bytes,sizeof(bytes),0) && !fsync(fd)); close(fd);
}

int main(int argc, char **argv)
{
    assert(argc>=3); struct storage *store=NULL;
    if (!strcmp(argv[1],"legacy")) { legacy_fixture(argv[2]); return 0; }
    if (argc==5 && !strcmp(argv[3],"convert"))
    {
        if (!strcmp(argv[1],"run")) { cut=atoi(argv[4]); armed=1; }
        int opened=storage_open(&store,argv[2],0,0,0,NULL,NULL,0, 1);
        if (opened) fprintf(stderr,"Conversion open failed: %s (%d)\n",strerror(-opened),opened);
        assert(!opened);
        if (!strcmp(argv[1],"verify"))
        {
            unsigned char *bytes=malloc(2*1024*1024); assert(bytes && storage_size(store)==2*1024*1024);
            assert(storage_read(store,bytes,2*1024*1024,0)==2*1024*1024);
            for (int i=0;i<2*1024*1024;i++) assert(bytes[i]==(i>=13*4096 && i<14*4096 ? 'A' : 0));
            free(bytes); assert(!storage_finish(store)); storage_close(store); return 0;
        }
        printf("ACK %d\n",event); fflush(stdout); raise(SIGSTOP); _exit(0);
    }
    assert(!storage_open(&store,argv[2],16000ULL*1024*1024,0,1024ULL*1024*1024,NULL,NULL,0, 1));
    int directory=-1; char *prefix=NULL; assert(!storage_path(argv[2],&directory,&prefix)); free(prefix);
    unsigned char bytes[8192]; memset(bytes,'A',4096); memset(bytes+4096,'B',4096);
    if (!strcmp(argv[1],"prepare"))
    {
        assert(!durable_write(directory,".persistence","policy.txt","abcdefghijklmnopqrstuvwxyz\n",27,'\n',-1,NULL));
        assert(!durable_write(directory,".persistence","resume.txt",NULL,0,'\n',2100000000,NULL));
        assert(storage_write(store,bytes,sizeof(bytes),0)==sizeof(bytes));
        assert(!storage_snapshot_create(store,"Base"));
        assert(!storage_snapshot_create(store,"Rename me"));
        assert(!storage_snapshot_create(store,"Delete me"));
        assert(!storage_finish(store)); storage_close(store); return 0;
    }
    assert(argc==5);
    if (!strcmp(argv[1],"verify"))
    {
        verify(store,directory,argv[3],atoi(argv[4]));
        assert(!storage_finish(store)); storage_close(store); return 0;
    }
    cut=atoi(argv[4]);
    memset(bytes,'D',4096); assert(storage_write(store,bytes,4096,4096)==4096); assert(!storage_sync(store));
    uint64_t id=point(store,"Rename me"); armed=1;
    if (!strcmp(argv[3],"create")) assert(!storage_snapshot_create(store,"New point"));
    else if (!strcmp(argv[3],"rename")) assert(!storage_snapshot_rename(store,id,"Renamed"));
    else if (!strcmp(argv[3],"delete")) assert(!storage_snapshot_delete(store,"Delete me"));
    else if (!strcmp(argv[3],"policy-write")) assert(!durable_write(directory,".persistence","policy.txt","new\n",4,'\n',-1,NULL));
    else if (!strcmp(argv[3],"policy-clear")) assert(!durable_remove(directory,".persistence","policy.txt"));
    else if (!strcmp(argv[3],"policy-mark")) assert(!durable_write(directory,".persistence","resume.txt",NULL,0,'\n',2100000100,NULL));
    else
    {
        memset(bytes,'E',4096); assert(storage_write(store,bytes,4096,4096)==4096);
        if (!strcmp(argv[3],"checkpoint")) assert(!storage_sync(store));
        else { assert(!strcmp(argv[3],"finish")); assert(!storage_finish(store)); }
    }
    printf("ACK %d\n",event); fflush(stdout); raise(SIGSTOP);
    _exit(0);
}
