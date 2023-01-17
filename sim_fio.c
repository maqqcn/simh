/* sim_fio.c: simulator file I/O library

   Copyright (c) 1993-2008, Robert M Supnik

   Permission is hereby granted, free of charge, to any person obtaining a
   copy of this software and associated documentation files (the "Software"),
   to deal in the Software without restriction, including without limitation
   the rights to use, copy, modify, merge, publish, distribute, sublicense,
   and/or sell copies of the Software, and to permit persons to whom the
   Software is furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
   ROBERT M SUPNIK BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
   IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
   CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

   Except as contained in this notice, the name of Robert M Supnik shall not be
   used in advertising or otherwise to promote the sale, use or other dealings
   in this Software without prior written authorization from Robert M Supnik.

   03-Jun-11    MP      Simplified VMS 64b support and made more portable
   02-Feb-11    MP      Added sim_fsize_ex and sim_fsize_name_ex returning t_addr
                        Added export of sim_buf_copy_swapped and sim_buf_swap_data
   28-Jun-07    RMS     Added VMS IA64 support (from Norm Lastovica)
   10-Jul-06    RMS     Fixed linux conditionalization (from Chaskiel Grundman)
   15-May-06    RMS     Added sim_fsize_name
   21-Apr-06    RMS     Added FreeBSD large file support (from Mark Martinec)
   19-Nov-05    RMS     Added OS/X large file support (from Peter Schorn)
   16-Aug-05    RMS     Fixed C++ declaration and cast problems
   17-Jul-04    RMS     Fixed bug in optimized sim_fread (reported by Scott Bailey)
   26-May-04    RMS     Optimized sim_fread (suggested by John Dundas)
   02-Jan-04    RMS     Split out from SCP

   This library includes:

   sim_finit         -       initialize package
   sim_fopen         -       open file
   sim_fread         -       endian independent read (formerly fxread)
   sim_fwrite        -       endian independent write (formerly fxwrite)
   sim_fseek         -       conditionally extended (>32b) seek (
   sim_fseeko        -       extended seek (>32b if available)
   sim_can_seek      -       test for seekable (regular file)
   sim_fsize         -       get file size
   sim_fsize_name    -       get file size of named file
   sim_fsize_ex      -       get file size as a t_offset
   sim_fsize_name_ex -       get file size as a t_offset of named file
   sim_buf_copy_swapped -    copy data swapping elements along the way
   sim_buf_swap_data -       swap data elements inplace in buffer if needed
   sim_byte_swap_data -      swap data elements inplace in buffer
   sim_buf_pack_unpack -     pack or unpack data between buffers
   sim_shmem_open            create or attach to a shared memory region
   sim_shmem_close           close a shared memory region
   sim_chdir                 change working directory
   sim_mkdir                 create a directory
   sim_rmdir                 remove a directory
   sim_getcwd                get the current working directory
   sim_copyfile              copy a file
   sim_filepath_parts        expand and extract filename/path parts
   sim_dirscan               scan for a filename pattern
   sim_get_filelist          get a list of files matching a pattern
   sim_free_filelist         free a filelist
   sim_print_filelist        print the elements of a filelist

   sim_fopen and sim_fseek are OS-dependent.  The other routines are not.
   sim_fsize is always a 32b routine (it is used only with small capacity random
   access devices like fixed head disks and DECtapes).
*/

#define IN_SIM_FIO_C 1              /* Include from sim_fio.c */

#include "sim_defs.h"

t_bool sim_end;                     /* TRUE = little endian, FALSE = big endian */
t_bool sim_taddr_64;                /* t_addr is > 32b and Large File Support available */
t_bool sim_toffset_64;              /* Large File (>2GB) file I/O Support available */

#if defined(fprintf)                /* Make sure to only use the C rtl stream I/O routines */
#undef fprintf
#undef fputs
#undef fputc
#endif

#ifndef MAX
#define MAX(a,b)  (((a) >= (b)) ? (a) : (b))
#endif
#ifndef MIN
#define MIN(a,b)  (((a) <= (b)) ? (a) : (b))
#endif

#define FIO_DBG_PACK    1       /* Pack/Unpack Test Detail */
#define FIO_DBG_SCAN    2       /* File/Directory Scan Detail */

static DEBTAB fio_debug[] = {
  {"PACK",     FIO_DBG_PACK,      "Pack/Unpack Test Detail"},
  {"SCAN",     FIO_DBG_SCAN,      "File/Directory Scan Detail"},
  {0}
};

static const char *sim_fio_test_description (DEVICE *dptr)
{
return "SCP FIO Testing";
}

static UNIT sim_fio_unit = { 0 };

static DEVICE sim_fio_test_dev = {
    "SCP-FIO", &sim_fio_unit, NULL, NULL, 
    1, 0, 0, 0, 0, 0, 
    NULL, NULL, NULL, NULL, NULL, NULL, 
    NULL, DEV_NOSAVE|DEV_DEBUG, 0, 
    fio_debug, NULL, NULL, NULL, NULL, NULL,
    sim_fio_test_description};

/* OS-independent, endian independent binary I/O package

   For consistency, all binary data read and written by the simulator
   is stored in little endian data order.  That is, in a multi-byte
   data item, the bytes are written out right to left, low order byte
   to high order byte.  On a big endian host, data is read and written
   from high byte to low byte.  Consequently, data written on a little
   endian system must be byte reversed to be usable on a big endian
   system, and vice versa.

   These routines are analogs of the standard C runtime routines
   fread and fwrite.  If the host is little endian, or the data items
   are size char, then the calls are passed directly to fread or
   fwrite.  Otherwise, these routines perform the necessary byte swaps.
   Sim_fread swaps in place, sim_fwrite uses an intermediate buffer.
*/

int32 sim_finit (void)
{
union {int32 i; char c[sizeof (int32)]; } end_test;

end_test.i = 1;                                         /* test endian-ness */
sim_end = (end_test.c[0] != 0);
sim_toffset_64 = (sizeof(t_offset) > sizeof(int32));    /* Large File (>2GB) support */
sim_taddr_64 = sim_toffset_64 && (sizeof(t_addr) > sizeof(int32));
return sim_end;
}

/* Copy little endian data to local buffer swapping if needed */
void sim_buf_swap_data (void *bptr, size_t size, size_t count)
{
if (sim_end || (count == 0) || (size == sizeof (char)))
    return;
sim_byte_swap_data (bptr, size, count);
}

void sim_byte_swap_data (void *bptr, size_t size, size_t count)
{
uint32 j;
int32 k;
unsigned char by, *sptr, *dptr;

if (sim_end || (count == 0) || (size == sizeof (char)))
    return;
for (j = 0, dptr = sptr = (unsigned char *) bptr;       /* loop on items */
     j < count; j++) { 
    for (k = (int32)(size - 1); k >= (((int32) size + 1) / 2); k--) {
        by = *sptr;                                     /* swap end-for-end */
        *sptr++ = *(dptr + k);
        *(dptr + k) = by;
        }
    sptr = dptr = dptr + size;                          /* next item */
    }
}

size_t sim_fread (void *bptr, size_t size, size_t count, FILE *fptr)
{
size_t c;

if ((size == 0) || (count == 0))                        /* check arguments */
    return 0;
c = fread (bptr, size, count, fptr);                    /* read buffer */
if (sim_end || (size == sizeof (char)) || (c == 0))     /* le, byte, or err? */
    return c;                                           /* done */
sim_buf_swap_data (bptr, size, c);
return c;
}

void sim_buf_copy_swapped (void *dbuf, const void *sbuf, size_t size, size_t count)
{
size_t j;
int32 k;
const unsigned char *sptr = (const unsigned char *)sbuf;
unsigned char *dptr = (unsigned char *)dbuf;

if (sim_end || (size == sizeof (char))) {
    memcpy (dptr, sptr, size * count);
    return;
    }
for (j = 0; j < count; j++) {                           /* loop on items */
    for (k = (int32)(size - 1); k >= 0; k--)
        *(dptr + k) = *sptr++;
    dptr = dptr + size;
    }
}

static uint32 _bit_index (uint32 bit, uint32 bits, t_bool LSB)
{
uint32 base, offset;

if (LSB)
    return bit;
//return (bits * (bit / bits)) + bits - ((bit % bits) + 1); /* Reverse bit ordering - likely not useful */
base = (bits * (bit / bits)) - (((bits % 8) == 0) ? 8 : 0);
offset = ((base / bits) * (bits % 8));
bit = (bit % bits) + offset;
return base + (bits - (((bit + (bits % 8)) / 8) * 8) - (bits % 8)) + offset + ((bit + (bits % 8)) % 8);
}

t_bool sim_buf_pack_unpack (const void *sptr,          /* source buffer pointer */
                            void *dptr,                /* destination buffer pointer */
                            uint32 sbits,              /* source buffer element size in bits */
                            t_bool sLSB_o_numbering,   /* source numbered using LSB ordering */
                            uint32 scount,             /* count of source elements */
                            uint32 dbits,              /* interesting bits of each destination element */
                            t_bool dLSB_o_numbering)   /* destination numbered using LSB ordering */
{
const uint8 *s = (const uint8 *)sptr;
uint8 *d = (uint8 *)dptr;
uint32 bits_to_process;         /* bits in current source element remaining to be processed */
uint32 sbit_offset, dbit_offset;/* source and destination bit offsets */
uint32 sx;                      /* source byte index */
uint32 dx;                      /* destination byte index */
uint32 bit;                     /* Current Bit number */
uint32 element;                 /* Current element number */

sim_debug (FIO_DBG_PACK, &sim_fio_test_dev, "sim_buf_pack_unpack(sbits=%d, dLSB_o=%s, scount=%d, dbits=%d, dLSB_o=%s)\n", sbits, sLSB_o_numbering ? "True" : "False", scount, dbits, dLSB_o_numbering ? "True" : "False");
if (((dbits * scount) & 7) != 0)
    return TRUE;                    /* Error - Can't process all source elements */
memset (d, 0, (dbits * scount) >> 3);

if (((sbits % 8) == 0)                  &&
    (sbits == dbits)                    &&
    (sLSB_o_numbering == dLSB_o_numbering)) {
    sim_buf_copy_swapped (dptr, sptr, sbits >> 3, scount);
    return FALSE;
    }
bits_to_process = MIN (sbits, dbits);
for (element = 0; element < scount; element++) {
    sbit_offset = element * sbits;
    dbit_offset = element * dbits;
    for (bit = 0; bit < bits_to_process; bit++, sbit_offset++, dbit_offset++) {
        sx = _bit_index (sbit_offset, sbits, sLSB_o_numbering);
        dx = _bit_index (dbit_offset, dbits, dLSB_o_numbering);
        d[dx >> 3] |= (((s[sx >> 3] >> (sx & 7)) & 1) << (dx & 7));
        }
    }
return FALSE;
}

