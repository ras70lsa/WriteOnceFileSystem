#define FUSE_USE_VERSION 31

//================================= Includes =================================//

#include "OnDiskStructure.h"
#include "ecc.cpp"
#include "readFunctions.cpp"
#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <assert.h>
#include <stdlib.h>
#include <endian.h>
#include <openssl/hmac.h>
#include <math.h>
#include "config/hashConstants.c"
#include "requestKey.cpp"

//========================== Function Declarations ===========================//

static void *mount_init(struct fuse_conn_info *conn, struct fuse_config *cfg);
static m_hdr* find(const char* path);
void exit_program();
int checkHash(const char* file_name, const char* key);

//========================== Global Variables ===============================//

static unsigned long HASH_BLOCK_SIZE = DEF_HASH_BLOCK_SIZE;
unsigned long image_file_size; 		// Stored to see if offset is safe or not
FILE* fp;
size_t prev_offset = 0;

static void *mount_init(struct fuse_conn_info *conn,
			struct fuse_config *cfg)
{
	(void) conn;
	cfg->kernel_cache = 1;
	return NULL;
}

/*
* Return the metadata at the given path
*/
static m_hdr* find(const char* path) {
	if (path == NULL) {
		return NULL;
	}

	//printf("Calling find on %s. \n", path);
	char* pathCopy = (char*) malloc(strlen(path) + 1);
	strcpy(pathCopy, path);
	char* token = strtok(pathCopy, "/");
	m_hdr* root = readHeader(fp, 0);
	m_hdr* current = root;

	//printf("token found\n");
	while (token != NULL) {
		if (strcmp(current -> name, token) !=0) {
			return NULL;
		}
		token = strtok(NULL, "/");

		if (token == NULL) {
			break;
		}

		if (current -> type == 1) {
			return current;
		}

		uint64_t initOffset = current -> offset;
		int childFound = 0;
		for (unsigned int i =0; i< current -> length; i++) {
			uint64_t nextOffset = initOffset + i*sizeof(uint64_t);
			uint64_t next_header_block = read64(fp, nextOffset);
			if (next_header_block > image_file_size) {
				printf("Attempting to traverse to a location out of range. \n");
				printf("Please verify the correctness of the image \n");
				//exit_program();
			}
			m_hdr* child = readHeader(fp, next_header_block);

			if (strcmp(child -> name, token) == 0) {
				current = child;
				childFound = 1;
				break;
			}
		}

		if (!childFound) {
			return NULL;
		}
	}
	return current;
}

static int mount_getattr(const char *path, struct stat *stbuf,
			 struct fuse_file_info *fi)
{
	(void) fi;
	int res = 0;

	if(!path) { return -ENOENT; }

	memset(stbuf, 0, sizeof(struct stat));

	if (strcmp(path, "/") == 0) {
		stbuf -> st_mode = S_IFDIR | 0444;	// Read only access
		stbuf -> st_nlink = 2;
		return res;
	}

	m_hdr* head = find(path);
	if (head == NULL) {
		return -ENOENT;
	}
	if (head -> type == 0) {				// Directory
		stbuf -> st_mode = S_IFDIR | 0444;	// Read only access
		stbuf -> st_nlink = head -> length; // for a directory length signifies # subchildren

	} else if (head -> type == 1) { 		// File 
		stbuf->st_mode = S_IFREG | 0444;	// Read only access
		stbuf->st_size = head -> length;
		double file_size = head->length;
		double block_size = 4096;			// Default block size to 4k
		stbuf->st_blksize = block_size;
		int num_blocks = ceil(file_size/block_size); 
		stbuf-> st_blocks = num_blocks;
	} else {
		res = -ENOENT;
	}
	stbuf->st_mtime = head -> time;

	return res;
}

static int mount_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			 off_t offset, struct fuse_file_info *fi,
			 enum fuse_readdir_flags flags)
{
	(void) offset;
	(void) fi;
	(void) flags;

	m_hdr* dir_header;
	if (strcmp(path, "/") == 0) {
		dir_header = readHeader(fp, 0);
		// Address of struct stat = NULL, next offset = 0 (not used)
		filler(buf, ".", NULL, 0, static_cast<fuse_fill_dir_flags>(0));
		filler(buf, "..", NULL, 0, static_cast<fuse_fill_dir_flags>(0));
		filler(buf, dir_header -> name, NULL, 0, static_cast<fuse_fill_dir_flags>(0));
		return 0;
	} else {
		dir_header = find(path);
	}

