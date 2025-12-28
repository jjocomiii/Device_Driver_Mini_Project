// ds1302_rpi_rtc.c
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/rtc.h>
#include <linux/bcd.h>
#include <linux/platform_device.h>
#include <linux/device.h>

#define DRV_NAME "ds1302_rpi"

// DS1302 register write addresses (even). Read = addr|1
#define DS1302_REG_SECONDS   0x80
#define DS1302_REG_MINUTES   0x82
#define DS1302_REG_HOURS     0x84
#define DS1302_REG_DATE      0x86
#define DS1302_REG_MONTH     0x88
#define DS1302_REG_DAY       0x8A
#define DS1302_REG_YEAR      0x8C
#define DS1302_REG_WP        0x8E

// ====== module params (BCM GPIO 번호) ======
static int gpio_clk = 5;
static int gpio_io  = 6;
static int gpio_ce  = 13;

module_param(gpio_clk, int, 0444);
module_param(gpio_io,  int, 0444);
module_param(gpio_ce,  int, 0444);

MODULE_PARM_DESC(gpio_clk, "DS1302 CLK GPIO (BCM)");
MODULE_PARM_DESC(gpio_io,  "DS1302 IO/DAT GPIO (BCM)");
MODULE_PARM_DESC(gpio_ce,  "DS1302 CE/RST GPIO (BCM)");

struct ds1302_priv {
	struct rtc_device *rtc;
	struct mutex lock;
};

static struct ds1302_priv *g_priv;

static inline void ds_delay(void) { udelay(1); }

static inline void clk_write(int v) { gpio_set_value(gpio_clk, v); }
static inline void ce_write(int v)  { gpio_set_value(gpio_ce,  v); }
static inline void io_write(int v)  { gpio_set_value(gpio_io,  v); }
static inline int  io_read(void)    { return gpio_get_value(gpio_io); }

static inline void io_dir_out(int v) { gpio_direction_output(gpio_io, v); }
static inline void io_dir_in(void)   { gpio_direction_input(gpio_io); }

static inline void ds1302_clock_pulse(void)
{
	clk_write(1); ds_delay();
	clk_write(0); ds_delay();
}

// LSB first TX
static void ds1302_tx_u8(u8 v)
{
	int i;
	io_dir_out(0);
	for (i = 0; i < 8; i++) {
		io_write((v >> i) & 1);
		ds_delay();
		ds1302_clock_pulse();
	}
}

// LSB first RX  ✅ FIXED: 8 clocks, sample on CLK=1
static u8 ds1302_rx_u8(void)
{
	int i;
	u8 temp = 0;

	io_dir_in();
	ds_delay();

	for (i = 0; i < 8; i++) {
		clk_write(1);
		ds_delay();
		if (io_read())
			temp |= (1u << i);
		clk_write(0);
		ds_delay();
	}
	return temp;
}

static inline void ds1302_begin(void)
{
	clk_write(0);
	ce_write(1);
	udelay(4);
}

static inline void ds1302_end(void)
{
	udelay(1);
	ce_write(0);
	udelay(4);
}

static void ds1302_write_reg_raw(u8 reg_even, u8 raw_bcd)
{
	ds1302_begin();
	ds1302_tx_u8(reg_even & 0xFE); // write
	ds1302_tx_u8(raw_bcd);
	ds1302_end();
}

static u8 ds1302_read_reg_raw(u8 reg_even)
{
	u8 v;
	ds1302_begin();
	ds1302_tx_u8((reg_even & 0xFE) | 0x01); // read = addr|1
	v = ds1302_rx_u8();
	ds1302_end();
	return v;
}

static void ds1302_write_protect(bool enable)
{
	ds1302_write_reg_raw(DS1302_REG_WP, enable ? 0x80 : 0x00);
}

// CH bit(Seconds[7])가 1이면 오실레이터 stop 상태 -> 내려서 동작시키기
static void ds1302_ensure_osc_running(struct ds1302_priv *p)
{
	u8 sec;

	mutex_lock(&p->lock);
	ds1302_write_protect(false);

	sec = ds1302_read_reg_raw(DS1302_REG_SECONDS);
	if (sec & 0x80) { // CH set => stopped
		ds1302_write_reg_raw(DS1302_REG_SECONDS, sec & 0x7F); // CH=0
	}

	ds1302_write_protect(true);
	mutex_unlock(&p->lock);
}

// ✅ 안전하게 drvdata 찾기(널이면 ENODEV로 반환)
static inline struct ds1302_priv *ds_priv_from_dev(struct device *dev)
{
	struct ds1302_priv *p = NULL;

	if (dev)
		p = dev_get_drvdata(dev);
	if (!p && dev && dev->parent)
		p = dev_get_drvdata(dev->parent);
	if (!p)
		p = g_priv; // fallback

	return p;
}

