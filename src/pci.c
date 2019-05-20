#include "doomdriver.h"
#include "harddoom2.h"
#include "doomcode2.h"

#include <linux/err.h>
#include <linux/mutex.h>


static DEFINE_MUTEX(global_driver_lock);



static int doomdev_probe (struct pci_dev *dev, const struct pci_device_id *id);

static void doomdev_remove (struct pci_dev *dev);

static int doomdev_suspend (struct pci_dev *dev, pm_message_t state);

static int doomdev_resume (struct pci_dev *dev);

static void doomdev_shutdown (struct pci_dev *dev);


static struct pci_device_id id_table[2] = {
    {PCI_DEVICE(HARDDOOM2_VENDOR_ID, HARDDOOM2_DEVICE_ID)},
    {}
};

static struct pci_driver drv = {
    .name = DRIVER_NAME,
    .id_table = id_table,
    .probe = doomdev_probe,
    .remove = doomdev_remove,
    .suspend = doomdev_suspend,
    .resume = doomdev_resume,
    .shutdown = doomdev_shutdown,
};


struct doomdevice* devices[MAX_DEVICE_COUNT];
static struct kmem_cache* doomdevice_cache;


static int doomdev_probe (struct pci_dev *dev, const struct pci_device_id *dev_id)
{
    int id;
    struct doomdevice* doomdev;
    int err;
    int i;

    mutex_lock(&global_driver_lock);

    for (id=0; id<MAX_DEVICE_COUNT; id++)
        if (devices[id] == NULL)
            break;
    if (id == MAX_DEVICE_COUNT)
    {
        err = ENOMEM;
        goto err_dev_count;
    }

    if (0 == (doomdev = kmem_cache_alloc(doomdevice_cache, 0)))
    {
        err = ENOMEM;
        goto err_cache_alloc;
    }

    doomdev->id = id;
    doomdev->pci_device = dev;
    mutex_init(&doomdev->lock);
    devices[id] = doomdev;
    pci_set_drvdata(dev, doomdev);

    mutex_unlock(&global_driver_lock);
    mutex_lock(&doomdev->lock);

    // boot the pcie device
    if ((err = pci_enable_device(dev)))
        goto err_pci_enable;

    // MMIO
    if ((err = pci_request_regions(dev, DRIVER_NAME)))
        goto err_pci_request_regions;

    if ((doomdev->registers = pci_iomap(dev, 0, DOOMDEV_REGISTER_SIZE)) == 0)
        goto err_pci_iomap;

    // DMA
    pci_set_master(dev);

    if ((err = pci_set_dma_mask(dev, DMA_BIT_MASK(DOOMDEV_ADDRESS_LENGTH))))
        goto err_pci_dma_mask;

    // Interrupts
    // TODO

    // Boot the device
    iowrite32(0, doomdev->registers+HARDDOOM2_FE_CODE_ADDR);
    for (i=0; i<sizeof(doomcode2)/sizeof(uint32_t); i++)
        iowrite32(doomcode2[i], doomdev->registers+HARDDOOM2_FE_CODE_WINDOW);

    iowrite32(HARDDOOM2_RESET_ALL, doomdev->registers+HARDDOOM2_RESET);
    iowrite32(HARDDOOM2_INTR_MASK, doomdev->registers+HARDDOOM2_INTR);
    iowrite32(HARDDOOM2_ENABLE_ALL^HARDDOOM2_ENABLE_CMD_FETCH, doomdev->registers+HARDDOOM2_ENABLE);
    iowrite32(0, doomdev->registers+HARDDOOM2_INTR); // TODO: add handled interrupts

    printk(KERN_INFO DOOMHDR "Loaded device (vendor %x, dev %x) with ID %d\n",
        dev_id->vendor,
        dev_id->device,
        id
    );

    if ((err = chardev_create(doomdev)))
        goto err_chardev_create;

    mutex_unlock(&doomdev->lock);

    return 0;

    mutex_lock(&doomdev->lock);
    chardev_destroy(doomdev);

err_chardev_create:
    iowrite32(0, doomdev->registers+HARDDOOM2_INTR_ENABLE);
    iowrite32(0, doomdev->registers+HARDDOOM2_ENABLE);
    ioread32(doomdev->registers+HARDDOOM2_FE_CODE_ADDR);

err_pci_dma_mask:
    pci_clear_master(dev);
    pci_iounmap(dev, doomdev->registers);

err_pci_iomap:
    pci_release_regions(dev);

err_pci_request_regions:
    pci_disable_device(dev);

err_pci_enable:
    mutex_unlock(&doomdev->lock);
    mutex_lock(&global_driver_lock);
    kmem_cache_free(doomdevice_cache, doomdev);
    devices[id] = NULL;

err_cache_alloc:
err_dev_count:
    mutex_unlock(&global_driver_lock);

    return err;
}


static void doomdev_remove (struct pci_dev *dev)
{
    int id;
    struct doomdevice* doomdev;
    doomdev = pci_get_drvdata(dev);

    mutex_lock(&doomdev->lock);

    chardev_destroy(doomdev);

    iowrite32(0, doomdev->registers+HARDDOOM2_INTR_ENABLE);
    iowrite32(0, doomdev->registers+HARDDOOM2_ENABLE);
    ioread32(doomdev->registers+HARDDOOM2_FE_CODE_ADDR);

    pci_clear_master(dev);
    pci_iounmap(dev, doomdev->registers);
    pci_release_regions(dev);
    pci_disable_device(dev);

    mutex_unlock(&doomdev->lock);
    mutex_lock(&global_driver_lock);

    id = doomdev->id;
    devices[id] = NULL;
    kmem_cache_free(doomdevice_cache, doomdev);

    mutex_unlock(&global_driver_lock);

    printk(KERN_INFO DOOMHDR "Removed device with ID %d\n", id);
}


static int doomdev_suspend (struct pci_dev *dev, pm_message_t state)
{
    struct doomdevice* doomdev;
    doomdev = pci_get_drvdata(dev);

    mutex_lock(&global_driver_lock);

    // TODO

    mutex_unlock(&global_driver_lock);
    return 0;
}


static int doomdev_resume (struct pci_dev *dev)
{
    struct doomdevice* doomdev;
    doomdev = pci_get_drvdata(dev);

    mutex_lock(&global_driver_lock);

    // TODO

    mutex_unlock(&global_driver_lock);
    return 0;
}


static void doomdev_shutdown (struct pci_dev *dev)
{}


int pci_init(void)
{
    int err;

    doomdevice_cache = KMEM_CACHE(doomdevice, 0);
    if (IS_ERR(doomdevice_cache))
    {
        err = PTR_ERR(doomdevice_cache);
        goto err_cache_init;
    }

    if ((err = pci_register_driver(&drv)))
        goto err_pci_register;

    return 0;

    pci_unregister_driver(&drv);
err_pci_register:
    kmem_cache_destroy(doomdevice_cache);
err_cache_init:

    return err;
}


void pci_exit(void)
{
    pci_unregister_driver(&drv);
    kmem_cache_destroy(doomdevice_cache);
}
