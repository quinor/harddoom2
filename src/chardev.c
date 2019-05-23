#include "doomdriver.h"
#include "doomdev2.h"

#include <linux/err.h>
#include <linux/cdev.h>
#include <linux/anon_inodes.h>
#include <linux/uaccess.h>
#include <linux/file.h>

static dev_t doom_major;

static struct cdev doom_cdev;

static struct class doom_class = {
    .name = "harddoom2",
    .owner = THIS_MODULE,
};

static int doom_open(struct inode *ino, struct file *file);
static int doom_release(struct inode *ino, struct file *file);
static ssize_t doom_write(struct file *file, const char __user *user_data, size_t size, loff_t *off);
static long doom_ioctl(struct file *file, unsigned int cmd, unsigned long arg);

static struct file_operations doom_fops = {
    .owner = THIS_MODULE,
    .open = doom_open,
    .write = doom_write,
    .unlocked_ioctl = doom_ioctl,
    .compat_ioctl = doom_ioctl,
    .release = doom_release,
};


static struct kmem_cache* doomfile_cache;
static struct kmem_cache* doombuffer_cache;


static int doom_open(struct inode *ino, struct file *file)
{
    int err;
    struct doomfile* df;
    struct doomfile aux = {0};

    if (0 == (df = kmem_cache_alloc(doomfile_cache, 0)))
    {
        err = ENOMEM;
        goto err_cache_alloc;
    }

    *df = aux; // zero everything. df->buffers == 0
    df->device = devices[MINOR(ino->i_rdev)];

    // TODO: cmd init

    file->private_data = df;

    return 0;

    kmem_cache_free(doomfile_cache, df);
err_cache_alloc:
    return err;
}


static int doom_release(struct inode *ino, struct file *file)
{
    int i;
    struct doomfile* df;

    df = file->private_data;

    for (i=0; i<7; i++)
        if (df->buffers.array[i] != 0)
            fput(df->buffers.array[i]->file);

    kmem_cache_free(doomfile_cache, df);
    return 0;
}


static int alloc_buffer_inode(struct doomfile* df, uint32_t size, uint32_t width, uint32_t height)
{
    int err;
    struct doombuffer* buf;
    struct file* file;
    int fd;

    if (0 == (buf = kmem_cache_alloc(doombuffer_cache, 0)))
    {
        err = ENOMEM;
        goto err_cache_alloc;
    }

    buf->size = size;
    buf->width = width;
    buf->height = height;
    mutex_init(&buf->lock);
    buf->device = df->device;

    if ((err = alloc_pagetable(buf)))
        goto err_alloc_pagetable;

    if ((fd = get_unused_fd_flags(O_RDWR)) < 0)
    {
        err = -fd;
        goto err_get_fd;
    }

    if (IS_ERR(file = anon_inode_getfile("doom_buffer", &buffer_fops, buf, O_RDWR)))
    {
        err = PTR_ERR(file);
        goto err_file;
    }
    file->f_mode |= FMODE_LSEEK | FMODE_PREAD | FMODE_PWRITE;
    buf->file = file;

    fd_install(fd, file);

    return fd;

err_file:

    put_unused_fd(fd);
err_get_fd:

    free_pagetable(buf);
err_alloc_pagetable:

    kmem_cache_free(doombuffer_cache, buf);
err_cache_alloc:
    return -err;
}

void destroy_buffer(struct doombuffer* buf)
{
    kmem_cache_free(doombuffer_cache, buf);
}


