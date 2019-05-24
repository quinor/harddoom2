#include "doomdriver.h"
#include "doomdev2.h"
#include "harddoom2.h"
#include "debug.h"

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
struct kmem_cache* doombuffer_cache;


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
    mutex_init(&df->lock);

    if (IS_ERR(df->cmd = alloc_pagetable(df->device, sizeof(cmd_t)*DOOMDEV_MAX_CMD_COUNT, 0, 0)))
    {
        err = PTR_ERR(df->cmd);
        goto err_cmd_init;
    }

    file->private_data = df;

    return 0;

    free_pagetable(df->cmd);
err_cmd_init:

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

    free_pagetable(df->cmd);
    kmem_cache_free(doomfile_cache, df);
    return 0;
}


static int alloc_buffer_inode(struct doomfile* df, uint32_t size, uint32_t width, uint32_t height)
{
    int err;
    struct doombuffer* buf;
    struct file* file;
    int fd;

    if (IS_ERR(buf = alloc_pagetable(df->device, size, width, height)))
    {
        err = PTR_ERR(buf);
        goto err_alloc_pagetable;
    }

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
            // TODO: repair partial fail?
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

                    if (i<2 && df->buffers.array[i]->size == 0)
                    {
                        err = EINVAL;
                        goto ioctl_fail;
                    }
                }


        ioctl_fail:
            mutex_unlock(&df->lock);
            return -err;
        }
        default:
            return -ENOTTY;
    }
}


static void write_cmd(struct doombuffer* cmdbuf, cmd_t *command, size_t pos)
{
    cmd_t* page;

    pos = pos*sizeof(cmd_t);
    // printk(KERN_INFO DOOMHDR "cmd written to %lx (shifted %lx)\n", pos, pos >> 12);
    page = (cmd_t*)(cmdbuf->usr_pagetable[pos >> 12]);
    page[pos&(PAGE_SIZE-1)] = *command;
}


static int decode_cmd(struct doomfile* df, cmd_t* decoded_cmd, struct doomdev2_cmd* raw_cmd)
{
    #define CHECK(cond) if(!(cond)) {return EINVAL;}

    #define IN_SURFACE(x, y, surf) (0 <= (x) && (x) <= (surf)->width && 0 <= (y) && (y) <= (surf)->height)
    // TODO: check params
    int i;
    for (i=0; i<8; i++)
        decoded_cmd->w[i] = 0;

    switch(raw_cmd->type)
    {
        case DOOMDEV2_CMD_TYPE_FILL_RECT:
        {
            CHECK(IN_SURFACE(
                raw_cmd->fill_rect.pos_x,
                raw_cmd->fill_rect.pos_y,
                df->buffers.name.surf_dst
            ))

            CHECK(IN_SURFACE(
                raw_cmd->fill_rect.pos_x+raw_cmd->fill_rect.width,
                raw_cmd->fill_rect.pos_y+raw_cmd->fill_rect.height,
                df->buffers.name.surf_dst
            ))

            decoded_cmd->w[0] = HARDDOOM2_CMD_W0(
                HARDDOOM2_CMD_TYPE_FILL_RECT,
                0
            );
            decoded_cmd->w[2] = HARDDOOM2_CMD_W2(
                raw_cmd->fill_rect.pos_x,
                raw_cmd->fill_rect.pos_y,
                0
            );
            decoded_cmd->w[6] = HARDDOOM2_CMD_W6_A(
                raw_cmd->fill_rect.width,
                raw_cmd->fill_rect.height,
                raw_cmd->fill_rect.fill_color
            );
            return 0;
        }
        case DOOMDEV2_CMD_TYPE_DRAW_LINE:
        {
            CHECK(IN_SURFACE(
                raw_cmd->draw_line.pos_a_x,
                raw_cmd->draw_line.pos_a_y,
                df->buffers.name.surf_dst
            ))

            CHECK(IN_SURFACE(
                raw_cmd->draw_line.pos_b_x,
                raw_cmd->draw_line.pos_b_y,
                df->buffers.name.surf_dst
            ))

            decoded_cmd->w[0] = HARDDOOM2_CMD_W0(
                HARDDOOM2_CMD_TYPE_DRAW_LINE,
                0
            );
            decoded_cmd->w[2] = HARDDOOM2_CMD_W2(
                raw_cmd->draw_line.pos_a_x,
                raw_cmd->draw_line.pos_a_y,
                0
            );
            decoded_cmd->w[3] = HARDDOOM2_CMD_W3(
                raw_cmd->draw_line.pos_b_x,
                raw_cmd->draw_line.pos_b_y
            );
            decoded_cmd->w[6] = HARDDOOM2_CMD_W6_A(
                0, 0,
                raw_cmd->draw_line.fill_color
            );
            return 0;
        }
        default:
        {
            decoded_cmd->w[0] = HARDDOOM2_CMD_W0(HARDDOOM2_CMD_TYPE_SETUP, 0); // NOP
            return 0;
        }
    }

    #undef CHECK
    #undef IN_SURFACE
}

struct doomdev2_cmd raw_cmds[DOOMDEV_MAX_CMD_COUNT-1];