static int ds1302_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	struct ds1302_priv *p = ds_priv_from_dev(dev);
	u8 raw_sec, sec, min, hour, mday, mon, wday, year;

	if (!p)
		return -ENODEV;

	mutex_lock(&p->lock);

	raw_sec = ds1302_read_reg_raw(DS1302_REG_SECONDS);
	// CH가 켜져있으면(멈춤) 읽는 김에 바로 내려서 살려줌
	if (raw_sec & 0x80) {
		ds1302_write_protect(false);
		ds1302_write_reg_raw(DS1302_REG_SECONDS, raw_sec & 0x7F);
		ds1302_write_protect(true);
		raw_sec &= 0x7F;
	}
	sec  = raw_sec & 0x7F;

	min  = ds1302_read_reg_raw(DS1302_REG_MINUTES);
	hour = ds1302_read_reg_raw(DS1302_REG_HOURS);
	mday = ds1302_read_reg_raw(DS1302_REG_DATE);
	mon  = ds1302_read_reg_raw(DS1302_REG_MONTH);
	wday = ds1302_read_reg_raw(DS1302_REG_DAY);
	year = ds1302_read_reg_raw(DS1302_REG_YEAR);

	mutex_unlock(&p->lock);

	tm->tm_sec  = bcd2bin(sec);
	tm->tm_min  = bcd2bin(min);

	// 24h 기준, 12h 케이스도 방어
	if (hour & 0x80) {
		int h = bcd2bin(hour & 0x1F);
		bool pm = !!(hour & 0x20);
		if (pm && h < 12) h += 12;
		if (!pm && h == 12) h = 0;
		tm->tm_hour = h;
	} else {
		tm->tm_hour = bcd2bin(hour & 0x3F);
	}

	tm->tm_mday = bcd2bin(mday);
	tm->tm_mon  = bcd2bin(mon) - 1;        // 0~11
	tm->tm_wday = (bcd2bin(wday) + 6) % 7; // 1~7 -> 0~6
	tm->tm_year = 100 + bcd2bin(year);     // 00~99 -> 2000~2099

	return 0;
}

static int ds1302_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	struct ds1302_priv *p = ds_priv_from_dev(dev);

	int s, mi, h, d, mon0, w0, y;
	u8 sec_run, sec_halt, min, hour, mday, mon, wday, year;

	if (!p)
		return -ENODEV;

	// ---- userland 실수 방어 (tm_year를 2025로 넣어도 돌아가게) ----
	s = tm->tm_sec;   if (s < 0) s = 0;   if (s > 59) s = 59;
	mi = tm->tm_min;  if (mi < 0) mi = 0; if (mi > 59) mi = 59;
	h = tm->tm_hour;  if (h < 0) h = 0;   if (h > 23) h = 23;

	d = tm->tm_mday;  if (d < 1) d = 1;   if (d > 31) d = 31;

	mon0 = tm->tm_mon;
	if (mon0 >= 1 && mon0 <= 12) mon0 -= 1;   // 1~12로 넣은 실수 방어
	if (mon0 < 0) mon0 = 0;
	if (mon0 > 11) mon0 = 11;

	w0 = tm->tm_wday;
	if (w0 >= 1 && w0 <= 7) w0 -= 1;          // 1~7로 넣은 실수 방어
	if (w0 < 0) w0 = 0;
	if (w0 > 6) w0 = 6;

	y = tm->tm_year;
	if (y > 1900) y -= 1900;                  // 2025 같은 absolute year 방어
	if (y >= 100) y -= 100;                   // 2000~2099 => 00~99
	if (y < 0) y = 0;
	if (y > 99) y %= 100;

	sec_run  = bin2bcd(s) & 0x7F;             // CH=0
	sec_halt = sec_run | 0x80;                // CH=1 (halt)
	min  = bin2bcd(mi);
	hour = bin2bcd(h);                        // 24h
	mday = bin2bcd(d);
	mon  = bin2bcd(mon0 + 1);
	wday = bin2bcd(w0 + 1);                   // 0~6 -> 1~7
	year = bin2bcd(y);

	mutex_lock(&p->lock);

	ds1302_write_protect(false);

	// ✅ 안전하게: 멈춘 상태(CH=1)로 초 먼저 써서 스톱 -> 나머지 -> 마지막에 CH=0로 재시작
	ds1302_write_reg_raw(DS1302_REG_SECONDS, sec_halt);

	ds1302_write_reg_raw(DS1302_REG_MINUTES, min);
	ds1302_write_reg_raw(DS1302_REG_HOURS,   hour);
	ds1302_write_reg_raw(DS1302_REG_DATE,    mday);
	ds1302_write_reg_raw(DS1302_REG_MONTH,   mon);
	ds1302_write_reg_raw(DS1302_REG_DAY,     wday);
	ds1302_write_reg_raw(DS1302_REG_YEAR,    year);

	ds1302_write_reg_raw(DS1302_REG_SECONDS, sec_run); // CH=0, start

	ds1302_write_protect(true);

	mutex_unlock(&p->lock);
	return 0;
}

