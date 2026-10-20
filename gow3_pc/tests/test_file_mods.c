/* Exercise actual guest file operations on a linked overlay and writable saves. */
#define _GNU_SOURCE
#include <unistd.h>
#include <pthread.h>
#include <assert.h>
#include <stdint.h>
static pthread_barrier_t read_ready, read_release;
static int pause_read;
static int parallel_pread;
#ifdef _WIN32
static ssize_t audit_pread(int fd,void *buffer,size_t size,int64_t offset);
#endif
static ssize_t audit_read(int fd,void *buffer,unsigned size);
#define read audit_read
#ifdef _WIN32
#include "compat_win.h"
#undef pread
#define pread audit_pread
#endif
#include "../src/runtime_file.c"
#undef read
static void pause_reader(void) {
    if (__atomic_exchange_n(&pause_read,0,__ATOMIC_SEQ_CST)) {
        pthread_barrier_wait(&read_ready); pthread_barrier_wait(&read_release);
    }
}
static ssize_t audit_read(int fd,void *buffer,unsigned size) {
    pause_reader();
    return read(fd,buffer,size);
}
#ifdef _WIN32
#undef pread
static ssize_t audit_pread(int fd,void *buffer,size_t size,int64_t offset) {
    pause_reader();
    if (parallel_pread) pthread_barrier_wait(&read_ready);
    return compat_pread(fd,buffer,size,offset);
}
#endif
static void *closing_reader(void *p) {
    char content[7]={0};
    assert(do_read(*(int *)p,content,6)==6 && !strcmp(content,"modded"));
    return NULL;
}
static void close_race(void) {
    /* 20 Oct 2026 audit: pause after acquiring the host fd, then close and reopen another file. */
    int fd=(int)do_open("/app0/asset.dcx",0,0); assert(fd>=3);
    assert(!pthread_barrier_init(&read_ready,NULL,2) && !pthread_barrier_init(&read_release,NULL,2));
    pause_read=1;
    pthread_t reader; assert(!pthread_create(&reader,NULL,closing_reader,&fd));
    pthread_barrier_wait(&read_ready);
    assert(!do_close(fd));
    int other=(int)do_open("/data/other",0x202,0644); assert(other>=3);
    assert(do_write(other,"other!",6)==6 && do_lseek(other,0,0)==0);
    pthread_barrier_wait(&read_release);
    assert(!pthread_join(reader,NULL));
    assert(!do_close(other) && !path_op("/data/other",2,0));
    assert(!pthread_barrier_destroy(&read_ready) && !pthread_barrier_destroy(&read_release));
}
static void *position_reader(void *p) {
    int fd=*(int *)p;
    char content[6];
    for (int i=0;i<10000;++i) {
        assert(do_pread(fd,content,6,0)==6 && !memcmp(content,"modded",6));
        assert(do_pwrite(fd,"modded",6,0)==6);
    }
    return NULL;
}
static void position_race(void) {
    /* 20 Oct 2026 audit: concurrent positioned reads must preserve the sequential offset. */
    int fd=(int)do_open("/data/position",0x202,0644); assert(fd>=3);
    assert(do_write(fd,"modded",6)==6);
    assert(do_lseek(fd,2,0)==2);
    pthread_t a,b;
    assert(!pthread_create(&a,NULL,position_reader,&fd) && !pthread_create(&b,NULL,position_reader,&fd));
    for (int i=0;i<10000;++i) {
        char c=0;
        assert(do_lseek(fd,2,0)==2 && do_read(fd,&c,1)==1 && c=='d');
    }
    assert(!pthread_join(a,NULL) && !pthread_join(b,NULL));
    assert(do_lseek(fd,0,1)==3 && !do_close(fd));
    assert(!path_op("/data/position",2,0));
}
typedef struct { int fd; int64_t offset; const char *expected; } PreadTask;
static void *parallel_reader(void *p) {
    PreadTask *task=p;
    for (int i=0;i<10000;++i) {
        char content[3];
        assert(do_pread(task->fd,content,3,task->offset)==3 && !memcmp(content,task->expected,3));
    }
    return NULL;
}
static void pread_race(void) {
    /* 20 Oct audit: both reads must reach positioned I/O on one File without a shared I/O lock. */
    int fd=(int)do_open("/app0/asset.dcx",0,0); assert(fd>=3);
    assert(!pthread_barrier_init(&read_ready,NULL,2));
    PreadTask first={fd,0,"mod"}, second={fd,3,"ded"};
    pthread_t a,b;
    parallel_pread=1;
    assert(!pthread_create(&a,NULL,parallel_reader,&first) && !pthread_create(&b,NULL,parallel_reader,&second));
    assert(!pthread_join(a,NULL) && !pthread_join(b,NULL));
    parallel_pread=0;
    assert(do_lseek(fd,0,1)==0 && !do_close(fd));
    assert(!pthread_barrier_destroy(&read_ready));
}
static int32_t guest_errno;
int32_t *runtime_errno(void) { return &guest_errno; }
int32_t runtime_guest_errno(int e) { return e; }
uintptr_t runtime_lookup(const RuntimeExport *table,size_t n,const char *name) {
    for (size_t i=0;i<n;++i) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}
