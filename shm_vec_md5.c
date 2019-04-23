/*
 * This file contains a program to search every Sublen character
 * substring in a set of text files from the Project Gutenberg
 * repository for a substring whose MD5 sum matches the given
 * target sum.  The text file are preloaded into a shared
 * memory segment called "/gutenberg"
 *
 * MD5 sums are calculated using C code copied from the Linux
 * md5sum command source which is in the coreutils package.
 * Sums are calculated 8 at a time using the AVX vector
 * extensions.
 *
 * The program spawns 1 to 64 threads to look for the
 * matching MD5 sum.
 *
 */

/*
 *
 * Usage:  This program is intended to be run on a
 * multicore machine.  The load is divided by having
 * each thread/instance of this program process a
 * subset of /gutenberg.  Command line parameter 1
 * gives how many threads we want to spawn.
 *    time shm_vec_md5 8 xxxxxxxxxxxxxxxxxxxxxxxxxxxxx
 */

/*
 * Build as: gcc -o shm_vec_md5 shm_vec_md5.c -lpthread -lrt -march=broadwell -mavx2 -O3
 */


#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <ctype.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <string.h>
#include <unistd.h>



/****************************** DEFINES ****************************/
const char *filelist = "filelist";
const char *hexdigits = "0123456789abcdef";
#define MAXNAMELEN 512
#define MXTHRD      64      /* Limit the number of threads */
#define MD5_DIGEST_LENGTH 16
        // The length of the target substring
#define Sublen     (22)

typedef struct {
    pthread_t   thread_id;  // returned from creat()
    int         thread_idx; // index in range 0 to Nthread
} THRDINFO;

    /* vector definition for 8 unsigned ints */
typedef uint32_t v8vecui __attribute__ ((vector_size (32)));

union v8ui {
    v8vecui  v;    // vector
    uint32_t s[8]; // scalar equivalent
};

union targetmd5 {
    // note that 4 is MD5_DIGEST_LENGTH/sizeof(uint32_t)
    uint32_t    i[4];       // target MD5 sum  as 4 ints
    uint8_t     c[16];      // target MD5 sum as 16 chars
};

    /* Macros to make reading the MD5 computation easier */
#define F(b,c,d)        ((((c) ^ (d)) & (b)) ^ (d))
#define G(b,c,d)        ((((b) ^ (c)) & (d)) ^ (c))
#define H(b,c,d)        ((b) ^ (c) ^ (d))
#define I(b,c,d)        (((~(d)) | (b)) ^ (c))

#define R0(a,b,c,d,k,s,t) { \
        a+=((k)+(t)+F((b),(c),(d))); \
        a=ROTATE(a,s); \
        a+=b; };\

#define R1(a,b,c,d,k,s,t) { \
        a+=((k)+(t)+G((b),(c),(d))); \
        a=ROTATE(a,s); \
        a+=b; };

#define R2(a,b,c,d,k,s,t) { \
        a+=((k)+(t)+H((b),(c),(d))); \
        a=ROTATE(a,s); \
        a+=b; };

#define R3(a,b,c,d,k,s,t) { \
        a+=((k)+(t)+I((b),(c),(d))); \
        a=ROTATE(a,s); \
        a+=b; };

#define ROTATE(a, s) ((a << s) + (a >> (32 - s)))



/************************** GLOBAL VARIABLES ***********************/
int         Nthread;        // number of threads working on the file list 
THRDINFO    Thrds [MXTHRD]; // table of thread indicies
union targetmd5 Findme;     // the md5 sum to locate
char       *Dataset;        // All files copied to shared memory
int         Fdshm;          // file descriptor for shared memory
int         Shmlen;         // length of the shared memory segment


/************************* FORWARD REFERENCES **********************/
void *do_vshm(void *);
int vmd5_19(union v8ui X[], union targetmd5);




