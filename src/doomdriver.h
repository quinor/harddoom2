#ifndef DOOMDRIVER_H
#define DOOMDRIVER_H

#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/cdev.h>
#include <linux/mutex.h>


#define MAX_DEVICE_COUNT 256

#define DOOMHDR "[HardDoom2] "
#define DRIVER_NAME "hardddoom2"

#define DOOMDEV_REGISTER_SIZE 0x2000
#define DOOMDEV_ADDRESS_LENGTH 40


#define BOUNDS(min, max, elt) ((min) <= (elt) && (max) >= (elt))


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
    struct doomdevice* device;

};

struct doombuffer
{
    uint32_t* pagetable;
    uint32_t page_c;
    uint32_t size;
    // width and height only in use for the surface
    uint32_t width;
    uint32_t height;
    struct doomdevice* device;
};

extern struct doomdevice* devices[];


int chardev_create(struct doomdevice* doomdev);
void chardev_destroy(struct doomdevice* doomdev);

int chardev_init(void);
void chardev_exit(void);


extern struct file_operations surface_fops;
extern struct file_operations buffer_fops;



int pci_init(void);
void pci_exit(void);


#endif // DOOMDRIVER_H