size_t sim_fwrite (const void *bptr, size_t size, size_t count, FILE *fptr)
{
size_t c, nelem, nbuf, lcnt, total;
int32 i;
const unsigned char *sptr;
unsigned char *sim_flip;

if ((size == 0) || (count == 0))                        /* check arguments */
    return 0;
if (sim_end || (size == sizeof (char)))                 /* le or byte? */
    return fwrite (bptr, size, count, fptr);            /* done */
sim_flip = (unsigned char *)malloc(FLIP_SIZE);
if (!sim_flip)
    return 0;
nelem = FLIP_SIZE / size;                               /* elements in buffer */
nbuf = count / nelem;                                   /* number buffers */
lcnt = count % nelem;                                   /* count in last buf */
if (lcnt) nbuf = nbuf + 1;
else lcnt = nelem;
total = 0;
sptr = (const unsigned char *) bptr;                    /* init input ptr */
for (i = (int32)nbuf; i > 0; i--) {                     /* loop on buffers */
    c = (i == 1)? lcnt: nelem;
    sim_buf_copy_swapped (sim_flip, sptr, size, c);
    sptr = sptr + size * count;
    c = fwrite (sim_flip, size, c, fptr);
    if (c == 0) {
        free(sim_flip);
        return total;
        }
    total = total + c;
    }
free(sim_flip);
return total;
}

/* Forward Declaration */

t_offset sim_ftell (FILE *st);

/* Get file size */

t_offset sim_fsize_ex (FILE *fp)
{
t_offset pos, sz;

if (fp == NULL)
    return 0;
pos = sim_ftell (fp);
if (sim_fseeko (fp, 0, SEEK_END))
    return 0;
sz = sim_ftell (fp);
if (sim_fseeko (fp, pos, SEEK_SET))
    return 0;
return sz;
}

t_offset sim_fsize_name_ex (const char *fname)
{
FILE *fp;
t_offset sz;

if ((fp = sim_fopen (fname, "rb")) == NULL)
    return 0;
sz = sim_fsize_ex (fp);
fclose (fp);
return sz;
}

uint32 sim_fsize_name (const char *fname)
{
return (uint32)(sim_fsize_name_ex (fname));
}

uint32 sim_fsize (FILE *fp)
{
return (uint32)(sim_fsize_ex (fp));
}

t_bool sim_can_seek (FILE *fp)
{
struct stat statb;

if ((0 != fstat (fileno (fp), &statb)) ||
    (0 == (statb.st_mode & S_IFREG)))
    return FALSE;
return TRUE;
}



#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#include <fcntl.h>
#else
#include <unistd.h>
#endif

static char *_sim_expand_homedir (const char *file, char *dest, size_t dest_size)
{
uint8 *without_quotes = NULL;
uint32 dsize = 0;

errno = 0;
if (((*file == '"') && (file[strlen (file) - 1] == '"')) ||
    ((*file == '\'') && (file[strlen (file) - 1] == '\''))) {
    without_quotes = (uint8*)malloc (strlen (file) + 1);
    if (without_quotes == NULL)
        return NULL;
    if (SCPE_OK != sim_decode_quoted_string (file, without_quotes, &dsize)) {
        free (without_quotes);
        errno = EINVAL;
        return NULL;
    }
    file = (const char*)without_quotes;
}

if (memcmp (file, "~/", 2) != 0)
    strlcpy (dest, file, dest_size);
else {
    char *cptr = getenv("HOME");
    char *cptr2;

    if (cptr == NULL) {
        cptr = getenv("HOMEPATH");
        cptr2 = getenv("HOMEDRIVE");
        }
    else
        cptr2 = NULL;
    if (cptr && (dest_size > strlen (cptr) + strlen (file) + 3))
        snprintf(dest, dest_size, "%s%s%s%s", cptr2 ? cptr2 : "", cptr, strchr (cptr, '/') ? "/" : "\\", file + 2);
    else
        strlcpy (dest, file, dest_size);
    while ((strchr (dest, '\\') != NULL) && ((cptr = strchr (dest, '/')) != NULL))
        *cptr = '\\';
    }
free (without_quotes);
return dest;
}

/*
 *  DBD9 packing/encoding is:
 *          9 character per pair of 36 bit words.
 *
 *    36b   Bit numbers using              bit
 *   word   standard bit numbering   byte  offset
 *      0 - 35 34 33 32 31 30 29 28     0     0
 *      0 - 27 26 25 24 23 22 21 20     1     8
 *      0 - 19 18 17 16 15 14 13 12     2    16
 *      0 - 11 10  9  8  7  6  5  4     3    24
 *      0 -  3  2  1  0 35 34 33 32     4    32
 *      1 - 31 30 29 28 27 26 25 24     5    40
 *      1 - 23 22 21 20 19 18 17 16     6    48
 *      1 - 15 14 13 12 11 10  9  8     7    56
 *      1 -  7  6  5  4  3  2  1  0     8    64
 *
 *   word   Bit numbers using PDP10 bit numbering
 *      0 - B0  1  2  3  4  5  6  7
 *      0 -  8  9 10 11 12 13 14 15
 *      0 - 16 17 18 19 20 21 22 23
 *      0 - 24 25 26 27 28 29 30 31
 *      0 - 32 33 34 35 B0  1  2  3
 *      1 -  4  5  6  7  8  9 10 11
 *      1 - 12 13 14 15 16 17 18 19
 *      1 - 20 21 22 23 24 25 26 27
 *      1 - 28 29 30 31 32 33 34 35
 *
 *  DLD9 packing/encoding is:   
 *          9 character per pair of 36 bit words.
 *
 *    36b   Bit numbers using              bit
 *   word   standard bit numbering   byte  offset
 *      0 -  7  6  5  4  3  2  1  0     0     0
 *      0 - 15 14 13 12 11 10  9  8     1     8
 *      0 - 23 22 21 20 19 18 17 16     2    16
 *      0 - 31 30 29 28 27 26 25 24     3    24
 *      0 -  3  2  1  0 35 34 33 32     4    32
 *      1 - 11 10  9  8  7  6  5  4     5    40
 *      1 - 19 18 17 16 15 14 13 12     6    48
 *      1 - 27 26 25 24 23 22 21 20     7    56
 *      1 - 35 34 33 32 31 30 29 28     8    64
 *
 *   word   Bit numbers using PDP10 bit numbering
 *      0 - 28 29 30 31 32 33 34 35
 *      0 - 20 21 22 23 24 25 26 27
 *      0 - 12 13 14 15 16 17 18 19
 *      0 -  4  5  6  7  8  9 10 11
 *      0 - 32 33 34 35 B0  1  2  3
 *      1 - 24 25 26 27 28 29 30 31
 *      1 - 16 17 18 19 20 21 22 23
 *      1 -  8  9 10 11 12 13 14 15
 *      1 - B0  1  2  3  4  5  6  7
 */

uint32 int32_data[] = {  0x00000000,  0x00000001,  0x00000002,  0x00000003, 
                         0x00000004,  0x00000005,  0x00000006,  0x00000007, 
                         0x00000008,  0x00000009,  0x0000000A,  0x0000000B,
                         0x0000000C,  0x0000000D,  0x0000000E,  0x0000000F};
uint32 res_32bitM[] = {  0x00000000,  0x01000000,  0x02000000,  0x03000000, 
                         0x04000000,  0x05000000,  0x06000000,  0x07000000, 
                         0x08000000,  0x09000000,  0x0A000000,  0x0B000000,
                         0x0C000000,  0x0D000000,  0x0E000000,  0x0F000000};
uint32 res_32_1[] =   {  0,  1,  0,  1, 
                         0,  1,  0,  1, 
                         0,  1,  0,  1,
                         0,  1,  0,  1};
uint16 int16_data[] = { 0x1234, 0x5678,
                        0x9ABC, 0xDEF0};
uint16 res_16bit[] =  { 0x3412, 0x7856,
                        0xBC9A, 0xF0DE};
uint8 res_8bit[] = {  0,  1,  2,  3, 
                      4,  5,  6,  7, 
                      8,  9, 10, 11,
                     12, 13, 14, 15};
uint8 res_4bit[] = {  0x10,  0x32, 0x54, 0x76, 
                      0x98,  0xba, 0xdc, 0xfe};
uint8 res_2bit[] = {  0xE4,  0xE4, 0xE4, 0xE4};
uint8 res_1bit[] = {  0xAA,  0xAA};
#if defined (USE_INT64)
t_uint64 int64_data[] = { 0x876543210, 0x012345678, 0x987654321, 0x123456789};
uint8 res_36bit[] = {0x10, 0x32, 0x54, 0x76, 0x88, 0x67, 0x45, 0x23, 0x01,
                     0x21, 0x43, 0x65, 0x87, 0x99, 0x78, 0x56, 0x34, 0x12};
