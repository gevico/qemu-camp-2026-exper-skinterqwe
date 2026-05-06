#include <linux/init.h>
#include <linux/module.h>

#include "gpgpu_char.h"
#include "gpgpu_pci.h"

static int __init gpgpu_init(void)
{
    int ret;

    ret = gpgpu_char_init();
    if (ret)
        return ret;

    ret = gpgpu_pci_register();
    if (ret) {
        gpgpu_char_exit();
        return ret;
    }

    pr_info("gpgpu: module loaded\n");
    return 0;
}

static void __exit gpgpu_exit(void)
{
    gpgpu_pci_unregister();
    gpgpu_char_exit();
    pr_info("gpgpu: module unloaded\n");
}

module_init(gpgpu_init);
module_exit(gpgpu_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Gevico Camp 2026");
MODULE_DESCRIPTION("Educational GPGPU PCI driver");
