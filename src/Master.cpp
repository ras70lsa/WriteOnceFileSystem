#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <sys/stat.h>
#include <dirent.h>
#include <ftw.h>
#include <iostream>
#include <algorithm>
#include <stdint.h>
#include <string>
#include <endian.h>
#include <stack>
#include <queue>
#include <openssl/hmac.h>
#include <cstddef>

#include "cxxopts.hpp"
#include "OnDiskStructure.h"
#include "schifra/schifra_galois_field.hpp"
#include "schifra/schifra_sequential_root_generator_polynomial_creator.hpp"
#include "schifra/schifra_reed_solomon_encoder.hpp"
#include "schifra/schifra_reed_solomon_file_encoder.hpp"

static int s_builder(const char *, const struct stat *, int, struct FTW *);
int run(std::string, std::string, std::string);

int imageDFS(const std::string& out_filename, node* root);
uint64_t writeDFS(node* node, FILE* output);
int hashAndAppend(const char*, const char*);
int addReedSolomon(std::string ifs, std::string ofs);

std::string parse_name(const std::string& path_name);
std::string space_pad(const std::string& s);

uint64_t find_header_size();

void write64(uint64_t, FILE*);
void write32(uint32_t, FILE*);

static uint64_t header_off;
static uint64_t file_off;

const int MAX_METADATA = 1000;
static int HASH_BLOCK_SIZE = 1024;

m_prs* meta;

int metadataPointer = 0;
int header_count = 0;
int subitems_count = 0;
std::stack<node> directories;

int main(int argc, char **argv){

  // PREVIOUS FUNCTIONAL CODE: ensure input is appropriate
  // if(argc != 3){
  //     if(argc < 2){
  //     	std::cout << "First argument must be a directory to master." << '\n';
  //     }
  //     if(argc < 3){
  //     	std::cout << "Second argument must be an output filename" << '\n';
  //     }
  //     return EXIT_FAILURE;
  // }
  try{
    cxxopts::Options options("Master", "Takes in a directory and outputs an imaged File System");
    options.add_options()
    ("o,output", "Name of output filename", cxxopts::value<std::string>())
    ("p,path", "relative path to directory to master", cxxopts::value<std::string>())
    ("k,key", "Key for sha256 hashing", cxxopts::value<std::string>())
    ;
    options.parse(argc, argv);

    if(options.count("output")!=1){
      std::cout << "please enter an output file name" << std::endl;
      return 0;
    }

    if(options.count("path")!=1){
      std::cout << "please enter a directory to parse" << std::endl;
      return 0;
    }

    if(options.count("key")!=1){
      std::cout << "please enter a key to use for security" << std::endl;
      return 0;
    }
    run(options["path"].as<std::string>(), options["output"].as<std::string>(), options["key"].as<std::string>());
  } catch (...) { // shouldn't get to here
    std::exception_ptr p = std::current_exception();
    std::clog <<(p ? p.__cxa_exception_type()->name() : "null") << std::endl;
    exit(1);
  }
}

int run(std::string root_directory, std::string wofs_filename, std::string key){

  //make dummy head node to store ptr to root of the tree
  m_prs h;
  h.type = DIRECTORY;
  h.length = 1;

  // make dummy head to store first root
  node head;
  head.data = &h;
  head.fill = 0;
  head.children = new node[1];

  directories.push(head);

  std::cout << "Traversing filesystem from directory "
  << '\"' << root_directory << '\"'
  << std::endl;

  // Using the nftw() funtion to update global structure
  //  see here: https://www.ibm.com/support/knowledgecenter/en/SSLTBW_2.3.0/com.ibm.zos.v2r3.bpxbd00/rnftw.htm
  // filepath
  // function to call on each directory/file
  // max number of directories that can be used

  // flags to specialize usage, we aren't using any right now
  int result =  nftw(root_directory.c_str(), s_builder, MAX_METADATA, 0);

  //iterate to real root
  node root = head.children[0];

  // initialize our array structure
  meta = (m_prs*) malloc(metadataPointer * sizeof(m_prs));
  int i = 0;

  // write the tree to an array  this should be put in a separate method later
  std::queue<node> q;
  q.push(root);
  while(!q.empty()){
    node t = q.front();
    q.pop();
    meta[i] = *t.data;
    i++;
    // if not a file recurs through sub files/directories
    if(t.fill != -1){
      for(int j = 0; j < t.data->length; j++){
        q.push(t.children[j]);
      }
    }
  }

  std::string pre_filename = wofs_filename + ".necc";
  // Now write the file to a structure
  std::cout << "Writing " << header_count << " files/directories to "
  << '\"' << pre_filename << '\"'
  << std::endl;
  node* r = &root;
  int imageStatus = imageDFS(pre_filename, r);
  std::cout << "Appending Sha256 Hash using Key" << "\n";
  int hashStatus = hashAndAppend(pre_filename.c_str(), key.c_str());
  //std::cout << "Converting to Reed Solomon Error Correcting Blocks, outputing to: " << wofs_filename << "\n";
  int reedSolomonStatus = addReedSolomon(pre_filename,wofs_filename);
  //int hashStatus = hashAndAppend(pre_filename.c_str(), pre_filename.c_str(), key.c_str());
  return 0;
}

