// Build filesystem images using the exact LittleFS source embedded in the firmware.
#include "lfs.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static unsigned char* flash;
static int read_block(const struct lfs_config* c,lfs_block_t block,lfs_off_t off,void* out,lfs_size_t n) {
    memcpy(out,flash+block*c->block_size+off,n); return 0;
}
static int program_block(const struct lfs_config* c,lfs_block_t block,lfs_off_t off,const void* data,lfs_size_t n) {
    unsigned char* destination=flash+block*c->block_size+off;
    const unsigned char* source=data;
    for(lfs_size_t i=0;i<n;++i) destination[i]&=source[i];
    return 0;
}
static int erase_block(const struct lfs_config* c,lfs_block_t block) {memset(flash+block*c->block_size,255,c->block_size);return 0;}
static int sync_blocks(const struct lfs_config* c) {(void)c;return 0;}
static void require(int condition,const char* what) {if(!condition){fprintf(stderr,"%s\n",what);exit(1);}}
static void copy_directory(lfs_t* fs,const char* source,const char* relative) {
    char directory[1024]; snprintf(directory,sizeof(directory),"%s/%s",source,relative);
    DIR* dir=opendir(directory); require(dir!=NULL,"Cannot read source directory"); struct dirent* entry;
    while((entry=readdir(dir))) {
        if(entry->d_name[0]=='.') continue;
        char local[2048],path[1024]; snprintf(local,sizeof(local),"%s/%s",directory,entry->d_name);
        snprintf(path,sizeof(path),"%s/%s",relative,entry->d_name); struct stat info;
        require(stat(local,&info)==0,"Cannot stat input");
        if(S_ISDIR(info.st_mode)) {require(lfs_mkdir(fs,path)==0,"Cannot create directory");copy_directory(fs,source,path);}
        else if(S_ISREG(info.st_mode)) {
            FILE* input=fopen(local,"rb");require(input!=NULL,"Cannot open input");
            lfs_file_t file; require(lfs_file_open(fs,&file,path,LFS_O_WRONLY|LFS_O_CREAT|LFS_O_TRUNC)==0,"Cannot create file");
            unsigned char buffer[4096]; size_t n;
            while((n=fread(buffer,1,sizeof(buffer),input))) require(lfs_file_write(fs,&file,buffer,n)==(lfs_ssize_t)n,"Filesystem full");
            require(!ferror(input),"Input read failed"); fclose(input);require(lfs_file_close(fs,&file)==0,"File commit failed");
        }
    }
    closedir(dir);
}
static void capacity_test(lfs_t* fs,const struct lfs_config* cfg) {
    unsigned char record[112]; memset(record,0x5a,sizeof(record));
    unsigned char header[128]={0};
    for(int segment=0;segment<85;++segment) {
        int bytes=segment<69?112:96;
        int rows=segment==68?32:64;
        char path[32];snprintf(path,sizeof(path),"/%c%08x.bin",segment<69?'m':'e',segment+1);
        lfs_file_t file;require(lfs_file_open(fs,&file,path,LFS_O_CREAT|LFS_O_WRONLY)==0,"Segment open failed");
        require(lfs_file_write(fs,&file,header,128)==128,"Segment header failed");
        for(int row=0;row<rows;++row) {require(lfs_file_write(fs,&file,record,bytes)==bytes,"Record write failed");require(lfs_file_sync(fs,&file)==0,"Record fsync failed");}
        require(lfs_file_close(fs,&file)==0,"Segment close failed");
        require(lfs_file_open(fs,&file,path,LFS_O_RDONLY)==0,"Segment reopen failed");
        require(lfs_file_size(fs,&file)==128+rows*bytes,"Unexpected segment length");
        lfs_file_close(fs,&file);
    }
    const char* names[]={"/configs.bin","/rotation-workspace"};
    unsigned char block[4096]={0};
    for(int n=0;n<2;++n) {
        lfs_file_t file;require(lfs_file_open(fs,&file,names[n],LFS_O_CREAT|LFS_O_WRONLY)==0,"Metadata open failed");
        for(int i=0;i<16;++i) require(lfs_file_write(fs,&file,block,sizeof(block))==sizeof(block),"Metadata capacity failed");
        require(lfs_file_close(fs,&file)==0,"Metadata commit failed");
    }
    long used=(long)lfs_fs_size(fs)*cfg->block_size;
    printf("Capacity passed: 4384 climate minutes + 1024 events + 64 KiB metadata + 64 KiB rotation; allocated %ld / %u bytes\n",used,cfg->block_size*cfg->block_count);
}
int main(int argc,char** argv) {
    const char* source=NULL;const char* output=NULL;unsigned size=1048576,block_size=4096;int capacity=0;
    for(int i=1;i<argc;++i) {
        if(!strcmp(argv[i],"--capacity-test")) capacity=1;
        else if(!strcmp(argv[i],"-c")&&i+1<argc) source=argv[++i];
        else if(!strcmp(argv[i],"-s")&&i+1<argc) size=strtoul(argv[++i],NULL,0);
        else if(!strcmp(argv[i],"-b")&&i+1<argc) block_size=strtoul(argv[++i],NULL,0);
        else if(!strcmp(argv[i],"-p")&&i+1<argc) ++i;
        else output=argv[i];
    }
    require(block_size==4096&&size>=4096*8&&size%4096==0,"Invalid filesystem geometry");
    require(capacity||(source&&output),"Usage: make_journal -c data -s bytes [-b 4096] image.bin");
    flash=malloc(size);require(flash!=NULL,"Allocation failed");memset(flash,255,size);
    struct lfs_config cfg={0};cfg.read=read_block;cfg.prog=program_block;cfg.erase=erase_block;cfg.sync=sync_blocks;
    cfg.read_size=128;cfg.prog_size=128;cfg.block_size=4096;cfg.block_count=size/4096;cfg.block_cycles=512;cfg.cache_size=512;cfg.lookahead_size=128;
    lfs_t fs;require(lfs_format(&fs,&cfg)==0,"Filesystem format failed");require(lfs_mount(&fs,&cfg)==0,"Filesystem mount failed");
    if(capacity) capacity_test(&fs,&cfg); else copy_directory(&fs,source,"");
    require(lfs_unmount(&fs)==0,"Filesystem unmount failed");
    if(output) {FILE* f=fopen(output,"wb");require(f!=NULL,"Cannot create image");require(fwrite(flash,1,size,f)==size,"Image write failed");require(fclose(f)==0,"Image close failed");}
    free(flash);return 0;
}
