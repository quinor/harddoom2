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
    .name = "doomdev",
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

    if (0 == (df->raw_cmds = vmalloc(sizeof(struct doomdev2_cmd)*DOOMDEV_MAX_CMD_COUNT)))
    {
        err = ENOMEM;
        goto err_rawcmd_alloc;
    }

    file->private_data = df;

    return 0;

    vfree(df->raw_cmds);
err_rawcmd_alloc:

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

    vfree(df->raw_cmds);
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
            if (copy_from_user(
                &ioctl_surf,
                (const void __user *)arg,
                sizeof(struct doomdev2_ioctl_create_surface)
            ))
                return -EFAULT;
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
            if (copy_from_user(
                &ioctl_buffer,
                (const void __user *)arg,
                sizeof(struct doomdev2_ioctl_create_buffer)
            ))
                return -EFAULT;

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
            struct doombuffer* buf;

            err = 0;
            mutex_lock(&df->lock);

            if (copy_from_user(
                &fds,
                (const void __user *)arg,
                sizeof(uint32_t)*7
            ))
            {
                err = EFAULT;
                goto ioctl_fail;
            }

            for (i=0; i<7; i++)
                if (fds[i] != -1)
                {
                    struct file* cur_file;
                    if ((cur_file = fget(fds[i])) == NULL)
                    {
                        err = EINVAL;
                        goto ioctl_fail;
                    }

                    buf = cur_file->private_data;

                    if (cur_file->f_op != &buffer_fops || buf->device != df->device)
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
    BUG_ON(pos>>12 >= cmdbuf->page_c);
    BUG_ON((pos & (PAGE_SIZE-1)) + sizeof(cmd_t) > PAGE_SIZE);

    page = (cmd_t*)(cmdbuf->usr_pagetable[pos >> 12]);
    page[(pos&(PAGE_SIZE-1))/sizeof(cmd_t)] = *command;
}


