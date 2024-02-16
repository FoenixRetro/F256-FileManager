// This file implements read(2) and write(2) along with a minimal console
// driver for reads from stdin and writes to stdout -- enough to enable
// cc65's stdio functions. It really should be written in assembler for
// speed (mostly for scrolling), but this will at least give folks a start.

#include <stddef.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>

#include "api.h"
#include "dirent.h"  // Users are expected to "-I ." to get the local copy.

#include "f256.h" // need for F1 key values

#define VECTOR(member) (size_t) (&((struct call*) 0xff00)->member)
#define EVENT(member)  (size_t) (&((struct events*) 0)->member)
#define CALL(fn) (unsigned char) ( \
                   asm("jsr %w", VECTOR(fn)), \
                   asm("stz %v", error), \
                   asm("ror %v", error), \
                   __A__)


#pragma bss-name (push, "KERNEL_ARGS")
struct call_args args; // in gadget's version of f256 lib, this is allocated and initialized with &args in crt0. 
#pragma bss-name (pop)

#pragma bss-name (push, "ZEROPAGE")
struct event_t event; // in gadget's version of f256 lib, this is allocated and initialized with &event in crt0. 
char error;
#pragma bss-name (pop)


#define MAX_DRIVES 8

// Just hard-coded for now.
#define MAX_ROW 60
#define MAX_COL 80

static char row = 0;
static char col = 0;
static char *line = (char*) 0xc000;

 
void
kernel_init(void)
{
    args.events.event = &event;
}

static void
cls()
{
    int i;
    char *vram = (char*)0xc000;
    
    asm("lda #$02");
    asm("sta $01");  
    
    for (i = 0; i < 80*60; i++) {
        *vram++ = 32;
    }
    
    row = col = 0;
    line = (char*)0xc000;
    
    asm("stz $1"); asm("lda #9"); asm("sta $d010");
    (__A__ = row, asm("sta $d016"), asm("stz $d017"));
    (__A__ = col, asm("sta $d014"), asm("stz $d015"));
    asm("lda #'_'"); asm("sta $d012");
    asm("stz $d011");
}

void
scroll()
{
    int i;
    char *vram = (char*)0xc000;
    
    asm("lda #$02");
    asm("sta $01");  
    
    for (i = 0; i < 80*59; i++) {
        vram[i] = vram[i+80];
    }
    vram += i;
    for (i = 0; i < 80; i++) {
        *vram++ = 32;
    }
}

static void 
out(char c)
{
    switch (c) {
    case 12: 
        cls();
        break;
    default:
        asm("lda #2");
        asm("sta $01");    
        line[col] = c;
        col++;
        if (col != MAX_COL) {
            break;
        }
    case 10:
    case 13:
        col = 0;
        row++;
        if (row == MAX_ROW) {
            scroll();
            row--;
            break;
        }
        line += 80;
        break;
    }
    
    asm("stz $01");
    (__A__ = row, asm("sta $d016"));
    (__A__ = col, asm("sta $d014"));
}  
    
char
GETIN()
{
    while (1) {
        
        CALL(NextEvent);
        if (error) {
            asm("jsr %w", VECTOR(Yield));
            continue;
        }
        
        if (event.type != EVENT(key.PRESSED)) {
            continue;
        }
        
        if (event.key.flags) {
        	// if a function key, return raw code.
        	if (event.key.raw >= CH_F1 && event.key.raw <= CH_F8)
        	{
        		return event.key.raw;
        	}
            continue;  // Meta key.
        }
        
        return event.key.ascii;
    }
}

static const char *
path_without_drive(const char *path, char *drive)
{
    *drive = 0;
    
    if (strlen(path) < 2) {
        return path;
    }
    
    if (path[1] != ':') {
        return path;
    }
    
    if ((*path >= '0') && (*path <= '7')) {
        *drive = *path - '0';
    }
        
    return (path + 2);
}

int
open(const char *fname, int mode, ...)
{
    int ret = 0;
    char drive;
    
    fname = path_without_drive(fname, &drive);
    
    args.common.buf = (uint8_t*) fname;
    args.common.buflen = strlen(fname);
    args.file.open.drive = drive;
    if (mode == 1) {
        mode = 0;
    } else {
        mode = 1;
    }
    args.file.open.mode = mode;
    ret = CALL(File.Open);
    if (error) {
        return -1;
    }
    
    for(;;) {
        event.type = 0;
        asm("jsr %w", VECTOR(NextEvent));
        switch (event.type) {
        case EVENT(file.OPENED):
            return ret;
        case EVENT(file.NOT_FOUND):
        case EVENT(file.ERROR):
            return -1;
        default:
        	continue;
        }
    }
}

