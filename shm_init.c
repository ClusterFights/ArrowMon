/*
 * This file is part of a program to search every 19 character
 * substring in a set of text files from the Project Gutenberg
 * repository for a substring whose MD5 sum matches the given
 * target sum.
 *
 * MD5 sums are calculated using C code copied from the Linux
 * md5sum command source which is in the coreutils package.
 *
 * The program spawns 1 to 64 threads to look for the
 * matching MD5 sum.
 *
 * The source text files are located in /mnt/md5/text/
 * and a list of full paths of the text files is in
 * /mnt/md5/filelist.  We scan each of the files in turn
 * looking for the target checksum.  The file set is
 * 400MB in size.
 *
 * This helper utility allocates 400MB of memory and copies
 * the files into a named shared memory segment.  The 
 * actual search routines attach the shared memory and
 * scan the files from memory.
 *
 */

/*
 * Run this program after getting a copy of the dataset
 * and before running the actual search program.  Invoke as:
 *    shm_init <# char in target string>
 */

/*
 * Build as: gcc -o shm_init shm_init.c  -lrt
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
#include <strings.h>
#include <unistd.h>



/****************************** DEFINES ****************************/
//const char *filelist = "/mnt/md5/filelist";
const char *filelist = "filelist";
const char *hexdigits = "0123456789abcdef";
#define MAXNAMELEN 512
#define DATASETSZ  450000000
#define MAXLINE    2048


/************************** GLOBAL VARIABLES ***********************/
int         Nfiles;         // total # files to process
char       *Namearray;      // Location of filelist in our virtual memory
char       *Dataset;        // All files copied to shared memory
int         Fdshm;          // FD to opened shared memory segment
int         Sublen;         // Length of the string to MD5 match


/************************* FORWARD REFERENCES **********************/
void do_files();




int main(int argc, char *argv[])
{
    int          ret;         // generic int return value from a system call
    int         *vret;        // generic void pointer return value from a system call
    int          fdfilelist;  // FD to 'filelist'
    struct stat  fileliststat;  // info about filelist

    /* sanity check */
    if ((argc !=  2)  ||
        (sscanf(argv[1], "%d", &Sublen) != 1) ||
        (Sublen < 19) || (Sublen > 55)) {
        printf("Usage: %s <substring lenght>\n", argv[0]);
        exit(1);
    }

    /* Open file of files and see how many there are. */
    fdfilelist = open(filelist, O_RDONLY);
    if (fdfilelist < 0) {
        printf("Unable to find file list at : %s\n", filelist);
        exit(1);
    }
    /* Sanity check the filelist file by validating its size */
    ret = fstat(fdfilelist, &fileliststat);
    if ((ret < 0) ||
        ((fileliststat.st_size % MAXNAMELEN) != 0)) {
        printf("Filelist size is not a multiple of MAXNAMELEN.  Exiting...\n");
        exit(1);
    }
    Nfiles = fileliststat.st_size / MAXNAMELEN ;

    /* info to user */
    printf("Found %d files in target dataset\n", Nfiles);

    /* Mamory map the file list to make walking it easier */
    vret = mmap((void *) 0, fileliststat.st_size, PROT_READ, MAP_SHARED, fdfilelist, 0);
    if (vret < 0) {
        printf("Unable to mmap the file list\n");
        perror(NULL);
        exit(1);
    }
    Namearray = (char *) vret;


    /* We use the named shared memory segment "/gutenberg".
     * Try to delete it to clean up any previous run.
     * Open/create the new /gutenberg. */
    (void) shm_unlink("/gutenberg");
    Fdshm = shm_open("/gutenberg", O_RDWR | O_CREAT, 0666);
    if (Fdshm < 0) {
        perror(NULL);
        exit(-1);
    }
    /* Mamory map the data set */
    vret = mmap((void *) 0, DATASETSZ, (PROT_READ | PROT_WRITE), MAP_SHARED, Fdshm, 0);
    if (vret < 0) {
        printf("Unable to mmap the data set shared segment\n");
        perror(NULL);
        exit(1);
    }
    Dataset = (char *) vret;

    // Set the length to 400+ MB
    ret  = ftruncate(Fdshm, DATASETSZ);
    if (ret < 0) {
        perror(NULL);
        exit(-1);
    }

    /* clear the shared memory segment */
    memset(Dataset, 0, DATASETSZ);

    /* scan the files loading their contents into shared memory */
    do_files();

    /* clean up and exit */
    munmap(Dataset, DATASETSZ);
    close(Fdshm);
    exit(0);
}


/*
 * do_files() : Load the files into the shared memory segment
 */
void do_files()
{
    int           fidx;        // target file index in filelist
    char         *filename;    // Name of a file from the filelist
    FILE         *fp;
    char          line[MAXLINE]; // we do not really know the max line size
    int           len;         // length of line
    char         *pshm;        // points to next location of write in shared mem
    char         *pret;        // return pointer from fgets
    int           i;           // generic loop index


    /* Point to start of shared memory block */
    pshm = Dataset;

    /* copy each file int the file list to the shared memory segment */
    for (fidx = 0; fidx < Nfiles; fidx += 1) {
        filename = Namearray + (fidx * MAXNAMELEN);

        printf("Adding file %s\n", filename);
        fp = fopen(filename, "r");
        if (fp == NULL) {
            printf("Unable to open %s.  Exiting...\n", filename);
            exit(-1);
        }

        /* read and copy each line from the file */
        while (1) {
            pret = fgets(line, MAXLINE, fp);
            if (pret == NULL) {
                break;
            }
            len = strlen(line);
            if (len == (MAXLINE - 1)) {
                printf("Warning: input line exceed length of %d.\n", MAXLINE - 1);
            }
            if (len < Sublen) {
                //printf("Discarding short line\n");
                continue;
            }
            for (i = 0; i < len; i++) {
                if (line[i] == '\r') {
                    continue;
                }
                // replace \n with null to make all lines strings
                if (line[i] == '\n') {
                    *pshm = (char) 0;
                }
                else {
                    *pshm = line[i];
                }
                pshm++;
                if ((int) (pshm - Dataset) == DATASETSZ) {
                    printf("Out of space in shared memory segment. Exiting...\n");
                    exit(-1);
                }
            }
        }
    }
    printf("Loaded %d characters\n", (int) (pshm - Dataset));

    // truncate the length to the actual length of the dataset
    if( 0 > ftruncate(Fdshm, (int) (pshm - Dataset + 100))) {
        perror(NULL);
        exit(-1);
    }
}