uint8 res_36bitM[]= {0x87, 0x65, 0x43, 0x21, 0x00, 0x12, 0x34, 0x56, 0x78,
                     0x98, 0x76, 0x54, 0x32, 0x11, 0x23, 0x45, 0x67, 0x89};
uint8 int64_data_dbd9[18];
uint8 int64_data_dld9[18];
#endif

static struct pack_test {
    const void *src;
    const void *exp_dst;
    uint32      sbits;
    t_bool      slsb;
    uint32      dbits;
    t_bool      dlsb;
    uint32      scount;
    t_bool      exp_stat;
    } p_test[] = {
#if defined (USE_INT64)
        {&int64_data, &res_36bitM, 64, TRUE,  36, FALSE, 4,  FALSE},
        {&res_36bitM, &int64_data, 36, FALSE, 64, TRUE,  4,  FALSE},
        {&int64_data, &res_36bit,  64, TRUE,  36, TRUE,  4,  FALSE},
        {&res_36bit,  &int64_data, 36, TRUE,  64, TRUE,  4,  FALSE},
#endif
        {&int16_data, &res_16bit,  16, TRUE,  16, FALSE, 4,  FALSE},
        {&int16_data, &res_16bit,  16, FALSE, 16, TRUE,  4,  FALSE},
        {&int16_data, &int16_data, 16, TRUE,  16, TRUE,  4,  FALSE},
        {&int16_data, &int16_data, 16, FALSE, 16, FALSE, 4,  FALSE},
        {&int32_data, &int32_data, 32, FALSE, 32, FALSE,16,  FALSE},
        {&int32_data, &int32_data, 32, TRUE,  32, TRUE, 16,  FALSE},
        {&int32_data, &res_32bitM, 32, TRUE,  32, FALSE,16,  FALSE},
        {&res_32bitM, &int32_data, 32, FALSE, 32, TRUE, 16,  FALSE},
        {&res_8bit,   &res_8bit,    8, TRUE,   8, FALSE,16,  FALSE},
        {&res_8bit,   &res_8bit,    8, FALSE,  8, TRUE, 16,  FALSE},
        {&res_8bit,   &res_8bit,    8, FALSE,  8, FALSE,16,  FALSE},
        {&res_8bit,   &res_8bit,    8, TRUE,   8, TRUE, 16,  FALSE},
        {&res_8bit,   &res_8bit,   16, TRUE,  16, TRUE,  8,  FALSE},
        {&res_8bit,   &res_8bit,   16, FALSE, 16, FALSE, 8,  FALSE},
        {&res_1bit,   &res_32_1,    1, TRUE,  32, TRUE, 16,  FALSE},
        {&res_8bit,   &int32_data,  8, TRUE,  32, TRUE,  2,  FALSE},
        {&res_4bit,   &int32_data,  4, TRUE,  32, TRUE, 16,  FALSE},
        {&int32_data, &res_8bit,   32, TRUE,   8, TRUE, 16,  FALSE},
        {&int32_data, &int32_data, 32, TRUE,  32, TRUE, 16,  FALSE},
        {&int32_data, &int32_data, 16, TRUE,  16, TRUE, 32,  FALSE},
        {&int32_data, &int32_data,  8, TRUE,   8, TRUE, 64,  FALSE},
        {&int32_data, &res_8bit,   32, TRUE,   8, TRUE, 16,  FALSE},
        {&int32_data, &res_4bit,   32, TRUE,   4, TRUE, 16,  FALSE},
        {&int32_data, &res_2bit,   32, TRUE,   2, TRUE, 16,  FALSE},
        {&int32_data, &res_1bit,   32, TRUE,   1, TRUE, 16,  FALSE},
        {NULL},
        };



static struct relative_path_test {
    const char  *input;
    t_bool      prepend_cwd;
    const char  *working_dir;
    const char  *result;
    } r_test[] = {
        {"/xx.dat",                 TRUE,  "xx",     "../xx.dat"},
        {"/file.dat",               TRUE,  "xx/t",   "../../file.dat"},
        {"/../../xxx/file.dat",     TRUE,  NULL,     "../../xxx/file.dat"},
        {"\\..\\..\\xxx\\file.dat", TRUE,  NULL,     "../../xxx/file.dat"},
        {"file.dat",                FALSE, NULL,     "./file.dat"},
        {"\\file.dat",              TRUE,  NULL,     "./file.dat"},
        {"C:/XXX/yyy/file.dat",     FALSE, NULL,     "C:/XXX/yyy/file.dat"},
        {"C:/Users/yyy/file.dat",   FALSE, NULL,     "C:/Users/yyy/file.dat"},
        {"W:/XXX/yyy/file.dat",     FALSE, NULL,     "W:/XXX/yyy/file.dat"},
        {"/file.dat",               TRUE,  NULL,     "./file.dat"},
        {"/x/filepath/file.dat",    FALSE, NULL,     "/x/filepath/file.dat"},
        {NULL},
        };

static struct get_filelist_test {
    const char *name;
    const char  *files[10];
    const char *search;
    int         expected_count;
   } get_test[] = {
        {"test-1",
         {"aab/bbc/ccd/eef/file.txt", 
          "aab/bbc/ccd/eef/file2.txt", 
          "aac/bbd/cce/eef/file2.txt", 
          NULL},
         "*.txt", 3},
        {"test-2",
         {"xab/bbc/ccd/eef/file.txt", 
          "xab/bbc/ccd/eef/file2.bbb", 
          "xac/bbd/cce/eef/file2.txt", 
          NULL},
         "*.txt", 2},
        {NULL},
    };


t_stat sim_fio_test (const char *cptr)
{
struct pack_test *pt;
struct relative_path_test *rt;
struct get_filelist_test *gt;
t_stat r = SCPE_OK;
char test_desc[512];
uint8 result[512];

sim_register_internal_device (&sim_fio_test_dev);
sim_fio_test_dev.dctrl = (sim_switches & SWMASK ('D')) ? FIO_DBG_PACK : 0;
sim_fio_test_dev.dctrl = (sim_switches & SWMASK ('S')) ? FIO_DBG_SCAN : 0;
sim_set_deb_switches (SWMASK ('F'));
sim_messagef (SCPE_OK, "sim_buf_pack_unpack - tests\n");
for (pt = p_test; pt->src; ++pt) {
    t_bool res;

    snprintf (test_desc, sizeof (test_desc), "%dbit%s->%dbit%s %d words", pt->sbits, pt->slsb ? "LSB" : "MSB", pt->dbits, pt->dlsb ? "LSB" : "MSB", pt->scount);
    memset (result, 0x80, sizeof (result));
    res = sim_buf_pack_unpack (pt->src, result, pt->sbits, pt->slsb, pt->scount, pt->dbits, pt->dlsb);
    if (res == pt->exp_stat) {
        if (!res) {
            if (0 == memcmp (pt->exp_dst, result, (pt->scount * pt->dbits) / 8))
                sim_messagef (SCPE_OK, "%s - GOOD\n", test_desc);
            else {
                uint32 i;

                r = sim_messagef (SCPE_IERR, "%s - BAD Data:\n", test_desc);
                sim_messagef (SCPE_IERR, "Off: Exp:    Got:\n");
                for (i = 0; i < ((pt->scount * pt->dbits) / 8); i++)
                    sim_messagef (SCPE_IERR, "%3d  0x%02X%s0x%02X\n", i, ((uint8 *)pt->exp_dst)[i], (((uint8 *)pt->exp_dst)[i] == result[i]) ? "    " : " != ", result[i]);
                }
            }
        }
    else
        r = sim_messagef (SCPE_IERR, "%s - BAD Status - Expected: %s got %s\n", test_desc, pt->exp_stat ? "True" : "False", res ? "True" : "False");
    }
sim_messagef (SCPE_OK, "Testing relative path logic:\n");
for (rt = r_test; rt->input; ++rt) {
    char input[PATH_MAX + 1];
    char cmpbuf[PATH_MAX + 1];
    char cwd[PATH_MAX + 1];
    char origcwd[PATH_MAX + 1];
    char *wd = sim_getcwd(cwd, sizeof (cwd));
    char *cp;
    const char *result;
    static const char seperators[] = "/\\";
    const char *sep;
    t_stat mkdir_stat = SCPE_OK;

    strlcpy (origcwd, cwd, sizeof (origcwd));
    if (rt->prepend_cwd) {
        strlcpy (input, cwd, sizeof (input));
        strlcat (input, rt->input, sizeof (input));
        }
    else
        strlcpy (input, rt->input, sizeof (input));
    if (rt->working_dir != NULL) {
        mkdir_stat = mkdir_cmd (0, rt->working_dir);
        sim_chdir (rt->working_dir);
        wd = sim_getcwd(cwd, sizeof (cwd));
        }
    for (sep = seperators; *sep != '\0'; ++sep) {
        while ((cp = strchr (input, *sep)))
            *cp = (*sep == '/') ? '\\' : '/';
        while ((cp = strchr (cwd, *sep)))
            *cp = (*sep == '/') ? '\\' : '/';
        result = sim_relative_path (input);
        strlcpy (cmpbuf, rt->result, sizeof (cmpbuf));
        if (strchr (input, *sep) != NULL) {         /* Input has separators? */
            while ((cp = strchr (cmpbuf, *sep)))
                *cp = (*sep == '/') ? '\\' : '/';   /* Change the expected result to match */
            }
        if (strcmp (result, cmpbuf) != 0) {
            r = sim_messagef (SCPE_IERR, "Relative Path Unexpected Result:\n");
            sim_messagef (SCPE_IERR, "    input: %s\n", input);
            sim_messagef (SCPE_IERR, "   result: %s\n", result);
            sim_messagef (SCPE_IERR, " expected: %s\n", cmpbuf);
            sim_messagef (SCPE_IERR, "      cwd: %s\n", cwd);
            }
        else {
            sim_messagef (SCPE_OK, "Relative Path Good Result:\n");
            sim_messagef (SCPE_OK, "    input: %s\n", input);
            sim_messagef (SCPE_OK, "   result: %s\n", result);
            }
        }
    sim_chdir (origcwd);
    if ((rt->working_dir != NULL) && (mkdir_stat == SCPE_OK)) {
        char *xdir = strdup (rt->working_dir);
        
        sim_rmdir (rt->working_dir);
        while ((cp = strrchr (xdir, '/')) != NULL) {
            *cp = '\0';
            sim_rmdir (xdir);
            }
        free (xdir);
        }
    }
sim_messagef (SCPE_OK, "Testing get filelist:\n");
for (gt = get_test; gt->name; ++gt) {
    char xpath[PATH_MAX + 1];
    int i;
    char **filelist;

    sim_messagef (SCPE_OK, "FileList test %s\n", gt->name);
    for (i=0; gt->files[i]; ++i) {
        char *c, *end;
        char *filename = sim_filepath_parts (gt->files[i], "nx");
        snprintf (xpath, sizeof (xpath), "testfiles/%s", gt->files[i]);
        end = strrchr (xpath, '/');
        *end = '\0';
        c = xpath;
        while ((c = strchr (c, '/'))) {
            *c = '\0';
            sim_mkdir (xpath);
            *c++ = '/';
            }
        sim_mkdir (xpath);
        *end = '/';
        fclose (fopen (xpath, "w"));
        free (filename);
        }
    snprintf (xpath, sizeof (xpath), "testfiles/%s", gt->search);
    filelist = sim_get_filelist (xpath);
    r = sim_messagef ((gt->expected_count != sim_count_filelist (filelist)) ? SCPE_IERR : SCPE_OK, 
                      "sim_get_filelist (\"%s\") yielded %d entries:\n", xpath, sim_count_filelist (filelist));
    sim_print_filelist (filelist);
    sim_free_filelist (&filelist);
    for (i=0; gt->files[i]; ++i) {
        char *c;
        char *filename = sim_filepath_parts (gt->files[i], "nx");
        snprintf (xpath, sizeof (xpath), "testfiles/%s", gt->files[i]);
        unlink (xpath);
        c = strrchr (xpath, '/');
        *c = '\0';
        sim_rmdir (xpath);
        while ((c = strrchr (xpath, '/'))) {
            *c = '\0';
            sim_rmdir (xpath);
            }
        sim_rmdir (xpath);
        free (filename);
        }
    }
return r;
}