static int 
kernel_read(int fd, void *buf, uint16_t nbytes)
{
    
    if (fd == 0) {
        // stdin
        *(char*)buf = GETIN();
        return 1;
    }
    
    if (nbytes > 255) {
        nbytes = 255;
    }
    
    args.file.read.stream = fd;
    args.file.read.buflen = nbytes;
    CALL(File.Read);
    if (error) {
        return -1;
    }

    for(;;) {
        event.type = 0;
        asm("jsr %w", VECTOR(NextEvent));
        switch (event.type) {
        case EVENT(file.DATA):
            args.common.buf = buf;
            args.common.buflen = event.file.data.delivered;
            asm("jsr %w", VECTOR(ReadData));
            if (!event.file.data.delivered) {
                return 256;
            }
            return event.file.data.delivered;
        case EVENT(file.EOFx):
            return 0;
        case EVENT(file.ERROR):
            return -1;
        default: 
        	continue;
        }
    }
}

int 
read(int fd, void *buf, uint16_t nbytes)
{
    char *data = buf;
    int  gathered = 0;
    
    // fread should be doing this, but it isn't, so we're doing it.
    while (gathered < nbytes) {
        int returned = kernel_read(fd, data + gathered, nbytes - gathered);
        if (returned <= 0) {
            break;
        }
        gathered += returned;
    }
    
    return gathered;
}

static int
kernel_write(uint8_t fd, void *buf, uint8_t nbytes)
{
    args.file.read.stream = fd;
    args.common.buf = buf;
    args.common.buflen = nbytes;
    CALL(File.Write);
    if (error) {
        return -1;
    }

    for(;;) {
        event.type = 0;
        asm("jsr %w", VECTOR(NextEvent));
        if (event.type == EVENT(file.WROTE)) {
            return event.file.data.delivered;
        }
        if (event.type == EVENT(file.ERROR)) {
            return -1;
        }
    }
}

int 
write(int fd, const void *buf, uint16_t nbytes)
{
    uint8_t  *data = buf;
    int      total = 0;
    
    uint8_t  writing;
    int      written;
    
    if (fd == 1) {
        int i;
        char *text = (char*) buf;
        for (i = 0; i < nbytes; i++) {
            out(text[i]);
        }
        return i;
    }
    
    while (nbytes) {
        
        if (nbytes > 254) {
            writing = 254;
        } else {
            writing = nbytes;
        }
        
        written = kernel_write(fd, data+total, writing);
        if (written <= 0) {
            return -1;
        }
        
        total += written;
        nbytes -= written;
    }
        
    return total;
}

int
close(int fd)
{
    args.file.close.stream = fd;
    asm("jsr %w", VECTOR(File.Close));
    for(;;) {
        event.type = 0;
        asm("jsr %w", VECTOR(NextEvent));
        switch (event.type) {
        case EVENT(file.CLOSED):
                return 0;
        case EVENT(file.ERROR):
                return -1;
        default: continue;
        }
    }
    
    return 0;
}


   
////////////////////////////////////////
// dirent

static char dir_stream[MAX_DRIVES];

DIR* __fastcall__ 
opendir (const char* name)
{
    char drive, stream;

// out(name[0]);
// out(name[1]);
// out(name[2]);
    
    name = path_without_drive(name, &drive);
//out(48+drive);
// out(48+(uint8_t)strlen(name));
   
    if (dir_stream[drive]) {
//out(64);
        return NULL;  // Only one at a time.
    }
    
    args.directory.open.drive = drive;
    args.common.buf = name;
    args.common.buflen = strlen(name);
//out(48+(uint8_t)args.common.buflen);
    stream = CALL(Directory.Open);
    if (error) {
//out(66); // B
        return NULL;
    }
//out(67); // C
    
    for(;;) {
        event.type = 0;
        asm("jsr %w", VECTOR(NextEvent));
        if (event.type == EVENT(directory.OPENED)) {
//out(68); // D
            break;
        }
        if (event.type == EVENT(directory.ERROR)) {
//out(69); // E
            return NULL;
        }
    }
    
    dir_stream[drive] = stream;
//out(70); // F
    return (DIR*) &dir_stream[drive];
}