// function called on each sub directory/file, updates the global information
static int s_builder(const char * path_name, const struct stat * object_info, int ftw, struct FTW * data){

    // Parse name to only the final
    const auto name = parse_name(path_name);
    const char* dir_file_name = name.c_str();

    // Ignore if current directory or parent directory
	if (name == "." || name == "..") {
		return 0;
	}

    header_count++;
	// store directory metadata
	if(ftw == FTW_D){

        //get filename
        std::string padded_name = space_pad(dir_file_name);
		const char* buffer = padded_name.c_str();

        // get number of files in directory algo from: https://stackoverfloxw.com/questions/1723002/how-to-list-all-subdirectories-in-a-given-directory-in-c?answertab=votes#tab-top
        int dir_length = 0;

    	struct dirent* d;
    	DIR* rdir = opendir(path_name);
    	while((d = readdir(rdir)) != NULL)
    	{
        	struct stat st;
					if (fstatat(dirfd(rdir), d->d_name, &st, 0) < 0){
            	perror(d->d_name);
        	}
        	else{
						char firstChar = (d-> d_name)[0];
						if (firstChar == '.') {
							continue;
						}

        		dir_length++;
        	}
    	}
    	closedir(rdir);

		// assign all values
        m_prs* h = new m_prs;
        subitems_count = subitems_count + dir_length;

        // Write data to
        strncpy(h->name, buffer, 256);
		h->type = DIRECTORY;
		h->length = dir_length;
		h->time = object_info->st_mtime;
		h->p = strdup(path_name);
        // insert into parent if there is one
        if(!directories.empty()){
            node parent = directories.top();
            directories.pop();

            //write child information
            parent.children[parent.fill].data = h;
            if (h == nullptr) {
                printf("H IS NULL!");
            }
            parent.children[parent.fill].fill = 0;
            parent.children[parent.fill].children = new node[h->length];

            //update the "fill" value
            parent.fill = parent.fill + 1;

            // if not parent not full add parent back to the stack
            if(parent.fill != parent.data->length){
                directories.push(parent);
            }
             // add this directory to the stack

            // Add ourself to the stack only if we have children
            if(parent.children[parent.fill-1].data->length != 0){
                directories.push(parent.children[parent.fill-1]);
            }

        }

		metadataPointer++;
        return 0;
	}

	// Store File Metadata - possibly add in FTW_NS and FTW_SNL functionality for failed symbolic links
	else if((ftw == FTW_F) || (ftw == FTW_SL)){

        //Pad filename to 256
        std::string padded_name = space_pad(dir_file_name);
        const char* buffer = padded_name.c_str();

		// assign all values
        m_prs* h = new m_prs;

        // assign data to header
        strncpy(h->name, buffer, 256);
        h->type = PLAIN_FILE;
		h->length = object_info->st_size;
		h->time = object_info->st_mtime; // time of last modification, could also use atime for last access or ctime for last status change
    h->p =  strdup(path_name);
		// open the file for reading, when writing to img use this stream
		    FILE* test = fopen(h->p, "r");
        while(test == nullptr){
            printf("file open is NULL!");
            printf("try again");
            test = fopen(h->p, "r");
        }
        fclose(test);

        // add to the tree
        if(!directories.empty()){
            // get parent node
            node parent = directories.top();
            directories.pop();

            // add in this node's data
            parent.children[parent.fill].data = h;
            parent.children[parent.fill].fill = -1;
            parent.children[parent.fill].children = NULL;

            //update the parent fill value
            parent.fill = parent.fill + 1;

            // if the parent is not full add it back
            if(parent.fill != parent.data->length){
                directories.push(parent);
            }
        }
		metadataPointer++;
        return 0;
	}
    return 7; // indicates an error
}

void write64(uint64_t item, FILE* output) {
  uint64_t bigEndian = htobe64(item);
  fwrite((char*) &bigEndian, sizeof(uint64_t), 1, output);
}

void write32(uint32_t item, FILE* output) {
  uint32_t bigEndian = htobe32(item);
  fwrite((char*) &bigEndian, sizeof(uint32_t), 1, output);
}


//returns final token separated by /
std::string parse_name(const std::string& path){

  const auto pos = path.find_last_of("\\/");
  const bool not_found = pos == std::string::npos;

  const auto leaf = not_found ? path : path.substr(pos+1);

  return leaf;
}

std::string space_pad(const std::string& s) {
  std::string buffer(256, ' ');
  auto last = std::copy(begin(s), end(s), begin(buffer));
  *last = '\0';

  return buffer;
}