static long doom_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    int err;
    struct doomfile* df;
    uint32_t size, width, height;

    df = file->private_data;

    // printk(KERN_INFO DOOMHDR "got IOCTL %d\n", cmd);

    switch(cmd)
    {
        case DOOMDEV2_IOCTL_CREATE_SURFACE:
        {
            struct doomdev2_ioctl_create_surface ioctl_surf;
            if ((err = copy_from_user(
                &ioctl_surf,
                (const void __user *)arg,
                sizeof(struct doomdev2_ioctl_create_surface)
            )))
                return -err;
            size = ioctl_surf.width*ioctl_surf.height;
            width = ioctl_surf.width;
            height = ioctl_surf.height;

            if (size > 2048*2048)
                return -EOVERFLOW;

            if (
                OOBOUNDS(1, 2048, width) ||
                OOBOUNDS(1, 2048, height) ||
                ((width&63) != 0)
            )
                return -EINVAL;
            return alloc_buffer_inode(df, size, width, height);
        }
        case DOOMDEV2_IOCTL_CREATE_BUFFER:
        {
            struct doomdev2_ioctl_create_buffer ioctl_buffer;
            if ((err = copy_from_user(
                &ioctl_buffer,
                (const void __user *)arg,
                sizeof(struct doomdev2_ioctl_create_buffer)
            )))
                return -err;

            size = ioctl_buffer.size;
            width = 0;
            height = 0;

            if (size > 2048*2048)
                return -EOVERFLOW;

            if (OOBOUNDS(1, 2048*2048, size))
                return -EINVAL;

            return alloc_buffer_inode(df, size, width, height);
        }
        case DOOMDEV2_IOCTL_SETUP:
        {
            uint32_t fds[7];
            int i;

            err = 0;
            mutex_lock(&df->lock);

            if ((err = copy_from_user(
                &fds,
                (const void __user *)arg,
                sizeof(uint32_t)*7
            )))
                goto ioctl_fail;

            for (i=0; i<7; i++)
                if (fds[i] != -1)
                {
                    struct file* cur_file;
                    if ((cur_file = fget(fds[i])) == NULL)
                    {
                        err = EINVAL;
                        goto ioctl_fail;
                    }

                    if (cur_file->f_op != &buffer_fops)
                    {
                        fput(cur_file);
                        err = EINVAL;
                        goto ioctl_fail;
                    }

                    if (df->buffers.array[i] != 0)
                        fput(df->buffers.array[i]->file);

                    df->buffers.array[i] = cur_file->private_data;
                }

        ioctl_fail:
            mutex_unlock(&df->lock);
            return -err;
        }
        default:
            return -ENOTTY;
    }
}


ssize_t doom_write(struct file *file, const char __user *user_data, size_t count, loff_t *off)
{
    int ret;
    struct doomfile* df;

    df = file->private_data;

    mutex_lock(&df->device->lock);

    if (count % sizeof(struct doomdev2_cmd) != 0)
    {
        ret = -EINVAL;
        goto err_end;
    }
    // TODO: send setup
    // TODO: run commands

err_end:
    mutex_unlock(&df->device->lock);
    return count;
}


int chardev_create(struct doomdevice* doomdev)
{
    // create device instance (the file will get created in /dev)
    doomdev->chr_device = device_create(
        &doom_class,
        &(doomdev->pci_device->dev),
        doom_major+doomdev->id,
        0,
        "doom%d",
        doomdev->id
    );
    if (0 == (doomdev->chr_device))
        return ENOMEM;
    return 0;
}


void chardev_destroy(struct doomdevice* doomdev)
{
    device_destroy(&doom_class, doom_major+doomdev->id);
}


int chardev_init(void)
{
    int err;

    // doomfile cache init
    doomfile_cache = KMEM_CACHE(doomfile, 0);
    if (IS_ERR(doomfile_cache))
    {
        err = PTR_ERR(doomfile_cache);
        goto err_cache_init;
    }

    // doombuffer cache init
    doombuffer_cache = KMEM_CACHE(doombuffer, 0);
    if (IS_ERR(doombuffer_cache))
    {
        err = PTR_ERR(doombuffer_cache);
        goto err_cache_init2;
    }

    // region
    if ((err = alloc_chrdev_region(&doom_major, 0, MAX_DEVICE_COUNT, "harddoom2")))
        goto err_alloc;

    // init and add the device
    cdev_init(&doom_cdev, &doom_fops);
    if ((err = cdev_add(&doom_cdev, doom_major, MAX_DEVICE_COUNT)))
        goto err_cdev;

    // register the class
    if ((err = class_register(&doom_class)))
        goto err_class;

    return 0;

    class_unregister(&doom_class);
err_class:

    cdev_del(&doom_cdev);
err_cdev:

    unregister_chrdev_region(doom_major, MAX_DEVICE_COUNT);
err_alloc:

    kmem_cache_destroy(doombuffer_cache);
err_cache_init2:

    kmem_cache_destroy(doomfile_cache);
err_cache_init:
    return err;
}


void chardev_exit(void)
{
    class_unregister(&doom_class);
    cdev_del(&doom_cdev);
    unregister_chrdev_region(doom_major, MAX_DEVICE_COUNT);
    kmem_cache_destroy(doombuffer_cache);
    kmem_cache_destroy(doomfile_cache);
}
