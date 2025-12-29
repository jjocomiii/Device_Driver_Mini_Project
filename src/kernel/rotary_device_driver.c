#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/jiffies.h>
#include <linux/poll.h>

#define DRIVER_NAME "rotary_device_driver"
#define CLASS_NAME  "rotary_device_class"
#define DEV_NAME    "rotary"

/* === 너 배선 고정 === */
#define S1_GPIO   17
#define S2_GPIO   27
#define KEY_GPIO  22

/* 디바운스(필요하면 조절) */
#define ROT_DEBOUNCE_MS  3
#define KEY_DEBOUNCE_MS  80

MODULE_LICENSE("GPL");
MODULE_AUTHOR("kkk + patched");
MODULE_DESCRIPTION("rotary + key driver");

/* ====== chardev ====== */
static dev_t device_number;
static struct cdev rotary_cdev;
static struct class *rotary_class;

/* ====== irq ====== */
static int irq_s1;
static int irq_key;

/* ====== 상태 ====== */
static long rotary_value;

/* ====== 이벤트 큐(작게) ====== */
enum { EV_ROTATE = 0, EV_KEY = 1 };

struct rot_event {
	int type;   // EV_ROTATE / EV_KEY
	int value;  // rotate: +1/-1, key: 1
};

#define QSIZE 32
static struct rot_event q[QSIZE];
static int qh, qt;

static DECLARE_WAIT_QUEUE_HEAD(rotary_wait_queue);

static unsigned long last_rot_j;
static unsigned long last_key_j;

/* 키 active-low 여부 (대부분 pull-up이라 눌림=0) */
static int key_active_low = 1;
module_param(key_active_low, int, 0444);

/* 방향 반대면 1 */
static int invert_dir = 0;
module_param(invert_dir, int, 0444);

static inline int q_empty(void) { return qh == qt; }
static inline int q_full(void)  { return ((qh + 1) % QSIZE) == qt; }

static void q_push(int type, int value)
{
	if (!q_full()) {
		q[qh].type  = type;
		q[qh].value = value;
		qh = (qh + 1) % QSIZE;
	}
	wake_up_interruptible(&rotary_wait_queue);
}

static int q_pop(struct rot_event *out)
{
	if (q_empty())
		return 0;
	*out = q[qt];
	qt = (qt + 1) % QSIZE;
	return 1;
}

/* ===== ISR: S1 Falling에서 방향 판정(S2 레벨) ===== */
static irqreturn_t rotary_s1_isr(int irq, void *dev_id)
{
	unsigned long now = jiffies;

	if (time_before(now, last_rot_j + msecs_to_jiffies(ROT_DEBOUNCE_MS)))
		return IRQ_HANDLED;
	last_rot_j = now;

	/* 원본 로직 유지: S1 falling 시점의 S2로 방향 결정 */
	{
		int s2 = gpio_get_value(S2_GPIO);
		int step = (s2 == 1) ? +1 : -1;   /* 필요하면 여기 반대로 */
		if (invert_dir) step = -step;

		rotary_value += step;
		q_push(EV_ROTATE, step);
		printk(KERN_INFO "rotary: S2=%d step=%d total=%ld\n", s2, step, rotary_value);

	}

	return IRQ_HANDLED;
}

/* ===== ISR: KEY (양엣지 받고, press만 필터) ===== */
static irqreturn_t rotary_key_isr(int irq, void *dev_id)
{
	unsigned long now = jiffies;
	int level, pressed;

	if (time_before(now, last_key_j + msecs_to_jiffies(KEY_DEBOUNCE_MS)))
		return IRQ_HANDLED;
	last_key_j = now;

	level = gpio_get_value(KEY_GPIO);
	pressed = key_active_low ? (level == 0) : (level == 1);

	if (!pressed)
		return IRQ_HANDLED; /* release 무시 */

	q_push(EV_KEY, 1);
	return IRQ_HANDLED;
}

/* ===== read: 이벤트 1개를 텍스트로 전달 =====
   ROTATE: "R +1 123\n" (delta, total)
   KEY:    "K\n"
*/
static ssize_t rotary_read(struct file *file, char __user *user_buff,
                           size_t count, loff_t *ppos)
{
	char buffer[64];
	int len;
	struct rot_event ev;

	if (wait_event_interruptible(rotary_wait_queue, !q_empty()))
		return -ERESTARTSYS;

	if (!q_pop(&ev))
		return 0;

	if (ev.type == EV_KEY) {
		len = snprintf(buffer, sizeof(buffer), "K\n");
	} else {
		len = snprintf(buffer, sizeof(buffer), "R %d %ld\n", ev.value, rotary_value);
	}

	if (count < len)
		return -EINVAL;

	if (copy_to_user(user_buff, buffer, len))
		return -EFAULT;

	return len;
}

