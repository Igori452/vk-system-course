#include "userfs.h"
#include <stddef.h>

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

enum {
	BLOCK_SIZE = 512,
	MAX_FILE_SIZE = 1024 * 1024 * 100,
};

/** Global error code. Set from any function on any error. */
static enum ufs_error_code ufs_error_code = UFS_ERR_NO_ERR;

struct block {
	/** Block memory. */
	char *memory;
	/** How many bytes are occupied. */
	int occupied;
	/** Next block in the file. */
	struct block *next;
	/** Previous block in the file. */
	struct block *prev;

	/* PUT HERE OTHER MEMBERS */
};

struct file {
	/** Double-linked list of file blocks. */
	struct block *block_list;
	/**
	 * Last block in the list above for fast access to the end
	 * of file.
	 */
	struct block *last_block;
	/** How many file descriptors are opened on the file. */
	int refs;
	/** File name. */
	char *name;
	/** Files are stored in a double-linked list. */
	struct file *next;
	struct file *prev;
	/** File size. */
	size_t size;
};

/** List of all files. */
static struct file *file_list = NULL;

struct filedesc {
	struct file *file;

	/** The file has been deleted. */
	bool deleted;

	/** Read/write iterators. */
	size_t file_it;

	int flags;
};

/**
 * An array of file descriptors. When a file descriptor is
 * created, its pointer drops here. When a file descriptor is
 * closed, its place in this array is set to NULL and can be
 * taken by next ufs_open() call.
 */
static struct filedesc **file_descriptors = NULL;
static int file_descriptor_count = 0;
static int file_descriptor_capacity = 0;

enum ufs_error_code
ufs_errno()
{
	return ufs_error_code;
}

int
ufs_open(const char *filename, int flags)
{
	struct file* head_file = file_list;
	struct file* open_file = NULL;

	while(head_file != NULL)
	{
		if (strcmp(head_file->name, filename) == 0)
		{
			open_file = head_file;
			break;
		}
		head_file = head_file->next;
	}

	if (open_file == NULL && flags == UFS_CREATE) 
	{
		struct file* new_file = malloc(sizeof(struct file));

		size_t len = strlen(filename);
		new_file->name = malloc(len + 1);
		memcpy(new_file->name, filename, len);
		new_file->name[len] = '\0'; 

		new_file->refs = 0;
		new_file->size = 0;

		new_file->block_list = NULL;
		new_file->last_block = NULL;

		new_file->prev = NULL;
		new_file->next = file_list;

		if (file_list != NULL)
		{
			file_list->prev = new_file;
		}
		file_list = new_file;

		open_file = file_list;
	}
	else if (open_file == NULL)
	{
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}

	size_t file_desc_it = 0;
	if (file_descriptor_capacity == 0) 
	{
		file_descriptors = malloc(sizeof(struct filedesc*));
		file_descriptor_capacity = 1;
	}
	else if (file_descriptor_capacity == file_descriptor_count) 
	{
		struct filedesc** tmp = realloc(
			file_descriptors, sizeof(struct filedesc*) * (++file_descriptor_capacity));

		file_descriptors = tmp;
		file_desc_it = file_descriptor_count;
	}
	else 
	{
		for (size_t i = 0; i < (size_t)file_descriptor_capacity; ++i)
		{
			if (file_descriptors[i] == NULL)
			{
				file_desc_it = i;
				break;
			}
		}
	}

	struct filedesc* new_filedesc = malloc(sizeof(struct filedesc));
		
	new_filedesc->file = open_file;
	new_filedesc->deleted = false;
	new_filedesc->file_it = 0;
	if (flags == UFS_CREATE)
	{
		new_filedesc->flags = UFS_READ_WRITE;
	}
	else 
	{
		new_filedesc->flags = flags;
	}

	++(new_filedesc->file->refs);

	file_descriptors[file_desc_it] = new_filedesc;
	return file_descriptor_count++;
}