static const struct rtc_class_ops ds1302_rtc_ops = {
	.read_time = ds1302_rtc_read_time,
	.set_time  = ds1302_rtc_set_time,
};

static int ds1302_request_gpios(void)
{
	int ret;

	if (!gpio_is_valid(gpio_clk) || !gpio_is_valid(gpio_io) || !gpio_is_valid(gpio_ce))
		return -EINVAL;

	ret = gpio_request(gpio_clk, DRV_NAME "_clk");
	if (ret) return ret;
	ret = gpio_request(gpio_io,  DRV_NAME "_io");
	if (ret) { gpio_free(gpio_clk); return ret; }
	ret = gpio_request(gpio_ce,  DRV_NAME "_ce");
	if (ret) { gpio_free(gpio_io); gpio_free(gpio_clk); return ret; }

	gpio_direction_output(gpio_clk, 0);
	gpio_direction_output(gpio_ce,  0);
	gpio_direction_output(gpio_io,  0);

	return 0;
}

static void ds1302_free_gpios(void)
{
	gpio_set_value(gpio_ce, 0);
	gpio_set_value(gpio_clk, 0);

	gpio_free(gpio_ce);
	gpio_free(gpio_io);
	gpio_free(gpio_clk);
}

static int ds1302_probe(struct platform_device *pdev)
{
	struct ds1302_priv *p;
	int ret;

	ret = ds1302_request_gpios();
	if (ret) return ret;

	p = devm_kzalloc(&pdev->dev, sizeof(*p), GFP_KERNEL);
	if (!p) {
		ds1302_free_gpios();
		return -ENOMEM;
	}

	mutex_init(&p->lock);
	platform_set_drvdata(pdev, p);

	// ✅ fallback 세팅(혹시 drvdata 못 찾는 케이스 방어)
	g_priv = p;

	// ✅ 모듈 로드 시 오실레이터(Clock Halt) 풀어주기
	ds1302_ensure_osc_running(p);

	p->rtc = devm_rtc_device_register(&pdev->dev, DRV_NAME, &ds1302_rtc_ops, THIS_MODULE);
	if (IS_ERR(p->rtc)) {
		ret = PTR_ERR(p->rtc);
		g_priv = NULL;
		ds1302_free_gpios();
		return ret;
	}

	// ✅ rtc device에서도 drvdata를 바로 찾을 수 있게 세팅
	dev_set_drvdata(&p->rtc->dev, p);

	dev_info(&pdev->dev, "loaded (CLK=%d IO=%d CE=%d) -> /dev/rtcX\n",
	         gpio_clk, gpio_io, gpio_ce);
	return 0;
}

static int ds1302_remove(struct platform_device *pdev)
{
	struct ds1302_priv *p = platform_get_drvdata(pdev);
	if (g_priv == p)
		g_priv = NULL;

	ds1302_free_gpios();
	return 0;
}

static struct platform_driver ds1302_driver = {
	.probe  = ds1302_probe,
	.remove = ds1302_remove,
	.driver = {
		.name = DRV_NAME,
	},
};

// 우리가 DT로 붙이는 게 아니라서 가짜 platform_device 하나 만들어 probe 트리거
static struct platform_device *ds1302_pdev;

static int __init ds1302_mod_init(void)
{
	int ret;

	ret = platform_driver_register(&ds1302_driver);
	if (ret) return ret;

	ds1302_pdev = platform_device_register_simple(DRV_NAME, -1, NULL, 0);
	if (IS_ERR(ds1302_pdev)) {
		ret = PTR_ERR(ds1302_pdev);
		platform_driver_unregister(&ds1302_driver);
		return ret;
	}

	return 0;
}

static void __exit ds1302_mod_exit(void)
{
	platform_device_unregister(ds1302_pdev);
	platform_driver_unregister(&ds1302_driver);
}

module_init(ds1302_mod_init);
module_exit(ds1302_mod_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("minseong");
MODULE_DESCRIPTION("DS1302 RTC driver (GPIO bit-bang) for Raspberry Pi wiring");
MODULE_VERSION("0.4");
