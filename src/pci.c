#include "doomdriver.h"
#include "doomcode2.h"

#include <linux/err.h>
#include <linux/mutex.h>


static DEFINE_MUTEX(global_driver_lock);



static int doomdev_probe (struct pci_dev *dev, const struct pci_device_id *id);

static void doomdev_remove (struct pci_dev *dev);

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
    .shutdown = doomdev_shutdown,
};


struct doomdevice* devices[MAX_DEVICE_COUNT];
static struct kmem_cache* doomdevice_cache;


static irqreturn_t doomdev_irq_handler(int irq, void *dev)
{
    uint32_t intr;
    struct doomdevice* doomdev;

    doomdev = dev;

    intr = ioread32(doomdev->registers + HARDDOOM2_INTR);
    if (intr == 0)
        return IRQ_NONE;

    iowrite32(intr, doomdev->registers + HARDDOOM2_INTR);

    if (intr & HARDDOOM2_INTR_PONG_SYNC)
        up(&doomdev->wait_pong);

    if (intr & (~HARDDOOM2_INTR_PONG_SYNC))
    {
        doomdev->enabled = 0;
        up(&doomdev->wait_pong);
        printk(KERN_ERR DOOMHDR "Interrupts caught on device %d: %x\n", doomdev->id, intr);
        printk(KERN_ERR DOOMHDR "Disabling the device %d, please restart it manually", doomdev->id);
    }

    return IRQ_HANDLED;
}


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
    doomdev->enabled = 1;
    mutex_init(&doomdev->lock);
    sema_init(&doomdev->wait_pong, 0);
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

    // if ((err = pci_set_consistent_dma_mask(dev, DMA_BIT_MASK(DOOMDEV_ADDRESS_LENGTH))))
    //     goto err_pci_dma_mask;

    // Interrupts
    if ((err = request_irq(
        dev->irq, doomdev_irq_handler, IRQF_SHARED, DRIVER_NAME, doomdev)))
        goto err_irq;

    // command pagetable
    if (IS_ERR(doomdev->cmd = alloc_pagetable(doomdev, sizeof(cmd_t)*DOOMDEV_MAX_CMD_COUNT, 0, 0)))
    {
        err = PTR_ERR(doomdev->cmd);
        goto err_cmd_init;
    }

    // Boot the device
    iowrite32(0, doomdev->registers+HARDDOOM2_FE_CODE_ADDR);
    for (i=0; i<sizeof(doomcode2)/sizeof(uint32_t); i++)
        iowrite32(doomcode2[i], doomdev->registers+HARDDOOM2_FE_CODE_WINDOW);

    iowrite32(HARDDOOM2_RESET_ALL, doomdev->registers+HARDDOOM2_RESET);
    iowrite32(HARDDOOM2_INTR_MASK, doomdev->registers+HARDDOOM2_INTR);
    iowrite32(HARDDOOM2_INTR_MASK ^ HARDDOOM2_INTR_FENCE ^ HARDDOOM2_INTR_PONG_ASYNC, doomdev->registers+HARDDOOM2_INTR_ENABLE);

    iowrite32(doomdev->cmd->dev_pagetable_handle, doomdev->registers+HARDDOOM2_CMD_PT);
    iowrite32(DOOMDEV_MAX_CMD_COUNT, doomdev->registers+HARDDOOM2_CMD_SIZE);
    iowrite32(0, doomdev->registers+HARDDOOM2_CMD_READ_IDX);
    iowrite32(0, doomdev->registers+HARDDOOM2_CMD_WRITE_IDX);

    iowrite32(HARDDOOM2_ENABLE_ALL, doomdev->registers+HARDDOOM2_ENABLE);

    if ((err = chardev_create(doomdev)))
        goto err_chardev_create;

    mutex_unlock(&doomdev->lock);

    printk(KERN_INFO DOOMHDR "Loaded device (vendor %x, dev %x) with ID %d\n",
        dev_id->vendor,
        dev_id->device,
        id
    );

    return 0;

    mutex_lock(&doomdev->lock);

    chardev_destroy(doomdev);

err_chardev_create:
    free_pagetable(doomdev->cmd);

err_cmd_init:
    iowrite32(0, doomdev->registers+HARDDOOM2_ENABLE);
    iowrite32(0, doomdev->registers+HARDDOOM2_INTR_ENABLE);
    free_irq(dev->irq, doomdev);

err_irq:
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

    free_pagetable(doomdev->cmd);

    iowrite32(0, doomdev->registers+HARDDOOM2_INTR_ENABLE);
    iowrite32(0, doomdev->registers+HARDDOOM2_ENABLE);
    free_irq(dev->irq, doomdev);

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
