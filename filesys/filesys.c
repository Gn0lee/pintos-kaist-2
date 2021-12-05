#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "devices/disk.h"

/* The disk that contains the file system. */
struct disk *filesys_disk;

static void do_format (void);

// Project 4-2 : fime path parsing

// defined in filesys.h
// struct path{
// 	char ** dirnames; //list of directories
// 	int dircount; //level of directory
// 	char * filename; //
// };

struct path* parse_filepath (const char *name_original){
	const int MAX_PATH_CNT = 30;
	struct path* path = malloc(sizeof(struct path));
	char **buf = calloc(sizeof(char *), MAX_PATH_CNT); // #ifdef DBG 🚨 로컬 변수 -> 함수 끝나면 정보 날라감; 메모리 할당해주기!
	int i = 0;

	int pathLen = strlen(name_original)+1;
	char* name = malloc(pathLen);
	strlcpy(name, name_original, pathLen);
	// printf("pathLen : %d // %s, %d, copied %s %d\n", pathLen, name_original, strlen(name_original), name, strlen(name)); // #ifdef DBG

	path->pathStart_forFreeing = name; // free this later

	char *token, *save_ptr;
	token = strtok_r(name, "/", &save_ptr);
	while (token != NULL)
	{
		// File name too long - 'test: create-long.c'
		if(strlen(token) > NAME_MAX){
			path->dircount = -1; // invalid path
			return path;
		}

		buf[i] = token;
		token = strtok_r(NULL, "/", &save_ptr);
		i++;
	}
	path->dirnames = buf; 
	path->dircount = i-1;
	path->filename = buf[i-1]; // last name in the path
	return path;
}

/* Initializes the file system module.
 * If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) {
	filesys_disk = disk_get (0, 1);
	if (filesys_disk == NULL)
		PANIC ("hd0:1 (hdb) not present, file system initialization failed");

	inode_init ();

	// Project 3. (parallel-merge)
	lock_init(&filesys_lock);

#ifdef EFILESYS
	fat_init ();

	if (format)
		do_format ();

	fat_open ();
#else
	/* Original FS */
	free_map_init ();

	if (format)
		do_format ();

	free_map_open ();
#endif
}

/* Shuts down the file system module, writing any unwritten data
 * to disk. */
void
filesys_done (void) {
	/* Original FS */
#ifdef EFILESYS
	fat_close ();
#else
	free_map_close ();
#endif
}

/* Creates a file named NAME with the given INITIAL_SIZE.
 * Returns true if successful, false otherwise.
 * Fails if a file named NAME already exists,
 * or if internal memory allocation fails. */
bool
filesys_create (const char *name, off_t initial_size) {
	lock_acquire(&filesys_lock);

	// Parse path and get directory
	struct path* path = parse_filepath(name);
	if(path->dircount==-1) return false; // create-empty, create-long

	struct dir* dir = find_subdir(path->dirnames, path->dircount);
	if(dir == NULL) {
		dir_close (dir);
		free_path(path);
		lock_release(&filesys_lock);
		return false;
	}

	struct inode *inode = NULL; 
	if(dir_lookup(dir, path->filename, &inode)){
		dir_close (dir);
		free_path(path);
		lock_release(&filesys_lock);
		return false; // create-exists (trying to create file that already exists)
	}

	disk_sector_t inode_sector = 0;
	// struct dir *dir = dir_open_root ();

	#ifdef EFILESYS
	cluster_t clst = fat_create_chain(0);
	ASSERT(clst >= 1);
	inode_sector = cluster_to_sector(clst);

	bool success = (dir != NULL			
			&& inode_create (inode_sector, initial_size, false)
			&& dir_add (dir, path->filename, inode_sector));

	if (!success)
		fat_remove_chain (inode_sector, 0);

	// struct path path = parse_filepath(name);
	// struct dir *dir = dir_open(find_subdir(path.dirnames, path.dircount));

	// create?
	#else
	bool success = (dir != NULL
			&& free_map_allocate (1, &inode_sector)
			&& inode_create (inode_sector, initial_size, false)
			&& dir_add (dir, name, inode_sector));
	if (!success && inode_sector != 0)
		free_map_release (inode_sector, 1);
	#endif

	dir_close (dir);
	free_path(path);

	lock_release(&filesys_lock);

	return success;
}

/* Opens the file with the given NAME.
 * Returns the new file if successful or a null pointer
 * otherwise.
 * Fails if no file named NAME exists,
 * or if an internal memory allocation fails. */
struct file *
filesys_open (const char *name) {
	lock_acquire(&filesys_lock);

	// Parse path and get directory
	struct path* path = parse_filepath(name);
	struct dir* dir = find_subdir(path->dirnames, path->dircount);
	if(dir == NULL) {
		dir_close (dir);
		free_path(path);
		return false;
	}

	// struct dir *dir = dir_open_root ();
	struct inode *inode = NULL;

	if (dir != NULL)
		dir_lookup (dir, path->filename, &inode);

	struct file *file = file_open (inode);

	dir_close (dir);
	free_path(path);

	lock_release(&filesys_lock);

	return file;
}

/* Deletes the file named NAME.
 * Returns true if successful, false on failure.
 * Fails if no file named NAME exists,
 * or if an internal memory allocation fails. */
bool
filesys_remove (const char *name) {
	lock_acquire(&filesys_lock);

	// Parse path and get directory
	struct path* path = parse_filepath(name);
	struct dir* dir = find_subdir(path->dirnames, path->dircount);
	if(dir == NULL) {
		dir_close (dir);
		free_path(path);	
		return false;
	}

	// struct dir *dir = dir_open_root ();
	bool success = dir != NULL && dir_remove (dir, name);

	dir_close (dir);
	free_path(path);

	lock_release(&filesys_lock);

	return success;
}

/* Formats the file system. */
static void
do_format (void) {
	printf ("Formatting file system...");

#ifdef EFILESYS
	/* Create FAT and save it to the disk. */
	fat_create ();
	// fat_put(ROOT_DIR_CLUSTER, EOChain); // done in 'fat_create'
	if (!dir_create (cluster_to_sector(ROOT_DIR_CLUSTER), DISK_SECTOR_SIZE/sizeof (struct dir_entry))) // file number limit
		PANIC ("root directory creation failed");
	fat_close ();
#else
	free_map_create ();
	if (!dir_create (ROOT_DIR_SECTOR, 16))
		PANIC ("root directory creation failed");
	free_map_close ();
#endif

	printf ("done.\n");
}

void free_path(struct path* path){
	free(path->pathStart_forFreeing); // free malloc'ed and copied path
	free(path->dirnames); // free buf
	free(path);
}