int sim_stat (const char *fname, struct stat *stat_str)
{
char namebuf[PATH_MAX + 1];

if (NULL == _sim_expand_homedir (fname, namebuf, sizeof (namebuf)))
    return -1;
return stat (namebuf, stat_str);
}

int sim_chdir(const char *path)
{
char pathbuf[PATH_MAX + 1];

if (NULL == _sim_expand_homedir (path, pathbuf, sizeof (pathbuf)))
    return -1;
return chdir (pathbuf);
}

int sim_mkdir(const char *path)
{
char pathbuf[PATH_MAX + 1];

if (NULL == _sim_expand_homedir (path, pathbuf, sizeof (pathbuf)))
    return -1;
#if defined(_WIN32)
return mkdir (pathbuf);
#else
return mkdir (pathbuf, 0777);
#endif
}

int sim_rmdir(const char *path)
{
char pathbuf[PATH_MAX + 1];

if (NULL == _sim_expand_homedir (path, pathbuf, sizeof (pathbuf)))
    return -1;
return rmdir (pathbuf);
}

static void _sim_dirlist_entry (const char *directory, 
                                 const char *filename,
                                 t_offset FileSize,
                                 const struct stat *filestat,
                                 void *context)
{
char **dirlist = *(char ***)context;
char FullPath[PATH_MAX + 1];
int listcount = 0;

if ((strcmp (filename, "..") == 0)       || /* Ignore previous dir */
    (strcmp (filename, ".") == 0)        || /* Ignore current dir */
    ((filestat->st_mode & S_IFDIR) == 0) || /* Ignore anything not a directory */
    (stop_cpu))
    return;
if (strcmp (filename, ".") == 0)
    filename = "";
snprintf (FullPath, sizeof (FullPath), "%s%s%s", directory, filename, (*filename != '\0') ? "/" : "");
if (dirlist != NULL) {
    while (dirlist[listcount++] != NULL);
    --listcount;
    }
dirlist = (char **)realloc (dirlist, (listcount + 2) * sizeof (*dirlist));
dirlist[listcount] = strdup (FullPath);
dirlist[listcount + 1] = NULL;
*(char ***)context = dirlist;
if (*filename != '\0') {
    strlcat (FullPath, "*", sizeof (FullPath));                     /* append wildcard selector */
    sim_dir_scan (FullPath, _sim_dirlist_entry, context);           /* recurse on this directory */
    }
}

static void _sim_filelist_entry (const char *directory, 
                                 const char *filename,
                                 t_offset FileSize,
                                 const struct stat *filestat,
                                 void *context)
{
char **filelist = *(char ***)context;
char FullPath[PATH_MAX + 1];
int listcount = 0;

snprintf (FullPath, sizeof (FullPath), "%s%s", directory, filename);
if (filelist != NULL) {
    while (filelist[listcount++] != NULL);
    --listcount;
    }
filelist = (char **)realloc (filelist, (listcount + 2) * sizeof (filelist));
filelist[listcount] = strdup (FullPath);
filelist[listcount + 1] = NULL;
*(char ***)context = filelist;
}

char **sim_get_filelist (const char *filename)
{
t_stat r;
char *dir = sim_filepath_parts (filename, "p");
size_t dirsize = strlen (dir);
char *file = sim_filepath_parts (filename, "nx");
char **dirlist, **dirs;
char **filelist = NULL;

sim_debug (FIO_DBG_SCAN, &sim_fio_test_dev, "sim_get_filelist(filename=\"%s\")\n", filename);
sim_debug (FIO_DBG_SCAN, &sim_fio_test_dev, " Looking for Directories in\"%s\"\n", dir);
dir = (char *)realloc (dir, dirsize + 2);
strlcat (dir, "*", dirsize + 2);
dirlist = NULL;
r = sim_dir_scan (dir, _sim_dirlist_entry, &dirlist);
free (dir);
sim_debug (FIO_DBG_SCAN, &sim_fio_test_dev, " %d directories found, r=%d\n", sim_count_filelist (dirlist), r);
if (r == SCPE_OK) {
    filelist = NULL;
    if (dirlist != NULL) {
        dirs = dirlist;
        while (*dirs && !stop_cpu) {
            size_t dfsize = 1 + strlen (file) + strlen (*dirs);
            char *dfile = (char *)malloc (dfsize);
            char **files;

            snprintf (dfile, dfsize, "%s%s", *dirs++, file);
            sim_debug (FIO_DBG_SCAN, &sim_fio_test_dev, "Checking for: %s\n", dfile);
            r = sim_dir_scan (dfile, _sim_filelist_entry, &filelist);
            free (dfile);
            files = filelist;
            if (sim_deb && files) {
                sim_debug (FIO_DBG_SCAN, &sim_fio_test_dev, "Result: %s\n", *files);
                while (*++files)
                    sim_debug (FIO_DBG_SCAN, &sim_fio_test_dev, "Result: %s\n", *files);
                }
            }
        }
    else
        sim_dir_scan (filename, _sim_filelist_entry, &filelist);
    free (file);
    sim_free_filelist (&dirlist);
    return filelist;
    }
free (file);
r = sim_dir_scan (filename, _sim_filelist_entry, &filelist);
if (r == SCPE_OK)
    return filelist;
return NULL;
}

void sim_free_filelist (char ***pfilelist)
{
char **listp = *pfilelist;

if (listp == NULL)
    return;
while (*listp != NULL)
    free (*listp++);
free (*pfilelist);
*pfilelist = NULL;
}

void sim_print_filelist (char **filelist)
{
if (filelist == NULL)
    return;
while (*filelist != NULL)
    sim_printf ("%s\n", *filelist++);
}

int sim_count_filelist (char **filelist)
{
int count = 0;

if (filelist == NULL)
    return count;
while (*filelist++ != NULL)
    ++count;
return count;
}



/* OS-dependent routines */

/* Optimized file open */
FILE* sim_fopen (const char *file, const char *mode)
{
FILE *f;
char namebuf[PATH_MAX + 1];

if (NULL == _sim_expand_homedir (file, namebuf, sizeof (namebuf)))
    return NULL;
#if defined (VMS)
f = fopen (namebuf, mode, "ALQ=32", "DEQ=4096",
                          "MBF=6", "MBC=127", "FOP=cbt,tef", "ROP=rah,wbh", "CTX=stm");
#elif (defined (__linux) || defined (__linux__) || defined (__hpux) || defined (_AIX)) && !defined (DONT_DO_LARGEFILE)
f = fopen64 (namebuf, mode);
#else
f = fopen (namebuf, mode);
#endif
return f;
}

#if !defined (DONT_DO_LARGEFILE)
/* 64b VMS */

#if ((defined (__ALPHA) || defined (__ia64)) && defined (VMS) && (__DECC_VER >= 60590001)) || \
    ((defined(__sun) || defined(__sun__)) && defined(_LARGEFILE_SOURCE))
#define S_SIM_IO_FSEEK_EXT_ 1
int sim_fseeko (FILE *st, t_offset offset, int whence)
{
return fseeko (st, (off_t)offset, whence);
}

t_offset sim_ftell (FILE *st)
{
return (t_offset)(ftello (st));
}

#endif