	if (dir_header == NULL) {			// Directory not found
		return -ENOENT;
	}

	if (dir_header -> type != 0) {		// Not a directory
		return -ENOENT; 
	}

	// Fill the buffer with . and ..
	filler(buf, ".", NULL, 0, static_cast<fuse_fill_dir_flags>(0));
	filler(buf, "..", NULL, 0, static_cast<fuse_fill_dir_flags>(0));

	// Fill the buffer with all subdirectories
	uint64_t initOffset = dir_header -> offset;
	for (unsigned int i =0; i< dir_header -> length; i++) {
		uint64_t nextOffset = initOffset + i*sizeof(uint64_t);
		uint64_t next_header_block = read64(fp, nextOffset);
		m_hdr* child = readHeader(fp, next_header_block);
		filler(buf, child->name, NULL, 0, static_cast<fuse_fill_dir_flags>(0));
	}

	return 0; // no more files
}

static int mount_open(const char *path, struct fuse_file_info *fi)
{
	m_hdr* file_header = find(path);


	if (file_header == NULL) {			// File not found
		return -ENOENT;
	}

	if (file_header -> type != 1) {		// Not a file
		return -ENOENT;
	}

	// Ensure privaledges
	if ((fi->flags & O_ACCMODE) != O_RDONLY)
		return -EACCES;

	return 0;
}

static int mount_read(const char *path, char *buf, size_t size, off_t offset,
		      struct fuse_file_info *fi)
{
	size_t len;
	(void) fi;
	
	m_hdr* file_header = find(path);

	if (file_header == NULL) {				// File not found
		return -ENOENT;
	}

	if (file_header -> type != 1) {			// Not a file
		return -ENOENT;
	}


	size_t length = file_header -> length;
	uint64_t data_block_offset = file_header -> offset;
	fseek(fp, data_block_offset, SEEK_SET);
	char* data_buffer = (char*) malloc(length);
	fread((void*)data_buffer, 1, length, fp);
	len = length;

	if ((unsigned uint64_t) offset < len) {
		if (offset + size > len)
			size = len - offset;
		memcpy(buf, data_buffer + offset, size);
	} else
		size = 0;

	free(data_buffer);
	prev_offset = offset;
	return size;
}

int checkHash(const char* file_name, const char* key) {
	
	struct stat st;
 	stat(file_name, &st);
  	long file_size = st.st_size;

  	int hash_size = 32; //size of each hash

  	// Determine the number of hashes that was generated during mastering
  	uint64_t number_hash_location = file_size - sizeof(uint32_t);
	uint32_t number_hashes = read32(fp, number_hash_location);
	uint64_t hashes_offset = number_hash_location - hash_size * number_hashes;
	uint64_t image_size = hashes_offset;
	uint64_t remaining = image_size;

	if (HASH_BLOCK_SIZE > image_size) {
		HASH_BLOCK_SIZE = image_size;
	}

	int block_size = HASH_BLOCK_SIZE;
	int hash_count = 0; // running tally of number of hashes checked

	// malloc the buffer because it could be large
	unsigned char* buffer = (unsigned char*) malloc(block_size * sizeof(char));
	if (buffer == NULL) {
		printf("Call to allocate buffer failed\n");
		exit(0);
	}

	while (remaining > 0) {

		// read the hash saved in the mastered image
	  	fseek(fp, hashes_offset, SEEK_SET);
	  	char mastered_hash[hash_size];
	  	fread(mastered_hash, hash_size, 1, fp);
	  	
	  	// generate the hash based on the data in the master image
	  	int data_block_local = hash_count * HASH_BLOCK_SIZE;
	  	fseek(fp, data_block_local, SEEK_SET);
	  	fread(buffer, sizeof(char), block_size, fp);
	  	unsigned char* digest;
	  	digest = HMAC(EVP_sha256(), key, strlen(key), buffer, block_size, NULL, NULL);

	  	// compare the two hashes
	  	for (int i =0; i< hash_size; i++) {
	    	if ((unsigned char) mastered_hash[i] != digest[i]) {
	    		free(buffer);
	      		return 0;
	    	}
	  	}

	  	hash_count = hash_count + 1;
	  	remaining = remaining - block_size;
	  	if ((uint64_t) block_size > remaining) {
	  		block_size = remaining;
	  	}
	  	hashes_offset = hashes_offset + hash_size;
  	}
  	free(buffer);
  	return 1;
}

