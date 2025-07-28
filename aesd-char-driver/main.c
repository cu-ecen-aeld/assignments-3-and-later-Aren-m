/**
 * @file aesdchar.c
 * @brief Functions and data related to the AESD char driver implementation
 *
 * Based on the implementation of the "scull" device driver, found in
 * Linux Device Drivers example code.
 *
 * @author Dan Walkes
 * @date 2019-10-22
 * @copyright Copyright (c) 2019
 *
**/

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h> // file_operations
#include "aesdchar.h"
#include "aesd_ioctl.h"
int aesd_major =   0; // use dynamic major
int aesd_minor =   0;

MODULE_AUTHOR("Risheek Mairal");
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

int aesd_open(struct inode *inode, struct file *filp)
{
    PDEBUG("open");

    struct aesd_dev *dev = container_of(inode->i_cdev, struct aesd_dev, cdev);
    filp->private_data = dev;

    return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");

    return 0;
}

ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = 0;
    struct aesd_dev *dev = filp->private_data;
    size_t entry_offset = 0;
    struct aesd_buffer_entry* buf_read = NULL;
    size_t read_size = 0;

    PDEBUG("read %zu bytes with offset %lld", count, *f_pos);

    if(mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    buf_read = aesd_circular_buffer_find_entry_offset_for_fpos(&dev->buffer, *f_pos, &entry_offset);

    if(!buf_read)
        goto escape;

    read_size = min(count, buf_read->size - entry_offset);

    if(copy_to_user(buf, buf_read->buffptr + entry_offset, read_size) > 0)
    {
        retval = -EFAULT;
        goto escape;
    }

    retval = read_size;
    *f_pos += retval;

escape:
    mutex_unlock(&dev->lock);
    return retval;
}

ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = -ENOMEM;
    struct aesd_dev *dev = filp->private_data;
    char *kcharbuffer = NULL;
    char *knewbuffer = NULL;
    size_t full_size = 0;
    struct aesd_buffer_entry add_entry;
    struct aesd_buffer_entry *oldest = NULL;

    PDEBUG("write %zu bytes with offset %lld", count, *f_pos);

    if(mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    kcharbuffer = kmalloc(count, GFP_KERNEL);
    if(!kcharbuffer)
        goto escape;

    if(copy_from_user(kcharbuffer, buf, count) > 0)
    {
        kfree(kcharbuffer);
        retval = -EFAULT;
        goto escape;
    }

    if(dev->partial_write)
    {
        full_size = count + dev->partial_write_size;
        knewbuffer = kmalloc(full_size, GFP_KERNEL);
        if(!knewbuffer)
        {
            kfree(kcharbuffer);
            goto escape;
        }

        memcpy(knewbuffer, dev->partial_write, dev->partial_write_size);
        memcpy(knewbuffer + dev->partial_write_size, kcharbuffer, count);

        kfree(kcharbuffer);
        kfree(dev->partial_write);
        dev->partial_write = knewbuffer;
        dev->partial_write_size = full_size;
    }
    else
    {
        dev->partial_write = kcharbuffer;
        dev->partial_write_size = count;
    }

    if(memchr(dev->partial_write, '\n', dev->partial_write_size) != NULL)
    {
        full_size = dev->partial_write_size;
        char *entrybuf = kmalloc(full_size, GFP_KERNEL);
        if(!entrybuf)
            goto escape;

        memcpy(entrybuf, dev->partial_write, full_size);
        add_entry.buffptr = entrybuf;
        add_entry.size = full_size;

        if(dev->buffer.full)
        {
            oldest = &dev->buffer.entry[dev->buffer.out_offs];
            if(oldest->buffptr)
            {
                kfree(oldest->buffptr);
                oldest->buffptr = NULL;
                oldest->size = 0;
            }
        }

        aesd_circular_buffer_add_entry(&dev->buffer, &add_entry);
        *f_pos += full_size;

        kfree(dev->partial_write);
        dev->partial_write = NULL;
        dev->partial_write_size = 0;
    }

    retval = count;

escape:
    mutex_unlock(&dev->lock);
    return retval;
}