ssize_t
ufs_write(int fd, const char *buf, size_t size)
{
	if (file_descriptor_capacity <= fd || fd < 0 || file_descriptors[fd] == NULL)
	{
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}

	if (file_descriptors[fd]->flags == UFS_READ_ONLY)
	{
		ufs_error_code = UFS_ERR_NO_PERMISSION;
		return -1;
	}

	if (buf == NULL || size == 0)
	{
		return 0;
	}

	struct file* write_file = file_descriptors[fd]->file;
	if (write_file->size < file_descriptors[fd]->file_it) 
	{
		file_descriptors[fd]->file_it = write_file->size;
	}

	size_t copy_size = size;
	size_t mem_it = file_descriptors[fd]->file_it;
	
	if (size < BLOCK_SIZE) 
	{
		copy_size = size;
	}
	else 
	{
		copy_size = BLOCK_SIZE;
	}

	struct block* head_block = write_file->block_list;

	while (mem_it > BLOCK_SIZE)
	{
		mem_it -= BLOCK_SIZE;
		head_block = head_block->next;
	}

	if (copy_size + mem_it >= BLOCK_SIZE)
	{
		copy_size = BLOCK_SIZE - mem_it;
	}

	size_t buf_it = 0;

	while(buf_it != size)
	{
		struct block* work_block = NULL;
		if (head_block == NULL)
		{
			if (write_file->size + BLOCK_SIZE > MAX_FILE_SIZE)
			{
				ufs_error_code = UFS_ERR_NO_MEM;
				return -1;
			}

			struct block* new_block = malloc(sizeof(struct block));
			new_block->memory = malloc(BLOCK_SIZE);

			new_block->occupied = 0;
			new_block->next = NULL;
			new_block->prev = NULL;

			if (write_file->size == 0)
			{
				write_file->block_list = new_block;
				write_file->last_block = new_block;
			}
			else 
			{
				write_file->last_block->next = new_block;
				new_block->prev = write_file->last_block;

				write_file->last_block = new_block;
			}

			work_block = new_block;
			head_block = new_block;
			write_file->size += BLOCK_SIZE;
		}
		else 
		{
			work_block = head_block;
		}

		memcpy(&work_block->memory[mem_it], &buf[buf_it], copy_size);

		int bias = copy_size + mem_it - work_block->occupied; 
		if (bias > 0)
		{
			work_block->occupied += bias;
		}

		buf_it += copy_size;

		if (size - buf_it >= BLOCK_SIZE)
		{
			copy_size = BLOCK_SIZE;
		}
		else 
		{
			copy_size = size - buf_it;
		}

		mem_it = 0;
		head_block = head_block->next;
	}

	file_descriptors[fd]->file_it += buf_it;
	return buf_it;
}

ssize_t
ufs_read(int fd, char *buf, size_t size)
{
	if (file_descriptor_capacity <= fd || fd < 0 || file_descriptors[fd] == NULL)
	{
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}

	if (file_descriptors[fd]->flags == UFS_WRITE_ONLY)
	{
		ufs_error_code = UFS_ERR_NO_PERMISSION;
		return -1;
	}

	if (buf == NULL || size == 0)
	{
		return 0;
	}

	struct file* read_file = file_descriptors[fd]->file;
	if (read_file->size < file_descriptors[fd]->file_it) 
	{
		file_descriptors[fd]->file_it = read_file->size;
	}

	struct block* head_block = read_file->block_list;

	size_t mem_it = file_descriptors[fd]->file_it;
	size_t tmp_size = size;
	size_t buf_it = 0;

	while(tmp_size != 0 && head_block != NULL) 
	{
		if (mem_it >= BLOCK_SIZE)
		{
			head_block = head_block->next;
			mem_it -= BLOCK_SIZE;
			continue;
		}

		size_t read_size;
		if ((int)tmp_size - (int)head_block->occupied + (int)mem_it <= 0)
		{
			read_size = tmp_size;
		}
		else 
		{
			read_size = head_block->occupied - mem_it;
		}

		memcpy(&buf[buf_it], &head_block->memory[mem_it], read_size);
		buf_it += read_size;

		head_block = head_block->next;
		tmp_size -= read_size;
		mem_it = 0;
	}

	file_descriptors[fd]->file_it += (size - tmp_size);

	return size - tmp_size;
}

static 
void free_file(struct file* delete_file) 
{
	struct block* head_block = delete_file->block_list;
	while(head_block != NULL)
	{
		struct block* delete_block = head_block;
		head_block = head_block->next;

		free(delete_block->memory);
		delete_block->occupied = 0;
		free(delete_block);
	}

	delete_file->block_list = NULL;
	delete_file->last_block = NULL;

	free(delete_file->name);
	free(delete_file);
}