/* Alpha UNIX - natively 64b */

#if defined (__ALPHA) && defined (__unix__)             /* Alpha UNIX */
#define S_SIM_IO_FSEEK_EXT_ 1
int sim_fseeko (FILE *st, t_offset offset, int whence)
{
return fseek (st, offset, whence);
}

t_offset sim_ftell (FILE *st)
{
return (t_offset)(ftell (st));
}

#endif

/* Windows */

#if defined (_WIN32)
#define S_SIM_IO_FSEEK_EXT_ 1
#include <sys/stat.h>

int sim_fseeko (FILE *st, t_offset offset, int whence)
{
return _fseeki64 (st, (__int64)offset, whence);
}

t_offset sim_ftell (FILE *st)
{
return (t_offset)_ftelli64 (st);
}

#endif                                                  /* end Windows */

/* Linux */

#if defined (__linux) || defined (__linux__) || defined (__hpux) || defined (_AIX)
#define S_SIM_IO_FSEEK_EXT_ 1
int sim_fseeko (FILE *st, t_offset xpos, int origin)
{
return fseeko64 (st, (off64_t)xpos, origin);
}

t_offset sim_ftell (FILE *st)
{
return (t_offset)(ftello64 (st));
}

#endif                                                  /* end Linux with LFS */

/* Apple OS/X */

#if defined (__APPLE__) || defined (__FreeBSD__) || defined(__NetBSD__) || defined (__OpenBSD__) || defined (__CYGWIN__) 
#define S_SIM_IO_FSEEK_EXT_ 1
int sim_fseeko (FILE *st, t_offset xpos, int origin) 
{
return fseeko (st, (off_t)xpos, origin);
}

t_offset sim_ftell (FILE *st)
{
return (t_offset)(ftello (st));
}

#endif  /* end Apple OS/X */
#endif /* !DONT_DO_LARGEFILE */

/* Default: no OS-specific routine has been defined */

#if !defined (S_SIM_IO_FSEEK_EXT_)
int sim_fseeko (FILE *st, t_offset xpos, int origin)
{
return fseek (st, (long) xpos, origin);
}

t_offset sim_ftell (FILE *st)
{
return (t_offset)(ftell (st));
}
#endif

int sim_fseek (FILE *st, t_addr offset, int whence)
{
return sim_fseeko (st, (t_offset)offset, whence);
}

#if defined(_WIN32)
const char *
sim_get_os_error_text (int Error)
{
static char szMsgBuffer[2048];
DWORD dwStatus;

dwStatus = FormatMessageA (FORMAT_MESSAGE_FROM_SYSTEM|
                           FORMAT_MESSAGE_IGNORE_INSERTS,     //  __in      DWORD dwFlags,
                           NULL,                              //  __in_opt  LPCVOID lpSource,
                           Error,                             //  __in      DWORD dwMessageId,
                           0,                                 //  __in      DWORD dwLanguageId,
                           szMsgBuffer,                       //  __out     LPTSTR lpBuffer,
                           sizeof (szMsgBuffer) -1,           //  __in      DWORD nSize,
                           NULL);                             //  __in_opt  va_list *Arguments
if (0 == dwStatus)
    snprintf(szMsgBuffer, sizeof(szMsgBuffer) - 1, "Error Code: 0x%X", Error);
while (sim_isspace (szMsgBuffer[strlen (szMsgBuffer)-1]))
    szMsgBuffer[strlen (szMsgBuffer) - 1] = '\0';
return szMsgBuffer;
}

t_stat sim_copyfile (const char *source_file, const char *dest_file, t_bool overwrite_existing)
{
char sourcename[PATH_MAX + 1], destname[PATH_MAX + 1];

if (NULL == _sim_expand_homedir (source_file, sourcename, sizeof (sourcename)))
    return sim_messagef (SCPE_ARG, "Error Copying - Problem Parsing Source Filename '%s'\n", source_file);
if (NULL == _sim_expand_homedir (dest_file, destname, sizeof (destname)))
    return sim_messagef (SCPE_ARG, "Error Copying - Problem Parsing Destination Filename '%s'\n", dest_file);
if (CopyFileA (sourcename, destname, !overwrite_existing))
    return SCPE_OK;
return sim_messagef (SCPE_ARG, "Error Copying '%s' to '%s': %s\n", source_file, dest_file, sim_get_os_error_text (GetLastError ()));
}

static void _time_t_to_filetime (time_t ttime, FILETIME *filetime)
{
t_uint64 time64;

time64 = 134774;                /* Days betwen Jan 1, 1601 and Jan 1, 1970 */
time64 *= 24;                   /* Hours */
time64 *= 3600;                 /* Seconds */
time64 += (t_uint64)ttime;      /* include time_t seconds */

time64 *= 10000000;             /* Convert seconds to 100ns units */
filetime->dwLowDateTime = (DWORD)time64;
filetime->dwHighDateTime = (DWORD)(time64 >> 32);
}

