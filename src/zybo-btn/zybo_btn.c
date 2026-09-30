// SPDX-License-Identifier: GPL-2.0
/*
 * zybo_btn - debounced push-button events for the Zybo Z7 MIO buttons
 *
 * Buttons come from the "button-gpios" DT property. Every edge (re)arms a
 * delayed work; when the line has been stable for debounce_ms the new level
 * is queued as a struct zybo_btn_event. Userspace reads the events from
 * /dev/zybo_btn, either blocking or through poll().
 *
 * sysfs (on the platform device):
 *   debounce_ms  rw  debounce time, 1..1000 ms
 *   presses      ro  press counter per button
 *   dropped      ro  events lost because the fifo was full
 */

#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/kfifo.h>
#include <linux/miscdevice.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/timekeeping.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#include "zybo_btn.h"

#define ZYBO_BTN_FIFO_LEN	32	/* power of two */
#define ZYBO_BTN_DEBOUNCE_MS	20

struct zybo_btn_priv;

struct zybo_btn {
	struct zybo_btn_priv *priv;
	struct gpio_desc *gpiod;
	struct delayed_work work;
	unsigned int index;
	int level;
	unsigned long presses;
};

struct zybo_btn_priv {
	struct device *dev;
	struct miscdevice misc;
	wait_queue_head_t wq;
	spinlock_t lock;		/* protects fifo */
	DECLARE_KFIFO(fifo, struct zybo_btn_event, ZYBO_BTN_FIFO_LEN);
	unsigned int debounce_ms;
	unsigned long dropped;
	unsigned int nbtn;
	struct zybo_btn btn[];
};

static void zybo_btn_work(struct work_struct *work)
{
	struct zybo_btn *btn = container_of(to_delayed_work(work),
					    struct zybo_btn, work);
	struct zybo_btn_priv *priv = btn->priv;
	struct zybo_btn_event ev;
	int val;

	val = gpiod_get_value_cansleep(btn->gpiod);
	if (val < 0 || val == btn->level)
		return;

	btn->level = val;
	if (val)
		btn->presses++;

	ev.timestamp_ns = ktime_get_ns();
	ev.button = btn->index;
	ev.pressed = val;

	spin_lock(&priv->lock);
	if (!kfifo_put(&priv->fifo, ev))
		priv->dropped++;
	spin_unlock(&priv->lock);

	wake_up_interruptible(&priv->wq);
}

static irqreturn_t zybo_btn_isr(int irq, void *data)
{
	struct zybo_btn *btn = data;
	unsigned int ms = READ_ONCE(btn->priv->debounce_ms);

	mod_delayed_work(system_wq, &btn->work, msecs_to_jiffies(ms));
	return IRQ_HANDLED;
}

static bool zybo_btn_pop(struct zybo_btn_priv *priv, struct zybo_btn_event *ev)
{
	bool ok;

	spin_lock(&priv->lock);
	ok = kfifo_get(&priv->fifo, ev);
	spin_unlock(&priv->lock);

	return ok;
}

static bool zybo_btn_pending(struct zybo_btn_priv *priv)
{
	bool pending;

	spin_lock(&priv->lock);
	pending = !kfifo_is_empty(&priv->fifo);
	spin_unlock(&priv->lock);
	return pending;
}

static ssize_t zybo_btn_read(struct file *file, char __user *buf,
			     size_t count, loff_t *ppos)
{
	struct zybo_btn_priv *priv = container_of(file->private_data,
						  struct zybo_btn_priv, misc);
	struct zybo_btn_event ev;
	size_t done = 0;
	int ret;

	if (count < sizeof(ev))
		return -EINVAL;

	for (;;) {
		while (done + sizeof(ev) <= count && zybo_btn_pop(priv, &ev)) {
			if (copy_to_user(buf + done, &ev, sizeof(ev)))
				return done ? done : -EFAULT;
			done += sizeof(ev);
		}
		if (done)
			return done;

		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;

		/* another reader may beat us to it, hence the loop */
		ret = wait_event_interruptible(priv->wq, zybo_btn_pending(priv));
		if (ret)
			return ret;
	}
}

static __poll_t zybo_btn_poll(struct file *file, poll_table *wait)
{
	struct zybo_btn_priv *priv = container_of(file->private_data,
						  struct zybo_btn_priv, misc);

	poll_wait(file, &priv->wq, wait);

	return zybo_btn_pending(priv) ? EPOLLIN | EPOLLRDNORM : 0;
}

static const struct file_operations zybo_btn_fops = {
	.owner	= THIS_MODULE,
	.read	= zybo_btn_read,
	.poll	= zybo_btn_poll,
	.llseek	= noop_llseek,
};

static ssize_t debounce_ms_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct zybo_btn_priv *priv = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", READ_ONCE(priv->debounce_ms));
}

static ssize_t debounce_ms_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct zybo_btn_priv *priv = dev_get_drvdata(dev);
	unsigned int val;
	int ret;

	ret = kstrtouint(buf, 0, &val);
	if (ret)
		return ret;
	if (val < 1 || val > 1000)
		return -EINVAL;

	WRITE_ONCE(priv->debounce_ms, val);
	return count;
}
static DEVICE_ATTR_RW(debounce_ms);