static __poll_t rotary_poll(struct file *file, poll_table *wait)
{
	__poll_t mask = 0;
	poll_wait(file, &rotary_wait_queue, wait);
	if (!q_empty())
		mask |= POLLIN | POLLRDNORM;
	return mask;
}

static struct file_operations fops = {
	.owner = THIS_MODULE,
	.read  = rotary_read,
	.poll  = rotary_poll,
};

static int __init rotary_driver_init(void)
{
	int ret;

	printk(KERN_INFO "===== rotary initializing =====\n");

	qh = qt = 0;
	rotary_value = 0;
	last_rot_j = 0;
	last_key_j = 0;

	/* 1) alloc dev number */
	ret = alloc_chrdev_region(&device_number, 0, 1, DRIVER_NAME);
	if (ret < 0) {
		printk(KERN_ERR "ERROR: alloc_chrdev_region\n");
		return ret;
	}

	/* 2) cdev add */
	cdev_init(&rotary_cdev, &fops);
	ret = cdev_add(&rotary_cdev, device_number, 1);
	if (ret < 0) {
		printk(KERN_ERR "ERROR: cdev_add\n");
		unregister_chrdev_region(device_number, 1);
		return ret;
	}

	/* 3) class / device */
	rotary_class = class_create(THIS_MODULE, CLASS_NAME);
	if (IS_ERR(rotary_class)) {
		ret = PTR_ERR(rotary_class);
		cdev_del(&rotary_cdev);
		unregister_chrdev_region(device_number, 1);
		return ret;
	}
	device_create(rotary_class, NULL, device_number, NULL, DEV_NAME); /* /dev/rotary */

	/* 4) GPIO request */
	ret = gpio_request(S1_GPIO, "rotary_s1");
	if (ret) goto err_gpio;
	ret = gpio_request(S2_GPIO, "rotary_s2");
	if (ret) goto err_gpio2;
	ret = gpio_request(KEY_GPIO, "rotary_key");
	if (ret) goto err_gpio3;

	gpio_direction_input(S1_GPIO);
	gpio_direction_input(S2_GPIO);
	gpio_direction_input(KEY_GPIO);

	/* 5) IRQ */
	irq_s1  = gpio_to_irq(S1_GPIO);
	irq_key = gpio_to_irq(KEY_GPIO);

	ret = request_irq(irq_s1, rotary_s1_isr, IRQF_TRIGGER_FALLING, "rotary_irq_s1", NULL);
	if (ret) goto err_irq;

	/* KEY는 rising/falling 둘 다 받고 press만 필터 */
	ret = request_irq(irq_key, rotary_key_isr,
	                  IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
	                  "rotary_irq_key", NULL);
	if (ret) goto err_irq2;

	printk(KERN_INFO "rotary driver init success -> /dev/%s (S1=%d S2=%d KEY=%d)\n",
	       DEV_NAME, S1_GPIO, S2_GPIO, KEY_GPIO);
	return 0;

err_irq2:
	free_irq(irq_s1, NULL);
err_irq:
	gpio_free(KEY_GPIO);
err_gpio3:
	gpio_free(S2_GPIO);
err_gpio2:
	gpio_free(S1_GPIO);
err_gpio:
	device_destroy(rotary_class, device_number);
	class_destroy(rotary_class);
	cdev_del(&rotary_cdev);
	unregister_chrdev_region(device_number, 1);
	return ret;
}

static void __exit rotary_driver_exit(void)
{
	free_irq(irq_key, NULL);
	free_irq(irq_s1, NULL);

	gpio_free(KEY_GPIO);
	gpio_free(S2_GPIO);
	gpio_free(S1_GPIO);

	device_destroy(rotary_class, device_number);
	class_destroy(rotary_class);
	cdev_del(&rotary_cdev);
	unregister_chrdev_region(device_number, 1);

	printk(KERN_INFO "rotary_driver_exit\n");
}

module_init(rotary_driver_init);
module_exit(rotary_driver_exit);