t_stat sim_set_file_times (const char *file_name, time_t access_time, time_t write_time)
{
char filename[PATH_MAX + 1];
FILETIME accesstime, writetime;
HANDLE hFile;
BOOL bStat;

_time_t_to_filetime (access_time, &accesstime);
_time_t_to_filetime (write_time, &writetime);
if (NULL == _sim_expand_homedir (file_name, filename, sizeof (filename)))
    return sim_messagef (SCPE_ARG, "Error Setting File Times - Problem Source Filename '%s'\n", filename);
hFile = CreateFileA (filename, GENERIC_READ|GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
if (hFile == INVALID_HANDLE_VALUE)
    return sim_messagef (SCPE_ARG, "Can't open file '%s' to set it's times: %s\n", filename, sim_get_os_error_text (GetLastError ()));
bStat = SetFileTime (hFile, NULL, &accesstime, &writetime);
CloseHandle (hFile);
return bStat ? SCPE_OK : sim_messagef (SCPE_ARG, "Error setting file '%s' times: %s\n", filename, sim_get_os_error_text (GetLastError ()));
}


#include <io.h>
#include <direct.h>
int sim_set_fsize (FILE *fptr, t_addr size)
{
return _chsize(_fileno(fptr), (long)size);
}

int sim_set_fifo_nonblock (FILE *fptr)
{
return -1;
}

struct SHMEM {
    HANDLE hMapping;
    size_t shm_size;
    void *shm_base;
    char *shm_name;
    };

t_stat sim_shmem_open (const char *name, size_t size, SHMEM **shmem, void **addr)
{
SYSTEM_INFO SysInfo;
t_bool AlreadyExists;

GetSystemInfo (&SysInfo);
*shmem = (SHMEM *)calloc (1, sizeof(**shmem));
if (*shmem == NULL)
    return SCPE_MEM;
(*shmem)->shm_name = (char *)calloc (1, 1 + strlen (name));
if ((*shmem)->shm_name == NULL) {
    free (*shmem);
    *shmem = NULL;
    return SCPE_MEM;
    }
strcpy ((*shmem)->shm_name, name);
(*shmem)->hMapping = INVALID_HANDLE_VALUE;
(*shmem)->shm_size = size;
(*shmem)->shm_base = NULL;
(*shmem)->hMapping = CreateFileMappingA (INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE|SEC_COMMIT, 0, (DWORD)(size+SysInfo.dwPageSize), name);
if ((*shmem)->hMapping == INVALID_HANDLE_VALUE) {
    DWORD LastError = GetLastError();

    sim_shmem_close (*shmem);
    *shmem = NULL;
    return sim_messagef (SCPE_OPENERR, "Can't CreateFileMapping of a %u byte shared memory segment '%s' - LastError=0x%X\n", (unsigned int)size, name, (unsigned int)LastError);
    }
AlreadyExists = (GetLastError () == ERROR_ALREADY_EXISTS);
(*shmem)->shm_base = MapViewOfFile ((*shmem)->hMapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);
if ((*shmem)->shm_base == NULL) {
    DWORD LastError = GetLastError();

    sim_shmem_close (*shmem);
    *shmem = NULL;
    return sim_messagef (SCPE_OPENERR, "Can't MapViewOfFile() of a %u byte shared memory segment '%s' - LastError=0x%X\n", (unsigned int)size, name, (unsigned int)LastError);
    }
if (AlreadyExists) {
    if (*((DWORD *)((*shmem)->shm_base)) == 0)
        Sleep (50);
    if (*((DWORD *)((*shmem)->shm_base)) != (DWORD)size) {
        DWORD SizeFound = *((DWORD *)((*shmem)->shm_base));
        sim_shmem_close (*shmem);
        *shmem = NULL;
        return sim_messagef (SCPE_OPENERR, "Shared Memory segment '%s' is %u bytes instead of %d\n", name, (unsigned int)SizeFound, (int)size);
        }
    }
else
    *((DWORD *)((*shmem)->shm_base)) = (DWORD)size;     /* Save Size in first page */

*addr = ((char *)(*shmem)->shm_base + SysInfo.dwPageSize);      /* Point to the second paget for data */
return SCPE_OK;
}

void sim_shmem_close (SHMEM *shmem)
{
if (shmem == NULL)
    return;
if (shmem->shm_base != NULL)
    UnmapViewOfFile (shmem->shm_base);
if (shmem->hMapping != INVALID_HANDLE_VALUE)
    CloseHandle (shmem->hMapping);
free (shmem->shm_name);
free (shmem);
}

int32 sim_shmem_atomic_add (int32 *p, int32 v)
{
return InterlockedExchangeAdd ((volatile long *) p,v) + (v);
}

t_bool sim_shmem_atomic_cas (int32 *ptr, int32 oldv, int32 newv)
{
return (InterlockedCompareExchange ((LONG volatile *) ptr, newv, oldv) == oldv);
}

#else /* !defined(_WIN32) */
#include <unistd.h>
int sim_set_fsize (FILE *fptr, t_addr size)
{
return ftruncate(fileno(fptr), (off_t)size);
}

#include <sys/stat.h>
#include <fcntl.h>
#if defined (HAVE_UTIME)
#include <utime.h>
#endif

const char *
sim_get_os_error_text (int Error)
{
return strerror (Error);
}

t_stat sim_copyfile (const char *source_file, const char *dest_file, t_bool overwrite_existing)
{
FILE *fIn = NULL, *fOut = NULL;
t_stat st = SCPE_OK;
char *buf = NULL;
size_t bytes;

fIn = sim_fopen (source_file, "rb");
if (!fIn) {
    st = sim_messagef (SCPE_ARG, "Can't open '%s' for input: %s\n", source_file, strerror (errno));
    goto Cleanup_Return;
    }
fOut = sim_fopen (dest_file, "wb");
if (!fOut) {
    st = sim_messagef (SCPE_ARG, "Can't open '%s' for output: %s\n", dest_file, strerror (errno));
    goto Cleanup_Return;
    }
buf = (char *)malloc (BUFSIZ);
while ((bytes = fread (buf, 1, BUFSIZ, fIn)))
    fwrite (buf, 1, bytes, fOut);
Cleanup_Return:
free (buf);
if (fIn)
    fclose (fIn);
if (fOut)
    fclose (fOut);
#if defined(HAVE_UTIME)
if (st == SCPE_OK) {
    struct stat statb;

    if (!sim_stat (source_file, &statb)) {
        struct utimbuf utim;

        utim.actime = statb.st_atime;
        utim.modtime = statb.st_mtime;
        if (utime (dest_file, &utim))
            st = SCPE_IOERR;
        }
    else
        st = SCPE_IOERR;
    }
#endif
return st;
}

t_stat sim_set_file_times (const char *file_name, time_t access_time, time_t write_time)
{
t_stat st = SCPE_IOERR;
#if defined (HAVE_UTIME)
struct utimbuf utim;

utim.actime = access_time;
utim.modtime = write_time;
if (!utime (file_name, &utim))
    st = SCPE_OK;
#else
st = SCPE_NOFNC;
#endif
return st;
}

int sim_set_fifo_nonblock (FILE *fptr)
{
struct stat stbuf;

if (!fptr || fstat (fileno(fptr), &stbuf))
    return -1;
#if defined(S_IFIFO) && defined(O_NONBLOCK)
if ((stbuf.st_mode & S_IFIFO)) {
    int flags = fcntl(fileno(fptr), F_GETFL, 0);
    return fcntl(fileno(fptr), F_SETFL, flags | O_NONBLOCK);
    }
#endif
return -1;
}

#if defined (__linux__) || defined (__APPLE__) || defined (__CYGWIN__) || defined (__FreeBSD__) || defined(__NetBSD__) || defined (__OpenBSD__)

#if defined (HAVE_SHM_OPEN)
#include <sys/mman.h>
#endif

struct SHMEM {
    int shm_fd;
    size_t shm_size;
    void *shm_base;
    char *shm_name;
    };

t_stat sim_shmem_open (const char *name, size_t size, SHMEM **shmem, void **addr)
{
#if defined (HAVE_SHM_OPEN) && defined (__GCC_HAVE_SYNC_COMPARE_AND_SWAP_4)
*shmem = (SHMEM *)calloc (1, sizeof(**shmem));
mode_t orig_mask;

*addr = NULL;
if (*shmem == NULL)
    return SCPE_MEM;
(*shmem)->shm_name = (char *)calloc (1, 1 + strlen (name) + ((*name != '/') ? 1 : 0));
if ((*shmem)->shm_name == NULL) {
    free (*shmem);
    *shmem = NULL;
    return SCPE_MEM;
    }

sprintf ((*shmem)->shm_name, "%s%s", ((*name != '/') ? "/" : ""), name);
(*shmem)->shm_base = MAP_FAILED;
(*shmem)->shm_size = size;
(*shmem)->shm_fd = shm_open ((*shmem)->shm_name, O_RDWR, 0);
if ((*shmem)->shm_fd == -1) {
    int last_errno;

    orig_mask = umask (0000);
    (*shmem)->shm_fd = shm_open ((*shmem)->shm_name, O_CREAT | O_RDWR, 0660);
    last_errno = errno;
    umask (orig_mask);                  /* Restore original mask */
    if ((*shmem)->shm_fd == -1) {
        sim_shmem_close (*shmem);
        *shmem = NULL;
        return sim_messagef (SCPE_OPENERR, "Can't shm_open() a %d byte shared memory segment '%s' - errno=%d - %s\n", (int)size, name, last_errno, strerror (last_errno));
        }
    if (ftruncate((*shmem)->shm_fd, size)) {
        sim_shmem_close (*shmem);
        *shmem = NULL;
        return SCPE_OPENERR;
        }
    }
else {
    struct stat statb;

    if ((fstat ((*shmem)->shm_fd, &statb)) ||
        ((size_t)statb.st_size != (*shmem)->shm_size)) {
        sim_shmem_close (*shmem);
        *shmem = NULL;
        return sim_messagef (SCPE_OPENERR, "Shared Memory segment '%s' is %d bytes instead of %d\n", name, (int)(statb.st_size), (int)size);
        }
    }
(*shmem)->shm_base = mmap(NULL, (*shmem)->shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, (*shmem)->shm_fd, 0);
if ((*shmem)->shm_base == MAP_FAILED) {
    int last_errno = errno;

    sim_shmem_close (*shmem);
    *shmem = NULL;
    return sim_messagef (SCPE_OPENERR, "Shared Memory '%s' mmap() failed. errno=%d - %s\n", name, last_errno, strerror (last_errno));
    }
*addr = (*shmem)->shm_base;
return SCPE_OK;
#else
*shmem = NULL;
return sim_messagef (SCPE_NOFNC, "Shared memory not available - Missing shm_open() API\n");
#endif
}

void sim_shmem_close (SHMEM *shmem)
{
#if defined (HAVE_SHM_OPEN)
if (shmem == NULL)
    return;
if (shmem->shm_base != MAP_FAILED)
    munmap (shmem->shm_base, shmem->shm_size);
if (shmem->shm_fd != -1) {
    shm_unlink (shmem->shm_name);
    close (shmem->shm_fd);
    }
free (shmem->shm_name);
free (shmem);
#endif
}

int32 sim_shmem_atomic_add (int32 *p, int32 v)
{
#if defined (__GCC_HAVE_SYNC_COMPARE_AND_SWAP_4)
return __sync_add_and_fetch ((int *) p, v);
#else
return *p + v;
#endif
}

t_bool sim_shmem_atomic_cas (int32 *ptr, int32 oldv, int32 newv)
{
#if defined (__GCC_HAVE_SYNC_COMPARE_AND_SWAP_4)
return __sync_bool_compare_and_swap (ptr, oldv, newv);
#else
if (*ptr == oldv) {
    *ptr = newv;
    return 1;
    }
else
    return 0;
#endif
}

#else /* !(defined (__linux__) || defined (__APPLE__)) */

t_stat sim_shmem_open (const char *name, size_t size, SHMEM **shmem, void **addr)
{
return SCPE_NOFNC;
}

void sim_shmem_close (SHMEM *shmem)
{
}

int32 sim_shmem_atomic_add (int32 *p, int32 v)
{
return -1;
}

t_bool sim_shmem_atomic_cas (int32 *ptr, int32 oldv, int32 newv)
{
return FALSE;
}

#endif /* defined (__linux__) || defined (__APPLE__) */
#endif /* defined (_WIN32) */

#if defined(__VAX)
/* 
 * We privide a 'basic' snprintf, which 'might' overrun a buffer, but
 * the actual use cases don't on other platforms and none of the callers
 * care about the function return value.
 */
int sim_vax_snprintf(char *buf, size_t buf_size, const char *fmt, ...)
{
va_list arglist;

va_start (arglist, fmt);
vsprintf (buf, fmt, arglist);
va_end (arglist);
return 0;
}
#endif

char *sim_getcwd (char *buf, size_t buf_size)
{
#if defined (VMS)
return getcwd (buf, buf_size, 0);
#else
return getcwd (buf, buf_size);
#endif
}

/*
 * Parsing and expansion of file names.
 *
 *    %~I%        - expands filepath value removing any surrounding quotes (" or ')
 *    %~fI%       - expands filepath value to a fully qualified path name
 *    %~pI%       - expands filepath value to a path only
 *    %~nI%       - expands filepath value to a file name only
 *    %~xI%       - expands filepath value to a file extension only
 *
 * The modifiers can be combined to get compound results:
 *
 *    %~pnI%      - expands filepath value to a path and name only
 *    %~nxI%      - expands filepath value to a file name and extension only
 *
 * In the above example above %I% can be replaced by other 
 * environment variables or numeric parameters to a DO command
 * invocation.
 */

char *sim_filepath_parts (const char *filepath, const char *parts)
{
size_t tot_len = 0, tot_size = 0;
char *fullpath = NULL, *result = NULL;
char *c, *name, *ext;
char chr;
const char *p;
char filesizebuf[32] = "";
char filedatetimebuf[64] = "";
char namebuf[PATH_MAX + 1];


/* Expand ~/ home directory */
if (NULL == _sim_expand_homedir (filepath, namebuf, sizeof (namebuf)))
    return NULL;
filepath = namebuf;

/* Check for full or current directory relative path */
if ((filepath[1] == ':')  ||
    (filepath[0] == '/')  || 
    (filepath[0] == '\\')){
        tot_len = 1 + strlen (filepath);
        fullpath = (char *)malloc (tot_len);
        if (fullpath == NULL)
            return NULL;
        strcpy (fullpath, filepath);
    }
else {          /* Need to prepend current directory */
    char dir[PATH_MAX+1] = "";
    char *wd = sim_getcwd(dir, sizeof (dir));

    if (wd == NULL)
        return NULL;
    tot_len = 1 + strlen (filepath) + 1 + strlen (dir);
    fullpath = (char *)malloc (tot_len);
    if (fullpath == NULL)
        return NULL;
    strlcpy (fullpath, dir, tot_len);
    if ((dir[strlen (dir) - 1] != '/') &&       /* if missing a trailing directory separator? */
        (dir[strlen (dir) - 1] != '\\'))
        strlcat (fullpath, "/", tot_len);       /*  then add one */
    strlcat (fullpath, filepath, tot_len);
    }
while ((c = strchr (fullpath, '\\')))           /* standardize on / directory separator */
       *c = '/';
if ((fullpath[1] == ':') && islower (fullpath[0]))
    fullpath[0] = toupper (fullpath[0]);
while ((c = strstr (fullpath + 1, "//")))       /* strip out redundant / characters (leaving the option for a leading //) */
       memmove (c, c + 1, 1 + strlen (c + 1));
while ((c = strstr (fullpath, "/./")))          /* strip out irrelevant /./ sequences */
       memmove (c, c + 2, 1 + strlen (c + 2));
while ((c = strstr (fullpath, "/../"))) {       /* process up directory climbing */
    char *cl = c - 1;

    while ((*cl != '/') && (cl > fullpath))
        --cl;
    if ((cl <= fullpath) ||                      /* Digest Leading /../ sequences */
        ((fullpath[1] == ':') && (c == fullpath + 2)))
        memmove (c, c + 3, 1 + strlen (c + 3)); /* and removing intervening elements */
    else
        if (*cl == '/')
            memmove (cl, c + 3, 1 + strlen (c + 3));/* and removing intervening elements */
        else
            break;
    }
if (!strrchr (fullpath, '/'))
    name = fullpath + strlen (fullpath);
else
    name = 1 + strrchr (fullpath, '/');
ext = strrchr (name, '.');
if (ext == NULL)
    ext = name + strlen (name);
tot_size = 0;
if (*parts == '\0')             /* empty part specifier means strip only quotes */
    tot_size = strlen (filepath);
if (strchr (parts, 't') ||      /* modification time or */
    strchr (parts, 'z')) {      /* or size requested? */
    struct stat filestat;
    struct tm *tm;

    memset (&filestat, 0, sizeof (filestat));
    (void)stat (fullpath, &filestat);
    if (sizeof (filestat.st_size) == 4)
        sprintf (filesizebuf, "%ld ", (long)filestat.st_size);
    else
        sprintf (filesizebuf, "%" LL_FMT "d ", (LL_TYPE)filestat.st_size);
    tm = localtime (&filestat.st_mtime);
    snprintf (filedatetimebuf, sizeof (filedatetimebuf), "%02d/%02d/%04d %02d:%02d %cM ", 1 + tm->tm_mon, tm->tm_mday, 1900 + tm->tm_year,
                                                              tm->tm_hour % 12, tm->tm_min, (0 == (tm->tm_hour % 12)) ? 'A' : 'P');
    }
for (p = parts; *p; p++) {
    switch (*p) {
        case 'f':
            tot_size += strlen (fullpath);
            break;
        case 'p':
            tot_size += name - fullpath;
            break;
        case 'n':
            tot_size += ext - name;
            break;
        case 'x':
            tot_size += strlen (ext);
            break;
        case 't':
            tot_size += strlen (filedatetimebuf);
            break;
        case 'z':
            tot_size += strlen (filesizebuf);
            break;
        }
    }
result = (char *)malloc (1 + tot_size);
*result = '\0';
if (*parts == '\0')             /* empty part specifier means strip only quotes */
    strlcat (result, filepath, 1 + tot_size);
for (p = parts; *p; p++) {
    switch (*p) {
        case 'f':
            strlcat (result, fullpath, 1 + tot_size);
            break;
        case 'p':
            chr = *name;
            *name = '\0';
            strlcat (result, fullpath, 1 + tot_size);
            *name = chr;
            break;
        case 'n':
            chr = *ext;
            *ext = '\0';
            strlcat (result, name, 1 + tot_size);
            *ext = chr;
            break;
        case 'x':
            strlcat (result, ext, 1 + tot_size);
            break;
        case 't':
            strlcat (result, filedatetimebuf, 1 + tot_size);
            break;
        case 'z':
            strlcat (result, filesizebuf, 1 + tot_size);
            break;
        }
    }
free (fullpath);
return result;
}

/*
 * relative file path processing
 *
 *    Input is a filepath which may contain either / or \ directory 
 *    separators (or both) and always returns a relative or complete path
 *    with / directory separators.
 */

const char *sim_relative_path (const char *filenamepath)
{
char dir[PATH_MAX+1] = "";
char *wd = sim_getcwd(dir, sizeof (dir));
char dsep = (strchr (dir, '/') != NULL) ? '/' : '\\';
char *cp;
static char buf[CBUFSIZE*4];
char *filepath = NULL;
char fsep = (strchr (filenamepath, '\\') != NULL) ? '\\' : '/';
char updir[4] = {'.', '.', fsep};
size_t offset = 0, lastdir = 0, updirs = 0, up, cwd_dirs;

filepath = sim_filepath_parts (filenamepath, "f");
if (strchr (filepath, fsep) == NULL) {  /* file directory path separators changed? */
    char csep = (fsep == '/') ? '\\' : '/';

    while ((cp = strchr (filepath, csep)))
        *cp = fsep;                     /* restore original file path separator */
    }
if (dsep != fsep) {                     /* if directory path separators aren't the same */
    while ((cp = strchr (dir, dsep)) != NULL)
        *cp = fsep;                     /* change to the file path separator */
    dsep = fsep;
    }
cp = dir - 1;
cwd_dirs = (isalpha(dir[0]) && (dir[1] == ':')) ? 1 : 0;
while ((cp = strchr (cp + 1, fsep)) != NULL)
    cwd_dirs++;
if (dir[strlen (dir) - 1] != fsep)
    cwd_dirs++;
buf[0] = '\0';
while ((dir[offset] != '\0') && (filepath[offset] != '\0')) {
    if (dir[offset] == dsep) {
        if (filepath[offset] == fsep) {
            lastdir = offset;
            ++offset;
            continue;
            }
        }

#if defined(_WIN32)                     /* Windows has case independent file names */
#define _CMP(x) toupper (x)
#else
#define _CMP(x) (x)
#endif
    if (_CMP(dir[offset]) != _CMP(filepath[offset]))
        break;
    ++offset;
    }
if (dir[offset] == '\0') {
    if (filepath[offset] == fsep) {
        lastdir = offset;
        ++offset;
        updirs = 0;
        }
    else {
        offset = lastdir + 1;
        updirs = 1;
        }
    }
else {
    updirs = 1;
    while (dir[++lastdir] != '\0') {
        if (dir[lastdir] == fsep)
            ++updirs;
        }
    }
if (updirs > 0) {
    if ((offset == 3) &&                /* if only match windows drive letter? */
        (dir[1] == ':') && 
        (dir[2] == dsep))
        offset = 0;                     /* */
    if ((offset > 0) && (updirs != cwd_dirs)) {
        for (up = 0; up < updirs; up++)
            strlcat (buf, updir, sizeof (buf));
        if (strlen (buf) > offset) {    /* if relative path prefix is longer than input path? */
            offset = 0;                 /* revert to original path */
            buf[0] = '\0';
            }
        }
    }
else
    strlcpy (buf, "./", sizeof (buf));
strlcat (buf, &filepath[offset], sizeof (buf));
if (dsep != '/')
    while ((cp = strchr (buf, dsep)) != NULL)
        *cp = '/';                      /* Always return with / as the directory separator */
free (filepath);
return buf;
}

#if defined (_WIN32)

t_stat sim_dir_scan (const char *cptr, DIR_ENTRY_CALLBACK entry, void *context)
{
HANDLE hFind;
WIN32_FIND_DATAA File;
struct stat filestat;
char WildName[PATH_MAX + 1];

if (NULL == _sim_expand_homedir (cptr, WildName, sizeof (WildName)))
    return SCPE_ARG;
cptr = WildName;
sim_trim_endspc (WildName);
if ((hFind =  FindFirstFileA (cptr, &File)) != INVALID_HANDLE_VALUE) {
    t_int64 FileSize;
    char DirName[PATH_MAX + 1], FileName[PATH_MAX + 1];
    char *c;
    const char *backslash = strchr (cptr, '\\');
    const char *slash = strchr (cptr, '/');
    const char *pathsep = (backslash && slash) ? MIN (backslash, slash) : (backslash ? backslash : slash);

    GetFullPathNameA(cptr, sizeof(DirName), DirName, (char **)&c);
    c = strrchr (DirName, '\\');
    *c = '\0';                                  /* Truncate to just directory path */
    if (!pathsep ||                             /* Separator wasn't mentioned? */
        (slash && (0 == strcmp (slash, "/*")))) 
        pathsep = "\\";                         /* Default to Windows backslash */
    if (*pathsep == '/') {                      /* If slash separator? */
        while ((c = strchr (DirName, '\\')))
            *c = '/';                           /* Convert backslash to slash */
        }
    sprintf (&DirName[strlen (DirName)], "%c", *pathsep);
    do {
        FileSize = (((t_int64)(File.nFileSizeHigh)) << 32) | File.nFileSizeLow;
        strlcpy (FileName, DirName, sizeof (FileName));
        strlcat (FileName, File.cFileName, sizeof (FileName));
        stat (FileName, &filestat);
        entry (DirName, File.cFileName, FileSize, &filestat, context);
        } while (FindNextFileA (hFind, &File));
    FindClose (hFind);
    }
else
    return SCPE_ARG;
return SCPE_OK;
}

#else /* !defined (_WIN32) */

#include <sys/stat.h>
#if defined (HAVE_GLOB)
#include <glob.h>
#else /* !defined (HAVE_GLOB) */
#include <dirent.h>
#if defined (HAVE_FNMATCH)
#include <fnmatch.h>
#endif
#endif /* defined (HAVE_GLOB) */

t_stat sim_dir_scan (const char *cptr, DIR_ENTRY_CALLBACK entry, void *context)
{
#if defined (HAVE_GLOB)
glob_t  paths;
#else
DIR *dir;
#endif
int found_count = 0;
struct stat filestat;
char *c;
char DirName[PATH_MAX + 1], WholeName[PATH_MAX + 1], WildName[PATH_MAX + 1], MatchName[PATH_MAX + 1];

memset (DirName, 0, sizeof(DirName));
memset (WholeName, 0, sizeof(WholeName));
memset (MatchName, 0, sizeof(MatchName));
if (NULL == _sim_expand_homedir (cptr, WildName, sizeof (WildName)))
    return SCPE_ARG;
cptr = WildName;
sim_trim_endspc (WildName);
c = sim_filepath_parts (cptr, "f");
strlcpy (WholeName, c, sizeof (WholeName));
free (c);
c = sim_filepath_parts (cptr, "nx");
strlcpy (MatchName, c, sizeof (MatchName));
free (c);
c = strrchr (WholeName, '/');
if (c) {
    memmove (DirName, WholeName, 1+c-WholeName);
    DirName[2+c-WholeName] = '\0';  /* Terminate after the path separator */
    }
else
    DirName[0] = '\0';
cptr = WholeName;
#if defined (HAVE_GLOB)
memset (&paths, 0, sizeof (paths));
if (0 == glob (cptr, 0, NULL, &paths)) {
#else
dir = opendir(DirName[0] ? DirName : "/.");
if (dir) {
    struct dirent *ent;
#endif
    t_offset FileSize;
    char *FileName;
     char *p_name;
#if defined (HAVE_GLOB)
    size_t i;
#endif

#if defined (HAVE_GLOB)
    for (i=0; i<paths.gl_pathc; i++) {
        FileName = (char *)malloc (1 + strlen (paths.gl_pathv[i]));
        sprintf (FileName, "%s", paths.gl_pathv[i]);
#else /* !defined (HAVE_GLOB) */
    while ((ent = readdir (dir))) {
#if defined (HAVE_FNMATCH)
        if (fnmatch(MatchName, ent->d_name, 0))
            continue;
#else /* !defined (HAVE_FNMATCH) */
        /* only match all names or exact name without fnmatch support */
        if ((strcmp(MatchName, "*") != 0) &&
            (strcmp(MatchName, ent->d_name) != 0))
            continue;
#endif /* defined (HAVE_FNMATCH) */
        FileName = (char *)malloc (1 + strlen (DirName) + strlen (ent->d_name));
        sprintf (FileName, "%s%s", DirName, ent->d_name);
#endif /* defined (HAVE_GLOB) */
        p_name = FileName + strlen (DirName);
        memset (&filestat, 0, sizeof (filestat));
        (void)stat (FileName, &filestat);
        FileSize = (t_offset)((filestat.st_mode & S_IFDIR) ? 0 : sim_fsize_name_ex (FileName));
        entry (DirName, p_name, FileSize, &filestat, context);
        free (FileName);
        ++found_count;
        }
#if defined (HAVE_GLOB)
    globfree (&paths);
#else
    closedir (dir);
#endif
    }
else
    return SCPE_ARG;
if (found_count)
    return SCPE_OK;
else
    return SCPE_ARG;
}
#endif /* !defined(_WIN32) */

/* Trim trailing spaces from a string

    Inputs:
        cptr    =       pointer to string
    Outputs:
        cptr    =       pointer to string
*/

char *sim_trim_endspc (char *cptr)
{
char *tptr;

tptr = cptr + strlen (cptr);
while ((--tptr >= cptr) && sim_isspace (*tptr))
    *tptr = 0;
return cptr;
}

int sim_isspace (int c)
{
return ((c < 0) || (c >= 128)) ? 0 : isspace (c);
}

int sim_islower (int c)
{
return (c >= 'a') && (c <= 'z');
}

int sim_isupper (int c)
{
return (c >= 'A') && (c <= 'Z');
}

int sim_toupper (int c)
{
return ((c >= 'a') && (c <= 'z')) ? ((c - 'a') + 'A') : c;
}

int sim_tolower (int c)
{
return ((c >= 'A') && (c <= 'Z')) ? ((c - 'A') + 'a') : c;
}

int sim_isalpha (int c)
{
return ((c < 0) || (c >= 128)) ? 0 : isalpha (c);
}

int sim_isprint (int c)
{
return ((c < 0) || (c >= 128)) ? 0 : isprint (c);
}

int sim_isdigit (int c)
{
return ((c >= '0') && (c <= '9'));
}

int sim_isgraph (int c)
{
return ((c < 0) || (c >= 128)) ? 0 : isgraph (c);
}

int sim_isalnum (int c)
{
return ((c < 0) || (c >= 128)) ? 0 : isalnum (c);
}

/* strncasecmp() is not available on all platforms */
int sim_strncasecmp (const char* string1, const char* string2, size_t len)
{
size_t i;
unsigned char s1, s2;

for (i=0; i<len; i++) {
    s1 = (unsigned char)string1[i];
    s2 = (unsigned char)string2[i];
    s1 = (unsigned char)sim_toupper (s1);
    s2 = (unsigned char)sim_toupper (s2);
    if (s1 < s2)
        return -1;
    if (s1 > s2)
        return 1;
    if (s1 == 0)
        return 0;
    }
return 0;
}

/* strcasecmp() is not available on all platforms */
int sim_strcasecmp (const char *string1, const char *string2)
{
size_t i = 0;
unsigned char s1, s2;

while (1) {
    s1 = (unsigned char)string1[i];
    s2 = (unsigned char)string2[i];
    s1 = (unsigned char)sim_toupper (s1);
    s2 = (unsigned char)sim_toupper (s2);
    if (s1 == s2) {
        if (s1 == 0)
            return 0;
        i++;
        continue;
        }
    if (s1 < s2)
        return -1;
    if (s1 > s2)
        return 1;
    }
return 0;
}

int sim_strwhitecasecmp (const char *string1, const char *string2, t_bool casecmp)
{
unsigned char s1 = 1, s2 = 1;   /* start with equal, but not space */

while ((s1 == s2) && (s1 != '\0')) {
    if (s1 == ' ') {            /* last character was space? */
        while (s1 == ' ') {     /* read until not a space */
            s1 = *string1++;
            if (sim_isspace (s1))
                s1 = ' ';       /* all whitespace is a space */
            else {
                if (casecmp)
                    s1 = (unsigned char)sim_toupper (s1);
                }
            }
        }
    else {                      /* get new character */
        s1 = *string1++;
        if (sim_isspace (s1))
            s1 = ' ';           /* all whitespace is a space */
        else {
            if (casecmp)
                s1 = (unsigned char)sim_toupper (s1);
            }
        }
    if (s2 == ' ') {            /* last character was space? */
        while (s2 == ' ') {     /* read until not a space */
            s2 = *string2++;
            if (sim_isspace (s2))
                s2 = ' ';       /* all whitespace is a space */
            else {
                if (casecmp)
                    s2 = (unsigned char)sim_toupper (s2);
                }
            }
        }
    else {                      /* get new character */
        s2 = *string2++;
        if (sim_isspace (s2))
            s2 = ' ';           /* all whitespace is a space */
        else {
            if (casecmp)
                s2 = (unsigned char)sim_toupper (s2);
            }
        }
    if (s1 == s2) {
        if (s1 == 0)
            return 0;
        continue;
        }
    if (s1 < s2)
        return -1;
    if (s1 > s2)
        return 1;
    }
return 0;
}

/* strlcat() and strlcpy() are not available on all platforms */
/* Copyright (c) 1998 Todd C. Miller <Todd.Miller@courtesan.com> */
/*
 * Appends src to string dst of size siz (unlike strncat, siz is the
 * full size of dst, not space left).  At most siz-1 characters
 * will be copied.  Always NUL terminates (unless siz <= strlen(dst)).
 * Returns strlen(src) + MIN(siz, strlen(initial dst)).
 * If retval >= siz, truncation occurred.
 */
size_t sim_strlcat(char *dst, const char *src, size_t size)
{
char *d = dst;
const char *s = src;
size_t n = size;
size_t dlen;

/* Find the end of dst and adjust bytes left but don't go past end */
while (n-- != 0 && *d != '\0')
    d++;
dlen = d - dst;
n = size - dlen;

if (n == 0)
    return (dlen + strlen(s));
while (*s != '\0') {
    if (n != 1) {
        *d++ = *s;
        n--;
        }
    s++;
    }
*d = '\0';

return (dlen + (s - src));          /* count does not include NUL */
}

/*
 * Copy src to string dst of size siz.  At most siz-1 characters
 * will be copied.  Always NUL terminates (unless siz == 0).
 * Returns strlen(src); if retval >= siz, truncation occurred.
 */
size_t sim_strlcpy (char *dst, const char *src, size_t size)
{
char *d = dst;
const char *s = src;
size_t n = size;

/* Copy as many bytes as will fit */
if (n != 0) {
    while (--n != 0) {
        if ((*d++ = *s++) == '\0')
            break;
        }
    }

    /* Not enough room in dst, add NUL and traverse rest of src */
    if (n == 0) {
        if (size != 0)
            *d = '\0';              /* NUL-terminate dst */
        while (*s++)
            ;
        }
return (s - src - 1);               /* count does not include NUL */
}