int main(int argc,char **argv) {
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
#endif
    char root[]="gow3-mod-files-XXXXXX";
    assert(mkdtemp(root));
    char game[512],user[512],source[512],link[512];
    snprintf(game,sizeof(game),"%s/game",root);
    snprintf(user,sizeof(user),"%s/user",root);
    snprintf(source,sizeof(source),"%s/mod.dcx",root);
    snprintf(link,sizeof(link),"%s/game/asset.dcx",root);
    assert(!mkdir(game,0755));
    FILE *f=fopen(source,"w"); assert(f); assert(fputs("modded",f)>=0); assert(!fclose(f));
#ifdef _WIN32
    /* Windows has no POSIX symlink API; hard links exercise the same linked asset reads. */
    assert(CreateHardLinkA(link,source,NULL));
#else
    char absolute[PATH_MAX]; assert(realpath(source,absolute));
    assert(!symlink(absolute,link));
#endif
    runtime_file_configure(game,user);
    if (argc==1 || !strcmp(argv[1],"--close-race")) close_race();
    if (argc==1 || !strcmp(argv[1],"--position-race")) position_race();
    if (argc==1 || !strcmp(argv[1],"--pread-race")) pread_race();
    /* 20 Oct 2026 audit: Windows separators must not let guest writes escape a mount. */
    const char *escapes[]={"/data/..\\outside", "/data/sub\\../outside", "/data/sub\\..\\outside", "/data/..\\..\\..\\Users\\x\\file"};
    for (size_t i=0;i<sizeof(escapes)/sizeof(*escapes);++i) {
        char host[1024];
        assert(resolve(escapes[i],host,sizeof(host),0)==EACCES);
        assert(do_open(escapes[i],0x202,0644)==-EACCES);
    }
    for (int i=0;i<3;++i) {
        const char *path=i==0 ? "/app0/asset.dcx" : i==1 ? "/hostapp/asset.dcx" : "asset.dcx";
        int fd=(int)do_open(path,0,0); assert(fd>=3);
        char content[8]={0}; assert(do_read(fd,content,6)==6 && !strcmp(content,"modded"));
        GuestStat info; assert(!do_stat(path,&info) && info.size==6);
        assert(!do_close(fd));
        assert(do_open(path,2,0)==-EROFS);
        assert(do_open(path,0x400,0)==-EROFS);
        assert(do_truncate(path,0)==-EROFS);
        assert(path_op(path,2,0)==-EROFS);
        assert(do_rename(path,"/data/moved")==-EROFS);
    }
    int dir=(int)do_open("/app0",0x20000,0); assert(dir>=3);
    char entries[1024]; int64_t count=do_getdents(dir,entries,sizeof(entries),NULL); assert(count>0);
    int found=0;
    for (int64_t p=0;p<count;) {
        uint16_t length; memcpy(&length,entries+p+4,2);
        assert(length);
        if (!strcmp(entries+p+8,"asset.dcx")) { assert(entries[p+6]==8); found=1; }
        p+=length;
    }
    assert(found && !do_close(dir));
    int save=(int)do_open("/data/test-save",0x202,0644); assert(save>=3);
    assert(do_write(save,"save",4)==4 && !do_close(save));
    save=(int)do_open("/data/test-save",0xa,0); assert(save>=3);
    assert(do_lseek(save,0,0)==0 && do_write(save,"!",1)==1);
    assert(do_lseek(save,0,1)==5 && do_lseek(save,-2,2)==3);
#ifdef _WIN32
    assert(do_pwrite(save,"a",1,1)==1 && do_lseek(save,0,1)==3);
#endif
    char ending[2]; assert(do_read(save,ending,2)==2 && !memcmp(ending,"e!",2));
    assert(do_lseek(save,-6,2)==-EINVAL && do_lseek(save,0,1)==5 && !do_close(save));
    assert(!path_op("/data/test-save",2,0));
    assert(!unlink(link) && !unlink(source) && !rmdir(game));
    const char *dirs[]={"temp0","download0","data"};
    for (int i=0;i<3;++i) { char p[1024]; snprintf(p,sizeof(p),"%s/%s",user,dirs[i]); assert(!rmdir(p)); }
    assert(!rmdir(user) && !rmdir(root));
    puts("Guest mod files: reads, stat, merged listing, readonly assets and writable saves PASS");
}