ssize_t doom_write(struct file *file, const char __user *user_data, size_t count, loff_t *off)
{
    int i;
    int err;
    int cmd_c = 0;
    struct doomfile* df;
    cmd_t cur_cmd = {0};
    void __iomem* reg_enable;

    // printk(KERN_INFO DOOMHDR "send attempt of %d\n", count);

    df = file->private_data;

    if (count % sizeof(struct doomdev2_cmd) != 0)
    {
        err = -EINVAL;
        // printk(KERN_INFO DOOMHDR "fail1\n");
        goto err_end;
    }

    count /= sizeof(struct doomdev2_cmd);

    // if (count >= 1000)
    //     printk(KERN_INFO DOOMHDR "big count is %d\n", count);
    if (count >= DOOMDEV_MAX_CMD_COUNT)
    {
        printk(KERN_INFO DOOMHDR "truncated count to %d from %d\n", DOOMDEV_MAX_CMD_COUNT-1, count);
        count = DOOMDEV_MAX_CMD_COUNT-1;
    }

    if ((err = -copy_from_user(
        &raw_cmds,
        user_data,
        sizeof(struct doomdev2_cmd)*count
    )))
    {
        // printk(KERN_INFO DOOMHDR "fail2\n");
        goto err_end;
    }

    // decode setup
    cur_cmd.w[0] = HARDDOOM2_CMD_W0_SETUP(
        HARDDOOM2_CMD_TYPE_SETUP, //type

        //flags
        (df->buffers.name.surf_dst != 0 ? HARDDOOM2_CMD_FLAG_SETUP_SURF_DST : 0) |
        (df->buffers.name.surf_src != 0 ? HARDDOOM2_CMD_FLAG_SETUP_SURF_SRC : 0) |
        (df->buffers.name.texture != 0 ? HARDDOOM2_CMD_FLAG_SETUP_TEXTURE : 0) |
        (df->buffers.name.flat != 0 ? HARDDOOM2_CMD_FLAG_SETUP_FLAT : 0) |
        (df->buffers.name.translation != 0 ? HARDDOOM2_CMD_FLAG_SETUP_TRANSLATION : 0) |
        (df->buffers.name.colormap != 0 ? HARDDOOM2_CMD_FLAG_SETUP_COLORMAP : 0) |
        (df->buffers.name.tranmap != 0 ? HARDDOOM2_CMD_FLAG_SETUP_TRANMAP : 0) ,

        df->buffers.name.surf_dst != 0 ? df->buffers.name.surf_dst->width : 0, //sdwidth
        df->buffers.name.surf_src != 0 ? df->buffers.name.surf_src->width : 0 //sswidth
    );
    for (i=0; i<7; i++)
        if (df->buffers.array[i] != 0)
            cur_cmd.w[i+1] = df->buffers.array[i]->dev_pagetable_handle;

    write_cmd(df->cmd, &cur_cmd, cmd_c++);

    // printk(KERN_INFO DOOMHDR "count is %d, cmd_c is %d\n", count, cmd_c);

    // decode rest of the commands
    while (cmd_c <= count)
    {
        if ((err = -decode_cmd(df, &cur_cmd, &raw_cmds[cmd_c-1])))
        {
            printk(KERN_INFO DOOMHDR "fail3\n");
            goto err_end;
        }
        // printk(KERN_INFO DOOMHDR "args: %lx, %lx, %d\n", df->cmd, &cur_cmd, cmd_c);
        write_cmd(df->cmd, &cur_cmd, cmd_c++);
    }


    mutex_lock(&df->device->lock);


    iowrite32(df->cmd->dev_pagetable_handle, df->device->registers+HARDDOOM2_CMD_PT);
    iowrite32(0, df->device->registers+HARDDOOM2_CMD_READ_IDX);
    iowrite32(cmd_c, df->device->registers+HARDDOOM2_CMD_WRITE_IDX);

    // TODO: run commands
    reg_enable = df->device->registers+HARDDOOM2_ENABLE;
    iowrite32(HARDDOOM2_ENABLE_CMD_FETCH|ioread32(reg_enable), reg_enable);

    if (ioread32(reg_enable) != HARDDOOM2_ENABLE_ALL)
    {
        printk(KERN_INFO DOOMHDR "registers enabled %lx (fe error %lx)\n",
            ioread32(reg_enable),
            ioread32(df->device->registers+HARDDOOM2_FE_ERROR_CODE)
        );
        printk(KERN_INFO DOOMHDR "target surface of size %d by %d (size: %d)\n",
            df->buffers.name.surf_dst->width,
            df->buffers.name.surf_dst->height,
            df->buffers.name.surf_dst->size
        );

        printk(KERN_INFO DOOMHDR "current command:\n\t%lx\n\t%lx\n\t%lx\n\t%lx\n\t%lx\n\t%lx\n\t%lx\n\t%lx\n",
            ioread32(df->device->registers+HARDDOOM2_FE_REG(0)),
            ioread32(df->device->registers+HARDDOOM2_FE_REG(1)),
            ioread32(df->device->registers+HARDDOOM2_FE_REG(2)),
            ioread32(df->device->registers+HARDDOOM2_FE_REG(3)),
            ioread32(df->device->registers+HARDDOOM2_FE_REG(4)),
            ioread32(df->device->registers+HARDDOOM2_FE_REG(5)),
            ioread32(df->device->registers+HARDDOOM2_FE_REG(6)),
            ioread32(df->device->registers+HARDDOOM2_FE_REG(7))
        );
    }

    // TODO: wait for the ran commands
    while (ioread32(df->device->registers+HARDDOOM2_CMD_READ_IDX) != cmd_c);

    iowrite32((~HARDDOOM2_ENABLE_CMD_FETCH)&ioread32(reg_enable), reg_enable);
    mutex_unlock(&df->device->lock);

    // printk(KERN_INFO DOOMHDR "succeeded with %d\n", (cmd_c-1)*sizeof(struct doomdev2_cmd));
    return (cmd_c-1)*sizeof(struct doomdev2_cmd);

err_end:

    // printk(KERN_INFO DOOMHDR "failed with %d\n", err);
    return err;
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