static ssize_t presses_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct zybo_btn_priv *priv = dev_get_drvdata(dev);
	int len = 0;
	unsigned int i;

	for (i = 0; i < priv->nbtn; i++)
		len += sysfs_emit_at(buf, len, "%s%lu", i ? " " : "",
				     READ_ONCE(priv->btn[i].presses));
	len += sysfs_emit_at(buf, len, "\n");

	return len;
}
static DEVICE_ATTR_RO(presses);

static ssize_t dropped_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct zybo_btn_priv *priv = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%lu\n", READ_ONCE(priv->dropped));
}
static DEVICE_ATTR_RO(dropped);

static struct attribute *zybo_btn_attrs[] = {
	&dev_attr_debounce_ms.attr,
	&dev_attr_presses.attr,
	&dev_attr_dropped.attr,
	NULL
};
ATTRIBUTE_GROUPS(zybo_btn);

static void zybo_btn_cancel_work(void *data)
{
	struct zybo_btn_priv *priv = data;
	unsigned int i;

	for (i = 0; i < priv->nbtn; i++)
		cancel_delayed_work_sync(&priv->btn[i].work);
}

static void zybo_btn_misc_deregister(void *data)
{
	misc_deregister(data);
}

static int zybo_btn_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct zybo_btn_priv *priv;
	unsigned int i;
	int count, irq, ret;
	u32 debounce;

	count = gpiod_count(dev, "button");
	if (count <= 0)
		return dev_err_probe(dev, count ? count : -ENODEV,
				     "no button-gpios in DT\n");

	priv = devm_kzalloc(dev, struct_size(priv, btn, count), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	priv->nbtn = count;
	priv->debounce_ms = ZYBO_BTN_DEBOUNCE_MS;
	if (!device_property_read_u32(dev, "debounce-interval", &debounce) &&
	    debounce >= 1 && debounce <= 1000)
		priv->debounce_ms = debounce;

	spin_lock_init(&priv->lock);
	init_waitqueue_head(&priv->wq);
	INIT_KFIFO(priv->fifo);
	platform_set_drvdata(pdev, priv);

	for (i = 0; i < priv->nbtn; i++) {
		priv->btn[i].priv = priv;
		priv->btn[i].index = i;
		INIT_DELAYED_WORK(&priv->btn[i].work, zybo_btn_work);
	}

	/* Acquire all GPIOs before registering work cancellation. On unwind:
	 * free IRQs -> cancel work -> release GPIOs -> free private data.
	 */
	for (i = 0; i < priv->nbtn; i++) {
		struct zybo_btn *btn = &priv->btn[i];

		btn->gpiod = devm_gpiod_get_index(dev, "button", i, GPIOD_IN);
		if (IS_ERR(btn->gpiod))
			return dev_err_probe(dev, PTR_ERR(btn->gpiod),
					     "button %u: gpio\n", i);

		btn->level = gpiod_get_value_cansleep(btn->gpiod);
		if (btn->level < 0)
			return dev_err_probe(dev, btn->level,
					     "button %u: initial level\n", i);
	}

	ret = devm_add_action_or_reset(dev, zybo_btn_cancel_work, priv);
	if (ret)
		return ret;

	for (i = 0; i < priv->nbtn; i++) {
		struct zybo_btn *btn = &priv->btn[i];

		irq = gpiod_to_irq(btn->gpiod);
		if (irq < 0)
			return dev_err_probe(dev, irq, "button %u: irq\n", i);

		ret = devm_request_irq(dev, irq, zybo_btn_isr,
				       IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
				       dev_name(dev), btn);
		if (ret)
			return dev_err_probe(dev, ret, "button %u: request irq\n", i);
	}

	/* single instance, the board only has one set of MIO buttons */
	priv->misc.minor = MISC_DYNAMIC_MINOR;
	priv->misc.name = "zybo_btn";
	priv->misc.fops = &zybo_btn_fops;
	priv->misc.parent = dev;

	ret = misc_register(&priv->misc);
	if (ret)
		return dev_err_probe(dev, ret, "misc_register\n");

	ret = devm_add_action_or_reset(dev, zybo_btn_misc_deregister, &priv->misc);
	if (ret)
		return ret;

	dev_info(dev, "%u buttons, debounce %u ms\n",
		 priv->nbtn, priv->debounce_ms);
	return 0;
}

static const struct of_device_id zybo_btn_of_match[] = {
	{ .compatible = "asv,zybo-btn" },
	{ }
};
MODULE_DEVICE_TABLE(of, zybo_btn_of_match);

static struct platform_driver zybo_btn_driver = {
	.probe = zybo_btn_probe,
	.driver = {
		.name = "zybo-btn",
		.of_match_table = zybo_btn_of_match,
		.dev_groups = zybo_btn_groups,
		/* an open file keeps priv alive, don't allow unbind under it */
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(zybo_btn_driver);

MODULE_AUTHOR("Asvasist");
MODULE_DESCRIPTION("Zybo Z7 MIO push-button driver");
MODULE_LICENSE("GPL");