void exit_program() {
	fclose(fp);
	exit(0);
}


/*
 * Command line options
 */
static struct options {
	const char *filename;
	const char *key;
	int show_help;
	int no_ecc;
} options;

#define OPTION(t, p)                           \
    { t, offsetof(struct options, p), 1 }
static const struct fuse_opt option_spec[] = {
	OPTION("--image=%s", filename),
	OPTION("-h", show_help),
	OPTION("--help", show_help),
	OPTION("--necc", no_ecc),
	FUSE_OPT_END
};

static void show_help(const char *progname)
{
	printf("usage: %s [options] <mountpoint>\n\n", progname);
	printf("File-system specific options:\n"
	       "    --image=<s>          Path to the image file"
	       "\n"
	       "    --key=<s>            Key to check dat validity"
	       "\n"
	       "    --help           	 Show help"
	       "\n");
}

static struct mount_opereration : fuse_operations {

	mount_opereration() {
		init       	= mount_init;
		getattr		= mount_getattr;
		readdir		= mount_readdir;
		open		= mount_open;
		read		= mount_read;
	}
} mount_oper_init;

int main(int argc, char *argv[])
{

	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

	/* Parse options */
	if (fuse_opt_parse(&args, &options, option_spec, NULL) == -1) {
		printf("Error parsing input \n");
		show_help(argv[0]);
		return 1;
	}

	if (options.show_help) {
		show_help(argv[0]);
		assert(fuse_opt_add_arg(&args, "--help") == 0);
		args.argv[0] = (char*) "";
		printf("\n \nHelp indicated...exiting program. \n");
		printf("Please run program without help. -h flag\n");
		exit(0);
	}

	const char* original_path;
	const char* file_name;
	if (options.filename == NULL) {
		printf("Please specify a path to the image \n");
		printf("Use --image=<image_name> \n");
		exit(0);
	} else {
		// Verify the path exists
		struct stat st;
    	original_path = options.filename;
    	int ableToFind = stat(original_path, &st);
    	if (ableToFind == -1) { // Path does not exist
      		printf("%s does not exist\n Please enter a valid path\n", original_path);
      		exit(0);
    	} 
		file_name = realpath(options.filename, NULL);
	}

	// Verify the validity of the image
	unsigned int min_key_length = 4;
    const char* key =  get_key_from_user();
    while (strlen(key) < min_key_length) {
      std::cout << "Please enter a valid key." << std::endl;
      std::cout << "Key must be longer than " << min_key_length << " characters." << std::endl;
      key =  get_key_from_user();
    }

	std::string outfile;
	if (options.no_ecc) {
		outfile = file_name;
	} else {
		std::string infile = file_name;
		outfile = infile + ".rec";
  		int decodeResults = decode(infile, outfile);
  		if (decodeResults) {
  			std::cout << "\033[0;31m" <<"Error" << "\033[0m" << std::endl;
  			printf("Unable to recover image from error correcing codes \n");
  			printf("Decoding exited with error status: %d\n", decodeResults);
  			printf("Please default to backups \n");
  			printf("Continue? Behavior of program may be undefined [y/n]: ");
  			char yn[5];
  			fgets(yn,4,stdin);
  			if (yn[0] != 'y') {
  				exit(0);
  			}
  			std::cout << "Continuing..." << std::endl;
  		}
	}
	
  	fp = fopen(outfile.c_str(), "r");
	int hash_correct = checkHash(outfile.c_str(), key);
	printf("\nVerifying hash... \n \n");
	if (!hash_correct) {
		std::cout << "\033[0;31m" <<"Error" << "\033[0m" << std::endl;
		printf("Data integrity issue detected.\n");
		printf("Please verify the key you are using is correct.\n");
		printf("Please correct issue offline and remount.\n");
		printf("Continue? Behavior of program may be undefined [y/n]: ");
  		char yn[5];
  		fgets(yn,4,stdin);
  		if (yn[0] != 'y') {
  			exit_program();
  		}
  		std::cout << "Continuing..." << std::endl;
	} else {
		std::cout << "\033[0;32m" <<"Hash passed" << "\033[0m" << std::endl;
	}

	struct stat st;
 	stat(outfile.c_str(), &st);
  	image_file_size = st.st_size;
  	printf("Mounting image %s \n", original_path);
	return fuse_main(args.argc, args.argv, &mount_oper_init, NULL);
}
