// ssd1306_text_i2c.c
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/string.h>

#define DEV_NAME "ssd1306"
#define WIDTH  128
#define HEIGHT 64
#define FB_SZ  (WIDTH * HEIGHT / 8)

static int bus = 1;
static int addr = 0x3c;
module_param(bus, int, 0444);
module_param(addr, int, 0444);

static struct i2c_client *g_client;
static u8 fb[FB_SZ];
static DEFINE_MUTEX(oled_lock);

static int ssd1306_cmd(u8 c)
{
	u8 buf[2] = {0x00, c}; /* 0x00 = command */
	return i2c_master_send(g_client, buf, 2);
}

static int ssd1306_data(const u8 *data, size_t len)
{
	/* 0x40 = data, chunk write */
	int ret;
	u8 *buf;
	size_t chunk, i = 0;

	buf = kmalloc(1 + 32, GFP_KERNEL);
	if (!buf) return -ENOMEM;

	while (i < len) {
		chunk = min((size_t)32, len - i);
		buf[0] = 0x40;
		memcpy(&buf[1], &data[i], chunk);
		ret = i2c_master_send(g_client, buf, 1 + chunk);
		if (ret < 0) { kfree(buf); return ret; }
		i += chunk;
	}

	kfree(buf);
	return 0;
}

static void set_pixel(int x, int y, int on)
{
	int idx;
	u8 mask;

	if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return;
	idx = x + (y / 8) * WIDTH;
	mask = 1u << (y % 8);
	if (on) fb[idx] |= mask;
	else    fb[idx] &= ~mask;
}

/* 최소 글꼴: 필요한 문자만(0-9, :, T,H,=,%,C,space,.,-) */
static const struct {
	char ch;
	u8 col[5];
} font5x7[] = {
	{' ', {0x00,0x00,0x00,0x00,0x00}},
	{'0', {0x3E,0x51,0x49,0x45,0x3E}},
	{'1', {0x00,0x42,0x7F,0x40,0x00}},
	{'2', {0x42,0x61,0x51,0x49,0x46}},
	{'3', {0x21,0x41,0x45,0x4B,0x31}},
	{'4', {0x18,0x14,0x12,0x7F,0x10}},
	{'5', {0x27,0x45,0x45,0x45,0x39}},
	{'6', {0x3C,0x4A,0x49,0x49,0x30}},
	{'7', {0x01,0x71,0x09,0x05,0x03}},
	{'8', {0x36,0x49,0x49,0x49,0x36}},
	{'9', {0x06,0x49,0x49,0x29,0x1E}},
	{':', {0x00,0x36,0x36,0x00,0x00}},
	{'=', {0x14,0x14,0x14,0x14,0x14}},
	{'-', {0x08,0x08,0x08,0x08,0x08}},
	{'.', {0x00,0x60,0x60,0x00,0x00}},
	{'%', {0x62,0x64,0x08,0x13,0x23}},
	{'C', {0x3E,0x41,0x41,0x41,0x22}},
	{'H', {0x7F,0x08,0x08,0x08,0x7F}},
	{'T', {0x01,0x01,0x7F,0x01,0x01}},
};

static const u8 *glyph(char c)
{
	int i;
	for (i = 0; i < ARRAY_SIZE(font5x7); i++) {
		if (font5x7[i].ch == c)
			return font5x7[i].col;
	}
	return font5x7[0].col; /* space */
}

static void draw_char(int x, int y, char c)
{
	int col, row;
	const u8 *g = glyph(c);

	for (col = 0; col < 5; col++) {
		u8 bits = g[col];
		for (row = 0; row < 7; row++) {
			int on = (bits >> row) & 1;
			set_pixel(x + col, y + row, on);
		}
	}
}

static void draw_text_line(int x, int y, const char *s)
{
	int i = 0;
	while (s[i] && x < (WIDTH - 6)) {
		draw_char(x, y, s[i]);
		x += 6; /* 5 + 1 spacing */
		i++;
	}
}

static int ssd1306_update(void)
{
	int p, ret;

	/* set addressing */
	ret = ssd1306_cmd(0x20); if (ret < 0) return ret; /* memory mode */
	ret = ssd1306_cmd(0x00); if (ret < 0) return ret; /* horizontal */

	for (p = 0; p < 8; p++) {
		ret = ssd1306_cmd(0xB0 + p); if (ret < 0) return ret; /* page */
		ret = ssd1306_cmd(0x00);     if (ret < 0) return ret; /* col low */
		ret = ssd1306_cmd(0x10);     if (ret < 0) return ret; /* col high */
		ret = ssd1306_data(&fb[p * WIDTH], WIDTH);
		if (ret < 0) return ret;
	}
	return 0;
}