int
ufs_close(int fd)
{
	if (file_descriptor_capacity <= fd || fd < 0 || file_descriptors[fd] == NULL)
	{
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}

	struct file* fl = file_descriptors[fd]->file;
	--(fl->refs);

	if (fl->refs == 0 && file_descriptors[fd]->deleted) 
	{
		free_file(file_descriptors[fd]->file);
	}

	free(file_descriptors[fd]);
	file_descriptors[fd] = NULL;
	--file_descriptor_count;

	return 0;
}

int
ufs_delete(const char *filename)
{

	struct file* head_file = file_list;
	struct file* delete_file = NULL;

	while(head_file != NULL)
	{
		if (strcmp(head_file->name, filename) == 0)
		{
			delete_file = head_file;
			break;
		}
		head_file = head_file->next;
	}

	if (delete_file == NULL) 
	{
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}

	if (file_list == delete_file) 
	{
		file_list = file_list->next;
	}
	if (delete_file->prev != NULL)
	{
		delete_file->prev->next = delete_file->next;
	}
	if (delete_file->next != NULL)
	{
		delete_file->next->prev = delete_file->prev;
	}

	delete_file->next = NULL;
	delete_file->prev = NULL;

	if (delete_file->refs == 0)
	{
		free_file(delete_file);
	}
	else 
	{
		for (size_t i = 0; i < (size_t)file_descriptor_capacity; ++i) 
		{
			if (file_descriptors[i] != NULL 
				&& strcmp(file_descriptors[i]->file->name, filename) == 0 
				&& !file_descriptors[i]->deleted)
			{
				file_descriptors[i]->deleted = true;
			}
		}
		
	}

	return 0;
}

#if NEED_RESIZE

int
ufs_resize(int fd, size_t new_size)
{
	if (file_descriptor_capacity <= fd || fd < 0 || file_descriptors[fd] == NULL)
	{
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}

	if (file_descriptors[fd]->flags == UFS_READ_ONLY)
	{
		ufs_error_code = UFS_ERR_NO_PERMISSION;
		return -1;
	}

	if (new_size > MAX_FILE_SIZE)
	{
		ufs_error_code = UFS_ERR_NO_MEM;
		return -1;
	}

	struct file* resize_file = file_descriptors[fd]->file;

	if (resize_file->size > new_size)
	{
		size_t count_delete = (size_t)((resize_file->size - new_size) / BLOCK_SIZE);
		struct block* tail_block = resize_file->last_block;

		for (size_t i = 0; i < count_delete; ++i) 
		{
			struct block* delete_block = tail_block;
			tail_block = tail_block->prev;

			free(delete_block->memory);
			delete_block->occupied = 0;
			free(delete_block);

			tail_block->next = NULL;
		}

		tail_block->occupied = new_size % BLOCK_SIZE;

		resize_file->last_block = tail_block;
		resize_file->size = new_size;
	}
	else if (resize_file->size < new_size)
	{
		size_t count_add = (size_t)((new_size - resize_file->size) / BLOCK_SIZE + 1);

		for (size_t i = 0; i < count_add; ++i) 
		{
			struct block* new_block = malloc(sizeof(struct block));
			new_block->memory = malloc(BLOCK_SIZE);

			new_block->occupied = 0;
			new_block->next = NULL;
			new_block->prev = NULL;

			if (resize_file->size == 0)
			{
				resize_file->block_list = new_block;
				resize_file->last_block = new_block;
			}
			else 
			{
				resize_file->last_block->next = new_block;
				new_block->prev = resize_file->last_block;

				resize_file->last_block = new_block;
			}
		}

		resize_file->size = new_size;
	}

	return 0;
}

#endif

void
ufs_destroy(void)
{
	for (size_t i = 0; i < (size_t)file_descriptor_capacity; ++i)
	{
		if (file_descriptors[i] != NULL)
		{
			ufs_close(i);
		}
	}

	struct file* head_file = file_list;
	while (head_file != NULL)
	{
		struct file* delete_file = head_file;
		head_file = head_file->next;

		free(delete_file->name);
	}

	free(file_descriptors);
}