loff_t aesd_llseek(struct file *filp, loff_t offset, int whence)
{
    loff_t retval = 0;
    struct aesd_dev *dev = filp->private_data;

    PDEBUG("seeking %lld bytes with whence %i", offset, whence);

    if(mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;
    retval = fixed_size_llseek(filp, offset, whence, dev->buffer.size);
    mutex_unlock(&dev->lock);

    return retval;
}

static long aesd_adjust_file_offset(struct file *filp, uint32_t write_cmd, uint32_t write_cmd_offset)
{
    long retval = -EINVAL;
    struct aesd_dev *dev = filp->private_data;
    struct aesd_buffer_entry add_entry;
    int rel_index = 0;

    PDEBUG("adjusting f_pos to %i relative index and command offset %i", write_cmd, write_cmd_offset);

    if(mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    rel_index = dev->buffer.out_offs + write_cmd;
    if(rel_index >= AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED)
        rel_index -= AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;

    if(!dev->buffer.entry[rel_index].buffptr)
        goto escape;

    if(write_cmd_offset >= dev->buffer.entry[rel_index].size)
        goto escape;

    filp->f_pos = write_cmd_offset;

    for(int i = 0; i < write_cmd; i++)
    {
        rel_index = (dev->buffer.out_offs + i) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;

        filp->f_pos += dev->buffer.entry[rel_index].size;
    }

    retval = 0;

escape:
    mutex_unlock(&dev->lock);
    return retval;
}

long aesd_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    long retval = -EINVAL;

    switch(cmd)
    {
        case AESDCHAR_IOCSEEKTO:
            struct aesd_seekto seekto;
            if(copy_from_user(&seekto, (const void __user *)arg, sizeof(seekto)) != 0)
                retval = -EFAULT;
            else
                retval = aesd_adjust_file_offset(filp, seekto.write_cmd, seekto.write_cmd_offset);
            break;
        default:
            retval = -ENOTTY;
            break;
    }

    return retval;
}

struct file_operations aesd_fops = 
{
    .owner =            THIS_MODULE,
    .read =             aesd_read,
    .write =            aesd_write,
    .open =             aesd_open,
    .release =          aesd_release,
    .llseek =           aesd_llseek,
    .unlocked_ioctl =   aesd_ioctl,
};

static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err, devno = MKDEV(aesd_major, aesd_minor);

    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &aesd_fops;
    err = cdev_add (&dev->cdev, devno, 1);
    if (err) {
        printk(KERN_ERR "Error %d adding aesd cdev", err);
    }
    return err;
}

int aesd_init_module(void)
{
    dev_t dev = 0;
    int result;
    result = alloc_chrdev_region(&dev, aesd_minor, 1,
            "aesdchar");
    aesd_major = MAJOR(dev);
    if (result < 0) {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }
    memset(&aesd_device,0,sizeof(struct aesd_dev));

    aesd_circular_buffer_init(&aesd_device.buffer);
    mutex_init(&aesd_device.lock);

    result = aesd_setup_cdev(&aesd_device);

    if( result ) {
        unregister_chrdev_region(dev, 1);
    }
    return result;

}

void aesd_cleanup_module(void)
{
    dev_t devno = MKDEV(aesd_major, aesd_minor);

    cdev_del(&aesd_device.cdev);

    int index;
    struct aesd_buffer_entry *entry;

    AESD_CIRCULAR_BUFFER_FOREACH(entry, &aesd_device.buffer, index)
    {
        if(entry->buffptr)
        {
            kfree(entry->buffptr);
            entry->buffptr = NULL;
            entry->size = 0;
        }
    }

    if(aesd_device.partial_write)
    {
        kfree(aesd_device.partial_write);
        aesd_device.partial_write = NULL;
        aesd_device.partial_write_size = 0;
    }

    unregister_chrdev_region(devno, 1);
}

module_init(aesd_init_module);
module_exit(aesd_cleanup_module);