VECTORS TO VICTORY!!!

The programs in this repository solve the ClusterFights
Gutenberg/MD5 challenge,  This program uses a vectors
to compute the MD5 sums.  It is intended to run on an
Intel processor with AVX2.

The program 'shm_init' copies the Gutenberg text from
files in the /mnt/md5/guten directory into a shared
memory segment located in /dev/shm/gutenberg.  The
data set is only 400MB and fits easily in system RAM.
As the data is copied CRLF is converted to a null,
and lines shorter than the target length are removed
entirely.

Build and run shm_init as:
    gcc -o shm_init shm_init.c  -lrt
    ./shm_init <substring length>


The program 'shm_vec_md5' search /dev/shm/gutenberg
for the substring matching the specified MD5 sum.
You have to compile shm_vec_md5 to match the length
of the substring.  The #define to change is called
'Sublen'.

Edit shm_vec_md5.c to set Sublen and then build and
run as:
    gcc -o shm_vec_md5 shm_vec_md5.c -lpthread -lrt -march=broadwell -mavx2 -O3
    ./shm_vec_md5 <number of thread> <MD5 sum to find>


