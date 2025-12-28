// dht11_ledbar.c
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/gpio.h>
#include <linux/fs.h>
#include <linux/delay.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/device.h>
#include <linux/jiffies.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/ioctl.h>

#define DRIVER_NAME "dht11"
#define CLASS_NAME  "dht11_class"

/* ===== wiring (BCM) ===== */
static int dht_gpio = 4;
module_param(dht_gpio, int, 0444);

static int led_gpios[8] = {23, 24, 25, 12, 16, 20, 21, 26};
static int led_gpios_num = 8;
module_param_array(led_gpios, int, &led_gpios_num, 0444);

/* ===== ioctl ===== */
#define DHT11_IOCTL_MAGIC 'd'
struct dht11_data {
	__u8 temp;
	__u8 humi;
	__u8 ok;   /* 1=valid, 0=invalid */
};
#define DHT11_IOCTL_READ _IOR(DHT11_IOCTL_MAGIC, 0x01, struct dht11_data)

/* ===== chardev ===== */
static dev_t device_number;
static struct cdev dht_cdev;
static struct class *dht_class;

/* ===== cache ===== */
static DEFINE_MUTEX(dht_lock);
static struct dht11_data g_cache;
static unsigned long g_last_sample_j;

/* ===== autopoll ===== */
static int autopoll = 1;          /* 1=enabled */
module_param(autopoll, int, 0644);
static int poll_ms = 2000;        /* DHT11은 보통 1초 이상 간격 권장 */
module_param(poll_ms, int, 0644);

static struct delayed_work poll_work;

static inline void ledbar_apply_from_humi(u8 humi)
{
	int level, i;

	/* 0~100% => 0~8 단계 (올림) */
	level = (humi * 8 + 99) / 100;
	if (level < 0) level = 0;
	if (level > 8) level = 8;

	for (i = 0; i < 8; i++)
		gpio_set_value(led_gpios[i], (i < level) ? 1 : 0);
}

/* 기다리기: 핀 상태가 level이 될 때까지, 최대 time_us 마이크로초 */
static int wait_pin_status(int level, int time_us)
{
	int counter = 0;

	while (gpio_get_value(dht_gpio) != level) {
		if (++counter > time_us)
			return -ETIMEDOUT;
		udelay(1);
	}
	return counter;
}

static int dht11_sample(u8 *out_temp, u8 *out_humi)
{
	u8 data[5] = {0,};
	unsigned long flags;
	int i, bit;
	int ret;

	/* DHT11: 최소 1초 이상 샘플 간격 권장 */
	if (time_before(jiffies, g_last_sample_j + msecs_to_jiffies(1100))) {
		unsigned long left = (g_last_sample_j + msecs_to_jiffies(1100)) - jiffies;
		msleep(jiffies_to_msecs(left));
	}

	/* start signal */
	ret = gpio_direction_output(dht_gpio, 0);
	if (ret) return ret;

	msleep(20); /* >=18ms LOW */

	gpio_set_value(dht_gpio, 1);
	udelay(30); /* 20~40us HIGH */

	ret = gpio_direction_input(dht_gpio);
	if (ret) return ret;

	/* 타이밍 구간: IRQ off (총 4~5ms 내외) */
	local_irq_save(flags);

	/* sensor response: LOW(80us) -> HIGH(80us) -> LOW(50us) */
	ret = wait_pin_status(0, 200);
	if (ret < 0) goto out_irq;

	ret = wait_pin_status(1, 200);
	if (ret < 0) goto out_irq;

	ret = wait_pin_status(0, 200);
	if (ret < 0) goto out_irq;

	/* 40 bits */
	for (i = 0; i < 40; i++) {
		/* each bit: LOW 50us then HIGH (26~28us=0, ~70us=1)
		   우리는 LOW 끝나고 HIGH 시작을 기다렸다가 35us 후 샘플 */
		ret = wait_pin_status(1, 120);
		if (ret < 0) goto out_irq;

		udelay(35);
		bit = gpio_get_value(dht_gpio) ? 1 : 0;

		data[i/8] <<= 1;
		data[i/8] |= (bit & 1);

		/* wait for HIGH end -> LOW */
		ret = wait_pin_status(0, 150);
		if (ret < 0) goto out_irq;
	}

out_irq:
	local_irq_restore(flags);
	if (ret < 0)
		return ret;

	/* checksum */
	if (data[4] != ((data[0] + data[1] + data[2] + data[3]) & 0xFF))
		return -EIO;

	*out_humi = data[0];
	*out_temp = data[2];

	g_last_sample_j = jiffies;
	return 0;
}