int main(int argc, char *argv[])
{
    int          i;           // generic loop counter
    char         c;           // generic char varible
    int          hex;         // hex digit in the input sum
    int          ret;         // generic int return value from a system call
    int         *vret;        // generic void pointer return value from a system call
    struct stat  shmstat;     // info about the shared memory segment

    /* sanity check */
    if ((argc !=  3) ||
        (sscanf(argv[1], "%d", &Nthread) != 1) ||
        (Nthread <= 0) || (Nthread >= MXTHRD) ||
        (Sublen < 19) || (Sublen > 55)) {
        printf("Usage: %s <Num-threads> <target MD5 sum>\n", argv[0]);
        exit(1);
    }
    if ((2 * MD5_DIGEST_LENGTH) != strnlen(argv[2], MD5_DIGEST_LENGTH +100)) { 
        printf("Invalid target MD5 sum length\n");
        printf("Usage: %s <num-threads> <MD5 checksum to locate>\n", argv[0]);
        exit(1);
    }
    for (i = 0; i < (2 * MD5_DIGEST_LENGTH); i++) {
        if (isxdigit(argv[2][i]) == 0) {
            printf("Invalid target MD5 sum character '%c'\n", argv[2][i]);
            printf("Usage: %s <num-threads> <MD5 checksum to locate>\n", argv[0]);
            exit(1);
        }
    }
    /* At this point we know that there are three arguments and the third
     * one has the length of an MD5 sum, and has only hex characters in in. */

    /* Put the sum from the command line into an array */
    Findme.i[0] = Findme.i[1] = Findme.i[2] = Findme.i[3] = 0;
    for (i = 0 ; i < MD5_DIGEST_LENGTH; i++) {
        c = tolower(argv[2][(2 * i)]);
        if (c <= '9')
            hex = (int)(c - '0');
        else
            hex = 10 + (int)(c - 'a');
        Findme.c[i] = hex << 4;
        c = tolower(argv[2][(2 * i) + 1]);
        if (c <= '9')
            hex = (int)(c - '0');
        else
            hex = 10 + (int)(c - 'a');
        Findme.c[i] = Findme.c[i] + hex;
    }

    /* Open the shared memory segment /gutenberg and map into our address space */
    Fdshm = shm_open("/gutenberg", O_RDONLY, 0666);
    if (Fdshm < 0) {
        printf("unable to open shared memory segment\n");
        perror(NULL);
        exit(-1);
    }
    ret = fstat(Fdshm, &shmstat);
    if (ret < 0) {
        printf("error getting /gutenberg info\n");
        perror(NULL);
        exit(1);
    }
    Shmlen = shmstat.st_size;

    /* Mamory map the data set */
    vret = mmap((void *) 0, Shmlen, PROT_READ, MAP_SHARED, Fdshm, 0);
    if (vret < 0) {
        printf("Unable to mmap the data set shared segment\n");
        perror(NULL);
        exit(1);
    }
    Dataset = (char *) vret;

    /* Create n threads */
    for (i = 0; i < Nthread; i++) {
        Thrds[i].thread_idx = i;
        ret = pthread_create( &(Thrds[i].thread_id), NULL, do_vshm,
              (void *) &(Thrds[i].thread_idx));
        if(ret != 0) {
            fprintf(stderr,"Error - pthread_create() return code: %d\n", ret);
            exit(-1);
        }
    }

    /* Wait for the threads to return.  If a thread finds a match it will print the
     * matching string and call exit().  We only get to the bottom of the joins if
     * the target string was not in the text files. */
    for (i = 0; i < Nthread; i++) {
        pthread_join(Thrds[i].thread_id, NULL);
    }
    printf("Target MD5 sum is not found\n");

    exit(0);
}



/*
 * do_vshm() : search for the Findme md5 sum
 */
void *do_vshm(void *pidx)
{
    int           myidx;       // our thread index
    char         *mydata;      // where we start scanning
    int           mylen;       // how many bytes we should scan
    int           cinx;        // index into data of the shm
    int           i,j;         // generic loop index
    union v8ui X[16];          // Eight 512 bit vectors of input strings
    int           match;       // index of a matching string
    uint32_t      maskor;      // mask onto end of string
    uint32_t      maskand;     // mask from end of string


    // The input string
    (void) memset((void *)X, 0, (size_t)(16 * sizeof(union v8ui)));
    X[14].s[0] = X[14].s[1] = X[14].s[2] = X[14].s[3] = \
    X[14].s[4] = X[14].s[5] = X[14].s[6] = X[14].s[7] = Sublen * 8 ;


    myidx = *((int *)pidx);

    /* The dataset at Dataset is of length Shmlen.  This length is
     * actually padded with 100 bytes (at the bottom of shm_init.c).
     * Our thread want to process (Shmlen -100) / Nthread characters
     * but we also want to scan a little into the next thread's 
     * data (or into the pad if the last thread). */

    mydata = Dataset + myidx * ((Shmlen -100) / Nthread);
    mylen  = ((Shmlen -100) / Nthread) + Sublen; 
    // compute end of messsage bit
    if (Sublen % 4 == 3) {
        maskand = 0x00FFFFFF;
        maskor = 0x80000000;
    }
    else if (Sublen % 4 == 2) {
        maskand = 0x0000FFFF;
        maskor = 0x00800000;
    }
    else if (Sublen % 4 == 1) {
        maskand = 0x000000FF;
        maskor = 0x00008000;
    }
    else {
        maskand = 0x00000000;
        maskor = 0x00000080;
    }

    /* Walk the file processing every Sublen character substring, 8 at a time */
    for (cinx = 0; cinx < mylen; cinx += 8) {
        for (i = 0; i < 8; i++) {
            for (j = 0; j < (Sublen / sizeof(uint32_t)) + 1; j++) { 
                X[j].s[i] = *((int *)(mydata + cinx + (j * 4) + i));
            }
            X[j - 1].s[i] &= maskand;
            X[j - 1].s[i] |= maskor;
        }
        match = vmd5_19(X, Findme);
        if (match >= 0) {
            printf("Match with string '");
            for (i = 0; i < Sublen; i++)
                putchar(*(mydata + cinx + match + i));
            printf("'\n");
            exit(0);
        }

        /* do not scan within Sublen character of a line end */
        /* Comparisons are unrolled here */
        for (i = 0; i < 8; i++) {
            if (*(mydata + cinx + Sublen + i) == (char) 0) {
                // skip to char after null (+1) backup 8 for cinx+=8
                cinx = cinx + Sublen + i + 1 - 8;
                continue;
            }
        }
    }
}



