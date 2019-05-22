#include "doomdriver.h"

#include <linux/fs.h>
#include <linux/uaccess.h>


static int buffer_open(struct inode *ino, struct file *file);
static int buffer_release(struct inode *ino, struct file *filep);
static ssize_t buffer_read(struct file *file, char __user *user_data, size_t size, loff_t *off);
static ssize_t buffer_write(struct file *file, const char __user *user_data, size_t size, loff_t *off);
static loff_t buffer_llseek(struct file *file, loff_t off, int whence);

struct file_operations buffer_fops = {
    .owner = THIS_MODULE,
    .open = buffer_open,
    .read = buffer_read,
    .write = buffer_write,
    .llseek = buffer_llseek,
    .release = buffer_release,
};


int alloc_pagetable(struct doombuffer* buf)
{
    int err;
    int n_pages = (buf->size+PAGE_SIZE-1)/PAGE_SIZE;
    dma_addr_t temp_handle;

    // printk(KERN_INFO DOOMHDR "buffer at %lx\n", buf);
    // printk(KERN_INFO DOOMHDR "allocating %d pages for a buffer of size %d\n", n_pages, buf->size);
    // printk(KERN_INFO DOOMHDR "(W: %d, H: %d page_c: %d)\n", buf->width, buf->height, buf->page_c);

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
    // printk(KERN_INFO DOOMHDR "allocated pagetable @%lx (cpu: %lx)\n", temp_handle, buf->dev_pagetable);

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
        // printk(KERN_INFO DOOMHDR "allocated page @%lx (cpu; %lx)\n", temp_handle, buf->usr_pagetable[buf->page_c]);
        buf->dev_pagetable[buf->page_c] = HARDDOOM2_PTE_VALID|HARDDOOM2_PTE_WRITABLE | (temp_handle >> 8);
        buf->page_c++;
    }
    // printk(KERN_INFO DOOMHDR "allocated %d pages\n", buf->page_c);

    return 0;

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

    return err;
}

void free_pagetable(struct doombuffer* buf)
{
    // printk(KERN_INFO DOOMHDR "freeing %d pages (table@ %lx)\n", buf->page_c, buf->usr_pagetable);
    while (buf->page_c)
    {
        buf->page_c--;
        // printk(KERN_INFO DOOMHDR "freeing page (usr)%lx (dev)%lx\n",
        //     buf->usr_pagetable[buf->page_c],
        //     (buf->dev_pagetable[buf->page_c] & HARDDOOM2_PTE_PHYS_MASK) << 8
        // );
        dma_free_coherent(
            &buf->device->pci_device->dev,
            PAGE_SIZE,
            buf->usr_pagetable[buf->page_c],
            (buf->dev_pagetable[buf->page_c] & HARDDOOM2_PTE_PHYS_MASK) << 8
        );
    }

    // printk(KERN_INFO DOOMHDR "freeing pagetable %lx\n", buf->usr_pagetable);

    kfree(buf->usr_pagetable);

    // printk(KERN_INFO DOOMHDR "freeing dev pagetable (usr)%lx (dev)%lx\n",
    //     buf->dev_pagetable,
    //     buf->dev_pagetable_handle << 8
    // );

    dma_free_coherent(
        &buf->device->pci_device->dev,
        PAGE_SIZE,
        buf->dev_pagetable,
        buf->dev_pagetable_handle << 8
    );
}


static int buffer_open(struct inode *ino, struct file *file)
{
    return alloc_pagetable(file->private_data);
}


static int buffer_release(struct inode *ino, struct file *file)
{
    // printk(KERN_INFO DOOMHDR "Destruction part 1 @ %lx\n", file->private_data);
    free_pagetable(file->private_data);
    // printk(KERN_INFO DOOMHDR "Destruction part 2\n");
    destroy_buffer(file->private_data);
    // printk(KERN_INFO DOOMHDR "Destruction part 3\n");
    return 0;
}


static ssize_t buffer_read(struct file *file, char __user *user_data, size_t count, loff_t *off)
{
    loff_t start;
    loff_t pos;
    loff_t end;
    struct doombuffer* buf;
    ssize_t ret;

    // printk(KERN_INFO DOOMHDR "reading from buffer...\n");
    // printk(KERN_INFO DOOMHDR "file had modes %x\n", file->f_mode);

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

    // printk(KERN_INFO DOOMHDR "writing to buffer...\n");

    buf = file->private_data;

    // printk(KERN_INFO DOOMHDR "japa\n");
    mutex_lock(&buf->lock);
    start = pos = *off;

    // printk(KERN_INFO DOOMHDR "kurwa\n");
    if (pos < 0 || pos > buf->size)
    {
        // printk(KERN_INFO DOOMHDR "write fail: position out of bounds\n");
        ret = -EFAULT;
        goto err_end;
    }

    // printk(KERN_INFO DOOMHDR "dupa\n");
    if (count > buf->size - pos)
        count = buf->size - pos;
    end = pos + count;

    if (pos == end)
    {
        // printk(KERN_INFO DOOMHDR "write fail: position at the end of the file\n");
        ret = -EFAULT;
        goto err_end;
    }

    // printk(KERN_INFO DOOMHDR "chuj\n");
    while (pos != end)
    {
        loff_t copy_end;
        copy_end = (pos + PAGE_SIZE)&(~(PAGE_SIZE-1));
        if (copy_end > end)
            copy_end = end;

        // printk(KERN_INFO DOOMHDR "pizda\n");
        // printk(KERN_INFO DOOMHDR "copy @pos %lx to %lx from %lx elts %x",
        //     pos,
        //         buf->usr_pagetable[pos>>12]+(pos & (PAGE_SIZE-1)),
        //     user_data+pos,
        //     copy_end-pos
        // );
        if ((ret = copy_from_user(
            buf->usr_pagetable[pos>>12]+(pos & (PAGE_SIZE-1)),
            user_data+pos,
            copy_end-pos
        )))
        {
            // printk(KERN_INFO DOOMHDR "write fail: copy_from_user failed\n");
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
