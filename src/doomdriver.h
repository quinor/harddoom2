#ifndef DOOMDRIVER_H
#define DOOMDRIVER_H

#include "harddoom2.h"


#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/cdev.h>
#include <linux/mutex.h>


#define MAX_DEVICE_COUNT 256

#define DOOMHDR "[HardDoom2] "
#define DRIVER_NAME "harddoom2"

#define DOOMDEV_REGISTER_SIZE 0x2000
#define DOOMDEV_ADDRESS_LENGTH 40


#define OOBOUNDS(min, max, elt) ((min) > (elt) || (max) < (elt))


typedef struct {uint32_t w[8];} cmd_t;


struct doomdevice
{
    int id;
    void __iomem* registers;
    struct pci_dev* pci_device;
    struct device* chr_device;
    struct mutex lock;
};

struct doomfile
{
    union {
        struct doombuffer* array[8];
        struct {
            struct doombuffer* surf_src;
            struct doombuffer* surf_dst;
            struct doombuffer* texture;
            struct doombuffer* flat;
            struct doombuffer* colormap;
            struct doombuffer* translation;
            struct doombuffer* tranmap;
            struct doombuffer* cmd;
        } name;
    } buffers;

    doombuffer* cmd;

    struct mutex lock;
    struct doomdevice* device;
};

struct doombuffer
{
    uint32_t* dev_pagetable;
    dma_addr_t dev_pagetable_handle;
    uint8_t** usr_pagetable;
    uint32_t page_c;
    uint32_t size;
    // width and height only in use for the surface
    uint32_t width;
    uint32_t height;
    struct file* file;

    struct mutex lock;
    struct doomdevice* device;
};


extern struct doomdevice* devices[];


void destroy_buffer(struct doombuffer* buf);

int chardev_create(struct doomdevice* doomdev);
void chardev_destroy(struct doomdevice* doomdev);

int chardev_init(void);
void chardev_exit(void);


int alloc_pagetable(struct doombuffer* buf);
void free_pagetable(struct doombuffer* buf);

extern struct file_operations buffer_fops;


int pci_init(void);
void pci_exit(void);


#endif // DOOMDRIVER_H
