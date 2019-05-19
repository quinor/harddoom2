#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/err.h>
#include "harddoom2.h"
#include "doomdriver.h"

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Wojciech Jablonski");
MODULE_DESCRIPTION("HardDoom2 device driver for Advanced Operating Systems class");



int harddoom2_init(void)
{
    int err;
    printk(KERN_INFO DOOMHDR "Loading HardDoom ][ (TM) module\n");

    if ((err = chardev_init()))
        goto err_chardev_init;

    if ((err = pci_init()))
        goto err_pci_init;

    return 0;

    pci_exit();
err_pci_init:

    chardev_exit();
err_chardev_init:

    printk(KERN_INFO DOOMHDR "Failed to load HardDoom ][ (TM) module\n");
    return err;
}

void harddoom2_exit(void)
{
    // no need for lock since noone uses the doomdevice right now
    pci_exit();
    chardev_exit();
    printk(KERN_INFO DOOMHDR "Unloading HardDoom ][ (TM) module\n");
}

module_init(harddoom2_init);
module_exit(harddoom2_exit);