struct dirent* __fastcall__ 
readdir(DIR* dir)
{
    static struct dirent dirent;
    
    if (!dir) {
        return NULL;
    }
    
    args.directory.read.stream = *(char*)dir;
    CALL(Directory.Read);
    if (error) {
        return NULL;
    }
    
    for(;;) {
        
        unsigned len;
        
        event.type = 0;
        asm("jsr %w", VECTOR(NextEvent));
        
        switch (event.type) {
        
        case EVENT(directory.VOLUME):
            
            dirent.d_blocks = 0;
            dirent.d_type = 2;
            break;
            
        case EVENT(directory.FILE): 
            
            args.common.buf = &dirent.d_blocks;
            args.common.buflen = sizeof(dirent.d_blocks);
            CALL(ReadExt);
            dirent.d_type = (dirent.d_blocks == 0);
            break;
                
        case EVENT(directory.FREE):
            // dirent doesn't care about these types of records.
            args.directory.read.stream = *(char*)dir;
            CALL(Directory.Read);
            if (!error) {
                continue;
            }
            // Fall through.
        
        case EVENT(directory.EOFx):
        case EVENT(directory.ERROR):
            return NULL;
            
        default: continue;
        }
        
        // Copy the name.
        len = event.directory.file.len;
        if (len >= sizeof(dirent.d_name)) {
            len = sizeof(dirent.d_name) - 1;
        }
            
        if (len > 0) {
            args.common.buf = &dirent.d_name;
            args.common.buflen = len;
            CALL(ReadData);
        }
        dirent.d_name[len] = '\0';
                
        return &dirent;
    }
}
    
    
int __fastcall__ 
closedir (DIR* dir)
{
    if (!dir) {
        return -1;
    }
    
    for(;;) {
        if (*(char*)dir) {
            args.directory.close.stream = *(char*)dir;
            CALL(Directory.Close);
            if (!error) {
                *(char*)dir = 0;
            }
        }
        event.type = 0;
        asm("jsr %w", VECTOR(NextEvent));
        if (event.type == EVENT(directory.CLOSED)) {
            *(char*)dir = 0;
            return 0;
        }
    }
}

int __fastcall__ 
remove(const char* name)
{
    char drive, stream;
    
    name = path_without_drive(name, &drive);
    args.file.delete.drive = drive;
    args.common.buf = name;
    args.common.buflen = strlen(name);
    stream = CALL(File.Delete);
    if (error) {
        return -1;
    }
    
    for(;;) {
        event.type = 0;
        asm("jsr %w", VECTOR(NextEvent));
        if (event.type == EVENT(file.DELETED)) {
            break;
        }
        if (event.type == EVENT(file.ERROR)) {
            return -1;
        }
    }
    
    return 0;
}

int __fastcall__ 
rename(const char* name, const char *to)
{
    char drive, stream, dest;
    
    name = path_without_drive(name, &drive);
    to = path_without_drive(to, &dest);
    if (dest != drive) {    
        // rename across drives is not supported.
        return -1;
    }
    
    args.file.delete.drive = drive;
    args.common.buf = name;
    args.common.buflen = strlen(name);
    args.common.ext = to;
    args.common.extlen = strlen(to);
    stream = CALL(File.Rename);
    if (error) {
        return -1;
    }
    
    for(;;) {
        event.type = 0;
        asm("jsr %w", VECTOR(NextEvent));
        if (event.type == EVENT(file.RENAMED)) {
            break;
        }
        if (event.type == EVENT(file.ERROR)) {
            return -1;
        }
    }
    
    return 0;
}


// wrapper to mkfs
//   pass the name you want for the formatted disk/SD card in name, and the drive number (0-2) in the drive param.
//   do NOT prepend the path onto name. 
// return negative number on any error
int __fastcall__
mkfs(const char* name, const char drive)
{
	char stream;
	
	args.file.delete.drive = drive;
    args.common.buf = name;
    args.common.buflen = strlen(name);
    stream = CALL(FileSystem.MkFS);
    if (error) {
        return -2;
    }
    
    for(;;) {
        event.type = 0;
        asm("jsr %w", VECTOR(NextEvent));
        if (event.type == EVENT(fs.CREATED)) {
            break;
        }
        if (event.type == EVENT(fs.ERROR)) {
            return -3;
        }
    }
    
    return 0;	
}
