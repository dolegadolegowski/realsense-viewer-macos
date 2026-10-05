// License: Apache 2.0. See LICENSE file in root directory.
//
// Gives the files a root-run RealSense tool created back to the user who launched it:
//
//   rs-macos-handback <uid> <gid> <stamp-file> <path>...
//
// Each path (a directory is searched up to 3 levels deep) changes owner only if it is a regular file or directory that
// root owns and that was created after <stamp-file> was last modified. Everything is opened without following links and
// changed through that descriptor, and regular files must have a single link, so links or files the user swapped in
// cannot redirect the change to another file.

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static uid_t uid;
static gid_t gid;
static struct timespec since;

static int created_since( const struct timespec * t )
{
    return t->tv_sec > since.tv_sec || ( t->tv_sec == since.tv_sec && t->tv_nsec >= since.tv_nsec );
}

static void hand_back( int dir, const char * name, int depth )
{
    int fd = openat( dir, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC );
    if( fd < 0 )
        return;
    struct stat st;
    if( fstat( fd, &st ) == 0 )
    {
        int ours = st.st_uid == 0 && created_since( &st.st_birthtimespec );
        if( S_ISREG( st.st_mode ) && ours && st.st_nlink == 1 )
            fchown( fd, uid, gid );
        else if( S_ISDIR( st.st_mode ) )
        {
            if( ours )
                fchown( fd, uid, gid );
            DIR * d = depth > 0 ? fdopendir( dup( fd ) ) : NULL;
            if( d )
            {
                struct dirent * entry;
                while( ( entry = readdir( d ) ) )
                    if( strcmp( entry->d_name, "." ) && strcmp( entry->d_name, ".." ) )
                        hand_back( dirfd( d ), entry->d_name, depth - 1 );
                closedir( d );
            }
        }
    }
    close( fd );
}

int main( int argc, char * argv[] )
{
    struct stat stamp;
    if( argc < 4 || stat( argv[3], &stamp ) != 0 )
    {
        fprintf( stderr, "usage: %s <uid> <gid> <stamp-file> <path>...\n", argv[0] );
        return 1;
    }
    uid = (uid_t)atoi( argv[1] );
    gid = (gid_t)atoi( argv[2] );
    since = stamp.st_mtimespec;
    for( int i = 4; i < argc; ++i )
        hand_back( AT_FDCWD, argv[i], 3 );
    return 0;
}