static int ssd1306_init_panel(void)
{
	int ret;

	ret = ssd1306_cmd(0xAE); if (ret < 0) return ret; /* display off */
	ret = ssd1306_cmd(0xD5); ret |= ssd1306_cmd(0x80);
	ret |= ssd1306_cmd(0xA8); ret |= ssd1306_cmd(0x3F);
	ret |= ssd1306_cmd(0xD3); ret |= ssd1306_cmd(0x00);
	ret |= ssd1306_cmd(0x40);
	ret |= ssd1306_cmd(0x8D); ret |= ssd1306_cmd(0x14);
	ret |= ssd1306_cmd(0x20); ret |= ssd1306_cmd(0x00);
	ret |= ssd1306_cmd(0xA1);
	ret |= ssd1306_cmd(0xC8);
	ret |= ssd1306_cmd(0xDA); ret |= ssd1306_cmd(0x12);
	ret |= ssd1306_cmd(0x81); ret |= ssd1306_cmd(0x7F);
	ret |= ssd1306_cmd(0xD9); ret |= ssd1306_cmd(0xF1);
	ret |= ssd1306_cmd(0xDB); ret |= ssd1306_cmd(0x40);
	ret |= ssd1306_cmd(0xA4);
	ret |= ssd1306_cmd(0xA6);
	ret |= ssd1306_cmd(0xAF); /* display on */
	if (ret < 0) return ret;

	memset(fb, 0x00, sizeof(fb));
	return ssd1306_update();
}

static ssize_t oled_write(struct file *f, const char __user *ubuf, size_t cnt, loff_t *ppos)
{
	char *kbuf;
	char *line1, *line2;
	size_t n = cnt;

	if (n == 0)
		return 0;

	if (n > 256 && n != FB_SZ)
		n = 256;

	kbuf = kzalloc(n + 1, GFP_KERNEL);
	if (!kbuf) return -ENOMEM;

	if (copy_from_user(kbuf, ubuf, n)) {
		kfree(kbuf);
		return -EFAULT;
	}
	kbuf[n] = '\0';

	mutex_lock(&oled_lock);

	if (n == FB_SZ) {
		memcpy(fb, kbuf, FB_SZ);
		ssd1306_update();
		mutex_unlock(&oled_lock);
		kfree(kbuf);
		return cnt;
	}

	/* text mode */
	memset(fb, 0x00, sizeof(fb));

	line1 = kbuf;
	line2 = strchr(kbuf, '\n');
	if (line2) {
		*line2 = '\0';
		line2++;
	}

	draw_text_line(0, 0, line1);
	if (line2)
		draw_text_line(0, 16, line2);

	ssd1306_update();

	mutex_unlock(&oled_lock);
	kfree(kbuf);
	return cnt;
}

static const struct file_operations oled_fops = {
	.owner = THIS_MODULE,
	.write = oled_write,
};

static struct miscdevice oled_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = DEV_NAME,
	.fops  = &oled_fops,
};

static int __init oled_init(void)
{
	struct i2c_adapter *adap;
	int ret;

	adap = i2c_get_adapter(bus);
	if (!adap) {
		pr_err("ssd1306: no i2c adapter %d\n", bus);
		return -ENODEV;
	}

	g_client = i2c_new_dummy_device(adap, addr);
	i2c_put_adapter(adap);
	if (IS_ERR(g_client)) {
		pr_err("ssd1306: cannot create i2c client addr=0x%x\n", addr);
		return PTR_ERR(g_client);
	}

	ret = misc_register(&oled_misc);
	if (ret) {
		i2c_unregister_device(g_client);
		return ret;
	}

	ret = ssd1306_init_panel();
	if (ret < 0) {
		misc_deregister(&oled_misc);
		i2c_unregister_device(g_client);
		return ret;
	}

	pr_info("ssd1306 ready: /dev/%s (bus=%d addr=0x%x)\n", DEV_NAME, bus, addr);
	return 0;
}

static void __exit oled_exit(void)
{
	memset(fb, 0x00, sizeof(fb));
	ssd1306_update();

	misc_deregister(&oled_misc);
	i2c_unregister_device(g_client);
	pr_info("ssd1306 exit\n");
}

module_init(oled_init);
module_exit(oled_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("kkk + patched");
MODULE_DESCRIPTION("SSD1306 I2C text/raw chardev (/dev/ssd1306)");

