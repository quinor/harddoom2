#include "doomdriver.h"
#include "doomdev2.h"

#include <linux/err.h>
#include <linux/cdev.h>
#include <linux/anon_inodes.h>
#include <linux/uaccess.h>

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

    if (IS_ERR(df = kmem_cache_alloc(doomfile_cache, 0)))
    {
        err = PTR_ERR(df);
        goto err_cache_alloc;
    }

    df->device = devices[MINOR(ino->i_rdev)];
    file->private_data = df;

    return 0;

    kmem_cache_free(doomfile_cache, df);
err_cache_alloc:
    return err;
}


static int doom_release(struct inode *ino, struct file *file)
{
    struct doomfile* df;
    df = file->private_data;

    kmem_cache_free(doomfile_cache, df);
    return 0;
}


static long doom_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    int err;
    struct doomfile* df;
    df = file->private_data;

    switch(cmd)
    {
        case DOOMDEV2_IOCTL_CREATE_SURFACE:
        {
            struct doomdev2_ioctl_create_surface ioctl_surf;
            struct doombuffer* buf;
            if ((err = copy_from_user(
                &ioctl_surf,
                (const void __user *)arg,
                sizeof(struct doomdev2_ioctl_create_surface))
            ))
                goto b1_err_getuser;

            if (IS_ERR(buf = kmem_cache_alloc(doombuffer_cache, 0)))
            {
                err = PTR_ERR(df);
                goto b1_err_cache_alloc;
            }

            buf->size = ioctl_surf.width*ioctl_surf.height;
            buf->width = ioctl_surf.width;
            buf->height = ioctl_surf.height;
            buf->device = df->device;

            if (!(
                BOUNDS(1, 2048, buf->width) &&
                BOUNDS(1, 2048, buf->height) &&
                ((buf->width&63) == 0)
            ))
            {
                err = -EINVAL;
                goto b1_err_out_of_bounds;
            }

            return anon_inode_getfd("doom_surface", &surface_fops, buf, 0);

        b1_err_out_of_bounds:
            kmem_cache_free(doombuffer_cache, buf);
        b1_err_cache_alloc:
        b1_err_getuser:
            return err;
        }
        case DOOMDEV2_IOCTL_CREATE_BUFFER:
        {
            struct doomdev2_ioctl_create_buffer ioctl_buffer;
            struct doombuffer* buf;
            if ((err = copy_from_user(
                &ioctl_buffer,
                (const void __user *)arg,
                sizeof(struct doomdev2_ioctl_create_buffer))
            ))
                goto b2_err_getuser;

            if (IS_ERR(buf = kmem_cache_alloc(doombuffer_cache, 0)))
            {
                err = PTR_ERR(df);
                goto b2_err_cache_alloc;
            }

            buf->size = ioctl_buffer.size;
            buf->device = df->device;

            if (!(BOUNDS(1, 2048*2048, buf->size)))
            {
                err = -EINVAL;
                goto b2_err_out_of_bounds;
            }

            return anon_inode_getfd("doom_buffer", &buffer_fops, buf, 0);

        b2_err_out_of_bounds:
            kmem_cache_free(doombuffer_cache, buf);
        b2_err_cache_alloc:
        b2_err_getuser:
            return err;
        }
        case DOOMDEV2_IOCTL_SETUP:
        {
            // TODO
            break;
        }
        default:
            return -ENOTTY;
    }
    return 0;
}


ssize_t doom_write(struct file *file, const char __user *user_data, size_t size, loff_t *off)
{
    // TODO
    return 0;
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
    if (IS_ERR(doomdev->chr_device))
        return PTR_ERR(doomdev->chr_device);
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
    doombuffer_cache = KMEM_CACHE(doomfile, 0);
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