/*
 * Compute 8 parallel MD5 sums on the 8x32 char input array X.
 * Compare the sums to 'target' and return the index number (0-7)
 * of a string that matches.  Return -1 if no match is found.
 */
int vmd5_19(union v8ui X[], union targetmd5 target)
{
    uint32_t  i;
    // md5 state for a given chuck
    union v8ui A;
    union v8ui B;
    union v8ui C;
    union v8ui D;
    union v8ui a0;
    union v8ui b0;
    union v8ui c0;
    union v8ui d0;

    //Initialize variables:
    A.s[0] = A.s[1] = A.s[2] = A.s[3] = A.s[4] = A.s[5] = A.s[6] = A.s[7] = 0x67452301;
    B.s[0] = B.s[1] = B.s[2] = B.s[3] = B.s[4] = B.s[5] = B.s[6] = B.s[7] = 0xefcdab89;
    C.s[0] = C.s[1] = C.s[2] = C.s[3] = C.s[4] = C.s[5] = C.s[6] = C.s[7] = 0x98badcfe;
    D.s[0] = D.s[1] = D.s[2] = D.s[3] = D.s[4] = D.s[5] = D.s[6] = D.s[7] = 0x10325476;
    a0.v = A.v;
    b0.v = B.v;
    c0.v = C.v;
    d0.v = D.v;


    /* Round 0 */
    R0(A.v, B.v, C.v, D.v, X[0].v, 7, 0xd76aa478);
    R0(D.v, A.v, B.v, C.v, X[1].v, 12, 0xe8c7b756);
    R0(C.v, D.v, A.v, B.v, X[2].v, 17, 0x242070db);
    R0(B.v, C.v, D.v, A.v, X[3].v, 22, 0xc1bdceee);
    R0(A.v, B.v, C.v, D.v, X[4].v, 7, 0xf57c0faf);
    R0(D.v, A.v, B.v, C.v, X[5].v, 12, 0x4787c62a);
    R0(C.v, D.v, A.v, B.v, X[6].v, 17, 0xa8304613);
    R0(B.v, C.v, D.v, A.v, X[7].v, 22, 0xfd469501);
    R0(A.v, B.v, C.v, D.v, X[8].v, 7, 0x698098d8);
    R0(D.v, A.v, B.v, C.v, X[9].v, 12, 0x8b44f7af);
    R0(C.v, D.v, A.v, B.v, X[10].v, 17, 0xffff5bb1);
    R0(B.v, C.v, D.v, A.v, X[11].v, 22, 0x895cd7be);
    R0(A.v, B.v, C.v, D.v, X[12].v, 7, 0x6b901122);
    R0(D.v, A.v, B.v, C.v, X[13].v, 12, 0xfd987193);
    R0(C.v, D.v, A.v, B.v, X[14].v, 17, 0xa679438e);
    R0(B.v, C.v, D.v, A.v, X[15].v, 22, 0x49b40821);
    /* Round 1 */
    R1(A.v, B.v, C.v, D.v, X[1].v, 5, 0xf61e2562);
    R1(D.v, A.v, B.v, C.v, X[6].v, 9, 0xc040b340);
    R1(C.v, D.v, A.v, B.v, X[11].v, 14, 0x265e5a51);
    R1(B.v, C.v, D.v, A.v, X[0].v, 20, 0xe9b6c7aa);
    R1(A.v, B.v, C.v, D.v, X[5].v, 5, 0xd62f105d);
    R1(D.v, A.v, B.v, C.v, X[10].v, 9, 0x02441453);
    R1(C.v, D.v, A.v, B.v, X[15].v, 14, 0xd8a1e681);
    R1(B.v, C.v, D.v, A.v, X[4].v, 20, 0xe7d3fbc8);
    R1(A.v, B.v, C.v, D.v, X[9].v, 5, 0x21e1cde6);
    R1(D.v, A.v, B.v, C.v, X[14].v, 9, 0xc33707d6);
    R1(C.v, D.v, A.v, B.v, X[3].v, 14, 0xf4d50d87);
    R1(B.v, C.v, D.v, A.v, X[8].v, 20, 0x455a14ed);
    R1(A.v, B.v, C.v, D.v, X[13].v, 5, 0xa9e3e905);
    R1(D.v, A.v, B.v, C.v, X[2].v, 9, 0xfcefa3f8);
    R1(C.v, D.v, A.v, B.v, X[7].v, 14, 0x676f02d9);
    R1(B.v, C.v, D.v, A.v, X[12].v, 20, 0x8d2a4c8a);
    /* Round 2 */
    R2(A.v, B.v, C.v, D.v, X[5].v, 4, 0xfffa3942);
    R2(D.v, A.v, B.v, C.v, X[8].v, 11, 0x8771f681);
    R2(C.v, D.v, A.v, B.v, X[11].v, 16, 0x6d9d6122);
    R2(B.v, C.v, D.v, A.v, X[14].v, 23, 0xfde5380c);
    R2(A.v, B.v, C.v, D.v, X[1].v, 4, 0xa4beea44);
    R2(D.v, A.v, B.v, C.v, X[4].v, 11, 0x4bdecfa9);
    R2(C.v, D.v, A.v, B.v, X[7].v, 16, 0xf6bb4b60);
    R2(B.v, C.v, D.v, A.v, X[10].v, 23, 0xbebfbc70);
    R2(A.v, B.v, C.v, D.v, X[13].v, 4, 0x289b7ec6);
    R2(D.v, A.v, B.v, C.v, X[0].v, 11, 0xeaa127fa);
    R2(C.v, D.v, A.v, B.v, X[3].v, 16, 0xd4ef3085);
    R2(B.v, C.v, D.v, A.v, X[6].v, 23, 0x04881d05);
    R2(A.v, B.v, C.v, D.v, X[9].v, 4, 0xd9d4d039);
    R2(D.v, A.v, B.v, C.v, X[12].v, 11, 0xe6db99e5);
    R2(C.v, D.v, A.v, B.v, X[15].v, 16, 0x1fa27cf8);
    R2(B.v, C.v, D.v, A.v, X[2].v, 23, 0xc4ac5665);
    /* Round 3 */
    R3(A.v, B.v, C.v, D.v, X[0].v, 6, 0xf4292244);
    R3(D.v, A.v, B.v, C.v, X[7].v, 10, 0x432aff97);
    R3(C.v, D.v, A.v, B.v, X[14].v, 15, 0xab9423a7);
    R3(B.v, C.v, D.v, A.v, X[5].v, 21, 0xfc93a039);
    R3(A.v, B.v, C.v, D.v, X[12].v, 6, 0x655b59c3);
    R3(D.v, A.v, B.v, C.v, X[3].v, 10, 0x8f0ccc92);
    R3(C.v, D.v, A.v, B.v, X[10].v, 15, 0xffeff47d);
    R3(B.v, C.v, D.v, A.v, X[1].v, 21, 0x85845dd1);
    R3(A.v, B.v, C.v, D.v, X[8].v, 6, 0x6fa87e4f);
    R3(D.v, A.v, B.v, C.v, X[15].v, 10, 0xfe2ce6e0);
    R3(C.v, D.v, A.v, B.v, X[6].v, 15, 0xa3014314);
    R3(B.v, C.v, D.v, A.v, X[13].v, 21, 0x4e0811a1);
    R3(A.v, B.v, C.v, D.v, X[4].v, 6, 0xf7537e82);
    R3(D.v, A.v, B.v, C.v, X[11].v, 10, 0xbd3af235);
    R3(C.v, D.v, A.v, B.v, X[2].v, 15, 0x2ad7d2bb);
    R3(B.v, C.v, D.v, A.v, X[9].v, 21, 0xeb86d391);

    //Add this chunk's hash to result so far:
    A.v = A.v + a0.v;
    B.v = B.v + b0.v;
    C.v = C.v + c0.v;
    D.v = D.v + d0.v;

    for (i = 0; i < 8; i++) {
        if ((target.i[0] == A.s[i]) && (target.i[1] == B.s[i]) &&
            (target.i[2] == C.s[i]) && (target.i[3] == D.s[i])) {
            return(i);
        }
    }
    return(-1);
}