static void poll_work_fn(struct work_struct *work)
{
	u8 t = 0, h = 0;
	int ret;

	mutex_lock(&dht_lock);
	ret = dht11_sample(&t, &h);
	if (ret == 0) {
		g_cache.temp = t;
		g_cache.humi = h;
		g_cache.ok   = 1;
		ledbar_apply_from_humi(h);
	} else {
		g_cache.ok = 0;
	}
	mutex_unlock(&dht_lock);

	if (autopoll)
		schedule_delayed_work(&poll_work, msecs_to_jiffies(poll_ms));
}

/* read: "T=23C H=45%\n" */
static ssize_t dht_read(struct file *filp, char __user *buf, size_t len, loff_t *off)
{
	char msg[64];
	int n;
	struct dht11_data d;

	if (*off > 0)
		return 0;

	mutex_lock(&dht_lock);
	d = g_cache;
	mutex_unlock(&dht_lock);

	if (d.ok)
		n = scnprintf(msg, sizeof(msg), "T=%uC H=%u%%\n", d.temp, d.humi);
	else
		n = scnprintf(msg, sizeof(msg), "DHT11 read error\n");

	if (len < n)
		return -EINVAL;

	if (copy_to_user(buf, msg, n))
		return -EFAULT;

	*off += n;
	return n;
}

static long dht_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct dht11_data d;

	if (cmd != DHT11_IOCTL_READ)
		return -ENOTTY;

	mutex_lock(&dht_lock);
	d = g_cache;
	mutex_unlock(&dht_lock);

	if (copy_to_user((void __user *)arg, &d, sizeof(d)))
		return -EFAULT;

	return 0;
}

static const struct file_operations fops = {
	.owner          = THIS_MODULE,
	.read           = dht_read,
	.unlocked_ioctl = dht_ioctl,
};

static int request_led_gpios(void)
{
	int i, ret;

	if (led_gpios_num != 8) {
		pr_err("led_gpios_num must be 8\n");
		return -EINVAL;
	}

	for (i = 0; i < 8; i++) {
		if (!gpio_is_valid(led_gpios[i]))
			return -EINVAL;

		ret = gpio_request(led_gpios[i], "dht11_ledbar");
		if (ret) goto fail;

		ret = gpio_direction_output(led_gpios[i], 0);
		if (ret) goto fail;
	}
	return 0;

fail:
	while (--i >= 0)
		gpio_free(led_gpios[i]);
	return ret;
}

static void free_led_gpios(void)
{
	int i;
	for (i = 0; i < 8; i++) {
		gpio_set_value(led_gpios[i], 0);
		gpio_free(led_gpios[i]);
	}
}

static int __init dht_init(void)
{
	int ret;

	pr_info("=== dht11 init (gpio=%d) ===\n", dht_gpio);

	/* gpio request */
	if (!gpio_is_valid(dht_gpio))
		return -EINVAL;

	ret = gpio_request(dht_gpio, "dht11_data");
	if (ret) return ret;

	/* LED BAR gpios */
	ret = request_led_gpios();
	if (ret) {
		gpio_free(dht_gpio);
		return ret;
	}

	/* chardev */
	ret = alloc_chrdev_region(&device_number, 0, 1, DRIVER_NAME);
	if (ret < 0) goto err_gpio;

	cdev_init(&dht_cdev, &fops);
	ret = cdev_add(&dht_cdev, device_number, 1);
	if (ret < 0) goto err_chrdev;

	dht_class = class_create(THIS_MODULE, CLASS_NAME);
	if (IS_ERR(dht_class)) {
		ret = PTR_ERR(dht_class);
		goto err_cdev;
	}

	if (!device_create(dht_class, NULL, device_number, NULL, DRIVER_NAME)) {
		ret = -ENOMEM;
		goto err_class;
	}

	/* init cache */
	mutex_lock(&dht_lock);
	memset(&g_cache, 0, sizeof(g_cache));
	g_cache.ok = 0;
	g_last_sample_j = jiffies - msecs_to_jiffies(2000);
	mutex_unlock(&dht_lock);

	/* start autopoll */
	INIT_DELAYED_WORK(&poll_work, poll_work_fn);
	if (autopoll)
		schedule_delayed_work(&poll_work, 0);

	pr_info("dht11 ready: /dev/%s\n", DRIVER_NAME);
	return 0;

err_class:
	class_destroy(dht_class);
err_cdev:
	cdev_del(&dht_cdev);
err_chrdev:
	unregister_chrdev_region(device_number, 1);
err_gpio:
	free_led_gpios();
	gpio_free(dht_gpio);
	return ret;
}

static void __exit dht_exit(void)
{
	if (autopoll)
		cancel_delayed_work_sync(&poll_work);

	device_destroy(dht_class, device_number);
	class_destroy(dht_class);
	cdev_del(&dht_cdev);
	unregister_chrdev_region(device_number, 1);

	free_led_gpios();
	gpio_free(dht_gpio);

	pr_info("dht11 exit\n");
}

module_init(dht_init);
module_exit(dht_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("kkk + patched");
MODULE_DESCRIPTION("DHT11 driver + LED BAR auto update");

