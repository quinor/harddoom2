#include "doomdriver.h"

#include <linux/fs.h>
#include <linux/uaccess.h>


static int buffer_release(struct inode *ino, struct file *filep);
static ssize_t buffer_read(struct file *file, char __user *user_data, size_t size, loff_t *off);
static ssize_t buffer_write(struct file *file, const char __user *user_data, size_t size, loff_t *off);
static loff_t buffer_llseek(struct file *file, loff_t off, int whence);

struct file_operations buffer_fops = {
    .owner = THIS_MODULE,
    .read = buffer_read,
    .write = buffer_write,
    .llseek = buffer_llseek,
    .release = buffer_release,
};


struct doombuffer* alloc_pagetable(struct doomdevice* device, uint32_t size, uint32_t width, uint32_t height)
{
    int err;
    struct doombuffer* buf;
    int n_pages = (size+PAGE_SIZE-1)/PAGE_SIZE;

    dma_addr_t temp_handle;

    if (0 == (buf = kmem_cache_alloc(doombuffer_cache, 0)))
    {
        err = ENOMEM;
        goto err_cache_alloc;
    }

    // printk(KERN_INFO DOOMHDR "doombuffer at  %lx\n", buf);

    buf->size = size;
    buf->width = width;
    buf->height = height;
    mutex_init(&buf->lock);
    buf->device = device;

    if (0 == (buf->dev_pagetable = dma_alloc_coherent(
        &buf->device->pci_device->dev,
        sizeof(uint32_t)*n_pages,
        &temp_handle,
        0
    )))
    {
        err = ENOMEM;
        goto err_alloc_dev_pagetable;
    }

    buf->dev_pagetable_handle = temp_handle >> 8;

    if (0 == (buf->usr_pagetable = kmalloc(sizeof(void*)*n_pages, 0)))
    {
        err = ENOMEM;
        goto err_alloc_usr_pagetable;
    }

    buf->page_c = 0;
    while (buf->page_c < n_pages)
    {
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
        buf->dev_pagetable[buf->page_c] = HARDDOOM2_PTE_VALID|HARDDOOM2_PTE_WRITABLE | (temp_handle >> 8);
        buf->page_c++;
    }

    return buf;

err_alloc_pages:
    while (buf->page_c)
    {
        buf->page_c--;
        dma_free_coherent(
            &buf->device->pci_device->dev,
            PAGE_SIZE,
            buf->usr_pagetable[buf->page_c],
            (buf->dev_pagetable[buf->page_c] & HARDDOOM2_PTE_PHYS_MASK) << 8
        );
    }

    kfree(buf->usr_pagetable);
err_alloc_usr_pagetable:

    dma_free_coherent(
        &buf->device->pci_device->dev,
        PAGE_SIZE,
        buf->dev_pagetable,
        buf->dev_pagetable_handle << 8
    );
err_alloc_dev_pagetable:

    kmem_cache_free(doombuffer_cache, buf);
err_cache_alloc:

    return ERR_PTR(err);
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
            (buf->dev_pagetable[buf->page_c] & HARDDOOM2_PTE_PHYS_MASK) << 8
        );
    }

    kfree(buf->usr_pagetable);

    dma_free_coherent(
        &buf->device->pci_device->dev,
        PAGE_SIZE,
        buf->dev_pagetable,
        buf->dev_pagetable_handle << 8
    );

    kmem_cache_free(doombuffer_cache, buf);
}


static int buffer_release(struct inode *ino, struct file *file)
{
    free_pagetable(file->private_data);
    return 0;
}


static ssize_t buffer_read(struct file *file, char __user *user_data, size_t count, loff_t *off)
{
    loff_t start;
    loff_t pos;
    loff_t end;
    struct doombuffer* buf;
    ssize_t ret;

    buf = file->private_data;

    mutex_lock(&buf->lock);
    start = pos = *off;

    if (pos < 0 || pos > buf->size)
    {
        ret = -EFAULT;
        goto err_end;
    }

    if (count > buf->size - pos)
        count = buf->size - pos;
    end = pos + count;

    if (pos == end)
    {
        ret = 0;
        goto err_end;
    }

    while (pos != end)
    {
        loff_t copy_end;
        copy_end = (pos + PAGE_SIZE)&(~(PAGE_SIZE-1));
        if (copy_end > end)
            copy_end = end;

        if ((ret = copy_to_user(
            user_data+pos,
            buf->usr_pagetable[pos>>12]+(pos & (PAGE_SIZE-1)),
            copy_end-pos
        )))
        {
            *off = pos;
            if (pos != start)
                ret = pos-start;
            else
                ret = -EFAULT;
            goto err_end;
        }

        pos = copy_end;
    }
    *off = pos;

    ret = pos-start;

err_end:
    mutex_unlock(&buf->lock);
    return ret;
}


static ssize_t buffer_write(struct file *file, const char __user *user_data, size_t count, loff_t *off)
{
    loff_t start;
    loff_t pos;
    loff_t end;
    struct doombuffer* buf;
    ssize_t ret;

    buf = file->private_data;

    mutex_lock(&buf->lock);

    // printk(KERN_INFO DOOMHDR "write request of size %d...\n", count);
    // printk(KERN_INFO DOOMHDR "buf stats: page_c %d, size %d, width %d, height %d\n",
        // buf->page_c, buf->size, buf->width, buf->height);

    start = pos = *off;

    if (pos < 0 || pos > buf->size)
    {
        // printk(KERN_INFO DOOMHDR "fail1\n");
        ret = -EFAULT;
        goto err_end;
    }

    if (count > buf->size - pos)
        count = buf->size - pos;
    end = pos + count;

    if (pos == end)
    {
        // printk(KERN_INFO DOOMHDR "count: %d buf->size: %d pos: %d\n", count, buf->size, pos);
        // printk(KERN_INFO DOOMHDR "fail2\n");
        ret = -EFAULT;
        goto err_end;
    }

    while (pos != end)
    {
        loff_t copy_end;
        copy_end = (pos + PAGE_SIZE)&(~(PAGE_SIZE-1));
        if (copy_end > end)
            copy_end = end;

        if ((ret = copy_from_user(
            buf->usr_pagetable[pos>>12]+(pos & (PAGE_SIZE-1)),
            user_data+pos,
            copy_end-pos
        )))
        {
            *off = pos;
            if (pos != start)
                ret = pos-start;
            else
                ret = -EFAULT;
            // printk(KERN_INFO DOOMHDR "fail3\n");
            goto err_end;
        }

        pos = copy_end;
    }
    *off = pos;

    ret = pos-start;

    // printk(KERN_INFO DOOMHDR "...wrote %d\n", ret);
    mutex_unlock(&buf->lock);
    return ret;

err_end:
    // printk(KERN_INFO DOOMHDR "failed with %d\n", ret);
    mutex_unlock(&buf->lock);
    return ret;
}


static loff_t buffer_llseek(struct file *file, loff_t filepos, int whence)
{
    struct doombuffer* buf;
    loff_t pos;

    buf = file->private_data;

    mutex_lock(&buf->lock);

    switch (whence)
    {
        case SEEK_SET:
            pos = filepos;
            break;
        case SEEK_CUR:
            pos = file->f_pos + filepos;
            break;
        case SEEK_END:
            pos = buf->size + filepos;
            break;
        default:
            return -EINVAL;
    }

    file->f_pos = pos;
    mutex_unlock(&buf->lock);
    return pos;
}
