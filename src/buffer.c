#include "doomdriver.h"


#include <linux/fs.h>



#define PAGE_SIZE (1<<12)


static int surface_open(struct inode *ino, struct file *file);
static int surface_release(struct inode *ino, struct file *filep);
static ssize_t surface_read(struct file *file, char __user *user_data, size_t size, loff_t *off);

struct file_operations surface_fops = {
    .owner = THIS_MODULE,
    .open = surface_open,
    .read = surface_read,
    .release = surface_release,
};


static int buffer_open(struct inode *ino, struct file *file);
static int buffer_release(struct inode *ino, struct file *filep);
static ssize_t buffer_write(struct file *file, const char __user *user_data, size_t size, loff_t *off);

struct file_operations buffer_fops = {
    .owner = THIS_MODULE,
    .open = buffer_open,
    .write = buffer_write,
    .release = buffer_release,
};


int alloc_pages(struct doombuffer* buf)
{
    int err;
    int n_pages = (buf->size+PAGE_SIZE-1)/PAGE_SIZE;

    return 0;
}


int surface_open(struct inode *ino, struct file *file)
{
    return 0;
}


int surface_release(struct inode *ino, struct file *filep)
{
    return 0;
}


ssize_t surface_read(struct file *file, char __user *user_data, size_t size, loff_t *off)
{
    return 0;
}


int buffer_open(struct inode *ino, struct file *file)
{
    return 0;
}


int buffer_release(struct inode *ino, struct file *filep)
{
    return 0;
}


ssize_t buffer_write(struct file *file, const char __user *user_data, size_t size, loff_t *off)
{
    return 0;
}


