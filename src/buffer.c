#include "doomdriver.h"

#include <linux/fs.h>


static int buffer_open(struct inode *ino, struct file *file);
static int buffer_release(struct inode *ino, struct file *filep);
static ssize_t buffer_read(struct file *file, char __user *user_data, size_t size, loff_t *off);
static ssize_t buffer_write(struct file *file, const char __user *user_data, size_t size, loff_t *off);

struct file_operations buffer_fops = {
    .owner = THIS_MODULE,
    .open = buffer_open,
    .read = buffer_read,
    .write = buffer_write,
    .release = buffer_release,
};


int alloc_pagetable(struct doombuffer* buf)
{
    int err;
    int n_pages = (buf->size+PAGE_SIZE-1)/PAGE_SIZE;

    if (0 == (buf->dev_pagetable = dma_alloc_coherent(
        &buf->device->pci_device->dev,
        PAGE_SIZE,
        &buf->dev_pagetable_handle,
        0
    )))
    {
        err = ENOMEM;
        goto err_alloc_dev_pagetable;
    }

    if (0 == (buf->usr_pagetable = kmalloc(2*PAGE_SIZE, 0)))
    {
        err = ENOMEM;
        goto err_alloc_usr_pagetable;
    }

    while (buf->page_c < n_pages)
    {
        dma_addr_t temp_handle;
        if (0 == (buf->usr_pagetable[buf->page_c] = dma_alloc_coherent(
            &buf->device->pci_device->dev,
            PAGE_SIZE,
            &temp_handle,
            0
        )))
        {
            err = ENOMEM;
            goto err_alloc_pages;
        }
        buf->dev_pagetable[buf->page_c] = 3 | (temp_handle >> 8);
        buf->page_c++;
    }

    return 0;

err_alloc_pages:
    while (buf->page_c)
    {
        buf->page_c--;
        dma_free_coherent(
            &buf->device->pci_device->dev,
            PAGE_SIZE,
            buf->usr_pagetable[buf->page_c],
            (buf->dev_pagetable[buf->page_c] & (~0xf)) << 8
        );
    }

    kfree(buf->usr_pagetable);
err_alloc_usr_pagetable:

    dma_free_coherent(
        &buf->device->pci_device->dev,
        PAGE_SIZE,
        buf->dev_pagetable,
        buf->dev_pagetable_handle
    );
err_alloc_dev_pagetable:

    return err;
}

void free_pagetable(struct doombuffer* buf)
{
    while (buf->page_c)
    {
        buf->page_c--;
        dma_free_coherent(
            &buf->device->pci_device->dev,
            PAGE_SIZE,
            buf->usr_pagetable[buf->page_c],
            (buf->dev_pagetable[buf->page_c] & (~0xf)) << 8
        );
    }

    kfree(buf->usr_pagetable);

    dma_free_coherent(
        &buf->device->pci_device->dev,
        PAGE_SIZE,
        buf->dev_pagetable,
        buf->dev_pagetable_handle
    );
}


int buffer_open(struct inode *ino, struct file *file)
{
    return alloc_pagetable(file->private_data);
}


int buffer_release(struct inode *ino, struct file *file)
{
    free_pagetable(file->private_data);
    return 0;
}


ssize_t buffer_read(struct file *file, char __user *user_data, size_t size, loff_t *off)
{
    return 0;
}


ssize_t buffer_write(struct file *file, const char __user *user_data, size_t size, loff_t *off)
{
    return 0;
}