static int decode_cmd(struct doomfile* df, cmd_t* decoded_cmd, struct doomdev2_cmd* raw_cmd)
{
    int i;

    #define CHECK(cond) if(!(cond)) {return EINVAL;}

    #define IN_SURFACE(x, y, surf) ((surf) != 0 && 0 <= (x) && (x) <= (surf)->width && 0 <= (y) && (y) <= (surf)->height)

    #define IS_TEXTURE(surf) ((surf) != 0)

    #define IN_FLAT(idx, surf) ((surf) != 0 && 0 <= (idx) && (idx) < (surf)->page_c)

    #define IN_COLORMAP(idx, surf) ((surf) != 0 && 0 <= (idx) && (idx) < ((surf)->size >> 8))

    #define IS_TRANMAP(surf) ((surf) != 0 && (surf)->size == (1<<16))

    #define CONVERT_FLAGS(flags) ((((flags) & DOOMDEV2_CMD_FLAGS_TRANSLATE) ? HARDDOOM2_CMD_FLAG_TRANSLATION : 0) | (((flags) & DOOMDEV2_CMD_FLAGS_COLORMAP) ? HARDDOOM2_CMD_FLAG_COLORMAP : 0) | (((flags) & DOOMDEV2_CMD_FLAGS_TRANMAP) ? HARDDOOM2_CMD_FLAG_TRANMAP : 0))

    for (i=0; i<8; i++)
        decoded_cmd->w[i] = 0;

    decoded_cmd->w[0] = HARDDOOM2_CMD_W0(HARDDOOM2_CMD_TYPE_SETUP, 0); // default NOP

    switch(raw_cmd->type)
    {
        case DOOMDEV2_CMD_TYPE_COPY_RECT:
        {
            struct doomdev2_cmd_copy_rect cur;
            cur = raw_cmd->copy_rect;

            CHECK(IN_SURFACE(
                cur.pos_src_x,
                cur.pos_src_y,
                df->buffers.name.surf_src
            ))

            CHECK(IN_SURFACE(
                cur.pos_src_x+cur.width,
                cur.pos_src_y+cur.height,
                df->buffers.name.surf_src
            ))

            CHECK(IN_SURFACE(
                cur.pos_dst_x,
                cur.pos_dst_y,
                df->buffers.name.surf_dst
            ))

            CHECK(IN_SURFACE(
                cur.pos_dst_x+cur.width,
                cur.pos_dst_y+cur.height,
                df->buffers.name.surf_dst
            ))

            decoded_cmd->w[0] = HARDDOOM2_CMD_W0(
                HARDDOOM2_CMD_TYPE_COPY_RECT,
                (df->buffers.name.surf_dst == df->buffers.name.surf_src ? HARDDOOM2_CMD_FLAG_INTERLOCK : 0)
            );
            decoded_cmd->w[2] = HARDDOOM2_CMD_W2(
                cur.pos_src_x,
                cur.pos_src_y,
                0
            );
            decoded_cmd->w[3] = HARDDOOM2_CMD_W3(
                cur.pos_dst_x,
                cur.pos_dst_y
            );
            decoded_cmd->w[6] = HARDDOOM2_CMD_W6_A(
                cur.width,
                cur.height,
                0
            );
            return 0;
        }
        case DOOMDEV2_CMD_TYPE_FILL_RECT:
        {
            struct doomdev2_cmd_fill_rect cur;
            cur = raw_cmd->fill_rect;

            CHECK(IN_SURFACE(
                cur.pos_x,
                cur.pos_y,
                df->buffers.name.surf_dst
            ))

            CHECK(IN_SURFACE(
                cur.pos_x+cur.width,
                cur.pos_y+cur.height,
                df->buffers.name.surf_dst
            ))

            decoded_cmd->w[0] = HARDDOOM2_CMD_W0(
                HARDDOOM2_CMD_TYPE_FILL_RECT,
                0
            );
            decoded_cmd->w[2] = HARDDOOM2_CMD_W2(
                cur.pos_x,
                cur.pos_y,
                0
            );
            decoded_cmd->w[6] = HARDDOOM2_CMD_W6_A(
                cur.width,
                cur.height,
                cur.fill_color
            );
            return 0;
        }
        case DOOMDEV2_CMD_TYPE_DRAW_LINE:
        {
            struct doomdev2_cmd_draw_line cur;
            cur = raw_cmd->draw_line;

            CHECK(IN_SURFACE(
                cur.pos_a_x,
                cur.pos_a_y,
                df->buffers.name.surf_dst
            ))

            CHECK(IN_SURFACE(
                cur.pos_b_x,
                cur.pos_b_y,
                df->buffers.name.surf_dst
            ))

            decoded_cmd->w[0] = HARDDOOM2_CMD_W0(
                HARDDOOM2_CMD_TYPE_DRAW_LINE,
                0
            );
            decoded_cmd->w[2] = HARDDOOM2_CMD_W2(
                cur.pos_a_x,
                cur.pos_a_y,
                0
            );
            decoded_cmd->w[3] = HARDDOOM2_CMD_W3(
                cur.pos_b_x,
                cur.pos_b_y
            );
            decoded_cmd->w[6] = HARDDOOM2_CMD_W6_A(
                0, 0,
                cur.fill_color
            );
            return 0;
        }
        case DOOMDEV2_CMD_TYPE_DRAW_BACKGROUND:
        {
            struct doomdev2_cmd_draw_background cur;
            cur = raw_cmd->draw_background;

            CHECK(IN_SURFACE(
                cur.pos_x,
                cur.pos_y,
                df->buffers.name.surf_dst
            ))

            CHECK(IN_SURFACE(
                cur.pos_x+cur.width,
                cur.pos_y+cur.height,
                df->buffers.name.surf_dst
            ))

            CHECK(IN_FLAT(cur.flat_idx, df->buffers.name.flat))

            decoded_cmd->w[0] = HARDDOOM2_CMD_W0(
                HARDDOOM2_CMD_TYPE_DRAW_BACKGROUND,
                0
            );
            decoded_cmd->w[2] = HARDDOOM2_CMD_W2(
                cur.pos_x,
                cur.pos_y,
                cur.flat_idx
            );
            decoded_cmd->w[6] = HARDDOOM2_CMD_W6_A(
                cur.width,
                cur.height,
                0
            );
            return 0;
        }
        case DOOMDEV2_CMD_TYPE_DRAW_COLUMN:
        {
            struct doomdev2_cmd_draw_column cur;
            uint32_t flags;
            cur = raw_cmd->draw_column;

            flags = CONVERT_FLAGS(cur.flags);

            CHECK(IN_SURFACE(
                cur.pos_x,
                cur.pos_a_y,
                df->buffers.name.surf_dst
            ))

            CHECK(IN_SURFACE(
                cur.pos_x,
                cur.pos_b_y,
                df->buffers.name.surf_dst
            ))

            CHECK(IS_TEXTURE(df->buffers.name.texture))

            if (flags & HARDDOOM2_CMD_FLAG_TRANSLATION)
                CHECK(IN_COLORMAP(cur.translation_idx, df->buffers.name.translation))

            if (flags & HARDDOOM2_CMD_FLAG_COLORMAP)
                CHECK(IN_COLORMAP(cur.colormap_idx, df->buffers.name.colormap))

            if (flags & HARDDOOM2_CMD_FLAG_TRANMAP)
                CHECK(IS_TRANMAP(df->buffers.name.tranmap))

            decoded_cmd->w[0] = HARDDOOM2_CMD_W0(
                HARDDOOM2_CMD_TYPE_DRAW_COLUMN,
                flags
            );
            decoded_cmd->w[1] = HARDDOOM2_CMD_W1(
                (flags & HARDDOOM2_CMD_FLAG_TRANSLATION ? cur.translation_idx : 0),
                (flags & HARDDOOM2_CMD_FLAG_COLORMAP ? cur.colormap_idx : 0)
            );
            decoded_cmd->w[2] = HARDDOOM2_CMD_W2(
                cur.pos_x,
                cur.pos_a_y,
                0
            );
            decoded_cmd->w[3] = HARDDOOM2_CMD_W3(
                cur.pos_x,
                cur.pos_b_y
            );
            decoded_cmd->w[4] = cur.ustart;
            decoded_cmd->w[5] = cur.ustep;
            decoded_cmd->w[6] = HARDDOOM2_CMD_W6_B(cur.texture_offset);
            decoded_cmd->w[7] = HARDDOOM2_CMD_W7_B(
                (df->buffers.name.texture->size-1) >> 6,
                cur.texture_height
            );

            return 0;
        }
        case DOOMDEV2_CMD_TYPE_DRAW_FUZZ:
        {
            struct doomdev2_cmd_draw_fuzz cur;
            cur = raw_cmd->draw_fuzz;

            CHECK(cur.fuzz_pos < 56 && cur.fuzz_pos >= 0)

            CHECK(IN_SURFACE(
                cur.pos_x,
                cur.pos_a_y,
                df->buffers.name.surf_dst
            ))

            CHECK(IN_SURFACE(
                cur.pos_x,
                cur.pos_b_y,
                df->buffers.name.surf_dst
            ))

            CHECK(cur.fuzz_start <= cur.pos_a_y && cur.pos_a_y <= cur.pos_b_y
                && cur.pos_b_y <= cur.fuzz_end)

            CHECK(IN_COLORMAP(cur.colormap_idx, df->buffers.name.colormap))

            decoded_cmd->w[0] = HARDDOOM2_CMD_W0(
                HARDDOOM2_CMD_TYPE_DRAW_FUZZ,
                0
            );
            decoded_cmd->w[1] = HARDDOOM2_CMD_W1(
                0,
                cur.colormap_idx
            );
            decoded_cmd->w[2] = HARDDOOM2_CMD_W2(
                cur.pos_x,
                cur.pos_a_y,
                0
            );
            decoded_cmd->w[3] = HARDDOOM2_CMD_W3(
                cur.pos_x,
                cur.pos_b_y
            );
            decoded_cmd->w[6] = HARDDOOM2_CMD_W6_C(
                cur.fuzz_start,
                cur.fuzz_end,
                cur.fuzz_pos
            );

            return 0;
        }
        case DOOMDEV2_CMD_TYPE_DRAW_SPAN:
        {
            struct doomdev2_cmd_draw_span cur;
            uint32_t flags;
            cur = raw_cmd->draw_span;

            flags = CONVERT_FLAGS(cur.flags);

            CHECK(IN_SURFACE(
                cur.pos_a_x,
                cur.pos_y,
                df->buffers.name.surf_dst
            ))

            CHECK(IN_SURFACE(
                cur.pos_b_x,
                cur.pos_y,
                df->buffers.name.surf_dst
            ))

            CHECK(IN_FLAT(cur.flat_idx, df->buffers.name.flat))

            if (flags & HARDDOOM2_CMD_FLAG_TRANSLATION)
                CHECK(IN_COLORMAP(cur.translation_idx, df->buffers.name.translation))

            if (flags & HARDDOOM2_CMD_FLAG_COLORMAP)
                CHECK(IN_COLORMAP(cur.colormap_idx, df->buffers.name.colormap))

            if (flags & HARDDOOM2_CMD_FLAG_TRANMAP)
                CHECK(IS_TRANMAP(df->buffers.name.tranmap))

            decoded_cmd->w[0] = HARDDOOM2_CMD_W0(
                HARDDOOM2_CMD_TYPE_DRAW_SPAN,
                flags
            );
            decoded_cmd->w[1] = HARDDOOM2_CMD_W1(
                (flags & HARDDOOM2_CMD_FLAG_TRANSLATION ? cur.translation_idx : 0),
                (flags & HARDDOOM2_CMD_FLAG_COLORMAP ? cur.colormap_idx : 0)
            );
            decoded_cmd->w[2] = HARDDOOM2_CMD_W2(
                cur.pos_a_x,
                cur.pos_y,
                0
            );
            decoded_cmd->w[3] = HARDDOOM2_CMD_W3(
                cur.pos_b_x,
                cur.pos_y
            );
            decoded_cmd->w[4] = cur.ustart;
            decoded_cmd->w[5] = cur.ustep;
            decoded_cmd->w[6] = cur.vstart;
            decoded_cmd->w[7] = cur.vstep;
            return 0;
        }
        default:
        {
            return EINVAL;
        }
    }

    #undef CHECK
    #undef IN_SURFACE
    #undef IN_TEXTURE
    #undef IN_FLAT
    #undef IN_COLORMAP
    #undef IS_TRANMAP
    #undef CONVERT_FLAGS
}