int imageDFS(const std::string& out_filename, node* root) {
  header_off = 0;
  file_off = find_header_size();

  FILE *output;
  output = fopen(out_filename.c_str(), "wb");

  writeDFS(root, output);
  fclose(output);

}

int hashAndAppend(const char* file_name, const char* key){

  FILE* fp = fopen(file_name, "a+");

  // get the file size
  struct stat st;
  stat(file_name, &st);
  long file_size = st.st_size;

  if (HASH_BLOCK_SIZE > file_size) {
    HASH_BLOCK_SIZE = file_size;
  }
  int block_size = HASH_BLOCK_SIZE;
  long remaining = file_size;
  uint32_t number_hashes = 0;
  // malloc a buffer for the data- may be large
  unsigned char* buffer = (unsigned char*) malloc(block_size * sizeof(char));
  int hash_size = 32;

  while (remaining > 0) {
    number_hashes = number_hashes + 1;
    int data_location = (number_hashes-1) * HASH_BLOCK_SIZE;
    fseek(fp, data_location, SEEK_SET);
    int bytes_read = fread(buffer, sizeof(char), block_size, fp);

    // make the hash Hash
    unsigned char* digest;
    digest = HMAC(EVP_sha256(), key, strlen(key), buffer, block_size, NULL, NULL);

    // Append the Hash to the file
    fwrite (digest, sizeof(char), hash_size, fp);

    remaining = remaining - block_size;
    if (block_size > remaining) {
      block_size = remaining;
    }

  }

  // Write the number of hashes generated and close the file
  write32(number_hashes, fp);
  fclose (fp);

  return 0;
}

int addReedSolomon(std::string ifn, std::string ofn){
  //Code taken from Schifra example
  const std::size_t field_descriptor    =   8;
   const std::size_t gen_poly_index      = 120;
   const std::size_t gen_poly_root_count =   6;
   const std::size_t code_length         = 255;
   const std::size_t fec_length          =   6;
   const std::string input_file_name     = ifn;
   const std::string output_file_name    = ofn;

   typedef schifra::reed_solomon::encoder<code_length,fec_length> encoder_t;
   typedef schifra::reed_solomon::file_encoder<code_length,fec_length> file_encoder_t;

   const schifra::galois::field field(field_descriptor,
                                      schifra::galois::primitive_polynomial_size06,
                                      schifra::galois::primitive_polynomial06);

   schifra::galois::field_polynomial generator_polynomial(field);

   if (
        !schifra::make_sequential_root_generator_polynomial(field,
                                                            gen_poly_index,
                                                            gen_poly_root_count,
                                                            generator_polynomial)
      )
   {
      std::cout << "Error - Failed to create sequential root generator!" << std::endl;
      return 1;
   }

   const encoder_t rs_encoder(field,generator_polynomial);

   file_encoder_t(rs_encoder, input_file_name, output_file_name);

   return 0;

}

uint64_t writeDFS(node* node, FILE* output) {

  uint64_t currentOffset = header_off;

  fseek(output, currentOffset, SEEK_SET);

  std::string padded_name = space_pad(node->data->name);
  fwrite(padded_name.c_str(), sizeof(char), 256, output);

  bool is_reg = node -> fill == -1;
  bool is_dir = node -> fill ==  0;

  if (is_reg) {
    write64(node->data->length, output);
    write64(node->data->time, output);
    write64(file_off, output);
    write32(node->data->type, output );

    fseek(output, file_off, SEEK_SET); // start at header
        uint64_t fileSize = node->data->length;
        char* file_buffer = new char[fileSize];
        size_t bytes;

        // bytes = fread(file_buffer, 1, 1, (FILE*) node -> data -> p);
        // printf("Value %s", file_buffer);
        FILE* open_file = fopen((node->data->p), "r");
        while (0 < (bytes = fread(file_buffer, 1, sizeof(file_buffer), open_file))){
            fwrite(file_buffer, 1, bytes, output);
        }
        file_off += fileSize;
        header_off += M_HDR_SIZE;
        fclose(open_file);
        free(file_buffer);

  } else if (is_dir) {

    write64(node->data->length, output);
    write64(node->data->time, output);
    uint64_t endOffset = currentOffset + M_HDR_SIZE;
    write64(endOffset, output);
    write32(node->data->type, output );

    uint64_t numChildren = node->data->length;
    header_off = endOffset + sizeof(uint64_t) * numChildren;

    for (int i = 0; i<numChildren; i++) {
            tree_node child = (node -> children)[i];
            uint64_t childOffset = writeDFS(&child, output);
            uint64_t desiredSeekLoc = endOffset + i * sizeof(uint64_t);

            fseek(output, desiredSeekLoc, SEEK_SET);
            write64(childOffset, output);
        }
    }

    return currentOffset;
}

uint64_t find_header_size(){
  uint64_t h_size = header_count * M_HDR_SIZE + subitems_count * sizeof(uint64_t);
  return h_size;
}