static ssize_t doom_write(struct file *file, const char __user *user_data, size_t count, loff_t *off)
{
    int err;
    uint32_t cmd_c = 0;
    uint32_t start;
    struct doomfile* df;
    cmd_t cur_cmd = {0};

    df = file->private_data;

    if (count % sizeof(struct doomdev2_cmd) != 0)
    {
        err = -EINVAL;
        goto err_end;
    }

    count /= sizeof(struct doomdev2_cmd);

    mutex_lock(&df->lock);
    mutex_lock(&df->device->lock);

    // that means that the device has crashed and does not accept commands
    if (!df->device->enabled)
    {
        err = -EIO;
        goto err_end_lock;
    }

    // space for setup and one free space for the cyclic buffer indices
    if (count >= DOOMDEV_MAX_CMD_COUNT-1)
        count = DOOMDEV_MAX_CMD_COUNT-2;

    if (copy_from_user(
        df->raw_cmds,
        user_data,
        sizeof(struct doomdev2_cmd)*count
    ))
    {
        err = -EFAULT;
        goto err_end_lock;
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

    if (df->buffers.name.surf_dst != 0)
        cur_cmd.w[1] = df->buffers.name.surf_dst->dev_pagetable_handle;
    if (df->buffers.name.surf_src != 0)
        cur_cmd.w[2] = df->buffers.name.surf_src->dev_pagetable_handle;
    if (df->buffers.name.texture != 0)
        cur_cmd.w[3] = df->buffers.name.texture->dev_pagetable_handle;
    if (df->buffers.name.flat != 0)
        cur_cmd.w[4] = df->buffers.name.flat->dev_pagetable_handle;
    if (df->buffers.name.translation != 0)
        cur_cmd.w[5] = df->buffers.name.translation->dev_pagetable_handle;
    if (df->buffers.name.colormap != 0)
        cur_cmd.w[6] = df->buffers.name.colormap->dev_pagetable_handle;
    if (df->buffers.name.tranmap != 0)
        cur_cmd.w[7] = df->buffers.name.tranmap->dev_pagetable_handle;


    start = ioread32(df->device->registers+HARDDOOM2_CMD_WRITE_IDX);

    write_cmd(df->device->cmd, &cur_cmd, start);
    start = (start+1)&(DOOMDEV_MAX_CMD_COUNT-1);
    cmd_c++;

    // decode rest of the commands
    while (cmd_c <= count)
    {
        if ((err = -decode_cmd(df, &cur_cmd, &df->raw_cmds[cmd_c-1])))
            goto err_end_lock;

         // last command
        if (cmd_c == count)
            cur_cmd.w[0] |= HARDDOOM2_CMD_FLAG_PING_SYNC;

        write_cmd(df->device->cmd, &cur_cmd, start);
        start = (start+1)&(DOOMDEV_MAX_CMD_COUNT-1);
        cmd_c++;
    }

    iowrite32(start, df->device->registers+HARDDOOM2_CMD_WRITE_IDX);
    down(&df->device->wait_pong);

    // that means that the device has crashed and does not accept commands, the operation has failed
    if (!df->device->enabled)
    {
        err = -EIO;
        goto err_end_lock;
    }

    mutex_unlock(&df->device->lock);
    mutex_unlock(&df->lock);

    return (cmd_c-1)*sizeof(struct doomdev2_cmd);

err_end_lock:
    mutex_unlock(&df->device->lock);
    mutex_unlock(&df->lock);

err_end:
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
    if ((err = alloc_chrdev_region(&doom_major, 0, MAX_DEVICE_COUNT, "doomdev")))
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
